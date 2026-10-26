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
