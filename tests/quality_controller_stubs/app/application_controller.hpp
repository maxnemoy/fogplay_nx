#pragma once
#include "player_adapter_fixture.hpp"
#include "core/constants/ui_text.hpp"
#include "features/settings/infrastructure/deferred_settings.hpp"

namespace fogplay {
inline void diagnostic(const std::string&) {}
struct TestOverlay {
    fog::video::Settings value;
    std::string saved;
    void setVideoSettings(const fog::video::Settings& settings) { value = settings; }
    void setSaveStatus(const std::string& status) { saved = status; }
};
// Only dependencies consumed by real video_settings_controller.cpp. All
// playback decisions go through the actual Player owned by the shared fixture.
class ApplicationController : public player_adapter_test::Fixture {
public:
    explicit ApplicationController(std::chrono::milliseconds delay = std::chrono::milliseconds(0),
        fog::ProviderId provider = fog::ProviderId::FogHub, int fps = 30)
        : settingsSaves_(delay) {
        videoSettings_.framesPerSecond = fps;
        begin(provider,fps);
    }
    void changeVideoSettings(const fog::video::Settings&,bool);
    fog::stream::Player::Admission playerAdmission() const { return currentAdmission(); }
    void applyPlayerEffects() { effects(); }
    fog::stream::Player& player_ = owner;
    fog::video::Settings videoSettings_;
    TestOverlay overlay_;
    std::string videoSaveStatus_;
    // Destroy first: completion callbacks still have their overlay/status owner.
    fog::settings::DeferredSettings settingsSaves_;
};
}