#pragma once
#include "features/streaming/domain/remote_input.hpp"
#include "core/constants/error_text.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace stream::protocol {
namespace input_detail {
inline void appendUtf16Unit(std::string& output, std::uint32_t unit) {
    output += static_cast<char>(unit & 0xff);
    output += static_cast<char>((unit >> 8) & 0xff);
}
inline std::string utf16le(const std::string& input) {
    if (input.size() > 512)
        throw std::invalid_argument(fog::constants::error::Stream::RemoteTextExceeds512UTF8Bytes);
    std::string output;
    output.reserve(input.size() * 2);
    for (std::size_t i = 0; i < input.size();) {
        const auto lead = static_cast<unsigned char>(input[i]);
        std::uint32_t scalar, minimum;
        std::size_t count;
        if (lead <= 0x7f) { scalar = lead; minimum = 0; count = 1; }
        else if (lead >= 0xc2 && lead <= 0xdf) { scalar = lead & 0x1f; minimum = 0x80; count = 2; }
        else if (lead >= 0xe0 && lead <= 0xef) { scalar = lead & 0x0f; minimum = 0x800; count = 3; }
        else if (lead >= 0xf0 && lead <= 0xf4) { scalar = lead & 0x07; minimum = 0x10000; count = 4; }
        else throw std::invalid_argument(fog::constants::error::Stream::InvalidUTF8RemoteText);
        if (input.size() - i < count)
            throw std::invalid_argument(fog::constants::error::Stream::InvalidUTF8RemoteText);
        for (std::size_t j = 1; j < count; ++j) {
            const auto continuation = static_cast<unsigned char>(input[i + j]);
            if ((continuation & 0xc0) != 0x80)
                throw std::invalid_argument(fog::constants::error::Stream::InvalidUTF8RemoteText);
            scalar = (scalar << 6) | (continuation & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            throw std::invalid_argument(fog::constants::error::Stream::InvalidUTF8RemoteText);
        if (scalar <= 0xffff) appendUtf16Unit(output, scalar);
        else {
            scalar -= 0x10000;
            appendUtf16Unit(output, 0xd800 | (scalar >> 10));
            appendUtf16Unit(output, 0xdc00 | (scalar & 0x3ff));
        }
        i += count;
    }
    return output;
}
inline std::string base64(const std::string& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        output += alphabet[a >> 2];
        output += alphabet[((a & 3) << 4) | (b >> 4)];
        output += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        output += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return output;
}
}
inline nlohmann::json remoteInput(const fog::remote::Input& input) {
    using Json = nlohmann::json;
    return std::visit([](const auto& value) -> Json {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, fog::remote::AbsoluteMouse>) {
            for (const auto dimension : {value.width,value.height,value.frameWidth,value.frameHeight})
                if (dimension < 1 || dimension > 8192)
                    throw std::invalid_argument(fog::constants::error::Stream::RemoteInputDimensionMustBeBetween1And8192);
            auto mouse = value;
            mouse.x = std::clamp(mouse.x,0,mouse.width-1);
            mouse.y = std::clamp(mouse.y,0,mouse.height-1);
            return {{"id", "mousemove"}, {"data", {{"width", mouse.width}, {"height", mouse.height},
                {"frameWidth", mouse.frameWidth}, {"frameHeight", mouse.frameHeight},
                {"x", mouse.x}, {"y", mouse.y}, {"controls_type", "default"}}}};
        } else if constexpr (std::is_same_v<T, fog::remote::MouseButton>) {
            if (value.button != 0 && value.button != 2)
                throw std::invalid_argument(fog::constants::error::Stream::UnsupportedRemoteMouseButton);
            return {{"id", value.down ? "mousedown" : "mouseup"}, {"data", {{"button", value.button}}}};
        } else if constexpr (std::is_same_v<T, fog::remote::Wheel>) {
            return {{"id", "mousewheel"}, {"data", {{"dy", value.dy}}}};
        } else if constexpr (std::is_same_v<T, fog::remote::Key>) {
            if (value.code != "Enter" && value.code != "Backspace" && value.code != "Tab" && value.code != "Escape")
                throw std::invalid_argument(fog::constants::error::Stream::UnsupportedRemoteKeyCode);
            return {{"id", value.down ? "keydown" : "keyup"}, {"data", {{"code", value.code},
                {"ctrlKey", false}, {"shiftKey", false}, {"altKey", false}}}};
        } else if constexpr (std::is_same_v<T, fog::remote::UnicodeText>) {
            return {{"id", "unicode_input"}, {"data", {{"input", input_detail::base64(input_detail::utf16le(value.utf8))}}}};
        } else if constexpr (std::is_same_v<T, fog::remote::Touch>) {
            const auto type = value.phase==fog::remote::TouchPhase::Down ? 0x656 : value.phase==fog::remote::TouchPhase::Move ? 0x657 : value.phase==fog::remote::TouchPhase::Up ? 0x658 : -1;
            return {{"id","gamepad_touch"},{"index",0},{"data",{{"type",type},{"finger",value.finger},{"x",value.x},{"y",value.y}}}};
        } else {
            const auto kind = value.kind==fog::remote::MotionSensor::Accelerometer ? 1 : value.kind==fog::remote::MotionSensor::Gyroscope ? 2 : -1;
            return {{"id","gamepad_sensor"},{"index",0},{"data",{{"sensor",kind},{"x",value.xyz[0]},{"y",value.xyz[1]},{"z",value.xyz[2]}}}};
        }
    }, input);
}
}
