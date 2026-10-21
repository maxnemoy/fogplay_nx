#pragma once
namespace stream {
enum class VideoCodec { H264, H265 };
inline const char* videoCodecName(VideoCodec codec) {
    return codec == VideoCodec::H265 ? "H.265" : "H.264";
}
}
