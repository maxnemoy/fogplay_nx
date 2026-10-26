#pragma once
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <optional>
#include <string>

namespace fogplay::platform {
inline constexpr const char* dataDir = "sdmc:/switch/fogplay_nx";
void checkResult(Result rc, const char* operation);
void checkStorage();
std::string deviceId();
std::optional<std::string> keyboard(const char* title, const std::string& initial, int maximum, bool password = false);
void fatalScreen(const std::string& message);

struct Runtime {
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void initGraphics();
    void initServices();
    void showStartup(const char* message);
    ~Runtime();
    bool romfs = false, sockets = false, nifm = false, pl = false, sdl = false, ttf = false, image = false;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* titleFont = nullptr;
};
}
