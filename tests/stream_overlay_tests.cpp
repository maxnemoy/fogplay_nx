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
void rumbleSettings() {
    using namespace fog::stream;
    using RumbleMode = fog::rumble::Mode;
    for (const auto provider : {fog::ProviderId::Mts, fog::ProviderId::FogHub}) {
        Overlay overlay;
        overlay.setProvider(provider);
        overlay.openMenu(); overlay.up();
        require(overlay.selection() == 5, "menu wraps to added rumble item");
        require(overlay.select().type == OverlayActionType::None &&
            overlay.mode() == OverlayMode::RumbleSettings && overlay.selection() == 0,
            "rumble opens without changing or sending remote controls");
        require(overlay.rumbleSettings().mode == RumbleMode::Balanced, "rumble defaults to balanced");
        require(overlay.left().type == OverlayActionType::RumbleSettingsChanged &&
            overlay.rumbleSettings().mode == RumbleMode::Disabled, "rumble left wraps to disabled");
        require(overlay.right().type == OverlayActionType::RumbleSettingsChanged &&
            overlay.rumbleSettings().mode == RumbleMode::Balanced, "rumble right wraps to balanced");
        require(overlay.select().type == OverlayActionType::RumbleSettingsChanged &&
            overlay.rumbleSettings().mode == RumbleMode::DualBand, "rumble A selects dual band");
        require(overlay.right().type == OverlayActionType::RumbleSettingsChanged &&
            overlay.rumbleSettings().mode == RumbleMode::Chromium, "rumble right selects Chromium");
        require(overlay.touch(200, 230).type == OverlayActionType::RumbleSettingsChanged &&
            overlay.rumbleSettings().mode == RumbleMode::Disabled, "rumble touch selects disabled");
        overlay.down();
        require(overlay.selection() == 1 && overlay.left().type == OverlayActionType::None &&
            overlay.right().type == OverlayActionType::None && overlay.rumbleSettings().mode == RumbleMode::Disabled,
            "rumble back row does not modify settings with directions");
        overlay.up();
        require(overlay.selection() == 0, "rumble vertical navigation returns to mode row");
        require(overlay.touch(200, 400).type == OverlayActionType::None &&
            overlay.rumbleSettings().mode == RumbleMode::Disabled, "rumble explanatory text ignores touch");
        overlay.back();
        require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 5,
            "rumble B returns to originating menu item");
        overlay.select();
        require(overlay.rumbleSettings().mode == RumbleMode::Disabled, "reopening rumble preserves selected mode");
        overlay.touch(200, 575);
        require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 5,
            "rumble touch back returns to originating menu item");
        overlay.close(); overlay.openMenu();
        require(overlay.rumbleSettings().mode == RumbleMode::Disabled, "closing stream menu preserves disabled mode");
        overlay.touch(416, 510);
        require(overlay.mode() == OverlayMode::RumbleSettings, "sixth menu item accepts touch");
        overlay.setRumbleSettings({RumbleMode::Balanced});
        overlay.setRumbleStatus("save failed");
        require(overlay.rumbleSettings().mode == RumbleMode::Balanced,
            "application can roll back setting after failed persistence");
        overlay.setProvider(provider == fog::ProviderId::Mts ? fog::ProviderId::FogHub : fog::ProviderId::Mts);
        require(overlay.mode() == OverlayMode::RumbleSettings && overlay.selection() == 0 &&
            overlay.rumbleSettings().mode == RumbleMode::Balanced, "rumble settings are independent of stream provider");
        overlay.down(); overlay.select();
        require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 5,
            "rumble A on back returns to menu");
        for (int i = 0; i < 30; ++i) {
            overlay.down();
            require(overlay.selection() >= 0 && overlay.selection() < 6, "six item menu navigation remains bounded");
        }
    }
}
void imageSettings(fog::stream::Overlay& overlay) {
    using namespace fog::stream;
    overlay.openMenu(); overlay.down(); overlay.down(); overlay.select();
    require(overlay.mode() == OverlayMode::ImageSettings && overlay.selection() == 1, "MTS image opens on editable quality");
    require(overlay.videoSettings().codec == ::stream::VideoCodec::H265, "fresh overlay defaults to H265");
    require(overlay.left().type == OverlayActionType::None && overlay.mode() == OverlayMode::QualitySelect &&
        overlay.selection() == 0 && overlay.videoSettings().quality == 0, "MTS quality left opens picker without applying");
    require(overlay.left().type == OverlayActionType::None && overlay.selection() == 4 &&
        overlay.videoSettings().quality == 0, "quality backward wrap only changes candidate");
    require(overlay.select().type == OverlayActionType::SettingsChanged && overlay.videoSettings().quality == 4,
        "quality applies once after confirmation");
    require(overlay.right().type == OverlayActionType::None && overlay.mode() == OverlayMode::QualitySelect &&
        overlay.selection() == 4, "quality right reopens current value");
    require(overlay.right().type == OverlayActionType::None && overlay.selection() == 0 &&
        overlay.select().type == OverlayActionType::SettingsChanged && overlay.videoSettings().quality == 0,
        "quality wraps to auto and applies only after confirmation");
    overlay.down();
    require(overlay.select().type == OverlayActionType::SettingsChanged && overlay.videoSettings().bitrateMbps == 30,
        "MTS bitrate adjustment");
    overlay.down();
    require(overlay.right().type == OverlayActionType::SettingsChanged && overlay.videoSettings().keyframeSeconds == 2,
        "MTS periodic keyframe adjustment");
    overlay.down();
    require(overlay.selection() == 4 && overlay.videoSettings().framesPerSecond == 30, "MTS exposes shared FPS row with thirty default");
    require(overlay.right().type == OverlayActionType::SettingsChanged && overlay.videoSettings().framesPerSecond == 60,
        "MTS frame rate selects sixty for next connection");
    require(overlay.left().type == OverlayActionType::SettingsChanged && overlay.videoSettings().framesPerSecond == 30,
        "MTS frame rate returns to thirty");
    require(overlay.touch(200,142 + 4*54 + 8).type == OverlayActionType::SettingsChanged &&
        overlay.videoSettings().framesPerSecond == 60, "MTS FPS touch selects sixty");
    overlay.down();
    require(overlay.select().type == OverlayActionType::RequestKeyframe, "MTS manual keyframe after FPS row");
    overlay.down(); overlay.down();
    require(overlay.selection() == 1, "MTS live navigation skips immutable codec");
    const auto prior = overlay.videoSettings();
    require(overlay.touch(200,170).type == OverlayActionType::None && overlay.videoSettings() == prior,
        "MTS codec touch ignored during stream");
    for (int i = 0; i < 50; ++i) {
        overlay.up(); require(overlay.selection() >= 1 && overlay.selection() <= 6, "MTS image navigation bounded over seven rows");
    }
    overlay.back();
    require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 2, "image B restores menu item");
    overlay.select(); overlay.touch(200,142 + 6*54 + 8);
    require(overlay.mode() == OverlayMode::Menu && overlay.selection() == 2, "MTS touch back fits screen and restores menu item");

    Overlay foghub; foghub.setProvider(fog::ProviderId::FogHub);
    foghub.openMenu(); foghub.down(); foghub.down(); foghub.select();
    require(foghub.mode() == OverlayMode::ImageSettings && foghub.selection() == 0, "FH image opens on editable codec");
    const auto fhInitial = foghub.videoSettings();
    foghub.setVideoChangesEnabled(false);
    require(foghub.left().type == OverlayActionType::None && foghub.videoSettings() == fhInitial,
        "FH connecting disables codec");
    require(foghub.touch(200,142 + 54 + 8).type == OverlayActionType::None && foghub.videoSettings() == fhInitial,
        "FH connecting disables bitrate");
    require(foghub.touch(200,142 + 2*54 + 8).type == OverlayActionType::SettingsChanged &&
        foghub.videoSettings().framesPerSecond == 60, "FH connecting allows FPS save for next connection");
    require(foghub.touch(200,142 + 3*54 + 8).type == OverlayActionType::RequestKeyframe,
        "FH connecting retains explicit keyframe action");
    foghub.touch(200,142 + 4*54 + 8);
    require(foghub.mode() == OverlayMode::Menu && foghub.selection() == 2, "FH connecting retains Back");
    foghub.select(); foghub.setVideoChangesEnabled(true);
    require(foghub.touch(200,170).type == OverlayActionType::SettingsChanged &&
        foghub.videoSettings().codec == ::stream::VideoCodec::H264, "FH codec accepts touch");
    require(foghub.right().type == OverlayActionType::SettingsChanged && foghub.videoSettings().codec == fhInitial.codec,
        "FH codec toggles back");
    foghub.down(); foghub.setProvider(fog::ProviderId::FogHub);
    require(foghub.selection() == 1, "unchanged provider preserves selected field");
    require(foghub.select().type == OverlayActionType::SettingsChanged && foghub.videoSettings().bitrateMbps == 30,
        "FH bitrate editable");
    require(foghub.videoSettings().quality == fhInitial.quality && foghub.videoSettings().keyframeSeconds == fhInitial.keyframeSeconds,
        "FH adjustments preserve MTS-only preferences");
    foghub.down();
    require(foghub.select().type == OverlayActionType::SettingsChanged && foghub.videoSettings().framesPerSecond == 30,
        "FH FPS row changes using A");
    foghub.down();
    require(foghub.select().type == OverlayActionType::RequestKeyframe, "FH manual keyframe after FPS row");
    foghub.down(); foghub.down();
    require(foghub.selection() == 0, "FH image navigation wraps over five rows");
    require(foghub.touch(200,430).type == OverlayActionType::None && foghub.mode() == OverlayMode::ImageSettings,
        "FH unused lower rows do not accept touch");
    for (int i = 0; i < 20; ++i) {
        foghub.up(); require(foghub.selection() >= 0 && foghub.selection() <= 4, "FH image navigation bounded");
    }
    foghub.touch(200,142 + 4*54 + 8);
    require(foghub.mode() == OverlayMode::Menu && foghub.selection() == 2, "FH touch Back restores image menu item");
    foghub.select(); foghub.setVideoChangesEnabled(false); foghub.setProvider(fog::ProviderId::Mts);
    require(foghub.selection() == 1, "provider switch resets to MTS editable quality");
    require(foghub.left().type == OverlayActionType::None && foghub.mode() == OverlayMode::QualitySelect &&
        foghub.videoSettings().quality == fhInitial.quality, "provider switch restores explicit MTS quality picker");
    foghub.setProvider(fog::ProviderId::FogHub);
    require(foghub.mode() == OverlayMode::ImageSettings && foghub.selection() == 0 &&
        foghub.videoSettings().quality == fhInitial.quality, "provider switch cancels picker and resets to FH codec");
    foghub.back();
    require(foghub.mode() == OverlayMode::Menu && foghub.selection() == 2, "FH controller Back restores menu item");
}
void qualityPicker() {
    using namespace fog::stream;
    fog::video::Settings initial;
    initial.quality = 3;
    initial.bitrateMbps = 15;
    initial.keyframeSeconds = 5;
    initial.framesPerSecond = 60;
    initial.codec = ::stream::VideoCodec::H264;
    for (int entry = 0; entry < 4; ++entry) {
        Overlay overlay; overlay.setVideoSettings(initial);
        overlay.openMenu(); overlay.down(); overlay.down(); overlay.select();
        const auto action = entry == 0 ? overlay.select() : entry == 1 ? overlay.left() :
            entry == 2 ? overlay.right() : overlay.touch(200, 204);
        require(action.type == OverlayActionType::None && overlay.mode() == OverlayMode::QualitySelect &&
            overlay.selection() == 3 && overlay.videoSettings() == initial, "every quality entry opens HIGH without action");
        overlay.down();
        require(overlay.selection() == 4 && overlay.videoSettings() == initial, "HIGH to ULTRA navigation does not apply");
        require(overlay.right().type == OverlayActionType::None && overlay.selection() == 0 &&
            overlay.videoSettings() == initial, "ULTRA to AUTO navigation does not apply");
        overlay.up();
        require(overlay.selection() == 4 && overlay.left().type == OverlayActionType::None &&
            overlay.selection() == 3 && overlay.videoSettings() == initial, "all directions only move candidate");
        overlay.down(); overlay.right();
        require(overlay.select().type == OverlayActionType::SettingsChanged && overlay.mode() == OverlayMode::ImageSettings &&
            overlay.selection() == 1, "AUTO confirmation emits one change and returns to quality row");
        auto expected = initial; expected.quality = 0;
        require(overlay.videoSettings() == expected, "quality confirmation preserves every other setting");
        require(overlay.select().type == OverlayActionType::None && overlay.select().type == OverlayActionType::None &&
            overlay.videoSettings() == expected, "reconfirming current quality emits no redundant command");
    }
    for (int target = 0; target < 5; ++target) {
        Overlay overlay; overlay.setVideoSettings(initial);
        overlay.openMenu(); overlay.down(); overlay.down(); overlay.select(); overlay.select();
        require(overlay.touch(0, 0).type == OverlayActionType::None && overlay.videoSettings() == initial &&
            overlay.mode() == OverlayMode::QualitySelect, "outside quality touch ignored");
        const auto action = overlay.touch(340, 238 + target * 64);
        require(action.type == (target == initial.quality ? OverlayActionType::None : OverlayActionType::SettingsChanged) &&
            overlay.videoSettings().quality == target && overlay.mode() == OverlayMode::ImageSettings && overlay.selection() == 1,
            "touch commits the exact quality option without cycling");
    }
    for (int escape = 0; escape < 5; ++escape) {
        Overlay overlay; overlay.setVideoSettings(initial);
        overlay.openMenu(); overlay.down(); overlay.down(); overlay.select(); overlay.select(); overlay.right();
        require(overlay.selection() == 4 && overlay.videoSettings() == initial, "cancel fixture has unapplied ULTRA candidate");
        if (escape == 0) require(overlay.back().type == OverlayActionType::None &&
            overlay.mode() == OverlayMode::ImageSettings && overlay.selection() == 1, "B cancels to image quality row");
        else if (escape == 1) require(overlay.touch(340, 590).type == OverlayActionType::None &&
            overlay.mode() == OverlayMode::ImageSettings, "touch cancel preserves quality");
        else if (escape == 2) { overlay.close(); require(overlay.mode() == OverlayMode::Closed, "close discards picker candidate"); }
        else if (escape == 3) { overlay.openMenu(); require(overlay.mode() == OverlayMode::Menu, "menu chord discards picker candidate"); }
        else {
            overlay.setProvider(fog::ProviderId::FogHub);
            require(overlay.mode() == OverlayMode::ImageSettings && overlay.selection() == 0,
                "provider change cancels pending MTS quality before any FH action");
            overlay.setProvider(fog::ProviderId::Mts);
            overlay.select();
            require(overlay.mode() == OverlayMode::QualitySelect && overlay.selection() == 3,
                "return to MTS starts from committed quality rather than stale candidate");
        }
        require(overlay.videoSettings() == initial, "every cancel path preserves original full settings");
    }
}
}
int main() {
    using namespace fog::stream;
    try {
        qualityPicker();
        Overlay overlay;
        require(overlay.mode() == OverlayMode::Closed, "initially closed");
        require(!overlay.appendText("outside"), "only keyboard accepts text");
        rumbleSettings();
        overlay.openMenu(); overlay.up(); overlay.up();
        require(overlay.selection() == 4, "close menu retains its original index");
        overlay.select(); require(overlay.mode() == OverlayMode::Closed, "close menu item");
        overlay.openMenu(); overlay.select();
        require(overlay.mode() == OverlayMode::Mouse, "mouse menu item");
        overlay.back(); require(overlay.mode() == OverlayMode::Menu, "mouse back enters menu");
        overlay.down(); overlay.down(); overlay.down(); overlay.select();
        require(overlay.mode() == OverlayMode::StopConfirm && overlay.selection() == 1, "stop defaults to continue");
        require(overlay.select().type == OverlayActionType::None && overlay.mode() == OverlayMode::Menu, "second A cannot end session");
        overlay.select(); overlay.left();
        require(overlay.select().type == OverlayActionType::StopSession && overlay.mode() == OverlayMode::Closed, "confirmed stop action");
        imageSettings(overlay);
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

