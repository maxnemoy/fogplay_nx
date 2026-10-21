#pragma once
// H.264 packetization-mode 1 / HEVC RFC 7798 receiver (base layer, no DONL). No RTC, platform or decoder dependencies.
// All methods must be serialized by the owner. Time is monotonic milliseconds.
#include "features/streaming/domain/media_annexb.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace stream {
class H264RtpAssembler {
public:
    explicit H264RtpAssembler(VideoCodec codec = VideoCodec::H264) : codec_(codec), parameters_(codec) {}
    struct Frame {
        std::vector<std::byte> data;
        std::uint32_t timestamp = 0;
        std::int64_t firstSequence = 0, lastSequence = 0;
    };
    struct Stats {
        std::uint64_t received = 0, candidates = 0, reordered = 0, duplicate = 0, late = 0;
        std::uint64_t gaps = 0, missing = 0, malformed = 0, dropped = 0, emitted = 0;
        std::uint64_t resyncs = 0, recovered = 0, pressure = 0, timeout = 0, eventOverflow = 0;
        std::uint64_t faultSerial = 0, foreignSsrc = 0, padding = 0;
    };
    static constexpr std::uint64_t ReorderMs = 20, AssemblyMs = 150;
    static constexpr std::size_t MaxPending = 256, MaxPendingBytes = 2 * 1024 * 1024;
    static constexpr std::size_t MaxFrameBytes = 4 * 1024 * 1024, MaxFrames = 8;
    const Stats& stats() const { return stats_; }
    bool waiting() const { return waiting_; }
    bool needsPli() const { return waiting_; }
    std::size_t pendingPackets() const { return pending_.size(); }
    std::size_t pendingBytes() const { return pendingBytes_; }
    std::size_t assemblyBytes() const { return au_.size(); }
    void receive(const std::byte* data, std::size_t size, std::uint64_t now) {
        ++stats_.received;
        Rtp p;
        if (!parse(data, size, p)) {
            ++stats_.malformed;
            damage(3, initialized_ ? expected_ : 0, 0, now);
            return;
        }
        if (!initialized_) {
            initialized_ = true; ssrc_ = p.ssrc; expected_ = p.sequence;
        } else if (p.ssrc != ssrc_) {
            // A negotiated track is one source. Foreign packets must not damage
            // its prediction chain. Adopting another SSRC requires renegotiation.
            ++stats_.foreignSsrc; return;
        }
        const auto rawDelta = std::uint16_t(p.sequence - std::uint16_t(expected_));
        const auto delta = rawDelta < 32768 ? std::int64_t(rawDelta) : std::int64_t(rawDelta) - 65536;
        const auto seq = expected_ + delta;
        if (seq < expected_) {
            // Recently consumed packets are duplicates; older/skipped packets late.
            if (std::find(recent_.begin(), recent_.end(), seq) != recent_.end()) ++stats_.duplicate;
            else ++stats_.late;
            return;
        }
        if (pending_.count(seq)) { ++stats_.duplicate; return; }
        if (p.payload.size() > MaxPendingBytes || pending_.size() >= MaxPending ||
            p.payload.size() > MaxPendingBytes - pendingBytes_ || delta > 4096) {
            ++stats_.pressure; damage(4, seq, p.timestamp, now);
            // Drop all buffered data, but retain the sequence watermark. First
            // packet after pressure is quarantined through an observed marker.
            pending_.clear(); pendingBytes_ = 0; gapSince_ = 0; gapActive_ = false;
            expected_ = seq + 1;
            return;
        }
        if (delta > 0) {
            ++stats_.reordered;
            if (!gapActive_) { ++stats_.candidates; gapActive_ = true; gapSince_ = now; }
        }
        p.arrived = now;
        pendingBytes_ += p.payload.size();
        pending_.emplace(seq, std::move(p));
        consume(now);
        tick(now);
    }
    // Consumer fell behind: discard the complete prediction chain and quarantine
    // through an observed AU boundary before accepting a complete IDR. Keep RTP
    // sequence state and matched parameter sets; never decode a random P-frame.
    void requestResync(std::uint64_t now) {
        ++stats_.pressure;
        damage(9, initialized_ ? expected_ : 0, lastTimestamp_, now);
        // Locally discarded reorder entries are not network packet loss.
        if (!pending_.empty()) expected_ = std::max(expected_, pending_.rbegin()->first + 1);
        pending_.clear(); pendingBytes_ = 0; gapActive_ = false; gapSince_ = 0;
    }
    void tick(std::uint64_t now) {
        if (active_ && now - auSince_ >= AssemblyMs) {
            ++stats_.timeout; damage(5, expected_, timestamp_, now);
        }
        if (gapActive_ && now - gapSince_ >= ReorderMs && !pending_.empty()) {
            const auto next = pending_.begin()->first;
            if (next > expected_) {
                ++stats_.gaps; stats_.missing += std::uint64_t(next - expected_);
                damage(1, expected_, pending_.begin()->second.timestamp, now);
                expected_ = next;
            }
            gapActive_ = false;
            consume(now);
        }
    }
    bool poll(Frame& frame) {
        if (frames_.empty()) return false;
        frame = std::move(frames_.front()); frames_.pop_front();
        frameBytes_ -= frame.data.size(); return true;
    }
    bool pollEvent(std::string& event) {
        if (events_.empty()) return false;
        event = std::move(events_.front()); events_.pop_front(); return true;
    }
    std::string diagnostics() const {
        return "rtp=" + std::to_string(stats_.received) +
            " gapCandidates=" + std::to_string(stats_.candidates) +
            " reordered=" + std::to_string(stats_.reordered) +
            " duplicate=" + std::to_string(stats_.duplicate) +
            " late=" + std::to_string(stats_.late) +
            " gaps=" + std::to_string(stats_.gaps) + " missing=" + std::to_string(stats_.missing) +
            " malformed=" + std::to_string(stats_.malformed) +
            " foreignSsrc=" + std::to_string(stats_.foreignSsrc) + " padding=" + std::to_string(stats_.padding) +
            " auDrop=" + std::to_string(stats_.dropped) + " auOut=" + std::to_string(stats_.emitted) +
            " resync=" + std::to_string(stats_.resyncs) + " recovered=" + std::to_string(stats_.recovered) +
            " pressure=" + std::to_string(stats_.pressure) + " timeout=" + std::to_string(stats_.timeout) +
            " waitIdr=" + std::to_string(waiting_) + " pending=" + std::to_string(pending_.size()) +
            " pendingBytes=" + std::to_string(pendingBytes_) + " auBytes=" + std::to_string(au_.size()) +
            " nextSeq=" + std::to_string(expected_) + " lastTs=" + std::to_string(lastTimestamp_) +
            " lastFirstSeq=" + std::to_string(lastFirst_) + " lastLastSeq=" + std::to_string(lastLast_) +
            " eventDrop=" + std::to_string(stats_.eventOverflow);
    }
private:
    struct Rtp {
        std::uint16_t sequence = 0;
        std::uint32_t timestamp = 0, ssrc = 0;
        bool marker = false;
        std::uint64_t arrived = 0;
        std::vector<std::byte> payload;
    };
    static unsigned byte(std::byte b) { return std::to_integer<unsigned>(b); }
    static std::uint16_t be16(const std::byte* p) { return std::uint16_t((byte(p[0]) << 8) | byte(p[1])); }
    static std::uint32_t be32(const std::byte* p) {
        return (std::uint32_t(byte(p[0])) << 24) | (std::uint32_t(byte(p[1])) << 16) |
            (std::uint32_t(byte(p[2])) << 8) | byte(p[3]);
    }
    static bool parse(const std::byte* data, std::size_t size, Rtp& p) {
        if (size < 12 || size > 65535 || byte(data[0]) >> 6 != 2) return false;
        const auto flags = byte(data[0]);
        auto offset = std::size_t(12 + (flags & 15) * 4);
        if (offset > size) return false;
        if (flags & 16) {
            if (size - offset < 4) return false;
            const auto extra = std::size_t(be16(data + offset + 2)) * 4;
            offset += 4;
            if (extra > size - offset) return false;
            offset += extra;
        }
        if (flags & 32) {
            const auto padding = byte(data[size - 1]);
            if (!padding || padding > size - offset) return false;
            size -= padding;
        }
        if (offset == size && !(flags & 32)) return false;
        p.sequence = be16(data + 2); p.timestamp = be32(data + 4);
        p.ssrc = be32(data + 8); p.marker = (byte(data[1]) & 128) != 0;
        p.payload.assign(data + offset, data + size);
        return true;
    }
    void event(unsigned reason, std::int64_t seq, std::uint32_t timestamp, std::uint64_t now) {
        if (events_.size() >= 32) { events_.pop_front(); ++stats_.eventOverflow; }
        // Fixed keys + numeric values. Never payloads, remote text or credentials.
        events_.push_back("reason=" + std::to_string(reason) + " ms=" + std::to_string(now) +
            " seq=" + std::to_string(seq) + " ts=" + std::to_string(timestamp) +
            " missing=" + std::to_string(stats_.missing) + " waitIdr=" + std::to_string(waiting_));
    }
    void damage(unsigned reason, std::int64_t seq, std::uint32_t ts, std::uint64_t now) {
        if (active_) ++stats_.dropped;
        if (!waiting_) ++stats_.resyncs;
        waiting_ = true; boundary_ = false; active_ = false; fu_ = false;
        au_.clear();
        frames_.clear(); frameBytes_ = 0; ++stats_.faultSerial;
        event(reason, seq, ts, now);
    }
    void consume(std::uint64_t now) {
        while (!pending_.empty() && pending_.begin()->first == expected_) {
            auto p = std::move(pending_.begin()->second);
            pendingBytes_ -= p.payload.size(); pending_.erase(pending_.begin());
            const auto seq = expected_++;
            recent_.push_back(seq); if (recent_.size() > 512) recent_.pop_front();
            process(p, seq, now);
        }
        if (pending_.empty()) { gapActive_ = false; }
        else {
            if (!gapActive_) ++stats_.candidates;
            gapActive_ = true;
            gapSince_ = pending_.begin()->second.arrived;
            for (const auto& item : pending_) gapSince_ = std::min(gapSince_, item.second.arrived);
        }
    }
    bool append(const std::byte* data, std::size_t size) {
        if (size > MaxFrameBytes - au_.size()) { ++stats_.pressure; return false; }
        au_.insert(au_.end(), data, data + size); return true;
    }
    bool nal(const std::byte* data, std::size_t size) {
        if (codec_ == VideoCodec::H265) return hevcNal(data, size);
        if (!size || byte(data[0]) & 128) return false;
        const auto type = byte(data[0]) & 31;
        if (!type || type > 23 || fu_) return false;
        const std::byte prefix[]{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
        if (!append(prefix, 4) || !append(data, size)) return false;
        if (type >= 1 && type <= 5) { vcl_ = true; if (type != 5) nonIdr_ = true; }
        if (type == 5) idr_ = true;
        if (type == 7 || type == 8) observeParameter(data, size, type);
        return true;
    }
    bool payload(const Rtp& p) {
        if (codec_ == VideoCodec::H265) return hevcPayload(p);
        const auto* data = p.payload.data(); const auto size = p.payload.size();
        const auto indicator = byte(data[0]), type = indicator & 31;
        if (indicator & 128) return false;
        if (type >= 1 && type <= 23) return nal(data, size);
        if (type == 24) {
            if (fu_ || size < 4) return false;
            std::size_t offset = 1;
            while (offset < size) {
                if (size - offset < 2) return false;
                const auto length = be16(data + offset); offset += 2;
                if (!length || length > size - offset || !nal(data + offset, length)) return false;
                offset += length;
            }
            return true;
        }
        if (type != 28 || size < 3) return false;
        const auto header = byte(data[1]), nalType = header & 31;
        const bool start = (header & 128) != 0, end = (header & 64) != 0;
        if (header & 32 || !nalType || nalType > 23 || (start && end)) return false;
        const auto reconstructed = (indicator & 96) | nalType;
        if (start) {
            if (fu_) return false;
            const std::byte head[]{std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte(reconstructed)};
            fuOffset_ = au_.size();
            if (!append(head, 5)) return false;
            fu_ = true; fuHeader_ = reconstructed;
            if (nalType >= 1 && nalType <= 5) { vcl_ = true; if (nalType != 5) nonIdr_ = true; }
            if (nalType == 5) idr_ = true;
        } else if (!fu_ || fuHeader_ != reconstructed) return false;
        if (!append(data + 2, size - 2)) return false;
        if (end) {
            fu_ = false;
            if (nalType == 7 || nalType == 8)
                observeParameter(au_.data() + fuOffset_ + 4, au_.size() - fuOffset_ - 4, nalType);
        }
        return true;
    }
    static bool hevcHeader(const std::byte* data, std::size_t size) {
        // Reject multilayer and forbidden headers; temporal_id_plus1 is nonzero.
        return size >= 2 && !(byte(data[0]) & 129) && !(byte(data[1]) & 0xf8) && (byte(data[1]) & 7);
    }
    void hevcType(unsigned type) {
        if (type <= 31) { vcl_ = true; if (type != 19 && type != 20) nonIdr_ = true; }
        if (type == 19 || type == 20) idr_ = true;
    }
    bool hevcNal(const std::byte* data, std::size_t size) {
        if (!hevcHeader(data, size) || size < 3 || fu_) return false;
        const auto type = (byte(data[0]) >> 1) & 63;
        // PACI, nested packetization and reserved/unspecified NALs are unsupported.
        if (type > 40 || (type >= 10 && type <= 15) || (type >= 22 && type <= 31)) return false;
        const std::byte prefix[]{std::byte{0},std::byte{0},std::byte{0},std::byte{1}};
        if (!append(prefix, 4) || !append(data, size)) return false;
        hevcType(type);
        if (type >= 32 && type <= 34) observeParameter(data, size, type);
        return true;
    }
    bool hevcPayload(const Rtp& p) {
        const auto* data = p.payload.data(); const auto size = p.payload.size();
        if (!hevcHeader(data, size)) return false;
        const auto type = (byte(data[0]) >> 1) & 63;
        if (type < 48) return hevcNal(data, size);
        if (type == 48) {
            if (fu_) return false;
            std::size_t offset = 2, count = 0;
            unsigned minTemporal = 7;
            while (offset < size) {
                if (size - offset < 2) return false;
                const auto length = be16(data + offset); offset += 2;
                if (length < 3 || length > size - offset || !hevcNal(data + offset, length)) return false;
                minTemporal = std::min(minTemporal, byte(data[offset + 1]) & 7);
                offset += length; ++count;
            }
            return count >= 2 && (byte(data[1]) & 7) == minTemporal;
        }
        if (type != 49 || size < 4) return false;
        const auto header = byte(data[2]), nalType = header & 63;
        const bool start = header & 128, end = header & 64;
        if ((start && end) || nalType > 40 || (nalType >= 10 && nalType <= 15) ||
            (nalType >= 22 && nalType <= 31)) return false;
        const auto first = (byte(data[0]) & 129) | (nalType << 1);
        const auto reconstructed = (first << 8) | byte(data[1]);
        if (start) {
            if (fu_) return false;
            const std::byte head[]{std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte(first),data[1]};
            fuOffset_ = au_.size();
            if (!append(head, 6)) return false;
            fu_ = true; fuHeader_ = reconstructed; hevcType(nalType);
        } else if (!fu_ || fuHeader_ != reconstructed) return false;
        if (!append(data + 3, size - 3)) return false;
        if (end) {
            fu_ = false;
            if (nalType >= 32 && nalType <= 34)
                observeParameter(au_.data() + fuOffset_ + 4, au_.size() - fuOffset_ - 4, nalType);
        }
        return true;
    }
    void observeParameter(const std::byte* data, std::size_t size, unsigned type) {
        fog::detail::VideoAccessUnit unit;
        unit.nals.push_back({reinterpret_cast<const unsigned char*>(data), size, type});
        parameters_.observe(unit);
    }
    void process(const Rtp& p, std::int64_t seq, std::uint64_t now) {
        // Padding-only packets consume sequence space, but their timestamp and
        // marker have no bearing on an access unit (including an in-flight FU).
        if (p.payload.empty()) { ++stats_.padding; return; }
        // Sequence numbers order decoding. RTP timestamps are presentation
        // times and can decrease for valid B pictures; never use them as a
        // chronological watermark. A completed AU timestamp must not reopen.
        if (haveTimestamp_ && p.timestamp == lastTimestamp_) {
            ++stats_.late;
            if (active_) damage(7, seq, p.timestamp, now);
            return;
        }
        if (!boundary_) {
            // Unknown initial AU boundary: preserve only individually verified
            // whole parameter NALs. Never forward its possibly partial slices.
            if (!active_) { active_ = true; auSince_ = now; timestamp_ = p.timestamp; }
            if (timestamp_ != p.timestamp) { au_.clear(); fu_ = false; timestamp_ = p.timestamp; }
            if (!payload(p) || (p.marker && fu_)) {
                ++stats_.malformed; au_.clear(); fu_ = false;
            }
            if (p.marker) {
                boundary_ = true; active_ = false; fu_ = false; au_.clear();
                lastTimestamp_ = p.timestamp; haveTimestamp_ = true;
                ++stats_.dropped;
            }
            return;
        }
        if (active_ && p.timestamp != timestamp_) {
            damage(2, seq, timestamp_, now);
            if (p.marker) { boundary_ = true; lastTimestamp_ = p.timestamp; haveTimestamp_ = true; }
            return;
        }
        if (!active_) {
            active_ = true; timestamp_ = p.timestamp; firstSequence_ = seq; auSince_ = now;
            vcl_ = idr_ = nonIdr_ = false;
        }
        if (!payload(p) || (p.marker && fu_)) {
            ++stats_.malformed; damage(3, seq, p.timestamp, now);
            if (p.marker) { boundary_ = true; lastTimestamp_ = p.timestamp; haveTimestamp_ = true; }
            return;
        }
        if (!p.marker) return;
        active_ = false; lastTimestamp_ = p.timestamp; haveTimestamp_ = true;
        if (waiting_ && (!idr_ || nonIdr_ || !vcl_)) {
            ++stats_.dropped; au_.clear(); return;
        }
        if (waiting_) {
            const auto unit = fog::detail::inspectVideo(
                reinterpret_cast<const unsigned char*>(au_.data()), au_.size(), codec_);
            std::vector<unsigned char> prefix;
            // The media decoder owns config availability; transport guarantees
            // completeness, and can restore matched config from startup quarantine.
            const bool configured = parameters_.recoveryPrefix(unit, prefix);
            if (codec_ == VideoCodec::H265 && !configured) { ++stats_.dropped; au_.clear(); return; }
            if (configured) {
                if (prefix.size() > MaxFrameBytes - au_.size()) {
                    ++stats_.pressure; damage(4, seq, p.timestamp, now); return;
                }
                std::vector<std::byte> restored;
                restored.reserve(prefix.size() + au_.size());
                for (auto value : prefix) restored.push_back(std::byte(value));
                restored.insert(restored.end(), au_.begin(), au_.end()); au_.swap(restored);
            }
            waiting_ = false; ++stats_.recovered; event(6, seq, p.timestamp, now);
        }
        if (frames_.size() >= MaxFrames || au_.size() > 16 * 1024 * 1024 - frameBytes_) {
            ++stats_.pressure; damage(4, seq, p.timestamp, now); return;
        }
        lastFirst_ = firstSequence_; lastLast_ = seq;
        frameBytes_ += au_.size();
        frames_.push_back({std::move(au_), p.timestamp, firstSequence_, seq});
        au_.clear(); ++stats_.emitted;
    }
    VideoCodec codec_;
    Stats stats_;
    bool initialized_ = false, waiting_ = true, boundary_ = false, active_ = false;
    bool fu_ = false, vcl_ = false, idr_ = false, nonIdr_ = false, haveTimestamp_ = false, gapActive_ = false;
    unsigned fuHeader_ = 0;
    std::uint32_t ssrc_ = 0, timestamp_ = 0, lastTimestamp_ = 0;
    std::int64_t expected_ = 0, firstSequence_ = 0, lastFirst_ = 0, lastLast_ = 0;
    std::uint64_t gapSince_ = 0, auSince_ = 0;
    std::size_t pendingBytes_ = 0, frameBytes_ = 0, fuOffset_ = 0;
    std::map<std::int64_t, Rtp> pending_;
    std::deque<std::int64_t> recent_;
    std::vector<std::byte> au_;
    fog::detail::VideoParameters parameters_;
    std::deque<Frame> frames_;
    std::deque<std::string> events_;
};
}
