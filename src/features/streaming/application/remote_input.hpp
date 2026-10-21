#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace fog::remote {
using Json = nlohmann::json;
// Dimensions must be 1..8192; x/y use rendered-area coordinates and are
// clamped to [0, width - 1] / [0, height - 1], without scaling to frame units.
// Invalid dimensions, buttons, key codes or text throw std::invalid_argument.
Json absoluteMouse(int x, int y, int width, int height, int frameWidth, int frameHeight);
// Only the browser's left (0) and right (2) mouse buttons are supported.
Json mouseButton(bool down, int button);
Json wheel(int dy);
// Supported browser codes: Enter, Backspace, Tab, Escape; no modifiers.
Json key(bool down, const std::string& code);
// Strict UTF-8, at most 512 bytes. Sends Base64 of UTF-16LE without a BOM.
Json unicodeText(const std::string& utf8);
}
