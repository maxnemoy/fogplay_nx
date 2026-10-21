#include "features/streaming/presentation/stream_overlay.hpp"
#include <iostream>
#include <stdexcept>

namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
void keyboard(fog::stream::Overlay& overlay) {
    overlay.openMenu(); overlay.down(); overlay.select();
    require(overlay.mode() == fog::stream::OverlayMode::Keyboard, "open keyboard");
}
fog::stream::OverlayAction key(fog::stream::Overlay& overlay, int index) {
    int x = 0, y = 244;
    if (index < 48) { x = 52 + index % 12 * 98; y += index / 12 * 65; }
    else if (index < 54) { x = 52 + (index - 48) * 196; y += 260; }
    else { const int starts[]{0, 3, 5, 7, 10}; x = 52 + starts[index - 54] * 98; y += 325; }
    return overlay.touch(x + 8, y + 8);
}
}
int main() {
    using namespace fog::stream;
    try {
        Overlay overlay;
        require(overlay.mode() == OverlayMode::Closed, "initially closed");
        require(!overlay.appendText("outside"), "only keyboard accepts text");
        overlay.openMenu(); overlay.up();
        require(overlay.selection() == 4, "menu wraps up");
        overlay.select(); require(overlay.mode() == OverlayMode::Closed, "close menu item");
        overlay.openMenu(); overlay.select();
        require(overlay.mode() == OverlayMode::Mouse, "mouse menu item");
        overlay.back(); require(overlay.mode() == OverlayMode::Menu, "mouse back enters menu");
        overlay.down(); overlay.down(); overlay.down(); overlay.select();
        require(overlay.mode() == OverlayMode::StopConfirm && overlay.selection() == 1, "stop defaults to continue");
        require(overlay.select().type == OverlayActionType::None && overlay.mode() == OverlayMode::Menu, "second A cannot end session");
        overlay.select(); overlay.left();
        require(overlay.select().type == OverlayActionType::StopSession && overlay.mode() == OverlayMode::Closed, "confirmed stop action");
        overlay.openMenu(); overlay.down(); overlay.down(); overlay.select();
        require(overlay.mode() == OverlayMode::ImageSettings && overlay.selection() == 1, "image settings opens on safe quality field");
        require(overlay.videoSettings().codec == ::stream::VideoCodec::H264, "default overlay codec");
        require(overlay.left().type == OverlayActionType::SettingsChanged && overlay.videoSettings().quality == 4, "left changes safe quality and reports action");
        require(overlay.right().type == OverlayActionType::SettingsChanged && overlay.videoSettings().quality == 0, "quality wraps back to auto");
        overlay.down();
        require(overlay.select().type == OverlayActionType::SettingsChanged && overlay.videoSettings().bitrateMbps == 30, "requested cap adjustment action");
        overlay.down();
        require(overlay.right().type == OverlayActionType::SettingsChanged && overlay.videoSettings().keyframeSeconds == 2, "periodic PLI adjustment action");
        overlay.down();
        require(overlay.select().type == OverlayActionType::RequestKeyframe, "manual keyframe explicit action");
        overlay.down(); overlay.down();
        require(overlay.selection() == 1, "live navigation skips immutable codec");
        const auto prior = overlay.videoSettings();
        require(overlay.touch(200, 170).type == OverlayActionType::None && overlay.videoSettings() == prior, "codec touch is ignored during stream");
        for (int i = 0; i < 50; ++i) { overlay.up(); require(overlay.selection() >= 1 && overlay.selection() <= 5, "live navigation bounded"); }
        overlay.back();
        require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 2, "image B returns to originating menu item");
        overlay.select(); overlay.touch(200, 495);
        require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 2, "image touch back returns to menu");
        keyboard(overlay);
        overlay.left(); require(overlay.selection() == 11, "keyboard left wraps in row");
        overlay.right(); require(overlay.selection() == 0, "keyboard right wraps in row");
        overlay.up(); require(overlay.selection() == 54, "up from top goes to send row");
        overlay.down(); require(overlay.selection() >= 0 && overlay.selection() < 12, "down wraps to first row by geometry");
        for (int i = 0; i < 2000; ++i) {
            switch (i % 4) { case 0: overlay.up(); break; case 1: overlay.left(); break; case 2: overlay.down(); break; case 3: overlay.right(); break; }
            require(overlay.selection() >= 0 && overlay.selection() < Overlay::KeyboardKeyCount, "repeated navigation in range");
        }
        for (int language = 0; language < 2; ++language) {
            for (int caps = 0; caps < 2; ++caps) {
                for (int i = 0; i < 48; ++i) {
                    key(overlay, 52); key(overlay, i);
                    require(!overlay.text().empty() && overlay.text().size() <= 2, "each grid cell inserts valid letter or punctuation");
                }
                key(overlay, 49);
            }
            key(overlay, 48);
        }
        key(overlay, 52);
        key(overlay, 48); key(overlay, 12);
        require(overlay.text() == "й", "Russian grid inserts Cyrillic");
        key(overlay, 49); key(overlay, 13);
        require(overlay.text() == "йЦ", "Russian uppercase");
        key(overlay, 51); require(overlay.text() == "й", "local deletion removes complete Cyrillic character");
        require(overlay.appendText("😀"), "four byte UTF8 accepted");
        overlay.eraseLast(); require(overlay.text() == "й", "deletion removes complete four byte character");
        key(overlay, 50); require(overlay.text() == "й ", "space action");
        const auto original = overlay.text();
        const std::string invalid[]{std::string(1, '\x80'), "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe0\x80\x80", "\xf0\x80\x80\x80", "\xe2\x82", std::string("a\0b", 3), "\n", "\t", "\x7f"};
        for (const auto& value : invalid) require(!overlay.appendText(value) && overlay.text() == original, "reject malformed and control input atomically");
        key(overlay, 52);
        require(overlay.appendText(std::string(510, 'a')), "buffer near limit");
        require(!overlay.appendText("😀") && overlay.text().size() == 510, "reject character crossing limit");
        require(overlay.appendText("й") && overlay.text().size() == 512, "UTF8 exactly fills limit");
        require(!overlay.appendText("x"), "full buffer rejects additional input");
        overlay.eraseLast(); require(overlay.text().size() == 510, "delete from full UTF8 buffer");
        for (int i = 0; i < 4; ++i) {
            const OverlayActionType types[]{OverlayActionType::Enter, OverlayActionType::Tab, OverlayActionType::Backspace, OverlayActionType::Escape};
            require(key(overlay, 55 + i).type == types[i] && overlay.text().size() == 510 && overlay.mode() == OverlayMode::Keyboard, "remote special keys preserve local text and keyboard");
        }
        const auto sent = key(overlay, 54);
        require(sent.type == OverlayActionType::SendText && sent.text == std::string(510, 'a'), "explicit send returns exact text");
        require(overlay.mode() == OverlayMode::Closed && overlay.text().empty(), "send closes and clears local text");
        keyboard(overlay);
        require(key(overlay, 54).type == OverlayActionType::None && overlay.mode() == OverlayMode::Keyboard, "empty send is disabled");
        overlay.appendText("unsent");
        require(key(overlay, 53).type == OverlayActionType::None && overlay.text().empty() && overlay.mode() == OverlayMode::Menu, "cancel discards without sending");
        keyboard(overlay); overlay.appendText("unsent"); overlay.back();
        require(overlay.text().empty() && overlay.selection() == 1, "controller cancel clears and restores menu item");
        keyboard(overlay); overlay.appendText("unsent"); overlay.close();
        require(overlay.text().empty(), "external close clears");
        overlay.openMenu();
        require(overlay.touch(-1, -1).type == OverlayActionType::None && overlay.mode() == OverlayMode::Menu, "outside touch ignored");
        overlay.touch(416, 258); require(overlay.mode() == OverlayMode::Keyboard, "menu touch opens keyboard");
        require(overlay.touch(143, 252).type == OverlayActionType::None && overlay.text().empty(), "key gap does not activate");
        overlay.appendText("unsent"); overlay.openMenu(); require(overlay.text().empty(), "opening menu discards unsent text");
        std::cout << "Stream overlay tests passed: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Stream overlay tests failed: " << error.what() << '\n';
        return 1;
    }
}

