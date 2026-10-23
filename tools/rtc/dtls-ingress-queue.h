// FogPlay DTLS ingress: bounded FIFO admission. Never waits for queue capacity.
#ifndef RTC_IMPL_DTLS_INGRESS_QUEUE_H
#define RTC_IMPL_DTLS_INGRESS_QUEUE_H
#include "message.hpp"
#include "rtc/dtlsdiagnostics.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>

namespace rtc::impl {
class DtlsIngressQueue {
public:
    static constexpr size_t Capacity = 1024;
    static constexpr size_t ControlReserve = 64;
    enum class Kind { Rtp, Control, Invalid };
    static Kind classify(const message_ptr &message) {
        if (!message || message->empty()) return Kind::Invalid;
        const auto first = std::to_integer<unsigned char>((*message)[0]);
        if (first >= 20 && first <= 63)
            return message->size() >= 13 ? Kind::Control : Kind::Invalid; // DTLS header
        if (first >= 128 && first <= 191 && message->size() >= 2) {
            const auto type = std::to_integer<unsigned char>((*message)[1]);
            // RFC5761: the full second byte, including RTP marker bit.
            if (type >= 192 && type <= 223)
                return message->size() >= 8 ? Kind::Control : Kind::Invalid; // SRTCP header
            return message->size() >= 12 ? Kind::Rtp : Kind::Invalid;
        }
        return Kind::Invalid;
    }
    // Signature mirrors Queue only to keep backend constructors uniform.
    DtlsIngressQueue(size_t, std::function<size_t(const message_ptr &)>, bool) {}
    void push(message_ptr message) {
        const auto kind = classify(message);
        std::lock_guard lock(mMutex);
        if (mStopping || mOverflow) { ++mDiagnostics.admissionStopped; return; }
        if (kind == Kind::Invalid) { ++mDiagnostics.admissionInvalid; return; }
        if (kind == Kind::Rtp && (mMedia >= Capacity - ControlReserve || mQueue.size() >= Capacity)) {
            ++mDiagnostics.admissionMediaDropped;
            return;
        }
        if (mQueue.size() >= Capacity) {
            const auto media = std::find_if(mQueue.begin(), mQueue.end(),
                [](const Entry &entry) { return entry.kind == Kind::Rtp; });
            if (media == mQueue.end()) {
                ++mDiagnostics.admissionCriticalOverflow;
                mOverflow = true; // receiver publishes terminal failure, never the producer
                return;
            }
            mQueue.erase(media);
            --mMedia;
            ++mDiagnostics.admissionMediaEvicted;
        }
        mQueue.push_back({std::move(message), Clock::now(), kind});
        if (kind == Kind::Rtp) { ++mMedia; ++mDiagnostics.admissionMediaAccepted; }
        else ++mDiagnostics.admissionControlAccepted;
        ++mDiagnostics.enqueued;
        mDiagnostics.queueHighWater = std::max(mDiagnostics.queueHighWater, uint64_t(mQueue.size()));
    }
    optional<message_ptr> pop() {
        std::lock_guard lock(mMutex);
        if (mQueue.empty() || mOverflow) return nullopt;
        auto &entry = mQueue.front();
        const auto age = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - entry.at).count());
        ++mDiagnostics.dequeued;
        mDiagnostics.residenceTotalUs += age;
        mDiagnostics.residenceMaxUs = std::max(mDiagnostics.residenceMaxUs, age);
        if (entry.kind == Kind::Rtp) --mMedia;
        auto message = std::move(entry.message);
        mQueue.pop_front();
        return message;
    }
    void stop() { std::lock_guard lock(mMutex); mStopping = true; }
    bool empty() const { std::lock_guard lock(mMutex); return mQueue.empty(); }
    bool running() const { std::lock_guard lock(mMutex); return !mOverflow && (!mStopping || !mQueue.empty()); }
    bool overflowed() const { std::lock_guard lock(mMutex); return mOverflow; }
    // Called on receiver only, after overflow is observed; promptly frees payloads.
    void discard() { std::lock_guard lock(mMutex); mQueue.clear(); mMedia = 0; }
    DtlsDiagnostics diagnostics() const {
        std::lock_guard lock(mMutex);
        auto d = mDiagnostics;
        d.available = true;
        d.queueDepth = mQueue.size();
        if (!mQueue.empty()) d.queueOldestAgeUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - mQueue.front().at).count());
        return d;
    }
private:
    using Clock = std::chrono::steady_clock;
    struct Entry { message_ptr message; Clock::time_point at; Kind kind; };
    mutable std::mutex mMutex;
    std::deque<Entry> mQueue;
    size_t mMedia = 0;
    bool mStopping = false, mOverflow = false;
    DtlsDiagnostics mDiagnostics;
};
} // namespace rtc::impl
#endif
