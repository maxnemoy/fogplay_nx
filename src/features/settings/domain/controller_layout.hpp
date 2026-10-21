#pragma once
#include <cstdint>
#include <string>

namespace fog::controller {
struct Settings {
    // Preserve the existing stream protocol mapping on first launch.
    bool xbox = true;
};
inline bool operator==(const Settings& a, const Settings& b) { return a.xbox == b.xbox; }
inline bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }

// Supply the platform's distinct, single-bit physical face button masks.
// Every other bit, including Plus/Minus, passes through unchanged.
struct FaceButtonMasks {
    std::uint64_t a;
    std::uint64_t b;
    std::uint64_t x;
    std::uint64_t y;
};
std::uint64_t mapUiButtons(std::uint64_t physical, const Settings& settings, const FaceButtonMasks& masks);
// Feed this result to the existing stream protocol mapping. Xbox layout keeps
// that mapping; Nintendo layout exchanges physical A/B and X/Y before it.
std::uint64_t mapStreamButtons(std::uint64_t physical, const Settings& settings, const FaceButtonMasks& masks);

// Only call for explicit controller hints, never arbitrary titles or content.
inline std::string uiHints(const std::string& text, const Settings& settings) {
    if (!settings.xbox) return text;
    auto result = text;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto wordByte = [](unsigned char value) {
            return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') || value == '_' || value >= 128;
        };
        if ((i > 0 && wordByte(static_cast<unsigned char>(text[i - 1]))) ||
            (i + 1 < text.size() && wordByte(static_cast<unsigned char>(text[i + 1])))) continue;
        switch (text[i]) {
            case 'A': result[i] = 'B'; break;
            case 'B': result[i] = 'A'; break;
            case 'X': result[i] = 'Y'; break;
            case 'Y': result[i] = 'X'; break;
        }
    }
    return result;
}

}
