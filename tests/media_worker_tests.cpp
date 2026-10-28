#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/infrastructure/media_frame_queue.hpp"
#include "features/streaming/domain/audio_latency_policy.hpp"
#include <SDL.h>
#include <opus/opus.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void until(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!ready()) {
        check(std::chrono::steady_clock::now() < deadline, "worker progress timed out");
        std::this_thread::sleep_for(1ms);
    }
}
using Bytes = std::vector<std::byte>;
static std::vector<Bytes> load(const char* path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(input)), {});
    check(!data.empty(), "missing H264 fixture");
    const auto size = data.size(); data.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    auto* parser = av_parser_init(AV_CODEC_ID_H264);
    auto* context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    check(parser && context, "fixture parser unavailable");
    std::vector<Bytes> result;
    std::size_t offset = 0;
    do {
        unsigned char* output = nullptr; int count = 0;
        const int used = av_parser_parse2(parser, context, &output, &count,
            offset < size ? data.data() + offset : nullptr, static_cast<int>(size - offset),
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        check(used >= 0, "fixture parse failed");
        if (count) result.emplace_back(reinterpret_cast<std::byte*>(output), reinterpret_cast<std::byte*>(output) + count);
        if (offset == size) break;
        check(used || count, "fixture parser stalled");
        offset += used;
    } while (true);
    av_parser_close(parser); avcodec_free_context(&context);
    check(result.size() >= 3, "fixture needs multiple access units");
    return result;
}
class Client final : public stream::StreamingClient {
public:
    std::mutex mutex;
    std::deque<stream::Packet> video, audio;
    std::atomic<unsigned> polls{0};
    void push(const Bytes& bytes, bool sound = false, std::uint64_t generation = 1, std::uint32_t timestamp = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        stream::Packet packet; packet.kind = sound ? stream::Kind::Audio : stream::Kind::Video;
        packet.data = bytes; packet.videoGeneration = generation; packet.timestamp = timestamp;
        packet.enqueueMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) - 7;
        (sound ? audio : video).push_back(std::move(packet));
    }
    void pushAudioBatch(const std::vector<Bytes>& packets, std::size_t first, std::size_t count, std::uint64_t ageMs) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        for (auto i = first; i < first + count; ++i) {
            stream::Packet packet; packet.kind = stream::Kind::Audio; packet.data = packets.at(i);
            packet.enqueueMs = now - ageMs; audio.push_back(std::move(packet));
        }
    }
    bool poll(stream::Packet& packet) override { return pop(video, packet); }
    bool pollAudio(stream::Packet& packet) override { return pop(audio, packet); }
    bool pop(std::deque<stream::Packet>& queue, stream::Packet& packet) {
        std::lock_guard<std::mutex> lock(mutex); ++polls;
        if (queue.empty()) return false;
        packet = std::move(queue.front()); queue.pop_front(); return true;
    }
    bool start(const stream::StreamConnection&) override { return true; }
    bool configureVideo(const fog::video::Settings&) override { return true; }
    bool configureRelay(bool) override { return true; }
    bool configurePlayStation(bool) override { return true; }
    bool updateVideoSettings(const fog::video::Settings&) override { return true; }
    stream::VideoCodec selectedVideoCodec() const override { return stream::VideoCodec::H264; }
    std::string qualityStatus() const override { return {}; }
    void pump() override {}
    void observePlayback(const stream::PlaybackObservation&) override {}
    std::uint64_t queuedVideoAgeMs() const override { return 0; }
    std::size_t queuedVideoPackets() const override { return 0; }
    bool ready() const override { return true; }
    std::string status() const override { return {}; }
    std::string error() const override { return {}; }
    bool retryableError() const override { return false; }
    bool disconnected() const override { return false; }
    bool disconnectForRetry() override { return false; }
    std::string diagnostics() const override { return {}; }
    std::string transportTimings() const override { return {}; }
    std::string videoDiagnostics() const override { return {}; }
    std::string gapDiagnostics() const override { return {}; }
    std::string queueDiagnostics() const override { return {}; }
    std::string mutexDiagnostics() const override { return {}; }
    std::string dtlsDiagnostics() const override { return {}; }
    std::string rtcpDiagnostics() const override { return {}; }
    std::string inputDiagnostics() const override { return {}; }
    bool pollVideoDiagnostic(std::string&) override { return false; }
    bool waitingForVideoKeyframe() const override { return false; }
    bool requestVideoKeyframe() override { return true; }
    void setDecoderWaitingForKeyframe(bool) override {}
    bool sendGamepad(const stream::Gamepad&) override { return true; }
    bool sendInput(const fog::remote::Input&) override { return true; }
    void close() override {}
};
static std::atomic<bool> captureAudio{false};
static std::mutex audioCaptureMutex;
static std::vector<std::uint64_t> audioHashes;
static std::uint64_t pcmHash(const opus_int16* pcm, int count) {
    std::uint64_t result = 1469598103934665603ULL;
    for (int i = 0; i < count * 2; ++i) { result ^= static_cast<std::uint16_t>(pcm[i]); result *= 1099511628211ULL; }
    return result;
}
extern "C" int __real_opus_decode(OpusDecoder*, const unsigned char*, opus_int32, opus_int16*, int, int);
extern "C" int __wrap_opus_decode(OpusDecoder* decoder, const unsigned char* data, opus_int32 size,
    opus_int16* pcm, int frames, int fec) {
    const int result = __real_opus_decode(decoder, data, size, pcm, frames, fec);
    if (captureAudio && result > 0) {
        std::lock_guard<std::mutex> lock(audioCaptureMutex); audioHashes.push_back(pcmHash(pcm, result));
    }
    return result;
}
static std::thread::id applicationThread;
static std::atomic<bool> wrongThread{false}, failSend{false}, blockSend{false}, enteredSend{false};
static std::atomic<unsigned> hevcOpens{0};
static std::mutex gateMutex;
static std::condition_variable gate;
extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" int __wrap_avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    if (codec->id == AV_CODEC_ID_HEVC) ++hevcOpens;
    return __real_avcodec_open2(context, codec, options);
}
extern "C" int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
extern "C" int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    if (std::this_thread::get_id() == applicationThread) wrongThread = true;
    if (blockSend) {
        enteredSend = true; gate.notify_all();
        std::unique_lock<std::mutex> lock(gateMutex);
        gate.wait(lock, [] { return !blockSend.load(); });
    }
    if (failSend.exchange(false)) return AVERROR_INVALIDDATA;
    return __real_avcodec_send_packet(context, packet);
}
extern "C" int __real_sws_scale(SwsContext*, const uint8_t* const[], const int[], int, int, uint8_t* const[], const int[]);
extern "C" int __wrap_sws_scale(SwsContext* context, const uint8_t* const source[], const int sourceStride[],
    int y, int h, uint8_t* const destination[], const int destinationStride[]) {
    if (std::this_thread::get_id() == applicationThread) wrongThread = true;
    return __real_sws_scale(context, source, sourceStride, y, h, destination, destinationStride);
}
extern "C" int __real_SDL_UpdateYUVTexture(SDL_Texture*, const SDL_Rect*, const Uint8*, int, const Uint8*, int, const Uint8*, int);
extern "C" int __wrap_SDL_UpdateYUVTexture(SDL_Texture* texture, const SDL_Rect* rect,
    const Uint8* y, int ys, const Uint8* u, int us, const Uint8* v, int vs) {
    if (std::this_thread::get_id() != applicationThread) wrongThread = true;
    return __real_SDL_UpdateYUVTexture(texture, rect, y, ys, u, us, v, vs);
}
static Bytes sound() {
    int error = 0;
    auto* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    check(encoder && !error, "Opus encoder unavailable");
    std::array<opus_int16, 960*2> samples{}; Bytes result(4096);
    const auto size = opus_encode(encoder, samples.data(), 960, reinterpret_cast<unsigned char*>(result.data()), result.size());
    opus_encoder_destroy(encoder); check(size > 0, "Opus encode failed"); result.resize(size); return result;
}
static void pacing() {
    using Queue = fog::detail::MediaFrameQueue;
    Queue queue; queue.reset(30);
    const auto now = Queue::Clock::time_point{};
    Queue::Frame frame;
    std::uint64_t dropped = 0;
    queue.push({}, 0xfffffff0, now);
    queue.push({}, 0xfffffff0 + std::uint32_t(3000), now);
    queue.push({}, 0xfffffff0 + std::uint32_t(6000), now);
    check(queue.take(frame, now, dropped) && !queue.take(frame, now + 31ms, dropped), "RTP/fps pacing released future frames");
    check(queue.take(frame, now + 34ms, dropped) && queue.take(frame, now + 68ms, dropped), "RTP wrap pacing lost frames");
    queue.reset(30);
    for (int i = 0; i < 12; ++i) dropped += queue.push({}, i*3000, now);
    check(queue.size() == 3 && dropped == 9, "presentation queue exceeded its bound");
    check(queue.take(frame, now, dropped) && !queue.take(frame, now + 31ms, dropped), "dropped burst frames retained future timing slots");
    check(queue.take(frame, now + 1s, dropped) && dropped == 10 && queue.size() == 0, "late queue did not coalesce already due frames");
    queue.reset(30); dropped = 0;
    queue.push({}, 0, now); queue.push({}, 0, now);
    check(queue.take(frame, now, dropped) && !queue.take(frame, now, dropped) &&
        queue.take(frame, now + 34ms, dropped), "missing RTP clock bypassed fps pacing");
    queue.reset(30); queue.push({}, 0, now + 10s);
    check(queue.take(frame, now + 10s, dropped), "reset retained old timing or frames");
    for (int i = 1; i < 12; ++i) queue.push({}, i*3000, now + 10s);
    check(!queue.take(frame, now + 10010ms, dropped) && queue.take(frame, now + 10034ms, dropped),
        "burst timeline rebase bypassed configured presentation interval");
    queue.reset(30);
    unsigned uploaded = 0, received = 0;
    const std::array<std::int64_t, 4> jitter{{0, 200000, -200000, 0}};
    for (unsigned refresh = 0; refresh < 180; ++refresh) {
        const auto refreshTime = now + std::chrono::nanoseconds(
            static_cast<std::int64_t>(refresh) * 16666667 + jitter[refresh % jitter.size()]);
        while (received < 90) {
            const auto arrival = now + std::chrono::nanoseconds(static_cast<std::int64_t>(received) * 33333333) - 5ms;
            if (arrival > refreshTime) break;
            queue.push({}, received * 3000, arrival); ++received;
        }
        if (queue.take(frame, refreshTime, dropped)) ++uploaded;
    }
    check(uploaded >= 89 && uploaded <= 90, "60 Hz VSYNC jitter degraded anchored 30 fps pacing");
    std::cout << "Pacing: " << uploaded << " uploads / 180 jittered 60 Hz refreshes (30 fps target)\n";
}
static std::uint64_t metric(const std::string& text, const char* name) {
    const auto at = text.find(std::string(name) + "=");
    check(at != std::string::npos, "missing audio metric");
    return std::stoull(text.substr(at + std::strlen(name) + 1));
}
static void audioPolicy() {
    using Policy = fog::detail::AudioLatencyPolicy;
    const auto bytes = [](std::uint32_t ms) { return ms * 192; };
    check(Policy::select(960, bytes(40), 60).frames == 960, "age boundary discarded normal jitter");
    check(Policy::select(960, bytes(40), 61).frames == 0, "stale audio kept accumulating above refill floor");
    check(Policy::select(960, bytes(20), 200).frames == 960, "catchup starved output below refill floor");
    check(Policy::select(960, bytes(80), 0).frames == 0, "full SDL queue retained incoming latency");
    check(Policy::select(960, bytes(70), 0).frames == 0, "ordinary packet was fragmented at output ceiling");
    const auto longFrame = Policy::select(5760, 0, 0);
    check(longFrame.skip == 1920 && longFrame.frames == 3840, "120ms Opus frame could not refill bounded output");
    // Burst, drain, and fresh recovery repeated across multiple legal Opus
    // durations. Consume all compressed input even while output cannot advance.
    for (const unsigned frames : {120, 240, 480, 960, 1920, 2880, 5760}) {
        std::uint32_t queued = 0;
        std::uint64_t discarded = 0, admitted = 0;
        for (unsigned episode = 0; episode < 4; ++episode) {
            for (unsigned packet = 0; packet < 40; ++packet) {
                const auto selection = Policy::select(frames, queued * 4, 200);
                queued += selection.frames; discarded += selection.skip; admitted += selection.frames;
                check(queued <= 3840, "burst exceeded 80ms output budget");
            }
            check(queued >= 1920, "burst catchup failed to maintain jitter reserve");
            queued = 0;
            const auto live = Policy::select(frames, queued * 4, 0);
            check(live.frames > 0, "catchup did not recover on fresh audio");
        }
        check(discarded > admitted, "stale bursts did not advance past obsolete PCM");
    }
}
static std::vector<Bytes> audioSequence(unsigned count) {
    int error = 0;
    auto* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    check(encoder && !error, "Opus encoder unavailable");
    std::vector<Bytes> result;
    for (unsigned frame = 0; frame < count; ++frame) {
        std::array<opus_int16, 960 * 2> pcm{};
        for (unsigned i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<opus_int16>((i * 997 + frame * 3109) % 60000 - 30000);
        Bytes bytes(4096);
        const auto size = opus_encode(encoder, pcm.data(), 960,
            reinterpret_cast<unsigned char*>(bytes.data()), bytes.size());
        check(size > 0, "Opus sequence encode failed"); bytes.resize(size); result.push_back(std::move(bytes));
    }
    opus_encoder_destroy(encoder);
    return result;
}
static void audioCatchup(SDL_Renderer* renderer) {
    constexpr unsigned burst = 12, episodes = 3, stride = burst + 1;
    const auto sequence = audioSequence(episodes * stride + burst);
    int error = 0;
    auto* reference = opus_decoder_create(48000, 2, &error);
    check(reference && !error, "reference Opus decoder unavailable");
    std::vector<std::uint64_t> expected;
    for (const auto& packet : sequence) {
        std::array<opus_int16, 5760 * 2> pcm{};
        const auto count = __real_opus_decode(reference, reinterpret_cast<const unsigned char*>(packet.data()),
            packet.size(), pcm.data(), 5760, 0);
        check(count > 0, "reference decode failed"); expected.push_back(pcmHash(pcm.data(), count));
    }
    opus_decoder_destroy(reference);
    Client client;
    fog::Media media(renderer, stream::VideoCodec::H264, true);
    { std::lock_guard<std::mutex> lock(audioCaptureMutex); audioHashes.clear(); }
    captureAudio = true;
    media.startWorkers(client, 30);
    for (unsigned episode = 0; episode < episodes; ++episode) {
        client.pushAudioBatch(sequence, episode * stride, burst, 400);
        until([&] { return media.audioPackets() == episode * stride + burst; });
        // Real SDL dummy device drains independently. A fresh packet ends this
        // catchup episode before the next scheduling-stall burst.
        std::this_thread::sleep_for(120ms);
        client.pushAudioBatch(sequence, episode * stride + burst, 1, 0);
        until([&] { return media.audioPackets() == (episode + 1) * stride; });
    }
    // A fresh burst must also drain promptly when SDL is full. Waiting for
    // output capacity here would recreate the permanent compressed backlog.
    std::this_thread::sleep_for(120ms);
    client.pushAudioBatch(sequence, episodes * stride, burst, 0);
    until([&] { return media.audioPackets() == sequence.size(); });
    media.stopWorkers(); captureAudio = false;
    const auto text = media.audioDiagnostics();
    check(metric(text, "audio_pcm_discarded_packets") >= 18, "stale bursts remained queued instead of catching up");
    check(metric(text, "audio_stale_packets") >= 18, "stale PCM suppression was not diagnosed");
    check(metric(text, "audio_max_queued_ms") <= 80, "real SDL output exceeded software queue budget");
    check(metric(text, "audio_overflow_packets") >= 6, "fresh burst backed up compressed input at full SDL output");
    check(metric(text, "audio_catchups") == episodes + 1, "repeated catchup episodes were lost or never ended");
    check(media.audioQueueResets() == 0, "catchup cleared already queued sound");
    { std::lock_guard<std::mutex> lock(audioCaptureMutex);
        check(audioHashes == expected, "catchup skipped Opus state or altered decode order/output"); }
    fog::Media::WorkerError event;
    check(!media.takeWorkerError(event), "catchup raised worker error");
    std::cout << "Audio catchup: " << text << "; all " << expected.size()
        << " decoded PCM hashes match continuous reference Opus\n";
}
static void workers(SDL_Renderer* renderer, const std::vector<Bytes>& units) {
    Client client;
    // Negotiation selects H264 although construction preferred H265.
    fog::Media media(renderer, stream::VideoCodec::H265, true);
    media.startWorkers(client, 30);
    struct ReleaseGate {
        ~ReleaseGate() { blockSend = false; gate.notify_all(); }
    } releaseGate;
    const auto audio = sound();
    blockSend = true; enteredSend = false;
    client.push(units[0]);
    until([] { return enteredSend.load(); });
    client.push(audio, true);
    until([&] { return media.audioPackets() == 1; });
    check(!media.preparePresentation() && media.videoWidth() == 0, "blocked decode had ready output");
    check(!media.diagnostics().empty() && !media.audioDiagnostics().empty(), "worker snapshots missing");
    media.setVideoCodec(stream::VideoCodec::H265);
    media.setVideoCodec(stream::VideoCodec::H264);
    media.resetVideoDecoder();
    check(media.waitingForVideoKeyframe() && blockSend, "UI reset waited for blocked codec");
    blockSend = false; gate.notify_all();
    client.push(units[0]);
    until([&] { return media.videoFrames() >= 2 && !media.waitingForVideoKeyframe(); });
    until([&] { return media.preparePresentation(); });
    check(media.draw() && media.uploadedFrames() == 1, "reset epoch lost new output or uploaded stale output");
    check(media.videoWidth() == 1280 && media.videoHeight() == 720, "720p worker output dimensions changed");
    check(media.diagnostics().find("codec=h264") != std::string::npos, "negotiated H264 fallback was ignored");
    check(hevcOpens == 1, "latest codec command was ignored during blocked decode");
    check(media.diagnostics().find("video_worker_queue_samples=0") == std::string::npos &&
        media.audioDiagnostics().find("audio_worker_queue_samples=0") == std::string::npos,
        "worker dequeue wait was not measured");
    const auto beforeGeneration = media.videoFrames();
    client.push(units[1], false, 2, 3000);
    until([&] { return media.workerVideoGeneration() == 2 && media.waitingForVideoKeyframe(); });
    check(media.videoFrames() == beforeGeneration && !media.preparePresentation() && media.draw(), "generation reset accepted delta or lost retained texture");
    client.push(units[0], false, 2, 6000);
    until([&] { return media.videoFrames() > beforeGeneration; });
    until([&] { return media.preparePresentation(); });
    failSend = true; client.push(units[1], false, 2, 9000);
    fog::Media::WorkerError event;
    until([&] { return media.takeWorkerError(event); });
    bool recoverable = false;
    try { std::rethrow_exception(event.error); }
    catch (const fog::VideoDecodeError& error) { recoverable = error.recoverable() && error.code() == AVERROR_INVALIDDATA; }
    check(recoverable && event.hasPacket && event.packet.timestamp == 9000 && media.draw(), "decoder error was lost or detached from packet/recovery");
    const auto beforeRecovery = media.videoFrames();
    client.push(units[0], false, 2, 12000);
    client.push(audio, true);
    until([&] { return media.audioPackets() >= 2; });
    check(media.videoFrames() == beforeRecovery, "decoder continued before UI recovery policy");
    media.resetVideoDecoder();
    until([&] { return media.videoFrames() > beforeRecovery; });
    until([&] { return media.preparePresentation(); });
    check(media.audioQueueResets() == 0, "workers cleared queued audio");
    client.push(Bytes(65537), true);
    until([&] { return media.takeWorkerError(event); });
    bool audioFailed = false;
    try { std::rethrow_exception(event.error); }
    catch (const fog::VideoDecodeError&) {}
    catch (const std::runtime_error&) { audioFailed = true; }
    check(audioFailed && event.hasPacket && event.packet.kind == stream::Kind::Audio,
        "audio failure did not reach UI fatal-error path");
    media.stopWorkers();
    check(!media.workersRunning(), "workers survived stop");
    const auto polls = client.polls.load();
    std::this_thread::sleep_for(10ms);
    check(client.polls == polls, "transport accessed after worker join");
    for (unsigned i = 0; i < 8; ++i) { media.startWorkers(client, 30); media.stopWorkers(); }
    check(!wrongThread, "codec/scale/texture thread affinity violated");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    applicationThread = std::this_thread::get_id();
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 3;
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!renderer) return 4;
    int result = 0;
    try { pacing(); audioPolicy(); audioCatchup(renderer); workers(renderer, load(argv[1])); }
    catch (const std::exception& error) { blockSend = false; gate.notify_all(); std::cerr << error.what() << '\n'; result = 1; }
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
    if (!result) std::cout << "Media workers PASS: real 720p30 H264/Opus, blocked-codec UI/audio independence, affinity, H264 negotiation fallback, reset/codec commands and generations, video recovery/audio fatal errors, dequeue timings, bounded RTP pacing, repeated joined shutdown\n";
    return result;
}
