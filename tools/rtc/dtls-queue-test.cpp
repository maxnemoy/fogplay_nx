#include "impl/queue.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

int main() {
    rtc::impl::Queue<int> plain(2);
    plain.push(1);
    assert(!plain.diagnostics().available);
    assert(plain.peek() == 1 && plain.exchange(2) == 1 && plain.pop() == 2);

    std::atomic<int64_t> secondAcceptedAt{0};
    rtc::impl::Queue<int> queue(1, [&](const int& value) {
        int64_t unset = 0;
        if (value == 2) secondAcceptedAt.compare_exchange_strong(unset,
            std::chrono::steady_clock::now().time_since_epoch().count());
        return size_t(1);
    }, true);
    queue.push(1);
    std::atomic<bool> started{false}, complete{false};
    std::thread producer([&] {
        started = true;
        queue.push(2);
        complete = true;
    });
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(60ms);
    assert(!complete.load());
    const auto occupied = queue.diagnostics();
    assert(occupied.queueDepth == 1 && occupied.queueHighWater == 1);
    assert(occupied.queueOldestAgeUs >= 50000);
    assert(occupied.fullPushWaitCount == 0); // completed waits only
    assert(queue.peek() == 1 && queue.pop() == 1);
    producer.join();
    const auto accepted = queue.diagnostics();
    assert(accepted.enqueued == 2 && accepted.dequeued == 1);
    assert(accepted.fullPushWaitCount == 1 && accepted.fullPushWaitTotalUs >= 40000);
    // The amount function runs under the queue lock after full-queue wait.
    // Compare to that independent acceptance instant to detect a timestamp
    // mistakenly captured before the blocked push, without a wall-time limit.
    assert(queue.pop() == 2);
    const auto drained = queue.diagnostics();
    const auto acceptedTime = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{secondAcceptedAt.load()}};
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - acceptedTime).count();
    const auto secondResidence = drained.residenceTotalUs - accepted.residenceTotalUs;
    assert(secondResidence <= uint64_t(elapsed) + 1000);
    assert(drained.queueDepth == 0 && drained.queueOldestAgeUs == 0);
    assert(drained.queueHighWater == 1 && drained.dequeued == 2);
    assert(drained.residenceMaxUs >= 50000);
    assert(drained.fullPushWaitMaxUs == drained.fullPushWaitTotalUs);

    queue.push(3);
    started = false;
    std::thread stoppedProducer([&] { started = true; queue.push(4); });
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(20ms);
    queue.stop();
    stoppedProducer.join();
    assert(queue.diagnostics().enqueued == 3);
    assert(queue.diagnostics().fullPushWaitCount == 2);
    assert(queue.pop() == 3 && !queue.pop());
    assert(!queue.running());
    std::puts("DTLS queue: occupancy, residence, accepted timestamp, full wait, stop wakeup, disabled semantics PASS");
}
