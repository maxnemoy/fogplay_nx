#include "features/streaming/infrastructure/media.hpp"
#include <SDL.h>
extern "C" {
#include <libavutil/log.h>
}
#include <cassert>
#include <cstdarg>
#include <iostream>
#include <stdexcept>
using LogCallback = void (*)(void*, int, const char*, va_list);
LogCallback currentCallback = av_log_default_callback;
bool failAudioOpen = false;
unsigned callbackChanges = 0;
extern "C" void __real_av_log_set_callback(LogCallback);
extern "C" void __wrap_av_log_set_callback(LogCallback callback) {
    currentCallback = callback;
    ++callbackChanges;
    __real_av_log_set_callback(callback);
}
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char*, int,
    const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained, int) {
    *obtained = *wanted;
    return failAudioOpen ? 0 : 1;
}
extern "C" void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
extern "C" void __wrap_SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
int main() {
    {
        fog::Media first(nullptr);
        const auto custom = currentCallback;
        assert(custom && custom != av_log_default_callback && callbackChanges == 1);
        {
            fog::Media second(nullptr);
            assert(currentCallback == custom && callbackChanges == 1);
        }
        assert(currentCallback == custom && callbackChanges == 1);
        failAudioOpen = true;
        bool rejected = false;
        try { fog::Media failed(nullptr); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected && currentCallback == custom && callbackChanges == 1);
    }
    assert(currentCallback == av_log_default_callback && callbackChanges == 2);
    bool rejected = false;
    try { fog::Media failed(nullptr); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && currentCallback == av_log_default_callback && callbackChanges == 4);
    failAudioOpen = false;
    for (int i = 0; i < 20; ++i) {
        fog::Media media(nullptr);
        assert(currentCallback != av_log_default_callback);
    }
    assert(currentCallback == av_log_default_callback && callbackChanges == 44);
    std::cout << "Decoder callback lifecycle passed: overlapping owners, initialization failure, default restoration, 20 cycles\n";
}
