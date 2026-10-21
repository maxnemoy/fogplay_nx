// Host simulation of Switch/newlib's unsupported pthread_detach.
#include "rtc/rtc.hpp"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std::chrono_literals;
namespace {
std::atomic<int> detachCalls{0};
std::atomic<bool> failCreate{false};
std::atomic<bool> failNextJoin{false};
struct StartGate {
    std::promise<void> release;
    std::shared_future<void> ready = release.get_future().share();
};
std::atomic<StartGate*> nextStartGate{nullptr};
struct HeldStart { void *(*entry)(void *); void *argument; StartGate *gate; };
void *heldThreadStart(void *argument) {
    std::unique_ptr<HeldStart> start(static_cast<HeldStart*>(argument));
    start->gate->ready.get();
    return start->entry(start->argument);
}
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int entries(const char *path) {
    auto *directory = opendir(path);
    require(directory != nullptr, "cannot read process counters");
    int count = 0;
    while (auto *entry = readdir(directory))
        if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}
void complete(std::shared_future<void> future) {
    require(future.wait_for(5s) == std::future_status::ready, "cleanup deadline exceeded");
    rtc::JoinCleanupThreads();
    future.get();
}
void joinFailure(int threads, int descriptors) {
    rtc::Preload();
    auto cleanup = rtc::Cleanup();
    require(cleanup.wait_for(5s) == std::future_status::ready, "join-fault cleanup deadline exceeded");
    failNextJoin = true;
    bool rejected = false;
    try { rtc::JoinCleanupThreads(); }
    catch (const std::system_error &) { rejected = true; }
    require(rejected && !failNextJoin, "injected failed join was not reported");
    // The first barrier must retain ownership of the still-joinable thread,
    // allowing a later barrier to join the real handle instead of terminating.
    rtc::JoinCleanupThreads();
    cleanup.get();
    require(entries("/proc/self/task") == threads, "retry barrier left a live thread");
    require(entries("/proc/self/fd") == descriptors, "retry barrier leaked descriptors");
    std::cout << "Cleanup join failure reports error and retains handle for successful retry\n";
}
void overlappingCleanupLock(int threads, int descriptors) {
    StartGate older, newer;
    rtc::Preload();
    nextStartGate = &older;
    auto first = rtc::Cleanup();
    // Reuse initialized globals while the older cleanup has not begun.
    rtc::Preload();
    nextStartGate = &newer;
    auto second = rtc::Cleanup();
    older.release.set_value();
    require(first.wait_for(5s) == std::future_status::ready, "older generation did not clean globals");
    first.get();
    // Globals are now torn down, but the newest cleanup future is pending.
    // Reinitialization must release mMutex while waiting for that future.
    std::promise<void> entered, finished;
    auto started = entered.get_future();
    auto initialized = finished.get_future();
    std::thread initializer([&] {
        entered.set_value();
        try { rtc::Preload(); finished.set_value(); }
        catch (...) { finished.set_exception(std::current_exception()); }
    });
    started.wait();
    require(initialized.wait_for(100ms) == std::future_status::timeout,
            "newest cleanup gate was bypassed");
    newer.release.set_value();
    if (initialized.wait_for(5s) != std::future_status::ready) {
        std::cerr << "Overlapping cleanup deadlocked: init held the mutex needed by pending cleanup\n";
        std::cerr.flush();
        // The injected deadlock cannot be joined. Fail this isolated host process
        // promptly rather than let the external test timeout hide the regression.
        std::_Exit(1);
    }
    initializer.join();
    initialized.get();
    second.get();
    complete(rtc::Cleanup());
    require(entries("/proc/self/task") == threads, "overlap regression left live threads");
    require(entries("/proc/self/fd") == descriptors, "overlap regression leaked descriptors");
    std::cout << "Overlapping cleanup generations release init mutex while waiting for completion\n";
}
void spawnFailure() {
    rtc::Preload();
    failCreate = true;
    auto cleanup = rtc::Cleanup();
    failCreate = false;
    require(cleanup.wait_for(2s) == std::future_status::ready, "failed spawn future never ready");
    rtc::JoinCleanupThreads();
    bool failed = false;
    try { cleanup.get(); } catch (const std::system_error &) { failed = true; }
    require(failed, "failed cleanup spawn was not reported through its future");
    bool rejected = false;
    try { rtc::Preload(); } catch (const std::system_error &) { rejected = true; }
    require(rejected, "failed cleanup allowed unverified generation reuse");
    require(detachCalls == 0, "cleanup attempted pthread_detach");
    std::cout << "Cleanup spawn failure reports error without terminating; reuse rejected\n";
    std::cout.flush();
    // This isolated fault case intentionally leaves globals initialized. The
    // host OS reclaims them; this is not an example of a safe Switch exit.
    std::_Exit(0);
}
}
extern "C" int pthread_detach(pthread_t) {
    ++detachCalls;
    return ENOSYS;
}
extern "C" int pthread_join(pthread_t thread, void **result) {
    using Join = int (*)(pthread_t, void **);
    static auto join = reinterpret_cast<Join>(dlsym(RTLD_NEXT, "pthread_join"));
    if (!join) return ENOSYS;
    if (failNextJoin.exchange(false)) return EINVAL;
    return join(thread, result);
}
extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                              void *(*entry)(void *), void *argument) {
    using Create = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static auto create = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (!create) return ENOSYS;
    if (failCreate) return EAGAIN;
    if (auto *gate = nextStartGate.exchange(nullptr)) {
        auto *held = new HeldStart{entry, argument, gate};
        const int result = create(thread, attributes, heldThreadStart, held);
        if (result) delete held;
        return result;
    }
    return create(thread, attributes, entry, argument);
}
int main(int argc, char **argv) {
    try {
        rtc::InitLogger(rtc::LogLevel::None);
        rtc::SetThreadPoolSize(2);
        if (argc == 2 && std::string(argv[1]) == "--spawn-failure") spawnFailure();
        const int threads = entries("/proc/self/task");
        const int descriptors = entries("/proc/self/fd");
        overlappingCleanupLock(threads, descriptors);
        for (int iteration = 0; iteration < 20; ++iteration) {
            rtc::Preload();
            complete(rtc::Cleanup());
            require(entries("/proc/self/task") == threads, "join barrier left a live thread");
            require(entries("/proc/self/fd") == descriptors, "cleanup leaked descriptors");
        }
        joinFailure(threads, descriptors);
        const int rapid = argc == 2 && std::string(argv[1]) == "--rapid-stress" ? 2000 : 100;
        std::vector<std::shared_future<void>> generations;
        for (int iteration = 0; iteration < rapid; ++iteration) {
            rtc::Preload();
            generations.push_back(rtc::Cleanup());
        }
        for (auto future : generations)
            require(future.wait_for(5s) == std::future_status::ready,
                    "overlapping cleanup generation did not complete");
        rtc::JoinCleanupThreads();
        for (auto future : generations) future.get();
        require(entries("/proc/self/task") == threads, "generation race left a live thread");
        require(entries("/proc/self/fd") == descriptors, "generation race leaked descriptors");
        require(detachCalls == 0, "cleanup attempted unsupported pthread_detach");
        std::cout << "Owned cleanup threads: 20 sequential and " << rapid
                  << " rapid generations passed; detach=0\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Owned cleanup thread regression failed: " << error.what() << '\n';
        return 1;
    }
}
