#pragma once
#include "features/streaming/domain/network_telemetry.hpp"
#include "features/streaming/infrastructure/network_snapshot_slot.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include <rtc/rtcpreceivingsession.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace stream {
// One worker owns paced regular RTCP for the two receive tracks. Stop admission
// is immediate; joining may wait for one already-running transport send and is
// deliberately separate so the rental HTTP termination can run first.
class RtcpFeedbackWorker final {
public:
    using Receiver = rtc::RtcpReceivingSession;
    using Clock = std::chrono::steady_clock;
    struct Snapshot {
        Receiver::FeedbackDiagnostics feedback;
        std::uint64_t flushCalls = 0, flushTotalUs = 0, flushMaxUs = 0, failures = 0;
        std::uint64_t rejectedSessions = 0;
        std::size_t sessions = 0;
        std::uint64_t inFlightMs = 0;
        bool running = false, stopped = false, inFlight = false;
        // Feedback counters are a worker observation, never an SDK caller query.
        bool feedbackAvailable = false, feedbackFinal = false, feedbackStale = true, feedbackAgeKnown = false, workerActive = false;
        std::uint64_t feedbackCapturedAtUs = 0, feedbackRevision = 0, feedbackAgeUs = 0;
        std::string text() const {
            std::ostringstream out;
            out << "scope=transport-lifetime sessions=" << sessions << " running=" << running
                << " stopped=" << stopped << " inFlight=" << inFlight << " inFlightMs=" << inFlightMs
                << " rtp=" << feedback.rtpPackets << " rtcp=" << feedback.rtcpPackets
                << " sr=" << feedback.senderReports << " malformed=" << feedback.malformedControl
                << " requests=" << feedback.regularRequests << " coalesced=" << feedback.coalescedRequests
                << " discarded=" << feedback.discardedReports << " pending=" << feedback.pending
                << " attempts=" << feedback.sendAttempts << " sent=" << feedback.sentPackets
                << " rr=" << feedback.rrPackets << " remb=" << feedback.rembPackets
                << " compound=" << feedback.compoundPackets << " sendFailures=" << feedback.sendFailures
                << " pendingAgeMaxUs=" << feedback.pendingAgeMaxUs
                << " sendAvgUs=" << (feedback.sendAttempts ? feedback.sendTotalUs / feedback.sendAttempts : 0)
                << " sendMaxUs=" << feedback.sendMaxUs
                << " workerCalls=" << flushCalls << " workerAvgUs=" << (flushCalls ? flushTotalUs / flushCalls : 0)
                << " workerMaxUs=" << flushMaxUs << " workerFailures=" << failures
                << " rejectedSessions=" << rejectedSessions
                << " feedbackAvailable=" << feedbackAvailable << " feedbackFinal=" << feedbackFinal
                << " feedbackStale=" << feedbackStale << " feedbackAgeUs=" << (feedbackAgeKnown ? std::to_string(feedbackAgeUs) : "-1")
                << " feedbackCapturedAtUs=" << feedbackCapturedAtUs << " feedbackRevision=" << feedbackRevision
                << " workerActive=" << workerActive;
            return out.str();
        }
    };
    RtcpFeedbackWorker() : state_(std::make_shared<State>()) {}
    ~RtcpFeedbackWorker() { requestStop(); join(); }
    RtcpFeedbackWorker(const RtcpFeedbackWorker&) = delete;
    RtcpFeedbackWorker& operator=(const RtcpFeedbackWorker&) = delete;

    bool addSession(const std::shared_ptr<Receiver>& session) {
        if (!session) return false;
        const auto state = state_;
        bool accepted = false;
        {
            std::lock_guard lock(state->mutex);
            if (!state->stop) {
                for (std::size_t i = 0; i < state->count; ++i)
                    if (state->sessions[i] == session) return true;
                if (state->count < state->sessions.size()) {
                    state->sessions[state->count++] = session;
                    accepted = true;
                } else ++state->stats.rejectedSessions;
            }
        }
        if (!accepted) session->discardPendingReports();
        state->changed.notify_all();
        return accepted;
    }
    void start() {
        const auto state = state_;
        std::lock_guard ownerLock(threadMutex_);
        {
            std::lock_guard lock(state->mutex);
            if (state->stop || state->started) return;
            state->started = true;
            state->stats.running = true;
        }
        try { thread_ = std::thread([state] { run(state); }); }
        catch (...) {
            std::lock_guard lock(state->mutex);
            state->started = false;
            state->stats.running = false;
            throw;
        }
    }
    void requestStop() {
        const auto state = state_;
        std::array<std::shared_ptr<Receiver>, 2> sessions;
        std::size_t count;
        {
            std::lock_guard lock(state->mutex);
            state->stop = true;
            sessions = state->sessions;
            count = state->count;
        }
        for (std::size_t i = 0; i < count; ++i) sessions[i]->discardPendingReports();
        state->changed.notify_all();
    }
    bool quiescent() const {
        std::array<std::shared_ptr<Receiver>, 2> sessions;
        std::size_t count;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->stop) return false;
            sessions = state_->sessions;
            count = state_->count;
        }
        // requestStop seals every receiver before returning. Poll-loop bookkeeping
        // is not an in-flight transport send and must not forbid safe reconnection.
        for (std::size_t i = 0; i < count; ++i)
            if (sessions[i]->getFeedbackDiagnostics().inFlight) return false;
        return true;
    }
    void join() {
        std::lock_guard ownerLock(threadMutex_);
        if (thread_.joinable()) thread_.join();
        else {
            std::lock_guard lock(state_->mutex);
            if(state_->stop&&!state_->started)state_->cache.closeWithoutFinal(1,0);
        }
    }
    using CachedRead = telemetry::network::SnapshotSlot<telemetry::network::RtcpSnapshot>::Read;
    struct NumericRead : CachedRead {
        // Worker activity includes SDK sampling/bookkeeping, not proof of send.
        bool workerActive = false, workerStarted = false, workerRunning = false, stopRequested = false;
        std::uint64_t workerActiveMs = 0;
    };
    static constexpr std::uint64_t SnapshotStaleAfterUs = 1000000;
    NumericRead numericSnapshot(std::uint64_t nowUs) const {
        NumericRead out;
        static_cast<CachedRead&>(out)=state_->cache.read(nowUs,SnapshotStaleAfterUs);
        std::lock_guard lock(state_->mutex);out.workerActive=state_->stats.inFlight;
        out.workerStarted=state_->started;out.workerRunning=state_->stats.running;out.stopRequested=state_->stop;
        if(out.workerActive)out.workerActiveMs=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-state_->flightStarted).count());
        return out;
    }
    Snapshot snapshot() const {
        const auto state=state_;
        const auto observed=state->cache.read(steadyUs(),SnapshotStaleAfterUs);
        Snapshot result;
        {
            std::lock_guard lock(state->mutex);
            result=state->stats;result.sessions=state->count;result.stopped=state->stop;result.workerActive=state->stats.inFlight;
        }
        if(observed.hasValue){
            const auto& part=observed.value.feedback;
#define FOG_RTCP_COPY(name) result.feedback.name=part.name
            FOG_RTCP_COPY(rtpPackets);FOG_RTCP_COPY(rtcpPackets);FOG_RTCP_COPY(senderReports);
            FOG_RTCP_COPY(malformedControl);FOG_RTCP_COPY(regularRequests);FOG_RTCP_COPY(coalescedRequests);
            FOG_RTCP_COPY(discardedReports);FOG_RTCP_COPY(sendAttempts);FOG_RTCP_COPY(sentPackets);
            FOG_RTCP_COPY(sendFailures);FOG_RTCP_COPY(rrPackets);FOG_RTCP_COPY(rembPackets);
            FOG_RTCP_COPY(compoundPackets);FOG_RTCP_COPY(sendTotalUs);FOG_RTCP_COPY(pendingAgeMaxUs);
            FOG_RTCP_COPY(sendMaxUs);FOG_RTCP_COPY(pending);FOG_RTCP_COPY(inFlight);
#undef FOG_RTCP_COPY
        }
        result.inFlight=result.feedback.inFlight;
        result.inFlightMs=0; // No unsampled current-send duration is invented from worker activity.
        result.feedbackAvailable=observed.hasValue;
        result.feedbackFinal=observed.publication==telemetry::network::Publication::FinalRetained;
        result.feedbackCapturedAtUs=observed.capturedAtUs;result.feedbackRevision=observed.revision;
        result.feedbackAgeKnown=observed.ageKnown;result.feedbackAgeUs=observed.ageUs;result.feedbackStale=observed.stale;
        return result;
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::array<std::shared_ptr<Receiver>, 2> sessions;
        std::size_t count = 0;
        bool stop = false, started = false;
        Snapshot stats;
        Clock::time_point flightStarted{};
        telemetry::network::SnapshotSlot<telemetry::network::RtcpSnapshot> cache;
        std::uint64_t nextPublicationUs = 0; // feedback worker only
        State() { cache.reset(1,0); }

    };
    static std::uint64_t steadyUs() {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
    }
    static void publish(const std::shared_ptr<State>& state,bool final=false) noexcept {
        const auto sampledAt=steadyUs();
        if(!final&&sampledAt<state->nextPublicationUs)return;
        state->nextPublicationUs=sampledAt+250000;
        std::array<std::shared_ptr<Receiver>,2> sessions;
        telemetry::network::RtcpSnapshot result;
        {
            std::lock_guard lock(state->mutex);
            sessions=state->sessions;result.sessions=state->count;
            const auto& stats=state->stats;
            result.flushCalls=stats.flushCalls;result.flushTotalUs=stats.flushTotalUs;result.flushMaxUs=stats.flushMaxUs;
            result.failures=stats.failures;result.rejectedSessions=stats.rejectedSessions;
            result.running=stats.running;result.stopped=state->stop;
        }
        try {
            // Only the existing feedback worker calls these SDK telemetry getters.
            // No SDK query runs under the publication/state mutex.
            for(std::size_t i=0;i<result.sessions;++i){
                const auto part=sessions[i]->getFeedbackDiagnostics();
#define FOG_RTCP_SUM(name) result.feedback.name+=part.name
                FOG_RTCP_SUM(rtpPackets);FOG_RTCP_SUM(rtcpPackets);FOG_RTCP_SUM(senderReports);
                FOG_RTCP_SUM(malformedControl);FOG_RTCP_SUM(regularRequests);FOG_RTCP_SUM(coalescedRequests);
                FOG_RTCP_SUM(discardedReports);FOG_RTCP_SUM(sendAttempts);FOG_RTCP_SUM(sentPackets);
                FOG_RTCP_SUM(sendFailures);FOG_RTCP_SUM(rrPackets);FOG_RTCP_SUM(rembPackets);
                FOG_RTCP_SUM(compoundPackets);FOG_RTCP_SUM(sendTotalUs);
#undef FOG_RTCP_SUM
                result.feedback.pendingAgeMaxUs=std::max(result.feedback.pendingAgeMaxUs,part.pendingAgeMaxUs);
                result.feedback.sendMaxUs=std::max(result.feedback.sendMaxUs,part.sendMaxUs);
                result.feedback.pending=result.feedback.pending||part.pending;
                result.feedback.inFlight=result.feedback.inFlight||part.inFlight;
            }
            state->cache.publish(1,0,result,steadyUs(),final);
        }catch(...){state->cache.failed(1,0);}
    }
    static void run(const std::shared_ptr<State>& state) noexcept {
        fog::diagnostics::observeThread("rtcp-feedback");
        std::unique_lock lock(state->mutex);
        while (!state->stop) {
            const auto sessions = state->sessions;
            const auto count = state->count;
            state->stats.inFlight = true;
            state->flightStarted = Clock::now();
            lock.unlock();
            publish(state);
            for (std::size_t i = 0; i < count; ++i) {
                {
                    std::lock_guard admission(state->mutex);
                    if (state->stop) break;
                }
                const auto started = Clock::now();
                bool failed = false;
                try { sessions[i]->flushPendingReports(); } catch (...) { failed = true; }
                const auto us = std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
                {
                    std::lock_guard update(state->mutex);
                    ++state->stats.flushCalls;
                    state->stats.flushTotalUs += us;
                    state->stats.flushMaxUs = std::max(state->stats.flushMaxUs, us);
                    if (failed) ++state->stats.failures;
                }
            }
            publish(state);
            lock.lock();
            state->stats.inFlight = false;
            state->changed.notify_all();
            if (!state->stop) state->changed.wait_for(lock, std::chrono::milliseconds(20));
        }
        state->stats.running = false;
        state->stats.inFlight = false;
        lock.unlock();
        publish(state,true);
        state->changed.notify_all();
    }
    std::shared_ptr<State> state_;
    mutable std::mutex threadMutex_;
    std::thread thread_;
};
} // namespace stream
