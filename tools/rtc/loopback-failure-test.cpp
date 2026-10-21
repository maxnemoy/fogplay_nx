#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <thread>

namespace {
struct InjectedPeerFailure final : std::exception {
    const char* what() const noexcept override { return "injected live-peer unwind"; }
};

std::atomic<bool> injectPeerFailure{false};
std::atomic<bool> peerFailureReached{false};

void recordLoopback(bool data, bool media, bool websocket) {
    // peerPhase reports while both peers and their callbacks still exist.
    // Throwing here exercises its real exceptional destruction order.
    if (data && media && !websocket && injectPeerFailure.exchange(false)) {
        peerFailureReached = true;
        throw InjectedPeerFailure{};
    }
}
}

#define RTC_LOOPBACK_REPORT(data, media, websocket) recordLoopback((data), (media), (websocket))
#define main loopbackEntryForFailureTest
#include "host-loopback.cpp"
#undef main
#undef RTC_LOOPBACK_REPORT

namespace {
int countEntries(const char* path) {
    DIR* directory = opendir(path);
    if (!directory) return -1;
    int count = 0;
    while (const auto* entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) ++count;
    }
    closedir(directory);
    return count;
}

bool checkBaseline(int tasks, int descriptors, const char* stage) {
    // A ready cleanup promise precedes the cleanup thread's actual return.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int currentTasks = -1, currentDescriptors = -1;
    do {
        currentTasks = countEntries("/proc/self/task");
        currentDescriptors = countEntries("/proc/self/fd");
        if (currentTasks == tasks && currentDescriptors == descriptors) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "%s leaked resources: tasks=%d/%d fds=%d/%d\n",
        stage, currentTasks, tasks, currentDescriptors, descriptors);
    return false;
}
}

int main() {
    const int tasks = countEntries("/proc/self/task");
    const int descriptors = countEntries("/proc/self/fd");
    if (tasks < 0 || descriptors < 0) {
        std::fprintf(stderr, "Linux /proc resource baselines unavailable\n");
        return 1;
    }

    injectPeerFailure = true;
    bool caught = false;
    try {
        (void)loopbackEntryForFailureTest();
    } catch (const InjectedPeerFailure&) {
        caught = true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Unexpected setup failure: %s\n", error.what());
        return 2;
    }
    if (!caught || !peerFailureReached) {
        std::fprintf(stderr, "Peer phase never reached the live-peer injection point\n");
        return 3;
    }
    try {
        if (!cleanupPhase()) {
            std::fprintf(stderr, "Injected-failure cleanup timed out\n");
            return 4;
        }
        if (!checkBaseline(tasks, descriptors, "Injected-failure cleanup")) return 5;

        // Re-enter the exact production loopback, including both serial phases.
        if (loopbackEntryForFailureTest() != 0) {
            std::fprintf(stderr, "Normal loopback did not recover after injection\n");
            return 6;
        }
        if (!checkBaseline(tasks, descriptors, "Recovery cleanup")) return 7;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Cleanup/recovery exception: %s\n", error.what());
        return 8;
    }
    std::printf("PASS live-peer exception unwind, resource cleanup, and full loopback recovery\n");
    return 0;
}
