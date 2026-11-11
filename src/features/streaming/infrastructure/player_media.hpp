#pragma once
#include "features/streaming/application/player_media.hpp"
#include <functional>
struct SDL_Renderer;
namespace fog::stream {
// Composition supplies the renderer and watchdog destination once. The Player
// and its media port never receive platform rendering or logger ownership.
class SdlPlayerMediaFactory final : public PlayerMediaFactory {
public:
    explicit SdlPlayerMediaFactory(SDL_Renderer*, std::function<void(const std::string&)> watchdog = {});
    SdlPlayerMediaFactory(SDL_Renderer*,std::function<void(const ::fog::media::telemetry::StallEvent&)>);
    std::unique_ptr<PlayerMedia> create() override;
    bool supportsH265() const override;
private:
    SDL_Renderer* renderer_;
    std::function<void(const std::string&)> watchdog_;
    std::function<void(const ::fog::media::telemetry::StallEvent&)> typedWatchdog_;
};
}
