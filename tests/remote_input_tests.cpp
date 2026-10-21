#include "features/streaming/application/remote_input.hpp"
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
template<class F> void rejects(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
std::string bytes(std::initializer_list<unsigned> input) {
    std::string result;
    for (const auto value : input) result += static_cast<char>(value);
    return result;
}
}
int main() {
    using namespace fog::remote;
    assert(absoluteMouse(640, 360, 1280, 720, 1920, 1080) == Json({
        {"id", "mousemove"}, {"data", {{"width", 1280}, {"height", 720},
        {"frameWidth", 1920}, {"frameHeight", 1080}, {"x", 640}, {"y", 360},
        {"controls_type", "default"}}}}));
    const auto edge = absoluteMouse(std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
                                    1280, 720, 8192, 8192);
    assert(edge["data"]["x"] == 0 && edge["data"]["y"] == 719);
    assert(absoluteMouse(1280, -1, 1280, 720, 1920, 1080)["data"]["x"] == 1279);
    assert(absoluteMouse(1280, -1, 1280, 720, 1920, 1080)["data"]["y"] == 0);
    const auto single = absoluteMouse(1, 1, 1, 1, 1, 1);
    assert(single["data"]["x"] == 0 && single["data"]["y"] == 0);
    for (const int invalid : {-1, 0, 8193, std::numeric_limits<int>::max()}) {
        rejects([&] { absoluteMouse(0, 0, invalid, 720, 1920, 1080); });
        rejects([&] { absoluteMouse(0, 0, 1280, invalid, 1920, 1080); });
        rejects([&] { absoluteMouse(0, 0, 1280, 720, invalid, 1080); });
        rejects([&] { absoluteMouse(0, 0, 1280, 720, 1920, invalid); });
    }
    for (const int button : {0, 2}) {
        assert(mouseButton(true, button) == Json({{"id", "mousedown"}, {"data", {{"button", button}}}}));
        assert(mouseButton(false, button) == Json({{"id", "mouseup"}, {"data", {{"button", button}}}}));
    }
    for (const int button : {-1, 1, 3, std::numeric_limits<int>::max()})
        rejects([&] { mouseButton(true, button); });
    for (const int dy : {-120, 0, 120, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()})
        assert(wheel(dy) == Json({{"id", "mousewheel"}, {"data", {{"dy", dy}}}}));
    for (const std::string code : {"Enter", "Backspace", "Tab", "Escape"}) {
        const Json data{{"code", code}, {"ctrlKey", false}, {"shiftKey", false}, {"altKey", false}};
        assert(key(true, code) == Json({{"id", "keydown"}, {"data", data}}));
        assert(key(false, code) == Json({{"id", "keyup"}, {"data", data}}));
    }
    for (const std::string code : {"", "enter", "KeyA", "Space", "Esc", "Enter\n"})
        rejects([&] { key(true, code); });
    const std::vector<std::pair<std::string, std::string>> examples{
        {"", ""}, {"A", "QQA="}, {"AB", "QQBCAA=="}, {"ABC", "QQBCAEMA"},
        {"Hello", "SABlAGwAbABvAA=="}, {u8"\u041f\u0440\u0438\u0432\u0435\u0442", "HwRABDgEMgQ1BEIE"},
        {u8"\U0001f600", "PdgA3g=="}, {u8"A\U0001f600\u0416", "QQA92ADeFgQ="},
        {u8"\U0010ffff", "/9v/3w=="}, {std::string(1, '\0'), "AAA="},
        {u8"\u0080", "gAA="}, {u8"\u07ff", "/wc="}, {u8"\u0800", "AAg="},
        {u8"\ud7ff", "/9c="}, {u8"\ue000", "AOA="}, {u8"\uffff", "//8="},
        {u8"\U00010000", "ANgA3A=="}
    };
    for (const auto& example : examples)
        assert(unicodeText(example.first) == Json({{"id", "unicode_input"}, {"data", {{"input", example.second}}}}));
    const std::vector<std::string> invalid{
        bytes({0x80}), bytes({0xbf}), bytes({0xc0, 0x80}), bytes({0xc1, 0xbf}),
        bytes({0xc2}), bytes({0xc2, 0x7f}), bytes({0xdf, 0xc0}),
        bytes({0xe0}), bytes({0xe0, 0xa0}), bytes({0xe0, 0x80, 0x80}), bytes({0xe0, 0x9f, 0xbf}),
        bytes({0xe1, 0x41, 0x80}), bytes({0xe1, 0x80, 0x41}),
        bytes({0xed, 0xa0, 0x80}), bytes({0xed, 0xbf, 0xbf}),
        bytes({0xf0}), bytes({0xf0, 0x90}), bytes({0xf0, 0x90, 0x80}),
        bytes({0xf0, 0x80, 0x80, 0x80}), bytes({0xf0, 0x8f, 0xbf, 0xbf}),
        bytes({0xf0, 0x41, 0x80, 0x80}), bytes({0xf0, 0x90, 0x41, 0x80}), bytes({0xf0, 0x90, 0x80, 0x41}),
        bytes({0xf4, 0x90, 0x80, 0x80}), bytes({0xf5, 0x80, 0x80, 0x80}),
        bytes({0xf8, 0x88, 0x80, 0x80, 0x80}), bytes({0xfc, 0x84, 0x80, 0x80, 0x80, 0x80}),
        bytes({0xfe}), bytes({0xff}), "ok" + bytes({0x80}), bytes({0xc2, 0x80, 0x80})
    };
    for (const auto& input : invalid) rejects([&] { unicodeText(input); });
    assert(unicodeText(std::string(512, 'x'))["data"]["input"].get<std::string>().size() == 1368);
    rejects([] { unicodeText(std::string(513, 'x')); });
    std::string multibyte;
    for (int i = 0; i < 128; ++i) multibyte += u8"\U0001f600";
    assert(multibyte.size() == 512);
    assert(unicodeText(multibyte)["data"]["input"].get<std::string>().size() == 684);
    rejects([&] { unicodeText(multibyte + "a"); });
    std::cout << "Remote input tests passed (mouse/button/wheel/key/UTF-8/UTF-16LE/Base64/bounds)\n";
}
