#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <utility>

namespace stream {
struct RtpRepairConfig {
    std::map<unsigned, bool> media; // payload type -> negotiated generic NACK
    std::map<unsigned, unsigned> rtx; // RTX payload type -> original payload type
    std::uint32_t mediaSsrc = 0, rtxSsrc = 0;
    bool nack() const { if (media.empty()) return false; for (const auto& item : media) if (!item.second) return false; return true; }
};
// Authenticated RTP only. This layer never interprets compressed video bytes.
class RtpRepair {
public:
    explicit RtpRepair(RtpRepairConfig config = {}) : config_(std::move(config)), source_(config_.mediaSsrc) {}
    const RtpRepairConfig& config() const { return config_; }
    std::string diagnostics() const {
        std::string payloads;
        for (const auto& item : config_.media) payloads += std::to_string(item.first) + ":n" + std::to_string(item.second) + ",";
        for (const auto& item : config_.rtx) payloads += std::to_string(item.first) + ":a" + std::to_string(item.second) + ",";
        return " repairPts=" + payloads + " repairMediaPtCount=" + std::to_string(config_.media.size()) + " repairRtxPtCount=" + std::to_string(config_.rtx.size()) +
            " repairFid=" + std::to_string(config_.mediaSsrc != 0 && config_.rtxSsrc != 0) +
            " rtxReceived=" + std::to_string(rtxReceived_) + " rtxRestored=" + std::to_string(restored_) +
            " repairControlRejected=" + std::to_string(controlRejected_) + " repairRejected=" + std::to_string(rejected_) + " nackBatches=" + std::to_string(nackBatches_);
    }
    // RTX requires an unambiguous FID association, or a learned primary source and
    // an outstanding OSN. Unsolicited RTX never selects the primary source.
    template <typename Missing> bool normalize(std::vector<std::byte>& packet, Missing missing) {
        std::size_t offset = 0, end = 0;
        if (!header(packet, offset, end)) { ++rejected_; return false; }
        const auto pt = byte(packet[1]) & 127;
        const auto ssrc = read32(packet.data() + 8);
        const auto media = config_.media.find(pt);
        if (media != config_.media.end()) {
            if (!source_) source_ = ssrc;
            if (!source_ || source_ != ssrc) { ++rejected_; return false; }
            return true;
        }
        const auto rtx = config_.rtx.find(pt);
        if (rtx == config_.rtx.end()) { ++rejected_; return false; }
        ++rtxReceived_;
        if (end - offset < 3 || !source_ || ssrc == source_ || (config_.rtxSsrc && ssrc != config_.rtxSsrc)) {
            ++rejected_; return false;
        }
        const auto osn = read16(packet.data() + offset);
        if (!config_.rtxSsrc && !missing(osn)) { ++rejected_; return false; }
        // Drop RTX padding and its two-byte OSN; CSRC and extension lengths stay valid.
        packet.resize(end);
        packet.erase(packet.begin() + offset, packet.begin() + offset + 2);
        packet[0] &= std::byte{0xdf};
        packet[1] = std::byte((byte(packet[1]) & 128) | rtx->second);
        write16(packet.data() + 2, osn); write32(packet.data() + 8, source_);
        ++restored_; return true;
    }
    // The pinned receiver remembers the sender of the first SR/RR in a compound
    // packet. Reports from the independent RTX source must not retarget PLI/RR.
    bool acceptControl(const std::vector<std::byte>& packet) {
        if (packet.size() < 8 || byte(packet[0]) >> 6 != 2) { ++controlRejected_; return false; }
        const auto length = (std::size_t(read16(packet.data() + 2)) + 1) * 4;
        if (length > packet.size() || length < 8) { ++controlRejected_; return false; }
        const auto type = byte(packet[1]);
        if (type == 200 || type == 201) {
            const auto required = (type == 200 ? 28u : 8u) + 24u * (byte(packet[0]) & 31u);
            const auto sender = read32(packet.data() + 4);
            if (length < required || (source_ && sender != source_) ||
                (config_.rtxSsrc && sender == config_.rtxSsrc)) { ++controlRejected_; return false; }
        }
        return true;
    }
    std::vector<std::byte> nack(const std::vector<std::uint16_t>& missing) {
        if (!source_ || missing.empty() || missing.size() > 256 || !config_.nack()) return {};
        // PID/BLP fields in extended sequence order; uint16 wrap is intentional.
        std::vector<std::byte> packet(12);
        packet[0] = std::byte{0x81}; packet[1] = std::byte{205};
        auto sender = source_ ^ 0x464f4701u;
        if (!sender) sender = 1;
        write32(packet.data() + 4, sender); write32(packet.data() + 8, source_);
        for (std::size_t i = 0; i < missing.size();) {
            const auto pid = missing[i++]; unsigned mask = 0;
            while (i < missing.size()) {
                const auto delta = std::uint16_t(missing[i] - pid);
                if (!delta || delta > 16) break;
                mask |= 1u << (delta - 1); ++i;
            }
            const auto at = packet.size(); packet.resize(at + 4);
            write16(packet.data() + at, pid); write16(packet.data() + at + 2, mask);
        }
        write16(packet.data() + 2, std::uint16_t(packet.size() / 4 - 1));
        ++nackBatches_; return packet;
    }
private:
    static unsigned byte(std::byte value) { return std::to_integer<unsigned>(value); }
    static std::uint16_t read16(const std::byte* data) { return (byte(data[0]) << 8) | byte(data[1]); }
    static std::uint32_t read32(const std::byte* data) {
        return (std::uint32_t(read16(data)) << 16) | read16(data + 2);
    }
    static void write16(std::byte* data, std::uint16_t value) { data[0] = std::byte(value >> 8); data[1] = std::byte(value); }
    static void write32(std::byte* data, std::uint32_t value) { write16(data, value >> 16); write16(data + 2, value); }
    static bool header(const std::vector<std::byte>& packet, std::size_t& offset, std::size_t& end) {
        end = packet.size();
        if (end < 12 || end > 65535 || byte(packet[0]) >> 6 != 2) return false;
        const auto flags = byte(packet[0]); offset = 12 + (flags & 15) * 4;
        if (offset > end) return false;
        if (flags & 16) {
            if (end - offset < 4) return false;
            const auto length = std::size_t(read16(packet.data() + offset + 2)) * 4;
            offset += 4; if (length > end - offset) return false; offset += length;
        }
        if (flags & 32) {
            const auto padding = byte(packet.back());
            if (!padding || padding > end - offset) return false;
            end -= padding;
        }
        return offset <= end;
    }
    RtpRepairConfig config_;
    std::uint32_t source_ = 0;
    std::uint64_t rtxReceived_ = 0, restored_ = 0, rejected_ = 0, controlRejected_ = 0, nackBatches_ = 0;
};
}
