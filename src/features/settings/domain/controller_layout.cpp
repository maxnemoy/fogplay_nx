#include "features/settings/domain/controller_layout.hpp"

namespace fog::controller {
namespace {
std::uint64_t swapFaceButtons(std::uint64_t physical, const FaceButtonMasks& masks) {
    auto mapped = physical & ~(masks.a | masks.b | masks.x | masks.y);
    if (physical & masks.a) mapped |= masks.b;
    if (physical & masks.b) mapped |= masks.a;
    if (physical & masks.x) mapped |= masks.y;
    if (physical & masks.y) mapped |= masks.x;
    return mapped;
}
}
std::uint64_t mapUiButtons(std::uint64_t physical, const Settings& settings, const FaceButtonMasks& masks) {
    return settings.xbox ? swapFaceButtons(physical, masks) : physical;
}
std::uint64_t mapStreamButtons(std::uint64_t physical, const Settings& settings, const FaceButtonMasks& masks) {
    return settings.xbox ? physical : swapFaceButtons(physical, masks);
}
}
