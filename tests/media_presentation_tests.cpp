#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/domain/media_annexb.hpp"
#include "features/streaming/domain/audio_queue_metrics.hpp"
#include "features/streaming/infrastructure/video_image_diagnostics.hpp"
#include <SDL.h>
#include <opus/opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/buffer.h>
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

extern "C" SDL_Texture* __real_SDL_CreateTexture(SDL_Renderer*, Uint32, int, int, int);
extern "C" int __real_SDL_UpdateYUVTexture(SDL_Texture*, const SDL_Rect*, const Uint8*, int, const Uint8*, int, const Uint8*, int);
extern "C" int __real_avcodec_receive_frame(AVCodecContext*, AVFrame*);
extern "C" int __real_av_hwframe_transfer_data(AVFrame*, const AVFrame*, int);
extern "C" int __real_SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
extern "C" Uint32 __real_SDL_GetQueuedAudioSize(SDL_AudioDeviceID);
extern "C" int __real_SDL_QueueAudio(SDL_AudioDeviceID, const void*, Uint32);
extern "C" void __real_SDL_ClearQueuedAudio(SDL_AudioDeviceID);
static bool failCreation = false, failUpload = false, emulateHardwareFrame = false;
static bool failTransfer = false, failDraw = false, mockAudio = false, failQueue = false;
static Uint32 audioBytes = 0;
static unsigned audioClears = 0;
extern "C" int __wrap_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture,
    const SDL_Rect* source, const SDL_Rect* destination) {
    if (failDraw) { failDraw = false; return -1; }
    return __real_SDL_RenderCopy(renderer, texture, source, destination);
}
extern "C" Uint32 __wrap_SDL_GetQueuedAudioSize(SDL_AudioDeviceID device) {
    return mockAudio ? audioBytes : __real_SDL_GetQueuedAudioSize(device);
}
extern "C" int __wrap_SDL_QueueAudio(SDL_AudioDeviceID device, const void* bytes, Uint32 size) {
    if (!mockAudio) return __real_SDL_QueueAudio(device, bytes, size);
    if (failQueue) { failQueue = false; return -1; }
    audioBytes += size;
    return 0;
}
extern "C" void __wrap_SDL_ClearQueuedAudio(SDL_AudioDeviceID device) {
    if (!mockAudio) { __real_SDL_ClearQueuedAudio(device); return; }
    ++audioClears; audioBytes = 0;
}
extern "C" int __wrap_avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    const int result = __real_avcodec_receive_frame(context, frame);
    if (result == 0 && emulateHardwareFrame) {
        emulateHardwareFrame = false;
        // A reference-counted marker exercises dispatch/ownership only, not a GPU.
        frame->hw_frames_ctx = av_buffer_alloc(1);
        if (!frame->hw_frames_ctx) return AVERROR(ENOMEM);
    }
    return result;
}
extern "C" int __wrap_av_hwframe_transfer_data(AVFrame* destination, const AVFrame* source, int flags) {
    if (failTransfer) { failTransfer = false; return AVERROR_EXTERNAL; }
    return __real_av_hwframe_transfer_data(destination, source, flags);
}
extern "C" SDL_Texture* __wrap_SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h) {
    if (failCreation) { failCreation = false; return nullptr; }
    return __real_SDL_CreateTexture(renderer, format, access, w, h);
}
extern "C" int __wrap_SDL_UpdateYUVTexture(SDL_Texture* texture, const SDL_Rect* rect,
    const Uint8* y, int ys, const Uint8* u, int us, const Uint8* v, int vs) {
    if (failUpload) { failUpload = false; return -1; }
    return __real_SDL_UpdateYUVTexture(texture, rect, y, ys, u, us, v, vs);
}
static void feed(fog::Media& media, const Bytes& bytes) {
    media.video(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
}
static std::uint64_t counter(const fog::Media& media, const char* name) {
    const auto text = media.diagnostics();
    const std::string key = std::string(name) + '=';
    const auto pos = text.find(key);
    check(pos != std::string::npos, "missing presentation counter");
    return std::stoull(text.substr(pos + key.size()));
}
static Bytes snapshot(fog::Media& media, SDL_Renderer* renderer) {
    check(media.draw(), "last texture unavailable");
    Bytes pixels(1280 * 720 * 4);
    check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(), 1280 * 4) == 0,
        "renderer readback failed");
    return pixels;
}
static void present(fog::Media& media, SDL_Renderer* renderer) {
    check(media.draw(), "presentation draw unavailable");
    SDL_RenderPresent(renderer);
    media.presented();
}
static void imageSamples() {
    using Sample = fog::detail::VideoImageSample;
    // Odd-sized, tightly packed IYUV planes exercise chroma offsets and edges.
    Bytes pixels(15 + 2 * 6, 128);
    std::fill(pixels.begin(), pixels.begin() + 15, 16);
    const auto black = Sample::inspect(pixels.data(), pixels.size(), 5, 3);
    check(black.points == 15 && black.nearBlack() && black.yMean == 16,
        "limited-range black/odd IYUV sample incorrect");
    std::fill(pixels.begin(), pixels.begin() + 15, 0);
    check(Sample::inspect(pixels.data(), pixels.size(), 5, 3).nearBlack(), "full-range black missed");
    pixels[14] = 235;
    check(!Sample::inspect(pixels.data(), pixels.size(), 5, 3).nearBlack(), "bright sample classified black");
    pixels[14] = 16; pixels.back() = 255;
    check(!Sample::inspect(pixels.data(), pixels.size(), 5, 3).nearBlack(), "low-luma saturated color classified black");
    check(!Sample::inspect(pixels.data(), pixels.size()-1, 5, 3).points &&
        !Sample::inspect(nullptr, 0, 5, 3).points && !Sample::inspect(pixels.data(), pixels.size(), 0, 3).points,
        "invalid image read by sampler");
    fog::detail::VideoImageDiagnostics stats;
    stats.uploaded(black, 5, 3); stats.uploaded(black, 5, 3);
    check(stats.blackRuns == 1 && stats.blackStreak == 2 && stats.unchangedStreak == 1, "black/unchanged run lost");
    stats.uploaded(black, 3, 5);
    check(stats.blackRuns == 2 && stats.blackStreak == 1 && stats.unchangedStreak == 0, "size change kept stale run");
}
static void blackImageMeasurements(SDL_Renderer* renderer, const Units& black, const Units& color) {
    fog::Media media(renderer, stream::VideoCodec::H264, true);
    check(counter(media, "image_samples") == 0, "unprimed texture counted as black sample");
    feed(media, black.front());
    check(media.preparePresentation(), "black fixture upload missing");
    check(counter(media, "image_near_black") == 1 && counter(media, "image_points") == 576 &&
        counter(media, "image_samples") == 1 && counter(media, "image_presented_black_frames") == 0,
        "decoded black image not sampled or prematurely presented");
    present(media, renderer); present(media, renderer);
    check(counter(media, "image_presented_black_frames") == 1 && counter(media, "image_presented_generation") == 1,
        "black presentation counted redraw twice");
    feed(media, black[1]);
    failUpload = true;
    bool failed = false;
    try { media.preparePresentation(); } catch (const std::runtime_error&) { failed = true; }
    check(failed && counter(media, "image_upload_errors") == 1 && counter(media, "image_samples") == 1,
        "failed upload changed observed texture");
    check(media.preparePresentation() && counter(media, "image_black_streak") == 2 &&
        counter(media, "image_unchanged_streak") == 1, "repeat black frame streak missing");
    feed(media, color.front());
    check(media.preparePresentation() && counter(media, "image_near_black") == 0 &&
        counter(media, "image_black_streak") == 0 && counter(media, "image_black_streak_max") == 2 &&
        counter(media, "image_black_runs") == 1, "black recovery lost historical evidence");
    failDraw = true;
    failed = false;
    try { media.draw(); } catch (const std::runtime_error&) { failed = true; }
    media.presented();
    check(failed && counter(media, "image_copy_errors") == 1 && counter(media, "image_presented_black") == 1,
        "failed draw acknowledged new color image");
    present(media, renderer);
    check(counter(media, "image_presented_black") == 0 && counter(media, "image_presented_generation") == 3,
        "color presentation did not update image evidence");
    check(media.diagnostics().size() < 4000 && media.imageDiagnostics().find("image_sample_max_us=") != std::string::npos,
        "image diagnostics exceed periodic log budget or lose sampling timing");
    std::cout << "PASS image diagnostics: decoded black/color, repeats, failed upload/draw, retained texture, recovery\n";
}
static void presentationCounts(SDL_Renderer* renderer, const Units& units) {
    fog::Media media(renderer, stream::VideoCodec::H264, true);
    media.presented();
    check(!media.draw() && media.presentedFrames() == 0, "empty present counted");
    feed(media, units.front());
    check(media.preparePresentation() && media.uploadedFrames() == 1, "new output upload missing");
    media.presented();
    check(media.presentedFrames() == 0, "upload without draw counted as present");
    check(media.draw() && media.presentedFrames() == 0, "draw counted before RenderPresent");
    SDL_RenderPresent(renderer);
    media.presented(); media.presented();
    present(media, renderer);
    check(media.presentedFrames() == 1, "repeated present counted same generation twice");
    feed(media, units[1]);
    check(media.preparePresentation() && media.uploadedFrames() == 2, "next generation upload missing");
    media.presented();
    check(media.presentedFrames() == 1, "new upload reused preceding draw");
    // A failed draw must invalidate a preceding successful draw token too.
    check(media.draw(), "pre-failure draw missing");
    failDraw = true;
    bool rejected = false;
    try { media.draw(); } catch (const std::runtime_error&) { rejected = true; }
    media.presented();
    check(rejected && media.presentedFrames() == 1, "failed draw counted a new generation");
    present(media, renderer);
    check(media.presentedFrames() == 2, "successful redraw after failure not counted");
    media.resetVideoDecoder();
    present(media, renderer);
    check(media.presentedFrames() == 2, "retained recovery texture counted twice");
    feed(media, units.front());
    check(media.preparePresentation(), "recovery generation unavailable");
    present(media, renderer);
    check(media.presentedFrames() == 3 && media.videoFrames() == 3 && media.uploadedFrames() == 3 &&
        media.supersededFrames() == 0, "decoded/uploaded/presented recovery counters diverged");
    check(media.draw(), "preceding generation draw missing");
    feed(media, units[1]);
    check(media.preparePresentation(), "intervening generation unavailable");
    media.presented();
    check(media.presentedFrames() == 3, "intervening upload reused stale draw acknowledgement");
    present(media, renderer);
    check(media.presentedFrames() == 4, "intervening generation fresh draw not counted");
    std::cout << "PASS new-generation presentation: draw/present separation, repeats, draw failure, decoder recovery\n";
}
static void audioObservations() {
    using Metrics = fog::detail::AudioQueueMetrics;
    using namespace std::chrono_literals;
    Metrics metrics;
    const auto start = Metrics::Clock::time_point{};
    metrics.observe(0, start);
    metrics.observe(0, start + 1s);
    check(metrics.emptyObservations == 0 && metrics.prolongedEmptyObservations == 0,
        "unprimed audio counted empty output");
    metrics.queued(3840, start + 1s);
    metrics.observe(0, start + 2s);
    metrics.observe(0, start + 2039ms);
    check(metrics.emptyObservations == 1 && metrics.prolongedEmptyObservations == 0,
        "brief hardware queue transfer counted as prolonged empty");
    metrics.observe(0, start + 2040ms);
    metrics.observe(0, start + 3s);
    check(metrics.emptyObservations == 1 && metrics.prolongedEmptyObservations == 1,
        "prolonged empty episode was missed or counted repeatedly");
    metrics.queued(1920, start + 3s);
    metrics.observe(0, start + 3010ms);
    metrics.queued(1920, start + 3020ms);
    metrics.observe(0, start + 3030ms);
    metrics.observe(0, start + 3070ms);
    check(metrics.emptyObservations == 3 && metrics.prolongedEmptyObservations == 2,
        "queue refill did not separate empty episodes");
    metrics.cleared(42240);
    metrics.observe(0, start + 4s);
    metrics.observe(0, start + 5s);
    check(metrics.resets == 1 && Metrics::milliseconds(metrics.discardedBytes) == 220 &&
        metrics.emptyObservations == 3 && metrics.prolongedEmptyObservations == 2,
        "intentional queue reset counted an underrun or lost discarded duration");
}
static Bytes opusPacket() {
    int error = 0;
    auto* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    check(encoder && !error, "Opus encoder unavailable");
    std::array<opus_int16, 960 * 2> pcm{};
    Bytes encoded(4096);
    const int size = opus_encode(encoder, pcm.data(), 960, encoded.data(), encoded.size());
    opus_encoder_destroy(encoder);
    check(size > 0, "Opus fixture encode failed");
    encoded.resize(size);
    return encoded;
}
static void audioCounts(SDL_Renderer* renderer) {
    mockAudio = true; audioBytes = 0; audioClears = 0;
    fog::Media media(renderer);
    const auto packet = opusPacket();
    const auto* bytes = reinterpret_cast<const std::byte*>(packet.data());
    check(media.audioUnderruns() == 0 && media.audioQueueResets() == 0, "initial audio counters not empty");
    media.audio(bytes, packet.size());
    check(media.audioPackets() == 1 && audioBytes == 3840 &&
        media.audioDiagnostics().find("audio_queued_ms=20") != std::string::npos, "queued duration not observed");
    audioBytes = 38400;
    media.audio(bytes, packet.size());
    check(media.audioQueueResets() == 0 && audioBytes == 42240, "200 ms boundary policy changed");
    media.audio(bytes, packet.size());
    check(media.audioQueueResets() == 1 && audioClears == 1 && audioBytes == 3840 &&
        media.audioDiagnostics().find("audio_reset_discarded_ms=220") != std::string::npos,
        "stale queue clear not counted or bounded");
    audioBytes = 0;
    check(media.audioUnderruns() == 0 &&
        media.audioDiagnostics().find("audio_empty_observations=1") != std::string::npos,
        "newly observed empty queue treated as device underrun");
    check(media.audioUnderruns() == 0, "same empty sample immediately counted prolonged");
    media.audio(bytes, packet.size());
    check(media.audioPackets() == 4 && media.audioQueueResets() == 1, "refill lost packet/reset counts");
    failQueue = true;
    bool rejected = false;
    try { media.audio(bytes, packet.size()); } catch (const std::runtime_error&) { rejected = true; }
    // audioPackets counts successful Opus decoding, including suppressed PCM;
    // queue submission is a later stage and must not be mistaken for delivery.
    check(rejected && media.audioPackets() == 5 && audioBytes == 3840,
        "failed queue lost decoded packet count or submitted extra audio");
    std::cout << "PASS audio observations: no sleeps, 40 ms threshold, priming, episode/refill/reset accounting, queue cap, queue failure\n";
    std::cout << media.audioDiagnostics() << '\n';
    mockAudio = false;
}
struct Reference {
    Bytes first, last;
    std::uint64_t decoded = 0;
};
static Reference reference(SDL_Renderer* renderer, const Units& units) {
    fog::Media media(renderer);
    Reference result;
    feed(media, units.front());
    result.first = snapshot(media, renderer);
    for (std::size_t i = 1; i < units.size(); ++i) feed(media, units[i]);
    result.last = snapshot(media, renderer);
    result.decoded = media.videoFrames();
    check(result.decoded == units.size(), "reference did not decode every AU");
    check(counter(media, "uploaded_frames") == result.decoded, "immediate compatibility changed");
    check(!media.preparePresentation(), "immediate caller retained an output");
    return result;
}
static void test(SDL_Renderer* renderer, const Units& units, const Units& changed) {
    const auto expected = reference(renderer, units);
    const auto changedExpected = reference(renderer, changed);
    check(expected.first != expected.last, "fixture must change between first and last output");
    fog::Media media(renderer, stream::VideoCodec::H264, true);
    feed(media, units.front());
    check(media.videoFrames() == 1 && media.videoWidth() == 640 && media.videoHeight() == 360,
        "decoded metadata unavailable before presentation");
    check(!media.draw(), "draw uploaded pending output");
    for (std::size_t i = 1; i < units.size(); ++i) feed(media, units[i]);
    check(media.videoFrames() == expected.decoded, "deferred batch dropped compressed/decode work");
    check(counter(media, "uploaded_frames") == 0 && counter(media, "scale_calls") == 0 &&
        counter(media, "upload_calls") == 0 && counter(media, "pending_frames") == 1 &&
        counter(media, "superseded_frames") == expected.decoded - 1, "batch was not coalesced before conversion/upload");
    check(media.preparePresentation(), "batch was not presented");
    check(snapshot(media, renderer) == expected.last, "coalesced output differs from actual last decoded frame");
    check(!media.preparePresentation(), "same pending output uploaded twice");
    check(counter(media, "uploaded_frames") == 1 && counter(media, "upload_calls") == 1 &&
        counter(media, "scale_calls") == 1 && counter(media, "pending_frames") == 0,
        "batch did not produce exactly one scale/upload");

    feed(media, units.front());
    check(snapshot(media, renderer) == expected.last, "draw performed hidden presentation");
    const auto decodedBeforeReset = media.videoFrames();
    media.resetVideoDecoder();
    check(media.videoFrames() == decodedBeforeReset && media.waitingForVideoKeyframe() &&
        !media.preparePresentation() && snapshot(media, renderer) == expected.last,
        "reset lost last texture or retained pending output");
    feed(media, units[1]);
    check(media.videoFrames() == decodedBeforeReset, "reset accepted delta before IDR");
    feed(media, units.front());
    check(media.videoFrames() == decodedBeforeReset + 1 && !media.waitingForVideoKeyframe() &&
        media.preparePresentation() && snapshot(media, renderer) == expected.first, "next IDR did not recover");

    feed(media, changed.front());
    check(media.videoWidth() == 640 && media.videoHeight() == 480 &&
        snapshot(media, renderer) == expected.first, "pending resolution changed last texture geometry");
    media.resetVideoDecoder();
    check(!media.preparePresentation() && media.videoWidth() == 640 && media.videoHeight() == 360 &&
        snapshot(media, renderer) == expected.first, "resolution reset did not discard staged output");
    feed(media, changed.front());
    failCreation = true;
    bool rejected = false;
    try { media.preparePresentation(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && snapshot(media, renderer) == expected.first && counter(media, "pending_frames") == 1,
        "texture allocation failure lost last good output");
    failUpload = true;
    rejected = false;
    try { media.preparePresentation(); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && snapshot(media, renderer) == expected.first, "candidate upload failure lost last good texture");
    check(media.preparePresentation() && snapshot(media, renderer) == changedExpected.first,
        "resolution transition could not retry after candidate failure");
    feed(media, changed[1]);
    media.setVideoCodec(stream::VideoCodec::H265);
    check(!media.preparePresentation() && snapshot(media, renderer) == changedExpected.first,
        "codec switch retained staged output or lost texture");
    media.setVideoCodec(stream::VideoCodec::H264);
    feed(media, units.front());
    check(media.preparePresentation() && snapshot(media, renderer) == expected.first,
        "H264 switch-back recovery failed");
    for (unsigned i = 0; i < 32; ++i) {
        feed(media, units[1]);
        media.resetVideoDecoder();
        check(!media.preparePresentation(), "repeated reset retained a frame reference");
        feed(media, units.front());
        check(media.preparePresentation(), "repeated deferred reset failed");
    }
    const auto visible = snapshot(media, renderer);
    const auto uploadsBeforeFailure = counter(media, "uploaded_frames");
    emulateHardwareFrame = true;
    failTransfer = true;
    feed(media, units[1]);
    check(counter(media, "transfer_calls") == 0 && snapshot(media, renderer) == visible,
        "video/draw performed deferred hardware transfer");
    bool codecError = false;
    try { media.preparePresentation(); }
    catch (const fog::VideoDecodeError& error) {
        codecError = error.code() == AVERROR_EXTERNAL && error.recoverable() &&
            std::string(error.what()).find("hardware frame transfer") != std::string::npos;
    }
    check(codecError && counter(media, "transfer_calls") == 1 &&
        counter(media, "uploaded_frames") == uploadsBeforeFailure && snapshot(media, renderer) == visible,
        "transfer failure escaped presentation recovery boundary or lost texture");
    media.resetVideoDecoder();
    check(!media.preparePresentation(), "transfer-error reset retained pending hardware reference");
    feed(media, units.front());
    check(media.preparePresentation(), "IDR recovery after transfer failure failed");
    std::cout << "PASS deferred H264: " << expected.decoded << " decoded AUs -> one upload, exact last-frame readback, "
        "draw isolation, pending reset, next IDR, dimensions, allocation/upload faults, codec switch, 32 resets, "
        "simulated hardware transfer error boundary\n";
    std::cout << media.diagnostics() << '\n';
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 3;
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!renderer) { if (surface) SDL_FreeSurface(surface); SDL_Quit(); return 4; }
    int result = 0;
    try {
        const auto units = load(argv[1]);
        imageSamples();
        blackImageMeasurements(renderer, load(argv[3]), units);
        test(renderer, units, load(argv[2]));
        presentationCounts(renderer, units);
        audioObservations();
        audioCounts(renderer);
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
    return result;
}
