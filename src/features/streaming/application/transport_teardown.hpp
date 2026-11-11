#pragma once
#include "core/diagnostics/thread_observation.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace fog::stream {
// Owns the old client until all its callback threads have joined. No transport
// API runs under this mailbox mutex. Construct before opening a paid session:
// thread creation failure must not force a synchronous fallback during stop.
template<class Client>
class TransportTeardown {
public:
    struct Result {
        std::unique_ptr<Client> client;
        bool retry = false;
        bool allowed = false;
    };
    TransportTeardown() : worker_([this] { run(); }) {}
    ~TransportTeardown() { finish(); }
    TransportTeardown(const TransportTeardown&) = delete;
    TransportTeardown& operator=(const TransportTeardown&) = delete;

    bool begin(std::unique_ptr<Client>& client, bool retry) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || client_ || !client) return false;
        client_ = std::move(client);
        retry_ = retry; allowed_ = false; done_ = false;
        changed_.notify_one();
        return true;
    }
    bool pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bool(client_);
    }
    Result take() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!done_) return {};
        done_ = false;
        return {std::move(client_), retry_, allowed_};
    }
    // Shutdown only, after cancellation and dispatch of any required rental
    // stop. A rejected retry may still own a non-cancellable transport call.
    Result finishRetained() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            changed_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
        return take();
    }
    void finish() noexcept { auto retained=finishRetained(); }
private:
    void run() {
        fog::diagnostics::observeThread("transport-teardown");
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            changed_.wait(lock, [this] { return stopping_ || (client_ && !done_); });
            if (!client_ || done_) {
                if (stopping_) return;
                continue;
            }
            auto* client = client_.get();
            const bool retry = retry_;
            lock.unlock();
            bool allowed = false;
            try {
                if (retry) allowed = client->disconnectForRetry();
                else { client->close(); allowed = true; }
            } catch (...) { /* Ownership returns to the caller for final close. */ }
            lock.lock();
            allowed_ = allowed;
            done_ = true;
            if (stopping_) return;
        }
    }
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::unique_ptr<Client> client_;
    bool retry_ = false, allowed_ = false, done_ = false, stopping_ = false;
    std::thread worker_;
};
}
