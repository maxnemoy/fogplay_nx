"""Exercise the real Moonlight pull-renderer queue with deterministic core doubles."""
from pathlib import Path
import re
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/infrastructure/moonlight/moonlight_session.cpp').read_text()


def extract_function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


capacity = re.search(r'static constexpr std::size_t VideoCapacity = \d+;', source)
assert capacity, 'Moonlight State must declare its bounded video queue capacity'
harness = r'''
#include "features/streaming/domain/streaming_client.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

constexpr int DR_OK = 0, DR_NEED_IDR = -1;
constexpr int FRAME_TYPE_IDR = 1, FRAME_TYPE_PFRAME = 2;
struct BUFFER_ENTRY { BUFFER_ENTRY* next; char* data; int length; };
struct DECODE_UNIT {
    int fullLength, frameType;
    std::uint32_t rtpTimestamp;
    BUFFER_ENTRY* bufferList;
};
using PDECODE_UNIT = DECODE_UNIT*;
struct Frame {
    char bytes[2];
    BUFFER_ENTRY second, first;
    DECODE_UNIT unit;
    explicit Frame(unsigned id, bool keyframe) : bytes{char(id), char(id + 1)},
        second{nullptr, &bytes[1], 1}, first{&second, &bytes[0], 1},
        unit{2, keyframe ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME, id, &first} {}
};
using VIDEO_FRAME_HANDLE = Frame*;
struct State {
    CAPACITY_DECLARATION
    std::atomic<bool> stop{false}, failed{false}, terminated{false}, assemblyWaiting{true};
    std::atomic<int> code{0};
    std::atomic<std::uint64_t> frames{0}, videoDrops{0};
    std::atomic<std::uint64_t> videoFullYields{0}, videoQueueHigh{0}, videoBatchMax{0}, videoQueueAgeMaxMs{0};
    std::atomic<std::uint64_t> videoInvalid{0}, videoWaitDrops{0}, videoResyncs{0};
    std::mutex mutex;
    std::deque<stream::Packet> video;
    std::uint64_t generation = 42;
};
struct FakeCore {
    std::deque<std::unique_ptr<Frame>> pending;
    std::unique_ptr<Frame> inFlight;
    unsigned polls = 0;
    std::vector<std::pair<unsigned, int>> completed;
    std::function<void()> onComplete;
    void add(unsigned id, bool keyframe = false, bool malformed = false) {
        auto frame = std::make_unique<Frame>(id, keyframe);
        if (malformed) frame->unit.fullLength = 3;
        pending.push_back(std::move(frame));
    }
} core;
bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* handle, PDECODE_UNIT* unit) {
    ++core.polls;
    assert(!core.inFlight);
    if (core.pending.empty()) return false;
    core.inFlight = std::move(core.pending.front()); core.pending.pop_front();
    *handle = core.inFlight.get(); *unit = &core.inFlight->unit;
    return true;
}
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle, int result) {
    assert(core.inFlight && core.inFlight.get() == handle);
    core.completed.emplace_back(handle->unit.rtpTimestamp, result);
    core.inFlight.reset();
    // Real requestDecoderRefresh discards already queued units. A replacement
    // IDR must arrive later; it cannot survive this flush in the same batch.
    if (result == DR_NEED_IDR) core.pending.clear();
    if (core.onComplete) core.onComplete();
}
std::uint64_t nowMs() { return 123456; }
'''.replace('CAPACITY_DECLARATION', capacity.group())
harness += extract_function('bool copyFrame(') + '\n'
harness += extract_function('void drainVideo(') + '\n'
harness += r'''
void resetCore() { assert(!core.inFlight); core = FakeCore{}; }
void consume(State& state, unsigned first, unsigned count, std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(state.mutex);
    assert(state.video.size() >= count);
    for (unsigned id = first; id < first + count; ++id) {
        const auto& packet = state.video.front();
        assert(packet.kind == stream::Kind::Video && packet.timestamp == id);
        assert(packet.videoGeneration == generation && packet.enqueueMs == nowMs());
        assert(packet.data == std::vector<std::byte>({std::byte(id), std::byte(id + 1)}));
        state.video.pop_front();
    }
}
void assertHealthy(const State& state, unsigned frames) {
    assert(state.frames == frames && state.videoDrops == 0);
    assert(state.videoInvalid == 0 && state.videoWaitDrops == 0 && state.videoResyncs == 0);
    assert(state.generation == 42 && !state.assemblyWaiting && !state.failed && !state.terminated);
    assert(!core.inFlight);
    for (const auto& completion : core.completed) assert(completion.second == DR_OK);
}
void burstPreservesFifo() {
    resetCore(); State state;
    for (unsigned id = 1; id <= 8; ++id) core.add(id, id == 1);
    drainVideo(state);
    assert(state.video.size() == 4 && core.pending.size() == 4 && core.polls == 4);
    assert(state.videoFullYields == 1);
    // A partially drained queue may pull only the number of newly free slots.
    consume(state, 1, 2, 42);
    drainVideo(state);
    assert(state.video.size() == 4 && core.pending.size() == 2 && core.polls == 6);
    assert(state.videoFullYields == 2);
    consume(state, 3, 4, 42);
    drainVideo(state);
    consume(state, 7, 2, 42);
    assert(state.video.empty() && core.pending.empty() && core.completed.size() == 8);
    assert(state.videoQueueHigh == 4 && state.videoBatchMax == 4);
    assertHealthy(state, 8);
}
void fullQueueDoesNotPoll() {
    resetCore(); State state; state.assemblyWaiting = false;
    state.video.resize(State::VideoCapacity);
    core.add(1);
    drainVideo(state);
    drainVideo(state);
    assert(core.polls == 0 && core.pending.size() == 1 && core.completed.empty());
    assert(state.video.size() == State::VideoCapacity && state.videoFullYields == 2);
    assert(state.videoBatchMax == 0 && state.videoDrops == 0 && state.generation == 42);
    assert(!state.assemblyWaiting && state.videoResyncs == 0);
}
void stoppedWorkerReturns() {
    resetCore(); State stopped; stopped.stop = true;
    core.add(1, true);
    drainVideo(stopped);
    assert(core.polls == 0 && core.completed.empty());
    assert(core.pending.size() == 1 && stopped.video.empty() && stopped.videoBatchMax == 0);
    resetCore(); State stopping;
    for (unsigned id = 1; id <= 10; ++id) core.add(id, id == 1);
    core.onComplete = [&] { stopping.stop = true; };
    drainVideo(stopping);
    assert(core.polls == 1 && core.completed.size() == 1 && core.pending.size() == 9);
    assert(stopping.videoBatchMax == 1 && stopping.video.size() == 1);
    assertHealthy(stopping, 1);
}
void oneInvocationHasFiniteBudget() {
    resetCore(); State state;
    for (unsigned id = 1; id <= 12; ++id) core.add(id, id == 1);
    unsigned next = 1;
    // Model a consumer taking every frame between completions, so queue capacity
    // cannot hide a missing per-call worker budget.
    core.onComplete = [&] { consume(state, next++, 1, 42); };
    drainVideo(state);
    assert(core.polls == 8 && core.completed.size() == 8 && core.pending.size() == 4);
    assert(next == 9 && state.video.empty() && state.videoBatchMax == 8 && state.videoQueueHigh == 1);
    drainVideo(state);
    assert(next == 13 && core.pending.empty() && core.completed.size() == 12);
    assert(state.videoBatchMax == 8);
    assertHealthy(state, 12);
}
void invalidFrameWaitsForIdr() {
    resetCore(); State state;
    core.add(1, true); core.add(2); core.add(3, false, true);
    drainVideo(state);
    assert(state.video.empty() && state.assemblyWaiting && state.generation == 43);
    assert(state.videoDrops == 3 && state.videoInvalid == 1 && state.videoResyncs == 1);
    assert(core.completed.size() == 3 && core.completed[2].second == DR_NEED_IDR);
    // Dependent frames cannot restart decoding; another malformed IDR must not
    // create a second generation change while the same recovery is active.
    core.add(4);
    drainVideo(state);
    core.add(5, true, true);
    drainVideo(state);
    assert(state.video.empty() && state.assemblyWaiting && state.generation == 43);
    assert(state.videoDrops == 5 && state.videoInvalid == 2 && state.videoWaitDrops == 1);
    assert(state.videoResyncs == 1 && core.completed[3].second == DR_NEED_IDR);
    assert(core.completed[4].second == DR_NEED_IDR);
    core.add(6, true); core.add(7);
    drainVideo(state);
    consume(state, 6, 2, 43);
    assert(!state.assemblyWaiting && state.generation == 43 && state.frames == 4);
    assert(state.videoResyncs == 1 && state.videoDrops == 5 && !state.failed);
    assert(core.completed.size() == 7 && core.completed[5].second == DR_OK && core.completed[6].second == DR_OK);
    assert(!core.inFlight);
}
void startupRequiresIdr() {
    resetCore(); State state;
    core.add(1); core.add(99, true);
    drainVideo(state);
    assert(core.pending.empty() && state.video.empty() && core.completed.size() == 1);
    core.add(2, true); core.add(3);
    drainVideo(state);
    consume(state, 2, 2, 42);
    assert(state.videoWaitDrops == 1 && state.videoDrops == 1 && state.videoInvalid == 0);
    assert(state.videoResyncs == 0 && state.generation == 42 && !state.assemblyWaiting);
    assert(core.completed[0].second == DR_NEED_IDR && core.completed[1].second == DR_OK);
}
int main() {
    assert(State::VideoCapacity == 4);
    burstPreservesFifo();
    fullQueueDoesNotPoll();
    stoppedWorkerReturns();
    oneInvocationHasFiniteBudget();
    invalidFrameWaitsForIdr();
    startupRequiresIdr();
    std::cout << "Moonlight video queue: 6 scenarios PASS (real copyFrame/drainVideo, fake core, real Packet)\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-moonlight-video-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I' + str(root / 'src'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
