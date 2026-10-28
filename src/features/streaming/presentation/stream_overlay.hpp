#pragma once
#include "core/domain/identity.hpp"
#include "features/settings/domain/video_settings.hpp"
#include "features/settings/domain/rumble_settings.hpp"
#include <cstddef>
#include <string>

struct SDL_Renderer;
namespace fog::ui { class Ui; }

namespace fog::stream {
enum class OverlayMode { Closed, Menu, Mouse, Keyboard, ImageSettings, StopConfirm, RumbleSettings };
enum class OverlayActionType { None, SendText, Enter, Tab, Backspace, Escape, StopSession, SettingsChanged, RequestKeyframe, RumbleSettingsChanged };
struct OverlayAction {
    OverlayActionType type = OverlayActionType::None;
    std::string text;
};
// Main-thread state and drawing only: no applets, IO, media pumping or presentation.
// Controller directions are discrete/repeated presses supplied by the caller.
class Overlay {
public:
    static constexpr std::size_t MaxTextBytes = 512;
    static constexpr int KeyboardKeyCount = 59;
    OverlayMode mode() const { return mode_; }
    int selection() const { return selection_; }
    const std::string& text() const { return text_; }
    bool russian() const { return russian_; }
    bool uppercase() const { return uppercase_; }
    void openMenu();
    void close(); // Discards unsent local text.
    void up();
    void down();
    OverlayAction left();
    OverlayAction right();
    void setProvider(ProviderId value);
    void setVideoChangesEnabled(bool value) { videoChangesEnabled_ = value; }
    void setVideoSettings(const video::Settings& value) { videoSettings_ = value; }
    const video::Settings& videoSettings() const { return videoSettings_; }
    void setVideoStatus(const std::string& value) { videoStatus_ = value; }
    void setSaveStatus(const std::string& value) { saveStatus_ = value; }
    void setRumbleSettings(const rumble::Settings& value) { rumbleSettings_ = value; }
    const rumble::Settings& rumbleSettings() const { return rumbleSettings_; }
    void setRumbleStatus(const std::string& value) { rumbleStatus_ = value; }
    OverlayAction select();
    OverlayAction back();
    // Coordinates in the UI's logical 1280x720 space. Activates only hit buttons.
    OverlayAction touch(int x, int y);
    void paint(ui::Ui& ui, SDL_Renderer* renderer) const;
    // Atomic append: rejects malformed UTF-8, embedded control bytes and overflow.
    bool appendText(const std::string& value);
    void eraseLast();
private:
    void move(int horizontal, int vertical);
    OverlayAction adjustVideo(int direction);
    OverlayAction adjustRumble(int direction);
    int firstImageSelection() const;
    int imageRowCount() const;
    std::string keyLabel(int index) const;
    OverlayMode mode_ = OverlayMode::Closed;
    int selection_ = 0;
    bool russian_ = false;
    bool uppercase_ = false;
    std::string text_;
    video::Settings videoSettings_;
    rumble::Settings rumbleSettings_;
    ProviderId provider_ = ProviderId::Mts;
    bool videoChangesEnabled_ = true;
    std::string videoStatus_, saveStatus_;
    std::string rumbleStatus_;
};
}
