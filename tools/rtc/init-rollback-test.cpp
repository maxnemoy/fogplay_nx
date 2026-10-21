// Linux host regression for startup rollback and subsequent RTC reuse.
#include "rtc/rtc.hpp"
#include "impl/threadpool.hpp"
#include "impl/pollservice.hpp"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <dirent.h>
#include <dlfcn.h>
#include <iostream>
#include <limits>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
using namespace std::chrono_literals;
namespace {
std::atomic<bool> failPipe{false};
std::atomic<int> failThreadAt{0};
std::atomic<int> threadCalls{0};
void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
int entries(const char *path) {
    auto *dir = opendir(path);
    require(dir != nullptr, std::string("opendir failed: ") + path);
    int count = 0;
    while (auto *entry = readdir(dir))
        if (entry->d_name[0] != '.') ++count;
    closedir(dir);
    return count;
}
void requireBaseline(int threads, int descriptors) {
    // Cleanup signals its promise just before its detached thread exits.
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (entries("/proc/self/task") == threads &&
            entries("/proc/self/fd") == descriptors) return;
        std::this_thread::sleep_for(5ms);
    }
    require(entries("/proc/self/task") == threads, "live threads remain after cleanup");
    require(entries("/proc/self/fd") == descriptors, "file descriptors remain after cleanup");
}
void cleanup() {
    auto completion = rtc::Cleanup();
    require(completion.wait_for(5s) == std::future_status::ready, "cleanup did not complete");
    rtc::JoinCleanupThreads();
    completion.get();
}
void verifyReinitialization(int threads, int descriptors) {
    rtc::Preload();
    require(rtc::impl::ThreadPool::Instance().count() == 2, "worker pool was not reinitialized");
    auto work = rtc::impl::ThreadPool::Instance().enqueue([] { return 42; });
    require(work.wait_for(2s) == std::future_status::ready && work.get() == 42,
            "reinitialized workers did not execute a task");
    int fds[2];
    require(pipe(fds) == 0, "test pipe failed");
    std::promise<void> readable;
    std::atomic<bool> fired{false};
    auto event = readable.get_future();
    rtc::impl::PollService::Instance().add(fds[0],
        {rtc::impl::PollService::Direction::In, 2s,
         [&readable, &fired](rtc::impl::PollService::Event value) {
             if (value == rtc::impl::PollService::Event::In && !fired.exchange(true))
                 readable.set_value();
         }});
    require(write(fds[1], "x", 1) == 1, "test wakeup write failed");
    const bool pollWorked = event.wait_for(2s) == std::future_status::ready;
    rtc::impl::PollService::Instance().remove(fds[0]);
    close(fds[0]);
    close(fds[1]);
    require(pollWorked, "reinitialized poll service did not deliver readiness");
    cleanup();
    requireBaseline(threads, descriptors);
}
void verifyFailure(const char *label, int threadFailure, bool pipeFailure,
                   int threads, int descriptors, bool invalidSctp = false) {
    if (invalidSctp) {
        rtc::SctpSettings settings;
        settings.recvBufferSize = std::numeric_limits<size_t>::max();
        rtc::SetSctpSettings(settings);
    }
    threadCalls = 0;
    failThreadAt = threadFailure;
    failPipe = pipeFailure;
    bool threw = false;
    try { rtc::Preload(); }
    catch (const std::exception &error) {
        threw = true;
        std::cout << label << ": " << error.what() << '\n';
    }
    failThreadAt = 0;
    failPipe = false;
    if (invalidSctp) rtc::SetSctpSettings({});
    require(threw, std::string(label) + ": injected startup failure was not observed");
    require(rtc::impl::ThreadPool::Instance().count() == 0,
            std::string(label) + ": workers remain immediately after failed startup");
    cleanup();
    requireBaseline(threads, descriptors);
    verifyReinitialization(threads, descriptors);
    std::cout << label << ": rollback, cleanup, and reinitialization passed\n";
}
}
extern "C" int __real_pipe(int descriptors[2]);
extern "C" int __wrap_pipe(int descriptors[2]) {
    if (failPipe) { errno = EMFILE; return -1; }
    return __real_pipe(descriptors);
}
// Interpose libstdc++'s pthread call too; --wrap alone cannot wrap calls in a DSO.
extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                              void *(*entry)(void *), void *argument) {
    using Create = int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static auto create = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (!create) return ENOSYS;
    const int call = ++threadCalls;
    if (failThreadAt > 0 && call == failThreadAt) return EAGAIN;
    return create(thread, attributes, entry, argument);
}
int main() {
    try {
        rtc::InitLogger(rtc::LogLevel::None);
        rtc::SetThreadPoolSize(2);
        const int threads = entries("/proc/self/task");
        const int descriptors = entries("/proc/self/fd");
        verifyFailure("partial worker spawn", 2, false, threads, descriptors);
        verifyFailure("wakeup pipe creation", 0, true, threads, descriptors);
        verifyFailure("poll thread creation", 3, false, threads, descriptors);
        verifyFailure("SCTP settings after global startup", 0, false, threads, descriptors, true);
        std::cout << "RTC initialization rollback regression passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "RTC initialization rollback regression failed: " << error.what() << '\n';
        return 1;
    }
}
