#pragma once
#include "features/streaming/domain/remote_input.hpp"
#include <string>

namespace fog::remote {
// Dimensions must be 1..8192. Coordinates are clamped to the rendered area.
Input absoluteMouse(int x, int y, int width, int height, int frameWidth, int frameHeight);
Input mouseButton(bool down, int button);
Input wheel(int dy);
Input key(bool down, const std::string& code);
// Strict UTF-8, at most 512 bytes. Encoding for the selected service is infrastructure-owned.
void validateText(const std::string& utf8);
Input unicodeText(const std::string& utf8);
}
