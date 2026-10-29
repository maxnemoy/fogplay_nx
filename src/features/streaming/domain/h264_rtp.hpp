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
        std::uint64_t firstPacketMs = 0;
    };
    struct Stats {
        std::uint64_t received = 0, candidates = 0, reordered = 0, duplicate = 0, late = 0;
        std::uint64_t gaps = 0, missing = 0, malformed = 0, dropped = 0, emitted = 0;
        std::uint64_t resyncs = 0, recovered = 0, pressure = 0, timeout = 0, eventOverflow = 0;
        std::uint64_t faultSerial = 0, foreignSsrc = 0, padding = 0;
    };
    static constexpr std::uint64_t ReorderMs = 20, RepairMs = 180, AssemblyMs = 150;
    static constexpr std::uint64_t MinRepairMs = 100, MaxRepairMs = 220, RepairAssemblyMs = 300;
    void enableNack(bool enabled) { nackEnabled_ = enabled; }
    bool missingSequence(std::uint16_t sequence) const {
        if (!gapActive_ || pending_.empty()) return false;
        const auto delta = std::uint16_t(sequence - std::uint16_t(expected_));
        return delta < MaxPending && expected_ + delta < pending_.rbegin()->first && !pending_.count(expected_ + delta);
    }
    bool requestedSequence(std::uint16_t sequence, std::uint64_t now) const {
        const auto raw = std::uint16_t(sequence - std::uint16_t(expected_));
        const auto seq = expected_ + (raw < 32768 ? std::int64_t(raw) : std::int64_t(raw) - 65536);
        const auto active = requests_.find(seq), retired = history_.find(seq);
        if (active == requests_.end() && retired == history_.end()) return false;
        const auto first = active != requests_.end() ? active->second.firstMs : retired->second.firstMs;
        return now >= first && now - first <= HistoryMs;
    }
    std::vector<std::uint16_t> takeNacks(std::uint64_t now) {
        std::vector<std::uint16_t> result;
        // The owner may scan NACKs before tick(). An expired gap cannot send
        // more requests; leave its bounded evidence for expire() to snapshot
        // before history TTL pruning, even after a long consumer stall.
        if (gapActive_ && now - gapSince_ >= gapRepairMs_) return result;
        pruneHistory(now);
        if (!nackEnabled_ || !gapActive_ || pending_.empty() || now - gapSince_ < ReorderMs || now - gapSince_ >= gapRepairMs_ || now < nextNackScanMs_) return result;
        nextNackScanMs_ = now + 10;
        const auto last = std::min(expected_ + std::int64_t(MaxPending), pending_.rbegin()->first);
        for (auto seq = expected_; seq < last; ++seq) {
            if (pending_.count(seq)) continue;
            auto& request = requests_[seq];
            if (request.attempts >= 3 || (request.attempts && now - request.lastMs < retryMs())) continue;
            if (!request.attempts) request.firstMs = now;
            ++request.attempts; request.lastMs = now;
            if (!request.diagnosticAttempts) request.diagnosticFirstMs = now;
            ++request.diagnosticAttempts; request.diagnosticLastMs = now;
            result.push_back(std::uint16_t(seq)); ++nackPackets_;
        }
        return result;
    }
    std::uint32_t sourceSsrc() const { return initialized_ ? ssrc_ : 0; }
    static constexpr std::size_t MaxPending = 256, MaxPendingBytes = 2 * 1024 * 1024;
    static constexpr std::size_t MaxFrameBytes = 4 * 1024 * 1024, MaxFrames = 8;
    const Stats& stats() const { return stats_; }
    bool waiting() const { return waiting_; }
    bool needsPli() const { return waiting_; }
    std::size_t pendingPackets() const { return pending_.size(); }
    std::size_t pendingBytes() const { return pendingBytes_; }
    std::size_t assemblyBytes() const { return au_.size(); }
    void receive(const std::byte* data, std::size_t size, std::uint64_t now, std::size_t frameBudget = 2, std::uint64_t receivedMs = 0) {
        ++stats_.received;
        // Enforce the repair deadline even when the consumer pump was delayed.
        // A late retransmission cannot revive an already expired prediction chain.
        if (initialized_) expire(now);
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
            else { ++stats_.late; observeResponse(seq, p, now, true); }
            return;
        }
        if (pending_.count(seq)) { ++stats_.duplicate; return; }
        if (p.payload.size() > MaxPendingBytes || pending_.size() >= (nackEnabled_ ? 1024 : MaxPending) ||
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
        }
        observeResponse(seq, p, now, false);
        p.firstPacketMs = receivedMs ? receivedMs : now;
        p.arrived = now; p.arrivalOrder = stats_.received;
        p.repairMs = nackEnabled_ ? repairMs_ : ReorderMs;
        pendingBytes_ += p.payload.size();
        pending_.emplace(seq, std::move(p));
        tick(now, frameBudget);
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
    void tick(std::uint64_t now, std::size_t frameBudget = 2) {
        expire(now);
        // A repaired gap may release several frames at once. Continue bounded
        // catch-up on later pumps even when the network has no new packets.
        consume(now, frameBudget);
    }
private:
    void expire(std::uint64_t now) {
        // Snapshot generated NACK evidence before TTL pruning or AU damage can
        // retire it. Keep the existing damage/recovery order below unchanged.
        if (gapActive_ && now - gapSince_ >= gapRepairMs_ && !pending_.empty() && pending_.begin()->first > expected_)
            recordGapExpiry(pending_.begin()->first, now);
        pruneHistory(now);
        if (active_ && now - auSince_ >= (auRepair_ ? RepairAssemblyMs : AssemblyMs)) {
            ++stats_.timeout; damage(5, expected_, timestamp_, now);
        }
        if (gapActive_ && now - gapSince_ >= gapRepairMs_ && !pending_.empty()) {
            const auto next = pending_.begin()->first;
            if (next > expected_) {
                ++stats_.gaps; stats_.missing += std::uint64_t(next - expected_);
                damage(1, expected_, pending_.begin()->second.timestamp, now);
                expected_ = next;
            }
            gapActive_ = false;
        }
    }
public:
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
        return "nackEnabled=" + std::to_string(nackEnabled_) + " nackPackets=" + std::to_string(nackPackets_) +
            " repairedPackets=" + std::to_string(repairPackets_) + " repairWindowMs=" + std::to_string(nackEnabled_ ? repairMs_ : ReorderMs) +
            " gapRepairWindowMs=" + std::to_string(gapActive_ ? gapRepairMs_ : 0) +
            " nackRetryMs=" + std::to_string(retryMs()) +
            " nackArrivalSamples=" + std::to_string(arrivalSamples_) + " nackLateSamples=" + std::to_string(lateSamples_) +
            " nackRetrySamples=" + std::to_string(retrySamples_) +
            " nackFirstArrivalLastMs=" + std::to_string(arrivalLastMs_) +
            " nackFirstArrivalMeanMs=" + std::to_string(arrivalMeanMs_) + " nackFirstArrivalMaxMs=" + std::to_string(arrivalMaxMs_) +
            " nackArrivalDeviationMs=" + std::to_string(arrivalDeviationMs_) +
            " nackHistory=" + std::to_string(history_.size()) + " nackHistoryEvicted=" + std::to_string(historyEvicted_) +
            " catchupYields=" + std::to_string(catchupYields_) +
            " rtp=" + std::to_string(stats_.received) +
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
            " eventDrop=" + std::to_string(stats_.eventOverflow) +
            " expiredGaps=" + std::to_string(expiredGaps_) + " expiredPackets=" + std::to_string(expiredPackets_) +
            " expiredNoNackGaps=" + std::to_string(expiredNoNackGaps_) +
            " lastGapFirst=" + std::to_string(lastExpiry_.first) + " lastGapLast=" + std::to_string(lastExpiry_.last) +
            " lastGapDeadlineMs=" + std::to_string(lastExpiry_.deadlineMs);
    }
    std::string gapDiagnostics() const {
        return "expiredGaps=" + std::to_string(expiredGaps_) + " expiredPackets=" + std::to_string(expiredPackets_) +
            " expiredNoNackGaps=" + std::to_string(expiredNoNackGaps_) +
            " expiredUnrequestedPackets=" + std::to_string(expiredUnrequestedPackets_) +
            " expiredNackAttempts=" + std::to_string(expiredNackAttempts_) +
            " maxExpiryOvershootMs=" + std::to_string(maxExpiryOvershootMs_) +
            " matchedLatePackets=" + std::to_string(matchedLatePackets_) +
            " lastFirst=" + std::to_string(lastExpiry_.first) + " lastLast=" + std::to_string(lastExpiry_.last) +
            " lastDetectedMs=" + std::to_string(lastExpiry_.detectedMs) +
            " lastOldestEvidenceMs=" + std::to_string(lastExpiry_.oldestMs) +
            " lastBudgetMs=" + std::to_string(lastExpiry_.budgetMs) +
            " lastDeadlineMs=" + std::to_string(lastExpiry_.deadlineMs) +
            " lastExpiredMs=" + std::to_string(lastExpiry_.expiredMs) +
            " lastOvershootMs=" + std::to_string(lastExpiry_.overshootMs) +
            " lastFirstNackMs=" + std::to_string(lastExpiry_.firstNackMs) +
            " lastLastNackMs=" + std::to_string(lastExpiry_.lastNackMs) +
            " lastNackAttempts=" + std::to_string(lastExpiry_.nackAttempts) +
            " lastRequestedPackets=" + std::to_string(lastExpiry_.requestedPackets) +
            " lastPendingFirst=" + std::to_string(lastExpiry_.pendingFirst) +
            " lastPendingLast=" + std::to_string(lastExpiry_.pendingLast) +
            " lastPendingDepth=" + std::to_string(lastExpiry_.pendingDepth) +
            " lateSeq=" + std::to_string(lastLate_.sequence) + " lateArrivedMs=" + std::to_string(lastLate_.arrivedMs) +
            " lateGapFirst=" + std::to_string(lastLate_.first) + " lateGapLast=" + std::to_string(lastLate_.last) +
            " lateDeadlineMs=" + std::to_string(lastLate_.deadlineMs) + " lateByMs=" + std::to_string(lastLate_.lateByMs) +
            " lateFirstNackMs=" + std::to_string(lastLate_.firstNackMs) +
            " lateLastNackMs=" + std::to_string(lastLate_.lastNackMs) +
            " lateNackAttempts=" + std::to_string(lastLate_.nackAttempts);
    }
private:
    struct GapExpiry {
        std::int64_t first = 0, last = 0, pendingFirst = 0, pendingLast = 0;
        std::uint64_t detectedMs = 0, oldestMs = 0, budgetMs = 0, deadlineMs = 0, expiredMs = 0, overshootMs = 0;
        std::uint64_t firstNackMs = 0, lastNackMs = 0, nackAttempts = 0, requestedPackets = 0, pendingDepth = 0;
    };
    struct LateExpiry {
        std::int64_t sequence = 0, first = 0, last = 0;
        std::uint64_t arrivedMs = 0, deadlineMs = 0, lateByMs = 0, firstNackMs = 0, lastNackMs = 0, nackAttempts = 0;
    };
    struct Rtp {
        std::uint16_t sequence = 0;
        std::uint32_t timestamp = 0, ssrc = 0;
        bool marker = false;
        std::uint64_t arrived = 0, arrivalOrder = 0, repairMs = RepairMs;
        std::vector<std::byte> payload;
        std::uint64_t firstPacketMs = 0;
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
        if (reason == 1) events_.back() += " gapLast=" + std::to_string(lastExpiry_.last) +
            " detectedMs=" + std::to_string(lastExpiry_.detectedMs) + " oldestMs=" + std::to_string(lastExpiry_.oldestMs) +
            " budgetMs=" + std::to_string(lastExpiry_.budgetMs) + " deadlineMs=" + std::to_string(lastExpiry_.deadlineMs) +
            " overshootMs=" + std::to_string(lastExpiry_.overshootMs) +
            " firstNackMs=" + std::to_string(lastExpiry_.firstNackMs) + " lastNackMs=" + std::to_string(lastExpiry_.lastNackMs) +
            " nackAttempts=" + std::to_string(lastExpiry_.nackAttempts) +
            " pendingFirst=" + std::to_string(lastExpiry_.pendingFirst) + " pendingLast=" + std::to_string(lastExpiry_.pendingLast) +
            " pendingDepth=" + std::to_string(lastExpiry_.pendingDepth);
    }
    void damage(unsigned reason, std::int64_t seq, std::uint32_t ts, std::uint64_t now) {
        retireRequests(now);
        if (active_) ++stats_.dropped;
        if (!waiting_) ++stats_.resyncs;
        waiting_ = true; boundary_ = false; active_ = false; fu_ = false; auRepair_ = false;
        au_.clear();
        frames_.clear(); frameBytes_ = 0; ++stats_.faultSerial;
        event(reason, seq, ts, now);
    }
    void consume(std::uint64_t now, std::size_t frameBudget) {
        const auto initialEmitted = stats_.emitted;
        unsigned consumed = 0;
        frameBudget = std::min(frameBudget, std::size_t(2));
        while (!pending_.empty() && pending_.begin()->first == expected_) {
            if (consumed >= 128 || stats_.emitted - initialEmitted >= frameBudget) { ++catchupYields_; break; }
            auto p = std::move(pending_.begin()->second);
            pendingBytes_ -= p.payload.size(); pending_.erase(pending_.begin());
            const auto seq = expected_++;
            recent_.push_back(seq); if (recent_.size() > 512) recent_.pop_front();
            process(p, seq, now);
            ++consumed;
        }
        for (auto it = requests_.begin(); it != requests_.end() && it->first < expected_;) {
            history_[it->first] = it->second; it = requests_.erase(it);
        }
        if (pending_.empty() || pending_.begin()->first == expected_) {
            gapActive_ = false;
            if (pending_.empty()) retireRequests(now);
        } else if (!gapActive_ || gapSequence_ != expected_) {
            ++stats_.candidates; gapActive_ = true; gapSequence_ = expected_;
            const Rtp* oldest = &pending_.begin()->second;
            for (const auto& item : pending_) {
                const auto& evidence = item.second;
                if (evidence.arrived < oldest->arrived ||
                    (evidence.arrived == oldest->arrived && evidence.arrivalOrder < oldest->arrivalOrder)) oldest = &evidence;
            }
            // Keep the original budget with the oldest buffered evidence. A
            // repaired leading hole must neither shorten nor restart its age
            // when response timing changes the estimate for future packets.
            gapSince_ = oldest->arrived; gapRepairMs_ = oldest->repairMs; gapDetectedMs_ = now;
            nextNackScanMs_ = gapSince_ + ReorderMs;
            if (active_ && nackEnabled_) auRepair_ = true;
        }
        pruneHistory(now);
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
            active_ = true; timestamp_ = p.timestamp; firstSequence_ = seq; auSince_ = now; auRepair_ = false;
            auFirstPacketMs_ = p.firstPacketMs;
            vcl_ = idr_ = nonIdr_ = false;
        }
        // Arrival order can differ from sequence order, especially during repair.
        auFirstPacketMs_ = std::min(auFirstPacketMs_, p.firstPacketMs);
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
        frames_.push_back({std::move(au_), p.timestamp, firstSequence_, seq, auFirstPacketMs_});
        au_.clear(); ++stats_.emitted;
    }
    struct Request {
        unsigned attempts = 0;
        std::uint64_t firstMs = 0, lastMs = 0, expiredDeadlineMs = 0;
        std::uint64_t diagnosticFirstMs = 0, diagnosticLastMs = 0, diagnosticAttempts = 0;
        std::uint64_t expiredFirstNackMs = 0, expiredLastNackMs = 0, expiredNackAttempts = 0;
        std::int64_t expiredFirst = 0, expiredLast = 0;
        bool gapExpired = false;
    };
    void recordGapExpiry(std::int64_t next, std::uint64_t now) {
        GapExpiry summary;
        summary.first = expected_; summary.last = next - 1;
        summary.detectedMs = gapDetectedMs_; summary.oldestMs = gapSince_; summary.budgetMs = gapRepairMs_;
        summary.deadlineMs = gapSince_ + gapRepairMs_; summary.expiredMs = now;
        summary.overshootMs = now - summary.deadlineMs;
        summary.pendingFirst = next; summary.pendingLast = pending_.rbegin()->first; summary.pendingDepth = pending_.size();
        for (auto it = requests_.lower_bound(expected_); it != requests_.end() && it->first < next; ++it) {
            const auto retired = history_.find(it->first);
            ++summary.requestedPackets;
            recordExpiredRequest(it->second, summary, retired != history_.end() ? &retired->second : nullptr);
        }
        // AU damage can retire requests while the same hole remains active.
        // Later scans can generate another request for that sequence. Count
        // its identity once while retaining all recorded generated attempts.
        for (auto it = history_.lower_bound(expected_); it != history_.end() && it->first < next; ++it) {
            if (requests_.count(it->first)) continue;
            ++summary.requestedPackets; recordExpiredRequest(it->second, summary);
        }
        ++expiredGaps_; expiredPackets_ += std::uint64_t(next - expected_);
        if (!summary.nackAttempts) ++expiredNoNackGaps_;
        expiredUnrequestedPackets_ += std::uint64_t(next - expected_) - summary.requestedPackets;
        expiredNackAttempts_ += summary.nackAttempts;
        maxExpiryOvershootMs_ = std::max(maxExpiryOvershootMs_, summary.overshootMs);
        lastExpiry_ = summary;
    }
    static constexpr std::uint64_t HistoryMs = 2000;
    static constexpr std::size_t MaxHistory = 512;
    void recordExpiredRequest(Request& request, GapExpiry& summary, const Request* retired = nullptr) {
        const auto first = retired ? std::min(request.diagnosticFirstMs, retired->diagnosticFirstMs) : request.diagnosticFirstMs;
        const auto last = retired ? std::max(request.diagnosticLastMs, retired->diagnosticLastMs) : request.diagnosticLastMs;
        const auto attempts = request.diagnosticAttempts + (retired ? retired->diagnosticAttempts : 0);
        if (!summary.nackAttempts) summary.firstNackMs = first;
        else summary.firstNackMs = std::min(summary.firstNackMs, first);
        summary.lastNackMs = std::max(summary.lastNackMs, last); summary.nackAttempts += attempts;
        request.gapExpired = true; request.expiredDeadlineMs = summary.deadlineMs;
        request.expiredFirst = summary.first; request.expiredLast = summary.last;
        // Retiring active requests can replace the old history record. Carry
        // merged expiry evidence separately from the estimator's request timing.
        request.expiredFirstNackMs = first; request.expiredLastNackMs = last; request.expiredNackAttempts = attempts;
    }
    void pruneHistory(std::uint64_t now) {
        if (now >= nextHistoryPruneMs_) {
            for (auto it = history_.begin(); it != history_.end();) {
                if (now >= it->second.firstMs && now - it->second.firstMs > HistoryMs) it = history_.erase(it);
                else ++it;
            }
            nextHistoryPruneMs_ = now + 32;
        }
        while (history_.size() > MaxHistory) { history_.erase(history_.begin()); ++historyEvicted_; }
    }
    void retireRequests(std::uint64_t now) {
        for (const auto& item : requests_) {
            auto request = item.second;
            const auto retired = history_.find(item.first);
            if (retired != history_.end()) {
                // Only diagnostic evidence spans repeated retirement. Keep the
                // established retry/estimator fields exactly as generated.
                request.diagnosticFirstMs = std::min(request.diagnosticFirstMs, retired->second.diagnosticFirstMs);
                request.diagnosticLastMs = std::max(request.diagnosticLastMs, retired->second.diagnosticLastMs);
                request.diagnosticAttempts += retired->second.diagnosticAttempts;
            }
            history_[item.first] = request;
        }
        requests_.clear(); pruneHistory(now);
    }
    std::uint64_t retryMs() const {
        return arrivalSamples_ ? std::clamp(arrivalMeanMs_ + arrivalDeviationMs_ + 10, std::uint64_t(40), std::uint64_t(140)) : 90;
    }
    bool validResponsePayload(const Rtp& p) const {
        const auto* data = p.payload.data(); const auto size = p.payload.size();
        if (codec_ == VideoCodec::H265) {
            if (!hevcHeader(data, size)) return false;
            const auto type = (byte(data[0]) >> 1) & 63;
            const auto validType = [](unsigned t) { return t <= 40 && !(t >= 10 && t <= 15) && !(t >= 22 && t <= 31); };
            if (type < 48) return size >= 3 && validType(type);
            if (type == 49) return size >= 4 && (byte(data[2]) & 192) != 192 && validType(byte(data[2]) & 63);
            if (type != 48) return false;
            std::size_t offset = 2, count = 0;
            unsigned minTemporal = 7;
            while (offset < size) {
                if (size - offset < 2) return false;
                const auto length = be16(data + offset); offset += 2;
                if (length < 3 || length > size - offset || !hevcHeader(data + offset, length) || !validType((byte(data[offset]) >> 1) & 63)) return false;
                minTemporal = std::min(minTemporal, byte(data[offset + 1]) & 7); offset += length; ++count;
            }
            return count >= 2 && (byte(data[1]) & 7) == minTemporal;
        }
        if (!size || (byte(data[0]) & 128)) return false;
        const auto type = byte(data[0]) & 31;
        if (type >= 1 && type <= 23) return true;
        if (type == 28) return size >= 3 && !(byte(data[1]) & 32) && (byte(data[1]) & 192) != 192 && (byte(data[1]) & 31) >= 1 && (byte(data[1]) & 31) <= 23;
        if (type != 24 || size < 4) return false;
        std::size_t offset = 1;
        while (offset < size) {
            if (size - offset < 2) return false;
            const auto length = be16(data + offset); offset += 2;
            if (!length || length > size - offset || (byte(data[offset]) & 128) || !(byte(data[offset]) & 31) || (byte(data[offset]) & 31) > 23) return false;
            offset += length;
        }
        return true;
    }
    void observeResponse(std::int64_t seq, const Rtp& p, std::uint64_t now, bool late) {
        auto active = requests_.find(seq);
        auto expired = history_.find(seq);
        if (active == requests_.end() && expired == history_.end()) return;
        if (!validResponsePayload(p)) return;
        const auto request = active != requests_.end() ? active->second : expired->second;
        if (active != requests_.end()) { requests_.erase(active); if (!late) ++repairPackets_; }
        if (expired != history_.end()) history_.erase(expired);
        if (now < request.firstMs || now - request.firstMs > HistoryMs) return;
        // This correlates a requested sequence with its arrival. Without RTX it
        // may be a delayed original; retries also make the responsible send
        // ambiguous. Retain both labels instead of presenting it as network RTT.
        if (late && request.gapExpired) {
            ++matchedLatePackets_;
            lastLate_.sequence = seq; lastLate_.first = request.expiredFirst; lastLate_.last = request.expiredLast;
            lastLate_.arrivedMs = now; lastLate_.deadlineMs = request.expiredDeadlineMs;
            lastLate_.lateByMs = now >= request.expiredDeadlineMs ? now - request.expiredDeadlineMs : 0;
            lastLate_.firstNackMs = request.expiredFirstNackMs; lastLate_.lastNackMs = request.expiredLastNackMs;
            lastLate_.nackAttempts = request.expiredNackAttempts;
        }
        const auto sample = now - request.firstMs;
        ++arrivalSamples_; if (late) ++lateSamples_; if (request.attempts > 1) ++retrySamples_;
        arrivalLastMs_ = sample; arrivalMaxMs_ = std::max(arrivalMaxMs_, sample);
        if (arrivalSamples_ == 1) { arrivalMeanMs_ = sample; arrivalDeviationMs_ = std::max(std::uint64_t(5), sample / 4); }
        else {
            const auto error = sample > arrivalMeanMs_ ? sample - arrivalMeanMs_ : arrivalMeanMs_ - sample;
            arrivalDeviationMs_ = (3 * arrivalDeviationMs_ + error + 2) / 4;
            arrivalMeanMs_ = (7 * arrivalMeanMs_ + sample + 4) / 8;
        }
        const auto target = std::clamp(ReorderMs + arrivalMeanMs_ + 2 * arrivalDeviationMs_ + 20, MinRepairMs, MaxRepairMs);
        if (target > repairMs_) {
            repairMs_ = target; nextWindowDecreaseMs_ = now + 1000;
        } else if (target < repairMs_ && now >= nextWindowDecreaseMs_) {
            // A fast original reordered after NACK is not proof of a fast
            // retransmission path. Lower the budget gently, once per second.
            repairMs_ = std::max(target, repairMs_ - 10); nextWindowDecreaseMs_ = now + 1000;
        }
    }
    std::map<std::int64_t, Request> requests_, history_;
    bool nackEnabled_ = false;
    std::uint64_t nackPackets_ = 0, repairPackets_ = 0;
    std::uint64_t repairMs_ = RepairMs, gapRepairMs_ = RepairMs;
    std::uint64_t arrivalSamples_ = 0, lateSamples_ = 0, retrySamples_ = 0;
    std::uint64_t arrivalLastMs_ = 0, arrivalMeanMs_ = 0, arrivalMaxMs_ = 0, arrivalDeviationMs_ = 0;
    std::uint64_t historyEvicted_ = 0, catchupYields_ = 0;
    std::uint64_t expiredGaps_ = 0, expiredPackets_ = 0, expiredNoNackGaps_ = 0, expiredUnrequestedPackets_ = 0;
    std::uint64_t expiredNackAttempts_ = 0, maxExpiryOvershootMs_ = 0, matchedLatePackets_ = 0;
    GapExpiry lastExpiry_;
    LateExpiry lastLate_;
    std::uint64_t nextHistoryPruneMs_ = 0, nextNackScanMs_ = 0, nextWindowDecreaseMs_ = 0;
    VideoCodec codec_;
    Stats stats_;
    bool initialized_ = false, waiting_ = true, boundary_ = false, active_ = false;
    bool fu_ = false, vcl_ = false, idr_ = false, nonIdr_ = false, haveTimestamp_ = false, gapActive_ = false, auRepair_ = false;
    unsigned fuHeader_ = 0;
    std::uint32_t ssrc_ = 0, timestamp_ = 0, lastTimestamp_ = 0;
    std::int64_t expected_ = 0, firstSequence_ = 0, lastFirst_ = 0, lastLast_ = 0, gapSequence_ = 0;
    std::uint64_t gapSince_ = 0, gapDetectedMs_ = 0, auSince_ = 0, auFirstPacketMs_ = 0;
    std::size_t pendingBytes_ = 0, frameBytes_ = 0, fuOffset_ = 0;
    std::map<std::int64_t, Rtp> pending_;
    std::deque<std::int64_t> recent_;
    std::vector<std::byte> au_;
    fog::detail::VideoParameters parameters_;
    std::deque<Frame> frames_;
    std::deque<std::string> events_;
};
}
