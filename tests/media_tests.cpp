#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/domain/media_annexb.hpp"
#include <SDL.h>
#include <opus/opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<unsigned char>;
using Units = std::vector<Bytes>;
static void check(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}
struct Parser {
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_H264);
    AVCodecContext* context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    ~Parser() { if (parser) av_parser_close(parser); avcodec_free_context(&context); }
};
static Units load(const char* path) {
    std::ifstream input(path, std::ios::binary);
    Bytes bytes((std::istreambuf_iterator<char>(input)), {});
    check(!bytes.empty(), "fixture empty");
    const auto length = bytes.size();
    bytes.resize(length + AV_INPUT_BUFFER_PADDING_SIZE);
    Parser parser;
    check(parser.parser && parser.context, "parser unavailable");
    Units units;
    std::size_t offset = 0;
    while (offset < length) {
        unsigned char* output = nullptr; int outputSize = 0;
        const int used = av_parser_parse2(parser.parser, parser.context, &output, &outputSize,
            bytes.data() + offset, static_cast<int>(length - offset), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        check(used >= 0 && (used || outputSize), "parser stalled");
        offset += used;
        if (outputSize) units.emplace_back(output, output + outputSize);
    }
    unsigned char* output = nullptr; int outputSize = 0;
    av_parser_parse2(parser.parser, parser.context, &output, &outputSize, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (outputSize) units.emplace_back(output, output + outputSize);
    check(units.size() >= 3, "fixture needs IDR and delta frames");
    return units;
}
static Bytes selectNals(const Bytes& input, bool parameters) {
    Bytes output;
    for (const auto& nal : fog::detail::inspectVideo(input.data(), input.size()).nals) {
        if ((nal.type == 7 || nal.type == 8) != parameters) continue;
        output.insert(output.end(), {0, 0, 0, 1});
        output.insert(output.end(), nal.data, nal.data + nal.size);
    }
    return output;
}
static Bytes onlySps(const Bytes& input) {
    Bytes output;
    for (const auto& nal : fog::detail::inspectVideo(input.data(), input.size()).nals) {
        if (nal.type != 7) continue;
        output.insert(output.end(), {0, 0, 0, 1});
        output.insert(output.end(), nal.data, nal.data + nal.size);
    }
    return output;
}
static void feed(fog::Media& media, const Bytes& bytes) {
    media.video(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
}
static void checkLetterbox(fog::Media& media, SDL_Renderer* renderer) {
    if (media.videoWidth() * 720 == media.videoHeight() * 1280) return;
    SDL_SetRenderDrawColor(renderer, 255, 0, 255, 255);
    SDL_RenderClear(renderer);
    check(media.draw(), "redraw missing");
    std::array<unsigned char, 4> pixel{};
    const SDL_Rect corner{0, 0, 1, 1};
    check(SDL_RenderReadPixels(renderer, &corner, SDL_PIXELFORMAT_RGBA32, pixel.data(), 4) == 0 &&
        !pixel[0] && !pixel[1] && !pixel[2], "overlay remnants in letterbox bars");
}
static Bytes opusPacket() {
    int error = 0;
    auto* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    check(encoder && !error, "Opus encoder missing");
    std::array<opus_int16, 960 * 2> silence{};
    Bytes encoded(4096);
    const int size = opus_encode(encoder, silence.data(), 960, encoded.data(), encoded.size());
    opus_encoder_destroy(encoder);
    check(size > 0, "Opus fixture encode failed");
    encoded.resize(size);
    return encoded;
}
static void test(fog::Media& media, SDL_Renderer* renderer, const Units& original, const Units& changed) {
    for (const auto& unit : original) feed(media, unit);
    check(media.videoFrames() >= 3 && media.draw(), "decoded video missing");
    check(media.diagnostics().find("frame_pixfmt=yuv420p cpu_pixfmt=yuv420p") != std::string::npos &&
        media.diagnostics().find("transfer_calls=0") != std::string::npos, "actual software formats/transfer diagnostics missing");
    check(media.videoWidth() > 0 && media.videoHeight() > 0, "decoded dimensions missing");
    checkLetterbox(media, renderer);
    const auto audio = opusPacket();
    media.audio(reinterpret_cast<const std::byte*>(audio.data()), audio.size());
    const int oldWidth = media.videoWidth(), oldHeight = media.videoHeight();
    auto before = media.videoFrames();
    media.resetVideoDecoder();
    check(media.waitingForVideoKeyframe(), "reset should await keyframe");
    check(media.videoFrames() == before && media.audioPackets() == 1 && media.draw() &&
        media.videoWidth() == oldWidth && media.videoHeight() == oldHeight, "reset lost last frame/audio");
    checkLetterbox(media, renderer);
    feed(media, original[1]);
    check(media.videoFrames() == before && media.waitingForVideoKeyframe(), "delta frame accepted after reset");
    const auto cachedIdr = selectNals(original[0], false);
    feed(media, cachedIdr);
    check(media.videoFrames() > before && !media.waitingForVideoKeyframe(), "cached parameter IDR recovery failed");
    feed(media, original[1]);
    check(media.videoFrames() > before + 1, "delta frame decode did not resume");

    // An incomplete slice fails with a real FFmpeg invalid-data result.
    bool corruptRejected = false;
    try { feed(media, {0, 0, 0, 1, 0x65}); }
    catch (const fog::VideoDecodeError& error) {
        const std::string message = error.what();
        check(error.code() == AVERROR_INVALIDDATA && error.recoverable(), "invalid-data classification wrong");
        check(message.size() < 2048 && message.find("code=") != std::string::npos &&
            message.find("packet_bytes=5") != std::string::npos &&
            message.find("nal_types=0x00000020") != std::string::npos &&
            message.find("width=") != std::string::npos && message.find("profile=") != std::string::npos &&
            message.find("Invalid data") != std::string::npos &&
            message.find("codec=h264") != std::string::npos && message.find("thread_active=") != std::string::npos &&
            message.find("send_calls=") != std::string::npos && message.find("nv_events=0 nv_status=0 nv_fresh=0") != std::string::npos, "safe FFmpeg diagnostics missing");
        corruptRejected = true;
        std::cout << "Corrupt AU diagnostics: " << message << '\n';
    }
    check(corruptRejected, "corrupt access unit accepted");
    before = media.videoFrames();
    media.resetVideoDecoder();
    feed(media, original[1]);
    check(media.videoFrames() == before && media.draw(), "corruption recovery lost frame or accepted delta");
    feed(media, cachedIdr);
    check(media.videoFrames() > before && !media.waitingForVideoKeyframe(), "corruption recovery failed");

    // Same SPS ID, changed resolution: old PPS must be invalidated until resent.
    before = media.videoFrames();
    media.resetVideoDecoder();
    feed(media, onlySps(changed[0]));
    feed(media, cachedIdr);
    check(media.videoFrames() == before && media.waitingForVideoKeyframe(), "changed SPS reused stale PPS");
    feed(media, changed[0]);
    check(media.videoFrames() > before && !media.waitingForVideoKeyframe(), "new resolution IDR failed");
    check(media.videoWidth() != oldWidth || media.videoHeight() != oldHeight, "resolution did not change");
    for (std::size_t i = 1; i < changed.size(); ++i) feed(media, changed[i]);
    checkLetterbox(media, renderer);

    // Oversized parameter set cannot be cached and must force fresh parameters.
    media.resetVideoDecoder();
    before = media.videoFrames();
    Bytes oversizedSps(17 * 1024, 0x55);
    oversizedSps[0] = 0; oversizedSps[1] = 0; oversizedSps[2] = 0; oversizedSps[3] = 1; oversizedSps[4] = 0x67;
    feed(media, oversizedSps);
    feed(media, selectNals(changed[0], false));
    check(media.videoFrames() == before && media.waitingForVideoKeyframe(), "oversized SPS reused cached pair");
    feed(media, selectNals(changed[0], true));
    feed(media, selectNals(changed[0], false));
    check(media.videoFrames() > before, "separate parameter packets failed to recover");

    // Repeated teardown checks ownership under ASan/LSan on the host.
    const auto changedIdr = selectNals(changed[0], false);
    for (unsigned i = 0; i < 32; ++i) {
        before = media.videoFrames();
        media.resetVideoDecoder();
        feed(media, changedIdr);
        check(media.videoFrames() > before && media.draw(), "repeated reset failed");
    }
    media.audio(reinterpret_cast<const std::byte*>(audio.data()), audio.size());
    check(media.audioPackets() == 2, "audio did not survive video resets");
    bool rejected = false;
    try { media.video(nullptr, 4 * 1024 * 1024 + 1); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "oversize access unit accepted");
    rejected = false;
    try { media.video(nullptr, 5); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "null access unit accepted");
    std::cout << "Media tests passed: H264 frames=" << media.videoFrames()
        << ", Opus packets=2, retained frame/audio, corrupt-stream recovery, stale PPS rejected, resolution change, 32 resets\n";
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 3;
    auto* window = SDL_CreateWindow("media test", 0, 0, 1280, 720, 0);
    auto* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); return 4; }
    int result = 0;
    try {
        fog::Media media(renderer);
        test(media, renderer, load(argv[1]), load(argv[2]));
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return result;
}
