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
// Physical key of a scalar on a US keyboard; scalars without a known key map to themselves.
std::uint32_t physicalKey(std::uint32_t scalar) {
    if (scalar >= 'A' && scalar <= 'Z') return scalar - 'A' + 'a';
    static constexpr char shifted[] = "!@#$%^&*()_+{}|:\"<>?~";
    static constexpr char base[] = "1234567890-=[]\\;',./`";
    for (std::size_t i = 0; shifted[i]; ++i) if (scalar == static_cast<unsigned char>(shifted[i])) return static_cast<unsigned char>(base[i]);
    if (scalar >= 0x410 && scalar <= 0x42f) scalar += 0x20; // Cyrillic capitals
    if (scalar == 0x401) scalar = 0x451;                     // Ё
    // ЙЦУКЕН lowercase letters in U+0430..U+044F order, then ё, mapped to their US key.
    static constexpr char cyrillic[] = "f,dult;pbqrkvyjghcnea[wxio]sm'.z";
    if (scalar >= 0x430 && scalar <= 0x44f) return static_cast<unsigned char>(cyrillic[scalar - 0x430]);
    if (scalar == 0x451) return '`';
    return scalar;
}
}
std::vector<std::string> typingSegments(const std::string& utf8) {
    validateText(utf8);
    std::vector<std::string> segments;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const std::size_t count = lead <= 0x7f ? 1 : lead <= 0xdf ? 2 : lead <= 0xef ? 3 : 4;
        std::uint32_t scalar = count == 1 ? lead : count == 2 ? (lead & 0x1f) : count == 3 ? (lead & 0x0f) : (lead & 0x07);
        for (std::size_t j = 1; j < count; ++j) scalar = (scalar << 6) | (static_cast<unsigned char>(utf8[i + j]) & 0x3f);
        const auto key = physicalKey(scalar);
        if (segments.empty() || key == previous) segments.emplace_back();
        segments.back().append(utf8, i, count);
        previous = key;
        i += count;
    }
    return segments;
}
void validateText(const std::string& input) {
    if (input.size() > 512)
        throw std::invalid_argument(fog::constants::error::Stream::RemoteTextExceeds512UTF8Bytes);
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
        i += count;
    }
}
Input absoluteMouse(int x, int y, int width, int height, int frameWidth, int frameHeight) {
    validateDimension(width);
    validateDimension(height);
    validateDimension(frameWidth);
    validateDimension(frameHeight);
    return AbsoluteMouse{std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1), width, height, frameWidth, frameHeight};
}
Input mouseButton(bool down, int button) {
    if (button != 0 && button != 2)
        throw std::invalid_argument(fog::constants::error::Stream::UnsupportedRemoteMouseButton);
    return MouseButton{down, button};
}
Input wheel(int dy) { return Wheel{dy}; }
Input key(bool down, const std::string& code) {
    if (code != "Enter" && code != "Backspace" && code != "Tab" && code != "Escape")
        throw std::invalid_argument(fog::constants::error::Stream::UnsupportedRemoteKeyCode);
    return Key{down, code};
}
Input unicodeText(const std::string& utf8) {
    validateText(utf8);
    return UnicodeText{utf8};
}
}
