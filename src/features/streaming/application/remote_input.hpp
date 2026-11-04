#pragma once
#include "features/streaming/domain/remote_input.hpp"
#include <string>
#include <vector>

namespace fog::remote {
// Dimensions must be 1..8192. Coordinates are clamped to the rendered area.
Input absoluteMouse(int x, int y, int width, int height, int frameWidth, int frameHeight);
Input mouseButton(bool down, int button);
Input wheel(int dy);
Input key(bool down, const std::string& code);
// Strict UTF-8, at most 512 bytes. Encoding for the selected service is infrastructure-owned.
void validateText(const std::string& utf8);
Input unicodeText(const std::string& utf8);
// A remote host may replay one text message as a burst of key events inside a
// single game input frame; two presses of the same physical key in that burst
// can merge ("1991!" became "19!": 9+9 and 1+Shift+1). Split strict UTF-8 text
// before every character typed with the same key as the previous one (case,
// US shifted symbols and the Russian ЙЦУКЕН layout share keys). Concatenating
// the segments yields the input; empty input yields no segments.
std::vector<std::string> typingSegments(const std::string& utf8);
}
