#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/infrastructure/media_frame_queue.hpp"
#include "features/streaming/infrastructure/media_experiments.hpp"
#include "features/streaming/infrastructure/video_image_diagnostics.hpp"
#include "media_experiment_client.hpp"
#include <SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <condition_variable>
#include <cstring>
#include <set>
#include <thread>

using namespace fog::detail;
using Bytes = std::vector<unsigned char>;
static const auto uiThread = std::this_thread::get_id();
static std::map<SDL_Texture*, Uint32> textures;
static unsigned nvCreates = 0, nvUploads = 0, yuvUploads = 0;
static bool failNvCreate = false, failNvUpload = false, failYuvCreate = false, failYuvUpload = false;
static bool negative = false, synthetic = true;
static bool hardware = false;
static unsigned transfers = 0;
static int imageWidth = 63, imageHeight = 35;
static SDL_Texture* drawn = nullptr;
static constexpr bool nv12Api = SDL_VERSION_ATLEAST(2, 0, 16);
static thread_local unsigned pixelSeed = 0;
static std::mutex uploadMutex;
static std::condition_variable uploadChanged;
static bool holdUpload = false, uploadEntered = false, producerDone = false;
static std::set<const Uint8*> workerUploadBuffers;
static void ui() { assert(std::this_thread::get_id() == uiThread); }
static unsigned char luma(int x, int y) { return static_cast<unsigned char>(32 + (x + 3 * y + pixelSeed) % 180); }
static unsigned char chroma(int x, int y) { return static_cast<unsigned char>(64 + (3 * x + y + pixelSeed) % 100); }
static void inspectHeldUpload(const Uint8* pixels) {
    std::unique_lock<std::mutex> lock(uploadMutex);
    if (!holdUpload) return;
    workerUploadBuffers.insert(pixels);
    const auto layout = MediaPixelLayout::make(imageWidth, imageHeight);
    const Bytes retained(pixels, pixels + layout.size);
    uploadEntered = true; uploadChanged.notify_all();
    assert(uploadChanged.wait_for(lock, std::chrono::seconds(5), [] { return producerDone; }));
    assert(std::memcmp(pixels, retained.data(), retained.size()) == 0);
    holdUpload = false;
}
extern "C" int __real_avcodec_receive_frame(AVCodecContext*, AVFrame*);
extern "C" SDL_Texture* __real_SDL_CreateTexture(SDL_Renderer*, Uint32, int, int, int);
extern "C" void __real_SDL_DestroyTexture(SDL_Texture*);
extern "C" int __real_SDL_UpdateYUVTexture(SDL_Texture*, const SDL_Rect*, const Uint8*, int, const Uint8*, int, const Uint8*, int);
extern "C" int __real_SDL_RenderCopy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
extern "C" int __wrap_avcodec_receive_frame(AVCodecContext* codec, AVFrame* frame) {
    const int rc = __real_avcodec_receive_frame(codec, frame);
    if (rc || !synthetic) return rc;
    const auto pts = frame->pts;
    pixelSeed = static_cast<unsigned>(pts) % 19;
    av_frame_unref(frame);
    frame->format = AV_PIX_FMT_NV12; frame->width = imageWidth; frame->height = imageHeight; frame->pts = pts;
    assert(av_frame_get_buffer(frame, 32) == 0);
    const auto layout = MediaPixelLayout::make(imageWidth, imageHeight);
    assert(frame->linesize[0] >= imageWidth && frame->linesize[1] >= 2 * layout.chromaWidth);
    if (negative) for (int plane = 0; plane < 2; ++plane) {
        frame->data[plane] += frame->linesize[plane] * ((plane ? layout.chromaHeight : imageHeight) - 1);
        frame->linesize[plane] = -frame->linesize[plane];
    }
    for (int y = 0; y < imageHeight; ++y)
        for (int x = 0; x < imageWidth; ++x) frame->data[0][y * frame->linesize[0] + x] = luma(x, y);
    for (int y = 0; y < layout.chromaHeight; ++y)
        for (int x = 0; x < 2 * layout.chromaWidth; ++x) frame->data[1][y * frame->linesize[1] + x] = chroma(x, y);
    if (hardware) frame->hw_frames_ctx = av_buffer_alloc(1);
    return rc;
}
extern "C" int __wrap_av_hwframe_transfer_data(AVFrame* target, const AVFrame* source, int) {
    ++transfers;
    const int result = av_frame_ref(target, source);
    av_buffer_unref(&target->hw_frames_ctx);
    return result;
}
extern "C" SDL_Texture* __wrap_SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h) {
    ui();
    if (format == SDL_PIXELFORMAT_NV12) {
        ++nvCreates;
        if (failNvCreate) return nullptr;
    } else if (failYuvCreate) return nullptr;
    auto* texture = __real_SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, access, w, h);
    if (texture) textures[texture] = format;
    return texture;
}
extern "C" void __wrap_SDL_DestroyTexture(SDL_Texture* texture) {
    ui(); assert(textures.erase(texture) == 1); __real_SDL_DestroyTexture(texture);
}
extern "C" int __wrap_SDL_UpdateNVTexture(SDL_Texture* texture, const SDL_Rect* rect,
    const Uint8* y, int yp, const Uint8* uv, int uvp) {
    ui(); ++nvUploads; assert(textures.at(texture) == SDL_PIXELFORMAT_NV12);
    if (failNvUpload) return -1;
    pixelSeed = y[0] - 32;
    inspectHeldUpload(y);
    const auto layout = MediaPixelLayout::make(imageWidth, imageHeight);
    assert(yp == imageWidth && uvp == layout.chromaWidth * 2);
    for (int row = 0; row < imageHeight; ++row)
        for (int x = 0; x < imageWidth; ++x) assert(y[row * yp + x] == luma(x, row));
    for (int row = 0; row < layout.chromaHeight; ++row)
        for (int x = 0; x < uvp; ++x) assert(uv[row * uvp + x] == chroma(x, row));
    Bytes packed(layout.size), planar(layout.size);
    copyMediaPlane(packed.data(), imageWidth, y, yp, imageWidth, imageHeight);
    copyMediaPlane(packed.data() + layout.ySize, uvp, uv, uvp, uvp, layout.chromaHeight);
    mediaNv12ToIyuv(packed.data(), planar.data(), layout);
    return __real_SDL_UpdateYUVTexture(texture, rect, planar.data(), imageWidth, planar.data() + layout.ySize,
        layout.chromaWidth, planar.data() + layout.ySize + layout.chromaSize, layout.chromaWidth);
}
extern "C" int __wrap_SDL_UpdateYUVTexture(SDL_Texture* texture, const SDL_Rect* rect,
    const Uint8* y, int yp, const Uint8* u, int up, const Uint8* v, int vp) {
    ui(); ++yuvUploads; assert(textures.at(texture) == SDL_PIXELFORMAT_IYUV);
    if (failYuvUpload) return -1;
    pixelSeed = y[0] - 32;
    inspectHeldUpload(y);
    if (FOGPLAY_MEDIA_NV12 && synthetic) {
        for (int row = 0; row < imageHeight; ++row)
            for (int x = 0; x < imageWidth; ++x) assert(y[row * yp + x] == luma(x, row));
        for (int row = 0; row < (imageHeight + 1) / 2; ++row)
            for (int x = 0; x < (imageWidth + 1) / 2; ++x) {
                assert(u[row * up + x] == chroma(2 * x, row));
                assert(v[row * vp + x] == chroma(2 * x + 1, row));
            }
    }
    return __real_SDL_UpdateYUVTexture(texture, rect, y, yp, u, up, v, vp);
}
extern "C" int __wrap_SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst) {
    ui(); drawn = texture; return __real_SDL_RenderCopy(renderer, texture, src, dst);
}
static void feed(fog::Media& media, const Bytes& bytes) {
    media.video(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
}
static SDL_Texture* draw(fog::Media& media) { assert(media.draw()); return drawn; }
static void queueAndPool() {
    for (const int fps : {30, 60}) for (const bool paced : {false, true}) {
        MediaFrameQueue queue(MediaFrameQueue::Mode::Latest); queue.reset(fps, paced);
        const auto now = MediaFrameQueue::Clock::time_point{};
        std::uint64_t dropped = 0;
        for (int i = 0; i < 12; ++i) { MediaFrameQueue::Frame frame; frame.width = i; dropped += queue.push(std::move(frame), i * 9000, now); }
        MediaFrameQueue::Frame output;
        assert(queue.limit() == 1 && queue.size() == 1 && dropped == 11);
        assert(queue.take(output, now, dropped) && output.width == 11);
        queue.push({}, 0, now);
        if (!(paced && fps == 60)) assert(!queue.take(output, now, dropped));
        assert(queue.take(output, now + std::chrono::milliseconds(34), dropped));
        queue.reset(fps, paced); queue.push({}, 0, now);
        assert(queue.take(output, now, dropped));
    }
    for (const int fps : {30, 60}) {
        MediaFrameQueue queue(MediaFrameQueue::Mode::Latest); queue.reset(fps, fps == 60);
        const auto origin = MediaFrameQueue::Clock::time_point{};
        constexpr std::array<int, 4> jitterUs{0, 200, -200, 0};
        unsigned supplied = 0, shown = 0;
        std::uint64_t dropped = 0;
        MediaFrameQueue::Frame output;
        for (unsigned refresh = 0; refresh < 360; ++refresh) {
            const auto now = origin + std::chrono::nanoseconds(std::int64_t(refresh) * 1000000000 / 60) +
                std::chrono::microseconds(jitterUs[refresh % jitterUs.size()]);
            while (origin + std::chrono::nanoseconds(std::int64_t(supplied) * 1000000000 / 120) +
                std::chrono::milliseconds(5) <= now) {
                MediaFrameQueue::Frame input; input.width = static_cast<int>(supplied);
                dropped += queue.push(std::move(input), supplied * 750, now); ++supplied;
            }
            if (queue.take(output, now, dropped)) { ++shown; assert(output.width == static_cast<int>(supplied - 1)); }
            assert(queue.size() <= 1);
        }
        assert(shown == (fps == 30 ? 180 : 359));
        assert(dropped + shown + queue.size() == supplied);
    }
    auto pool = std::make_shared<MediaCpuBufferPool>();
    MediaFrameQueue queue;
    MediaFrameQueue::Frame live; live.pool = pool; live.pixels.resize(100); auto* original = live.pixels.data();
    queue.push(std::move(live), 0, {});
    auto other = pool->acquire(); assert(other.data() != original);
    MediaFrameQueue::Frame output; std::uint64_t dropped = 0;
    assert(queue.take(output, {}, dropped));
    other = pool->acquire(); assert(other.data() != original);
    output = {}; other = pool->acquire(); assert(other.data() == original);
    std::array<Bytes, 3> buffers;
    for (auto& bytes : buffers) { bytes.resize(100); pool->recycle(bytes); }
    assert(pool->acquire().capacity() != 0 && pool->acquire().capacity() != 0 && pool->acquire().capacity() == 0);
    MediaFrameQueue::Frame resetFrame; resetFrame.pool = pool; resetFrame.pixels.resize(100);
    original = resetFrame.pixels.data(); queue.push(std::move(resetFrame), 0, {});
    queue.reset(30); other = pool->acquire(); assert(other.data() == original);
    other.reserve(MediaCpuBufferPool::maximumBytes + 1); pool->recycle(other);
    assert(pool->acquire().capacity() == 0);
    std::array<std::thread, 2> participants;
    for (unsigned i = 0; i < participants.size(); ++i) participants[i] = std::thread([pool, i] {
        for (unsigned turn = 0; turn < 1000; ++turn) {
            MediaFrameQueue::Frame owned; owned.pool = pool; owned.pixels = pool->acquire();
            owned.pixels.resize(128, static_cast<unsigned char>(i));
            owned.pixels.front() = static_cast<unsigned char>(i);
            std::this_thread::yield(); assert(owned.pixels.front() == i);
        }
    });
    for (auto& participant : participants) participant.join();
    pool->clear(); assert(pool->acquire().capacity() == 0);
    bool rejected = false;
    try { unsigned char target[4]{}; copyMediaPlane(target, 2, target, 1, 2, 2); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    const auto layout = MediaPixelLayout::make(63, 35);
    Bytes nv(layout.size), iyuv(layout.size);
    for (std::size_t i = 0; i < nv.size(); ++i) nv[i] = static_cast<unsigned char>(i);
    mediaNv12ToIyuv(nv.data(), iyuv.data(), layout);
    const auto a = VideoImageSample::inspect(nv.data(), nv.size(), 63, 35, true);
    const auto b = VideoImageSample::inspect(iyuv.data(), iyuv.size(), 63, 35);
    assert(a.fingerprint == b.fingerprint && a.chromaDeviation == b.chromaDeviation && a.yMean == b.yMean);
}
static bool completed(std::future<bool> result) {
    assert(result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    return result.get();
}
static void workerPresentation(SDL_Renderer* renderer, stream::VideoCodec codec, const Bytes& bytes) {
    if (!(FOGPLAY_MEDIA_NV12 && FOGPLAY_MEDIA_CPU_POOL && FOGPLAY_MEDIA_TEXTURES == 2 && FOGPLAY_MEDIA_READY_DEPTH == 1)) return;
    synthetic = negative = hardware = true;
    imageWidth = 67; imageHeight = 37;
    workerUploadBuffers.clear();
    MediaExperimentClient client(codec);
    fog::Media media(renderer, codec, true);
    auto slot = std::chrono::steady_clock::now();
    for (unsigned restart = 0; restart < 3; ++restart) {
        media.startWorkers(client, 60);
        assert(media.workersRunning());
        for (unsigned round = 0; round < 8; ++round) {
            assert(completed(client.push(bytes)));
            {
                std::lock_guard<std::mutex> lock(uploadMutex);
                holdUpload = true; uploadEntered = producerDone = false;
            }
            const auto before = media.supersededFrames();
            // UI remains inside SDL upload while the actual video worker
            // converts distinct images and repeatedly evicts the depth-one slot.
            std::thread producer([&] {
                {
                    std::unique_lock<std::mutex> lock(uploadMutex);
                    assert(uploadChanged.wait_for(lock, std::chrono::seconds(5), [] { return uploadEntered; }));
                }
                for (unsigned i = 0; i < 20; ++i) assert(completed(client.push(bytes)));
                {
                    std::lock_guard<std::mutex> lock(uploadMutex); producerDone = true;
                }
                uploadChanged.notify_all();
            });
            slot += std::chrono::milliseconds(17);
            assert(media.preparePresentation(slot));
            producer.join();
            auto* first = draw(media);
            assert(media.supersededFrames() >= before + 19);
            slot += std::chrono::milliseconds(17);
            assert(media.preparePresentation(slot));
            assert(draw(media) != first && textures.size() == 2);
        }
        // A queued image from the old source generation must not survive the
        // admission rejection; the next matching IDR reinitializes decode.
        assert(completed(client.push(bytes)));
        const auto oldGeneration = client.generation.fetch_add(1);
        assert(!completed(client.push(bytes, oldGeneration)));
        slot += std::chrono::milliseconds(17);
        assert(!media.preparePresentation(slot));
        assert(completed(client.push(bytes)));
        assert(media.workerVideoGeneration() == oldGeneration + 1);
        assert(media.preparePresentation(slot)); draw(media);
        auto* visible = drawn;
        media.resetVideoDecoder();
        slot += std::chrono::milliseconds(17);
        assert(!media.preparePresentation(slot) && draw(media) == visible);
        bool decoded = false;
        // A packet admitted concurrently with the reset command may be dropped
        // by the production epoch gate; only an acknowledged post-reset IDR wins.
        for (unsigned i = 0; i < 4 && !decoded; ++i) decoded = completed(client.push(bytes));
        assert(decoded && !media.waitingForVideoKeyframe());
        assert(media.preparePresentation(slot)); draw(media);
        fog::Media::WorkerError error; assert(!media.takeWorkerError(error));
        visible = drawn;
        media.stopWorkers();
        assert(!media.workersRunning() && textures.size() == 1 && draw(media) == visible);
        assert(!media.preparePresentation(slot));
        // A new worker pair reuses only released CPU buffers; the visible
        // texture remains valid and the extra UI texture was actually retired.
        assert(client.queuedVideoPackets() == 0);
    }
    // Held UI buffers are drawn from a bounded reusable pool instead of one
    // fresh allocation per upload. Old-SDL fallback observes its reusable UI copy.
    assert(!workerUploadBuffers.empty() && workerUploadBuffers.size() < 24);
    hardware = false;
    std::cout << "PASS combined worker/UI codec=" << stream::videoCodecName(codec) <<
        " nvApi=" << nv12Api << " restarts=3 heldUploads=24 concurrentDecoded=480" <<
        " heldBufferAddresses=" << workerUploadBuffers.size() <<
        " depth1 evictions, immutable held bytes, generation rejection/reset, ring/spare destruction, bounded reuse\n";
}
int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    const auto codec = argc == 3 ? stream::VideoCodec::H265 : stream::VideoCodec::H264;
    std::ifstream file(argv[1], std::ios::binary); Bytes bytes((std::istreambuf_iterator<char>(file)), {}); assert(!bytes.empty());
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1); SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == 0);
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(surface); assert(renderer);
    queueAndPool();
    {
        fog::Media media(renderer, codec, true);
        feed(media, bytes); assert(media.preparePresentation()); auto* first = draw(media);
        negative = hardware = true; feed(media, bytes); assert(media.preparePresentation()); auto* second = draw(media);
        assert(transfers == 1); hardware = false;
        assert((FOGPLAY_MEDIA_TEXTURES == 2) ? second != first : second == first);
        feed(media, bytes); assert(media.preparePresentation()); assert(draw(media) == first);
        auto* visible = drawn;
        media.resetVideoDecoder(); assert(!media.preparePresentation() && draw(media) == visible);
        imageWidth = 65; imageHeight = 37;
        failNvCreate = FOGPLAY_MEDIA_NV12; failYuvCreate = true;
        feed(media, bytes);
        bool failed = false; try { media.preparePresentation(); } catch (const std::runtime_error&) { failed = true; }
        assert(failed && draw(media) == visible);
        failNvCreate = failYuvCreate = false;
        assert(media.preparePresentation()); draw(media);
        assert(media.uploadedFrames() == 4);
        media.stopWorkers(); assert(draw(media));
        media.setVideoCodec(codec == stream::VideoCodec::H265 ? stream::VideoCodec::H264 : stream::VideoCodec::H265);
        assert(!media.preparePresentation());
        media.setVideoCodec(codec); feed(media, bytes); assert(media.preparePresentation());
    }
    assert(textures.empty());
    if (FOGPLAY_MEDIA_NV12) {
        fog::Media media(renderer, codec, true);
        feed(media, bytes); assert(media.preparePresentation()); auto* visible = draw(media);
        imageWidth = 67; failNvUpload = failYuvUpload = true;
        feed(media, bytes); bool failed = false;
        try { media.preparePresentation(); } catch (const std::runtime_error&) { failed = true; }
        assert(failed && draw(media) == visible);
        failNvUpload = failYuvUpload = false;
        const auto attempts = nvUploads;
        assert(media.preparePresentation() && nvUploads == attempts && yuvUploads);
        assert(media.imageDiagnostics().find(nv12Api ? "mediaNv12Fallbacks=1" : "mediaNv12Fallbacks=0") != std::string::npos);
        if (!nv12Api) assert(media.imageDiagnostics().find("mediaNv12Available=0") != std::string::npos);
        synthetic = false; feed(media, bytes); assert(media.preparePresentation()); draw(media);
    }
    assert(textures.empty());
    workerPresentation(renderer, codec, bytes);
    assert(textures.empty());
    assert(FOGPLAY_MEDIA_NV12 && nv12Api ? nvCreates && nvUploads : !nvCreates && !nvUploads);
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
    std::cout << "PASS media experiments codec=" << stream::videoCodecName(codec) << " NV12=" << FOGPLAY_MEDIA_NV12 << " pool=" << FOGPLAY_MEDIA_CPU_POOL <<
        " textures=" << FOGPLAY_MEDIA_TEXTURES << " depth=" << FOGPLAY_MEDIA_READY_DEPTH << " nvApi=" << nv12Api <<
        ": odd/padded/negative planes, exact UV, fallback create/upload/fatal retry, retained texture, ring, reset/codec/stop, pool ownership, latest cadence, sample parity\n";
}
