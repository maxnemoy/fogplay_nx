#include "features/streaming/infrastructure/spsc_video_queue.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

static stream::Packet frame(unsigned id, std::size_t bytes = 32) {
    stream::Packet packet;
    packet.timestamp = id; packet.enqueueMs = id + 100;
    packet.videoGeneration = id / 31;
    packet.data.resize(bytes, std::byte(id & 255));
    return packet;
}
int main() {
    {
        stream::SpscVideoQueue queue;
        for (unsigned i = 0; i < queue.Capacity; ++i) assert(queue.push(frame(i)));
        assert(!queue.push(frame(99)) && queue.size() == queue.Capacity);
        assert(queue.age(200) == 100 && queue.bytes() == queue.Capacity * 32);
        for (unsigned i = 0; i < queue.Capacity; ++i) {
            stream::Packet packet;
            assert(queue.pop(packet) && packet.timestamp == i && packet.kind == stream::Kind::Video);
        }
        assert(queue.size() == 0 && queue.bytes() == 0);
        assert(queue.push(frame(1, queue.MaxBytes)));
        assert(!queue.push(frame(2, 1)));
        stream::Packet packet; assert(queue.pop(packet));
        assert(queue.push(frame(3)));
    }
    stream::SpscVideoQueue queue;
    constexpr unsigned Frames = 100000;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> snapshots{0};
    std::thread observer([&] {
        while (!done.load()) {
            assert(queue.size() <= queue.Capacity && queue.bytes() <= queue.MaxBytes);
            (void)queue.age(1000000);
            std::uint64_t generation = 0; (void)queue.oldestGeneration(generation);
            ++snapshots;
        }
    });
    std::thread producer([&] {
        for (unsigned i = 0; i < Frames; ++i)
            while (!queue.push(frame(i))) std::this_thread::yield();
    });
    std::thread consumer([&] {
        for (unsigned i = 0; i < Frames; ++i) {
            stream::Packet packet;
            while (!queue.pop(packet)) std::this_thread::yield();
            assert(packet.timestamp == i && packet.videoGeneration == i / 31 && packet.enqueueMs == i + 100);
            assert(packet.data.size() == 32 && packet.data.front() == std::byte(i & 255));
        }
    });
    producer.join(); consumer.join(); done = true; observer.join();
    assert(queue.size() == 0 && queue.bytes() == 0 && snapshots);
    std::cout << "SPSC video queue: bounded count/bytes, FIFO 100000 frames, generations and concurrent metadata snapshots PASS\n";
}
