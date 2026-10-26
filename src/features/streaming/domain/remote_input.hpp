#pragma once
#include <array>
#include <string>
#include <variant>

namespace fog::remote {
struct AbsoluteMouse {
    int x, y, width, height, frameWidth, frameHeight;
};
struct MouseButton { bool down; int button; };
struct Wheel { int dy; };
struct Key { bool down; std::string code; };
struct UnicodeText { std::string utf8; };
enum class TouchPhase { Down, Move, Up };
enum class MotionSensor { Accelerometer, Gyroscope };
struct Touch { TouchPhase phase; int finger; float x, y; };
struct Sensor { MotionSensor kind; std::array<float, 3> xyz; };
using Input = std::variant<AbsoluteMouse, MouseButton, Wheel, Key, UnicodeText, Touch, Sensor>;
}
