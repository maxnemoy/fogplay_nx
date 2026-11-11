#pragma once
#include <string>

#ifndef FOGPLAY_INPUT_CHANGE_ONLY
#define FOGPLAY_INPUT_CHANGE_ONLY 0
#endif
#ifndef FOGPLAY_MOUSE_PER_FRAME
#define FOGPLAY_MOUSE_PER_FRAME 0
#endif
#ifndef FOGPLAY_TYPED_INPUT
#define FOGPLAY_TYPED_INPUT 0
#endif
static_assert(FOGPLAY_INPUT_CHANGE_ONLY == 0 || FOGPLAY_INPUT_CHANGE_ONLY == 1, "Invalid input change-only option");
static_assert(FOGPLAY_MOUSE_PER_FRAME == 0 || FOGPLAY_MOUSE_PER_FRAME == 1, "Invalid mouse per-frame option");
static_assert(FOGPLAY_TYPED_INPUT == 0 || FOGPLAY_TYPED_INPUT == 1, "Invalid typed input option");
namespace fog::input_options {
inline constexpr bool changeOnly = FOGPLAY_INPUT_CHANGE_ONLY;
inline constexpr bool mousePerFrame = FOGPLAY_MOUSE_PER_FRAME;
inline constexpr bool typedPath = FOGPLAY_TYPED_INPUT;
inline std::string diagnostics() {
    return "Input options change_only=" + std::to_string(changeOnly) +
        " mouse_per_frame=" + std::to_string(mousePerFrame) + " typed_path=" + std::to_string(typedPath);
}
}
