#include "core/constants/error_text.hpp"
#include "features/streaming/application/remote_input.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace fog::remote {
namespace {
void validateDimension(int value) {
    if (value < 1 || value > 8192)
        throw std::invalid_argument(fog::constants::error::Stream::RemoteInputDimensionMustBeBetween1And8192);
}
void appendUtf16Unit(std::string& output, std::uint32_t unit) {
    output += static_cast<char>(unit & 0xff);
    output += static_cast<char>((unit >> 8) & 0xff);
}
std::string utf16le(const std::string& input) {
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
std::string base64(const std::string& bytes) {
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
Json absoluteMouse(int x, int y, int width, int height, int frameWidth, int frameHeight) {
    validateDimension(width);
    validateDimension(height);
    validateDimension(frameWidth);
    validateDimension(frameHeight);
    return {{"id", "mousemove"}, {"data", {{"width", width}, {"height", height},
            {"frameWidth", frameWidth}, {"frameHeight", frameHeight},
            {"x", std::clamp(x, 0, width - 1)}, {"y", std::clamp(y, 0, height - 1)},
            {"controls_type", "default"}}}};
}
Json mouseButton(bool down, int button) {
    if (button != 0 && button != 2)
        throw std::invalid_argument(fog::constants::error::Stream::UnsupportedRemoteMouseButton);
    return {{"id", down ? "mousedown" : "mouseup"}, {"data", {{"button", button}}}};
}
Json wheel(int dy) {
    return {{"id", "mousewheel"}, {"data", {{"dy", dy}}}};
}
Json key(bool down, const std::string& code) {
    if (code != "Enter" && code != "Backspace" && code != "Tab" && code != "Escape")
        throw std::invalid_argument(fog::constants::error::Stream::UnsupportedRemoteKeyCode);
    return {{"id", down ? "keydown" : "keyup"}, {"data", {{"code", code},
            {"ctrlKey", false}, {"shiftKey", false}, {"altKey", false}}}};
}
Json unicodeText(const std::string& utf8) {
    return {{"id", "unicode_input"}, {"data", {{"input", base64(utf16le(utf8))}}}};
}
}
