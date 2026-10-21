#pragma once
#include "features/streaming/domain/video_codec.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fog::detail {
// The bounded RTP assembler supplies Annex B access units. Keep only bounded
// parameter sets; no packet payload is included in decoder diagnostics.
struct VideoNal {
    const unsigned char* data;
    std::size_t size;
    unsigned type;
};
struct VideoAccessUnit {
    std::vector<VideoNal> nals;
    std::uint64_t types = 0;
    bool complete = true;
    bool idr = false;
};
inline std::size_t videoStartCode(const unsigned char* data, std::size_t size,
                                 std::size_t from, std::size_t& length) {
    for (std::size_t i = from; i + 2 < size; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { length = 3; return i; }
        if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
            length = 4; return i;
        }
    }
    length = 0;
    return size;
}
inline VideoAccessUnit inspectVideo(const unsigned char* data, std::size_t size,
                                    ::stream::VideoCodec codec = ::stream::VideoCodec::H264) {
    VideoAccessUnit result;
    std::size_t prefix = 0;
    auto start = videoStartCode(data, size, 0, prefix);
    if (start == size) { result.complete = false; return result; }
    while (start < size) {
        const auto begin = start + prefix;
        const auto next = videoStartCode(data, size, begin, prefix);
        auto end = next;
        while (end > begin && data[end - 1] == 0) --end;
        if (end > begin) {
            const bool hevc = codec == ::stream::VideoCodec::H265;
            const unsigned type = hevc ? (data[begin] >> 1) & 63 : data[begin] & 31;
            if ((data[begin] & 128) || (hevc && (end - begin < 2 ||
                (data[begin] & 1) || (data[begin + 1] & 0xf8) || !(data[begin + 1] & 7))))
                result.complete = false;
            result.types |= std::uint64_t{1} << type;
            result.idr |= hevc ? type == 19 || type == 20 : type == 5;
            if (result.nals.size() == 256) { result.complete = false; break; }
            result.nals.push_back({data + begin, end - begin, type});
        }
        start = next;
    }
    return result;
}
class VideoBits {
public:
    explicit VideoBits(const VideoNal& nal, unsigned headerBytes = 1) {
        unsigned zeros = 0;
        for (std::size_t i = headerBytes; i < nal.size && size_ < bytes_.size(); ++i) {
            const auto value = nal.data[i];
            if (zeros == 2 && value == 3) { zeros = 0; continue; }
            bytes_[size_++] = value;
            zeros = value == 0 ? zeros + 1 : 0;
        }
    }
    bool skip(unsigned count) {
        if (position_ + count > size_ * 8) return false;
        position_ += count;
        return true;
    }
    bool fixed(unsigned count, unsigned& value) {
        value = 0;
        for (unsigned i = 0, bit = 0; i < count; ++i) {
            if (!read(bit)) return false;
            value = (value << 1) | bit;
        }
        return true;
    }
    bool ue(unsigned& value) {
        unsigned zeros = 0, bit = 0;
        while (read(bit) && !bit) { if (++zeros > 30) return false; }
        if (!bit) return false;
        unsigned suffix = 0;
        for (unsigned i = 0; i < zeros; ++i) {
            if (!read(bit)) return false;
            suffix = (suffix << 1) | bit;
        }
        value = ((std::uint32_t{1} << zeros) - 1) + suffix;
        return true;
    }
private:
    bool read(unsigned& value) {
        if (position_ >= size_ * 8) return false;
        value = (bytes_[position_ / 8] >> (7 - position_ % 8)) & 1;
        ++position_;
        return true;
    }
    std::array<unsigned char, 128> bytes_{};
    std::size_t size_ = 0, position_ = 0;
};
inline bool videoParameterIds(const VideoNal& nal, unsigned& id, unsigned& parent) {
    if (!nal.size || (nal.data[0] & 128)) return false;
    VideoBits bits(nal);
    if (nal.type == 7) return bits.skip(24) && bits.ue(id) && id < 32;
    if (nal.type == 8) return bits.ue(id) && id < 256 && bits.ue(parent) && parent < 32;
    if (nal.type == 5) {
        unsigned ignored = 0;
        return bits.ue(ignored) && bits.ue(ignored) && bits.ue(id) && id < 256;
    }
    return false;
}
// Base-layer RFC 7798 streams without DONL. IDR only is a deliberate recovery
// boundary: CRA/BLA leading-picture rules are not inferred from RTP timestamps.
inline bool hevcParameterIds(const VideoNal& nal, unsigned& id, unsigned& parent) {
    if (nal.size < 3 || (nal.data[0] & 129) || (nal.data[1] & 0xf8) || !(nal.data[1] & 7)) return false;
    VideoBits bits(nal, 2);
    if (nal.type == 32) return bits.fixed(4, id) && id < 16;
    if (nal.type == 34) return bits.ue(id) && id < 64 && bits.ue(parent) && parent < 16;
    if (nal.type == 19 || nal.type == 20)
        return bits.skip(2) && bits.ue(id) && id < 64;
    if (nal.type != 33) return false;
    unsigned layers = 0;
    if (!bits.fixed(4, parent) || !bits.fixed(3, layers) || layers > 6 || !bits.skip(1 + 96)) return false;
    std::array<unsigned, 7> profiles{}, levels{};
    for (unsigned i = 0; i < layers; ++i)
        if (!bits.fixed(1, profiles[i]) || !bits.fixed(1, levels[i])) return false;
    if (layers && !bits.skip((8 - layers) * 2)) return false;
    for (unsigned i = 0; i < layers; ++i)
        if (!bits.skip((profiles[i] ? 88 : 0) + (levels[i] ? 8 : 0))) return false;
    return bits.ue(id) && id < 16;
}
class VideoParameters {
public:
    explicit VideoParameters(::stream::VideoCodec codec = ::stream::VideoCodec::H264) : codec_(codec) {}
    void observe(const VideoAccessUnit& unit) {
        if (!unit.complete) { clear(); return; }
        // Observe parent sets before children, including changes in one AU.
        const std::array<unsigned, 3> types = codec_ == ::stream::VideoCodec::H265 ?
            std::array<unsigned, 3>{32, 33, 34} : std::array<unsigned, 3>{7, 8, 0};
        for (unsigned type : types) {
            if (!type) continue;
            for (const auto& nal : unit.nals) {
                if (nal.type != type) continue;
                unsigned id = 0, parent = 0;
                if (nal.size > maxNalSize || !parameterIds(nal, id, parent)) {
                    clear(); continue;
                }
                const bool isVps = type == 32, isSps = type == 7 || type == 33;
                auto& destination = isVps ? vps_[id] : isSps ? sps_[id] : pps_[id];
                if (destination.bytes.size() == nal.size &&
                    std::equal(destination.bytes.begin(), destination.bytes.end(), nal.data)) continue;
                if (isVps) {
                    for (unsigned child = 0; child < sps_.size(); ++child) {
                        if (sps_[child].parent != static_cast<int>(id)) continue;
                        remove(sps_[child]);
                        for (auto& pps : pps_) if (pps.parent == static_cast<int>(child)) remove(pps);
                    }
                }
                if (isSps) {
                    for (auto& pps : pps_) if (pps.parent == static_cast<int>(id)) remove(pps);
                }
                remove(destination);
                if (total_ + nal.size > maxCacheSize) clear();
                destination.bytes.assign(nal.data, nal.data + nal.size);
                destination.parent = isVps || type == 7 ? -1 : static_cast<int>(parent);
                total_ += nal.size;
            }
        }
    }
    bool recoveryPrefix(const VideoAccessUnit& unit, std::vector<unsigned char>& prefix) const {
        prefix.clear();
        if (!unit.complete || !unit.idr) return false;
        std::array<bool, 16> includedVps{};
        std::array<bool, 32> includedSps{};
        std::array<bool, 256> includedPps{};
        for (const auto& nal : unit.nals) {
            const bool hevc = codec_ == ::stream::VideoCodec::H265;
            if (hevc && nal.type <= 31 && nal.type != 19 && nal.type != 20) return false;
            if (hevc ? nal.type != 19 && nal.type != 20 : nal.type != 5) continue;
            unsigned id = 0, ignored = 0;
            if (!parameterIds(nal, id, ignored)) return false;
            const auto& pps = pps_[id];
            if (pps.bytes.empty() || pps.parent < 0) return false;
            const auto parent = static_cast<unsigned>(pps.parent);
            const auto& sps = sps_[parent];
            if (sps.bytes.empty()) return false;
            if (hevc) {
                if (sps.parent < 0) return false;
                const auto vpsId = static_cast<unsigned>(sps.parent);
                const auto& vps = vps_[vpsId];
                if (vps.bytes.empty()) return false;
                if (!includedVps[vpsId]) { append(prefix, vps.bytes); includedVps[vpsId] = true; }
            }
            // Put the exact pair before slices, even if the AU repeats it.
            // This also handles parameter NALs following a slice in an AU.
            if (!includedSps[parent]) { append(prefix, sps.bytes); includedSps[parent] = true; }
            if (!includedPps[id]) { append(prefix, pps.bytes); includedPps[id] = true; }
        }
        return !prefix.empty();
    }
private:
    ::stream::VideoCodec codec_;
    bool parameterIds(const VideoNal& nal, unsigned& id, unsigned& parent) const {
        return codec_ == ::stream::VideoCodec::H265 ? hevcParameterIds(nal, id, parent) : videoParameterIds(nal, id, parent);
    }
    struct Parameter { std::vector<unsigned char> bytes; int parent = -1; };
    static constexpr std::size_t maxNalSize = 16 * 1024, maxCacheSize = 64 * 1024;
    std::array<Parameter, 16> vps_{};
    std::array<Parameter, 32> sps_{};
    std::array<Parameter, 256> pps_{};
    std::size_t total_ = 0;
    void remove(Parameter& parameter) {
        total_ -= parameter.bytes.size();
        // Free capacity as well: discarded versions must not accumulate above
        // the cache limit when a stream cycles through many parameter IDs.
        std::vector<unsigned char>().swap(parameter.bytes);
        parameter.parent = -1;
    }
    void clear() {
        for (auto& parameter : vps_) remove(parameter);
        for (auto& parameter : sps_) remove(parameter);
        for (auto& parameter : pps_) remove(parameter);
    }
    static void append(std::vector<unsigned char>& output, const std::vector<unsigned char>& nal) {
        output.insert(output.end(), {0, 0, 0, 1});
        output.insert(output.end(), nal.begin(), nal.end());
    }
};
}
