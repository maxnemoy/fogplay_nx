#include "features/streaming/infrastructure/media.hpp"
#include <SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <cassert>
#include <iostream>
static bool failOpen = false;
extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" int __wrap_avcodec_open2(AVCodecContext* c, const AVCodec* codec, AVDictionary** options) {
    return failOpen ? AVERROR(ENOMEM) : __real_avcodec_open2(c, codec, options);
}
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char*, int,
    const SDL_AudioSpec* wanted, SDL_AudioSpec* obtained, int) { *obtained = *wanted; return 1; }
extern "C" void __wrap_SDL_CloseAudioDevice(SDL_AudioDeviceID) {}
extern "C" void __wrap_SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
int main() {
    for (const bool codecChange : {false, true}) {
        fog::Media media(nullptr);
        failOpen = true;
        bool failed = false;
        try {
            if (codecChange) media.setVideoCodec(stream::VideoCodec::H265);
            else media.resetVideoDecoder();
        } catch (const fog::VideoDecodeError& e) { failed = !e.recoverable(); }
        assert(failed && media.waitingForVideoKeyframe());
        const std::byte au[]{std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{0x65},std::byte{0x80}};
        failed = false;
        try { media.video(au,sizeof(au)); }
        catch (const fog::VideoDecodeError& e) { failed = !e.recoverable() && e.code() == AVERROR(EINVAL); }
        assert(failed);
        failed = false;
        try { media.preparePresentation(); }
        catch (const fog::VideoDecodeError& e) { failed = !e.recoverable() && e.code() == AVERROR(EINVAL); }
        assert(failed && media.videoFrames() == 0 && !media.draw());
        assert(media.diagnostics().find("codec=none") != std::string::npos);
        failOpen = false;
        media.resetVideoDecoder();
        assert(media.waitingForVideoKeyframe() && !media.preparePresentation());
    }
    std::cout << "Media failed reset/codec switch: guarded decode/presentation, retained diagnostics, retry and destruction PASS\n";
}
