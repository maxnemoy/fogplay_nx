#pragma once
#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/domain/video_codec.hpp"
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace stream::protocol {
using Json = nlohmann::json;
// Only fixed, locally authored messages; never include a remote field in this type.
struct Error final : std::runtime_error {
    explicit Error(const char* message) : std::runtime_error(message) {}
};
struct Frame {
    std::string command;
    std::map<std::string, std::string> headers;
    std::string body;
};
// Incremental STOMP 1.2 parser; handles heartbeats, CRLF, content-length and NUL.
class Parser {
public:
    std::vector<Frame> feed(const std::string& bytes);
private:
    std::string pending_;
};
std::string frame(const std::string& command,
                  const std::map<std::string, std::string>& headers = {},
                  const std::string& body = {});
std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& encoded);
Json gamepad(const Gamepad& state);
struct IceServer { std::string url, username, credential; };
std::vector<IceServer> iceServers(const Json& initMessage);
// Keep one video codec per track, preferring HEVC only when explicitly selected.
// HEVC without a supported base-layer/no-DONL mode falls back to H.264.
std::string filterOffer(const std::string& sdp, VideoCodec preferred = VideoCodec::H264);
// Validate active filtered tracks and return their common selected video codec.
VideoCodec offeredVideoCodec(const std::string& filteredSdp);
std::vector<std::string> mediaMids(const std::string& sdp);
}
