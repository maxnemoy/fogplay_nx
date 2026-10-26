#include "features/streaming/infrastructure/stream_protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace stream::protocol {
namespace {
constexpr std::size_t MaxFrame = 256 * 1024;
std::string escape(const std::string& input) {
    std::string out;
    for (char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case ':': out += "\\c"; break;
        default: out += c;
        }
    }
    return out;
}
std::string unescape(const std::string& input) {
    std::string out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\') { out += input[i]; continue; }
        if (++i == input.size()) throw Error("Invalid STOMP escape");
        switch (input[i]) {
        case '\\': out += '\\'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 'c': out += ':'; break;
        default: throw Error("Invalid STOMP escape");
        }
    }
    return out;
}
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        result.push_back(std::move(line));
    }
    return result;
}
float axis(float x) {
    if (!std::isfinite(x)) return 0;
    return std::clamp(x, -1.0f, 1.0f);
}
float trigger(float x) { return std::max(0.0f, axis(x)); }
int decimal(const std::string& text) {
    if (text.empty() || text.size() > 9 ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); }))
        throw Error("Invalid decimal field");
    return std::stoi(text);
}
}

std::vector<Frame> Parser::feed(const std::string& bytes) {
    if (bytes.size() > MaxFrame || pending_.size() + bytes.size() > MaxFrame)
        throw Error("STOMP input limit exceeded");
    pending_ += bytes;
    std::vector<Frame> result;
    while (!pending_.empty()) {
        if (pending_[0] == '\n') { pending_.erase(0, 1); continue; }
        if (pending_[0] == '\r') {
            if (pending_.size() < 2) break;
            if (pending_[1] != '\n') throw Error("Invalid STOMP heartbeat");
            pending_.erase(0, 2); continue;
        }
        std::size_t cursor = 0;
        std::vector<std::string> headerLines;
        bool complete = false;
        while (cursor < pending_.size()) {
            const auto end = pending_.find('\n', cursor);
            if (end == std::string::npos) break;
            auto line = pending_.substr(cursor, end - cursor);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            cursor = end + 1;
            if (line.empty()) { complete = true; break; }
            headerLines.push_back(std::move(line));
            if (headerLines.size() > 64) throw Error("Too many STOMP headers");
        }
        if (!complete) break;
        if (headerLines.empty()) throw Error("Missing STOMP command");
        Frame f;
        f.command = headerLines.front();
        const bool escaped = f.command != "CONNECTED" && f.command != "CONNECT";
        for (std::size_t i = 1; i < headerLines.size(); ++i) {
            const auto colon = headerLines[i].find(':');
            if (colon == std::string::npos) throw Error("Invalid STOMP header");
            auto name = headerLines[i].substr(0, colon);
            auto value = headerLines[i].substr(colon + 1);
            f.headers.emplace(escaped ? unescape(name) : name, escaped ? unescape(value) : value);
        }
        std::size_t end;
        const auto length = f.headers.find("content-length");
        if (length != f.headers.end()) {
            const auto count = static_cast<std::size_t>(decimal(length->second));
            if (count > MaxFrame) throw Error("STOMP body limit exceeded");
            end = cursor + count;
            if (pending_.size() <= end) break;
            if (pending_[end] != '\0') throw Error("Missing STOMP terminator");
        } else {
            end = pending_.find('\0', cursor);
            if (end == std::string::npos) break;
        }
        f.body = pending_.substr(cursor, end - cursor);
        result.push_back(std::move(f));
        pending_.erase(0, end + 1);
    }
    return result;
}

std::string frame(const std::string& command, const std::map<std::string, std::string>& headers,
                  const std::string& body) {
    std::string result = command + '\n';
    for (const auto& pair : headers) {
        if (pair.first == "content-length") continue;
        result += command == "CONNECT" ? pair.first + ':' + pair.second + '\n'
                                        : escape(pair.first) + ':' + escape(pair.second) + '\n';
    }
    if (!body.empty()) result += "content-length:" + std::to_string(body.size()) + '\n';
    result += '\n';
    result += body;
    result += '\0';
    return result;
}

std::string base64Encode(const std::string& input) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(input[i]);
        const unsigned b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        const unsigned c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
        out += table[a >> 2]; out += table[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < input.size() ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < input.size() ? table[c & 63] : '=';
    }
    return out;
}
std::string base64Decode(const std::string& input) {
    static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (input.size() > MaxFrame || input.size() % 4 != 0) throw Error("Invalid base64 length");
    std::string out;
    for (std::size_t i = 0; i < input.size(); i += 4) {
        unsigned v[4]{};
        for (int j = 0; j < 4; ++j) {
            const char c = input[i + j];
            if (c == '=') {
                if (i + 4 != input.size() || j < 2 || (j == 2 && input[i + 3] != '='))
                    throw Error("Invalid base64 padding");
            } else {
                const auto p = table.find(c);
                if (p == std::string::npos) throw Error("Invalid base64 alphabet");
                v[j] = static_cast<unsigned>(p);
            }
        }
        if ((input[i + 2] == '=' && (v[1] & 15)) || (input[i + 3] == '=' && (v[2] & 3)))
            throw Error("Invalid base64 padding bits");
        out += static_cast<char>((v[0] << 2) | (v[1] >> 4));
        if (input[i + 2] != '=') out += static_cast<char>((v[1] << 4) | (v[2] >> 2));
        if (input[i + 3] != '=') out += static_cast<char>((v[2] << 6) | v[3]);
    }
    return out;
}
Json gamepad(const Gamepad& state, bool playStation) {
    Json buttons = Json::array(), axes = Json::array();
    for (const auto b : state.buttons) buttons.push_back(b ? 1 : 0);
    for (const auto a : state.axes) axes.push_back(axis(a));
    return {{"id", "gamepad"}, {"index", 0}, {"isPS", playStation},
            {"state", {{"buttons", buttons}, {"axes", axes},
                       {"rightTrigger", trigger(state.rightTrigger)}, {"leftTrigger", trigger(state.leftTrigger)}}}};
}
std::vector<IceServer> iceServers(const Json& initMessage) {
    const auto init = Json::parse(initMessage.at("data").get<std::string>());
    if (!init.is_array() || init.empty() || !init[0].is_string()) throw Error("Invalid ICE init envelope");
    const auto servers = Json::parse(init[0].get<std::string>());
    if (!servers.is_array() || servers.size() > 32) throw Error("Invalid ICE server list");
    std::vector<IceServer> result;
    for (const auto& server : servers) {
        auto urls = server.at("urls");
        if (urls.is_string()) urls = Json::array({urls});
        if (!urls.is_array() || urls.empty() || urls.size() > 16) throw Error("Invalid ICE urls");
        for (const auto& url : urls) {
            IceServer item{url.get<std::string>(), server.value("username", ""), server.value("credential", "")};
            if (item.url.size() > 2048 || item.username.size() > 1024 || item.credential.size() > 1024 ||
                (item.url.rfind("stun:", 0) != 0 && item.url.rfind("turn:", 0) != 0 && item.url.rfind("turns:", 0) != 0))
                throw Error("Invalid ICE server");
            result.push_back(std::move(item));
        }
    }
    return result;
}

std::vector<std::string> mediaMids(const std::string& sdp) {
    std::vector<std::string> result;
    for (const auto& line : lines(sdp)) {
        if (line.rfind("m=", 0) == 0) result.emplace_back();
        if (line.rfind("a=mid:", 0) == 0 && !result.empty()) result.back() = line.substr(6);
    }
    return result;
}

namespace {
std::string normalized(std::string value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    value = value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}
int rtxApt(const std::vector<std::string>& input, std::size_t begin, std::size_t end, int payload) {
    int apt = -1;
    for (auto i = begin; i < end; ++i) {
        if (input[i].rfind("a=fmtp:", 0) != 0) continue;
        std::istringstream fields(input[i].substr(7)); std::string id, body;
        fields >> id; if (decimal(id) != payload) continue;
        std::getline(fields, body); std::istringstream values(body); std::string entry;
        while (std::getline(values, entry, ';')) {
            const auto equal = entry.find('=');
            if (equal == std::string::npos || normalized(entry.substr(0, equal)) != "apt") continue;
            const auto value = normalized(entry.substr(equal + 1));
            if (apt != -1 || value.empty()) return -1;
            try { apt = decimal(value); } catch (const Error&) { return -1; }
            if (apt > 127 || apt == payload) return -1;
        }
    }
    return apt;
}
bool packetizationSupported(const std::vector<std::string>& input, std::size_t begin,
                            std::size_t end, const std::string& id, bool hevc) {
    std::map<std::string, std::string> parameters;
    for (auto j = begin + 1; j < end; ++j) {
        if (input[j].rfind("a=fmtp:", 0) != 0) continue;
        std::istringstream fields(input[j].substr(7));
        std::string payload, body;
        fields >> payload;
        if (decimal(payload) != decimal(id)) continue;
        std::getline(fields, body);
        std::istringstream values(body);
        std::string entry;
        while (std::getline(values, entry, ';')) {
            const auto equals = entry.find('=');
            if (equals == std::string::npos) return false;
            const auto key = normalized(entry.substr(0, equals));
            const auto value = normalized(entry.substr(equals + 1));
            if (key.empty() || value.empty() || !parameters.emplace(key, value).second) return false;
        }
    }
    for (const auto& parameter : parameters) {
        const auto& key = parameter.first; const auto& value = parameter.second;
        if (!hevc) {
            if (key == "packetization-mode" && value != "0" && value != "1") return false;
            continue;
        }
        if ((key == "sprop-max-don-diff" || key == "sprop-depack-buf-nalus" || key == "depack-buf-cap" ||
             key == "profile-space") && value != "0") return false;
        if (key == "tx-mode" && value != "srst") return false;
        // The experimental Switch path is limited to Main (8-bit 4:2:0).
        if (key == "profile-id" && value != "1") return false;
        if (key == "packetization-mode") return false;
    }
    return true;
}
VideoCodec commonOfferCodec(const std::vector<std::string>& input, VideoCodec preferred) {
    if (preferred != VideoCodec::H265) return VideoCodec::H264;
    bool found = false;
    for (std::size_t begin = 0; begin < input.size();) {
        if (input[begin].rfind("m=", 0) != 0) { ++begin; continue; }
        auto end = begin + 1;
        while (end < input.size() && input[end].rfind("m=", 0) != 0) ++end;
        std::istringstream media(input[begin]);
        std::string type, port, transport, id;
        media >> type >> port >> transport;
        bool receiving = true, hevc = false;
        for (auto i = begin + 1; i < end; ++i)
            if (input[i] == "a=inactive" || input[i] == "a=recvonly") receiving = false;
        if (type == "m=video" && port != "0" && receiving) {
            std::set<std::string> offered;
            while (media >> id) offered.insert(id);
            for (const auto& payload : offered) {
                for (auto i = begin + 1; i < end; ++i) {
                    const auto prefix = "a=rtpmap:" + payload + " ";
                    if (input[i].rfind(prefix, 0) == 0 && normalized(input[i].substr(prefix.size())) == "h265/90000" &&
                        packetizationSupported(input, begin, end, payload, true)) hevc = true;
                }
            }
            if (!hevc) return VideoCodec::H264;
            found = true;
        }
        begin = end;
    }
    return found ? VideoCodec::H265 : VideoCodec::H264;
}

}
std::string filterOffer(const std::string& sdp, VideoCodec preferred) {
    if (sdp.size() > MaxFrame) throw Error("SDP limit exceeded");
    auto input = lines(sdp);
    preferred = commonOfferCodec(input, preferred);
    std::string result;
    bool video = false;
    for (std::size_t begin = 0; begin < input.size();) {
        if (input[begin].rfind("m=", 0) != 0) { result += input[begin++] + "\r\n"; continue; }
        std::size_t end = begin + 1;
        while (end < input.size() && input[end].rfind("m=", 0) != 0) ++end;
        std::istringstream m(input[begin]);
        std::string type, port, transport, pt;
        m >> type >> port >> transport;
        std::set<int> offered, keep;
        if (type != "m=video" && type != "m=audio") {
            for (std::size_t i = begin; i < end; ++i) result += input[i] + "\r\n";
            begin = end; continue;
        }
        while (m >> pt) offered.insert(decimal(pt));
        std::set<int> h264, h265;
        std::map<int, std::string> mappings;
        for (std::size_t i = begin + 1; i < end; ++i) {
            if (input[i].rfind("a=rtpmap:", 0) != 0) continue;
            std::istringstream map(input[i].substr(9));
            std::string id, codec;
            map >> id >> codec;
            codec = normalized(codec);
            const int payload = decimal(id);
            if (payload > 127 || !mappings.emplace(payload, codec).second) throw Error("Invalid SDP payload mapping");
            if (!offered.count(payload)) continue;
            if (type == "m=video" && (codec == "h264/90000" || codec == "h265/90000")) {
                const bool hevc = codec == "h265/90000";
                if (packetizationSupported(input, begin, end, id, hevc)) (hevc ? h265 : h264).insert(payload);
            } else if (type == "m=audio" && (codec == "opus/48000/2" || codec == "opus/48000/1" || codec == "opus/48000"))
                keep.insert(payload);
        }
        if (type == "m=video") {
            keep = preferred == VideoCodec::H265 && !h265.empty() ? h265 : h264;
            const auto media = keep;
            unsigned fidGroups = 0;
            for (auto i = begin + 1; i < end; ++i) if (input[i].rfind("a=ssrc-group:FID ", 0) == 0) ++fidGroups;
            for (const auto& mapping : mappings)
                if (fidGroups <= 1 && offered.count(mapping.first) && mapping.second == "rtx/90000" &&
                    media.count(rtxApt(input, begin + 1, end, mapping.first))) keep.insert(mapping.first);
        }
        // Unsupported active media is rejected explicitly, never advertised with invented IDs.
        bool receivesRemote = true;
        for (std::size_t j = begin + 1; j < end; ++j)
            if (input[j] == "a=recvonly" || input[j] == "a=inactive") receivesRemote = false;
        const bool active = port != "0" && !keep.empty() && receivesRemote;
        if (type == "m=video" && active) video = true;
        result += type + ' ' + (active ? port : "0") + ' ' + transport;
        const auto& selected = keep.empty() ? offered : keep;
        for (const int payload : selected) result += ' ' + std::to_string(payload);
        result += "\r\n";
        for (std::size_t i = begin + 1; i < end; ++i) {
            const auto& line = input[i];
            // v0.24.1's SDP parser rejects wildcard RTCP feedback. Expand only onto
            // selected payloads so equivalent offers are still accepted safely.
            if (line.rfind("a=rtcp-fb:* ", 0) == 0) {
                for (const int payload : keep)
                    result += "a=rtcp-fb:" + std::to_string(payload) + line.substr(11) + "\r\n";
                continue;
            }
            bool remove = false;
            for (const std::string prefix : {"a=rtpmap:", "a=fmtp:", "a=rtcp-fb:"}) {
                if (line.rfind(prefix, 0) != 0) continue;
                const auto space = line.find_first_of(" \t", prefix.size());
                const auto id = line.substr(prefix.size(), space - prefix.size());
                if (id != "*" && !keep.count(decimal(id))) remove = true;
            }
            if (line == "a=sendrecv" || line == "a=recvonly" || line == "a=sendonly" || line == "a=inactive") {
                if (!active) remove = true;
            }
            if (!remove) result += line + "\r\n";
        }
        if (!active) result += "a=inactive\r\n";
        begin = end;
    }
    if (!video) throw Error("No supported video codec offered");
    return result;
}
RtpRepairConfig videoRepairConfig(const std::string& sdp) {
    if (sdp.size() > MaxFrame) throw Error("SDP limit exceeded");
    RtpRepairConfig result; const auto input = lines(sdp);
    std::map<unsigned, std::string> mappings;
    for (const auto& line : input) if (line.rfind("a=rtpmap:", 0) == 0) {
        std::istringstream fields(line.substr(9)); std::string id, codec; fields >> id >> codec;
        const auto pt = decimal(id); if (pt > 127) throw Error("Invalid repair payload");
        mappings.emplace(pt, normalized(codec));
        if (normalized(codec) == "h264/90000" || normalized(codec) == "h265/90000") result.media.emplace(pt, false);
    }
    for (const auto& item : mappings) if (item.second == "rtx/90000") {
        const auto apt = rtxApt(input, 0, input.size(), item.first);
        if (apt >= 0 && result.media.count(apt)) result.rtx.emplace(item.first, apt);
    }
    unsigned fidGroups = 0;
    for (const auto& line : input) {
        if (line.rfind("a=rtcp-fb:", 0) == 0) {
            std::istringstream fields(line.substr(10)); std::string id, feedback; fields >> id; std::getline(fields, feedback);
            if (normalized(feedback) == "nack") {
                if (id == "*") for (auto& item : result.media) item.second = true;
                else { const auto it = result.media.find(decimal(id)); if (it != result.media.end()) it->second = true; }
            }
        }
        if (line.rfind("a=ssrc-group:FID ", 0) == 0) {
            std::istringstream fields(line.substr(17)); std::uint64_t media = 0, rtx = 0; std::string extra;
            if (!(fields >> media >> rtx) || (fields >> extra) || !media || !rtx || media == rtx ||
                media > 0xffffffffu || rtx > 0xffffffffu) throw Error("Invalid repair SSRC group");
            ++fidGroups; result.mediaSsrc = media; result.rtxSsrc = rtx;
        }
    }
    // Multiple FID groups need a simulcast-aware SSRC selector. Retain ordinary
    // media and generic NACK, but do not guess an RTX association.
    if (fidGroups > 1) { result.mediaSsrc = 0; result.rtxSsrc = 0; result.rtx.clear(); }
    return result;
}
VideoCodec offeredVideoCodec(const std::string& sdp) {
    if (sdp.size() > MaxFrame) throw Error("SDP limit exceeded");
    const auto input = lines(sdp);
    bool found = false; VideoCodec selected = VideoCodec::H264;
    for (std::size_t begin = 0; begin < input.size();) {
        if (input[begin].rfind("m=", 0) != 0) { ++begin; continue; }
        auto end = begin + 1;
        while (end < input.size() && input[end].rfind("m=", 0) != 0) ++end;
        std::istringstream media(input[begin]);
        std::string type, port, transport, id;
        media >> type >> port >> transport;
        bool receiving = true;
        for (auto i = begin + 1; i < end; ++i)
            if (input[i] == "a=inactive" || input[i] == "a=recvonly") receiving = false;
        if (type == "m=video" && port != "0" && receiving) {
            std::set<std::string> offered;
            while (media >> id) offered.insert(id);
            if (offered.empty()) throw Error("Missing video payload");
            for (const auto& payload : offered) {
                if (decimal(payload) > 127) throw Error("Invalid selected video payload");
                std::string codec;
                for (auto i = begin + 1; i < end; ++i) {
                    const auto prefix = "a=rtpmap:" + payload + " ";
                    if (input[i].rfind(prefix, 0) != 0) continue;
                    if (!codec.empty()) throw Error("Ambiguous video payload mapping");
                    codec = normalized(input[i].substr(prefix.size()));
                }
                if (codec == "rtx/90000") {
                    const auto apt = rtxApt(input, begin + 1, end, decimal(payload));
                    if (apt < 0 || !offered.count(std::to_string(apt))) throw Error("Invalid selected RTX mapping");
                    continue;
                }
                if (codec != "h264/90000" && codec != "h265/90000") throw Error("Unsupported selected video codec");
                const auto candidate = codec == "h265/90000" ? VideoCodec::H265 : VideoCodec::H264;
                if (found && selected != candidate) throw Error("Mixed selected video codecs");
                if (!packetizationSupported(input, begin, end, payload, candidate == VideoCodec::H265))
                    throw Error("Unsupported selected video packetization");
                found = true; selected = candidate;
            }
        }
        begin = end;
    }
    if (!found) throw Error("No selected video codec");
    return selected;
}

}
