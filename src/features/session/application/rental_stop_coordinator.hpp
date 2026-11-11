#pragma once
#include "features/session/application/stop_retry_budget.hpp"
#include <chrono>

namespace fog {
// Application-thread policy only. Guards/journals and the shared serialized
// service worker retain their existing ownership. Snapshot only joined guards.
class RentalStopCoordinator {
public:
    using Clock = StopRetryBudget::Clock;
    using Time = Clock::time_point;
    enum class Target { Session, Checkout, Paired };
    enum class Action { None, Pause, Dispatch };
    enum class Completion { Confirmed, Retry, Paused };
    struct Snapshot {
        bool pending = false, paired = false, checkout = false;
    };
    struct Decision {
        Action action = Action::None;
        Target target = Target::Session;
        bool resetCheckoutChecks = false;
    };
    bool requested() const noexcept { return requested_; }
    bool paused() const noexcept { return paused_; }
    bool shutdownRequested() const noexcept { return shutdown_; }
    unsigned attempts() const noexcept { return budget_.attempts(); }
    bool exhausted(Time now) const noexcept { return budget_.exhausted(now); }
    void request() noexcept { requested_ = true; }
    void clearRequest() noexcept { if (!shutdown_) requested_ = false; }
    void shutdown() noexcept { shutdown_ = true; requested_ = true; }
    void pause() noexcept { paused_ = true; }
    // Provider resolution can precede recovery of a different provider's
    // journal. Reset its budget even while the aggregate session stays pending.
    void resolved() noexcept {
        budget_.reset();
        paused_ = false; retryScheduled_ = false; dispatchReserved_ = false;
        clearRequest();
    }
    void newRental() noexcept { if (!shutdown_) resolved(); }
    bool manualRetry() noexcept {
        if (shutdown_ || dispatchReserved_) return false;
        budget_.reset(); paused_ = false; retryScheduled_ = false;
        resetCheckoutChecks_ = true;
        return true;
    }
    // A provider's failPaid can already dispatch stop inside create/poll.
    // Count that implicit first attempt once, without auto-refilling the budget.
    void observedPairedStop(Time now) noexcept {
        request();
        if (!shutdown_ && !budget_.attempts()) budget_.begin(now);
    }
    Decision next(Snapshot snapshot, bool workerBusy, bool exitRequested, Time now) noexcept {
        if (shutdown_ || exitRequested || workerBusy || dispatchReserved_ || !snapshot.pending) return {};
        if (paused_ || !budget_.begin(now)) { pause(); return {Action::Pause}; }
        request();
        dispatchReserved_ = true;
        const auto target = snapshot.paired ? Target::Paired : snapshot.checkout ? Target::Checkout : Target::Session;
        bool resetChecks = false;
        if (target == Target::Checkout) {
            resetChecks = resetCheckoutChecks_;
            resetCheckoutChecks_ = false;
        }
        if (target == Target::Session) scheduleRetry(target, now, exitRequested);
        return {Action::Dispatch, target, resetChecks};
    }
    // Called after get() joined any shared task, or if async dispatch throws.
    // A failed dispatch still consumed its attempt; only the reservation ends.
    void workerJoined() noexcept { dispatchReserved_ = false; }
    void dispatchFailed() noexcept { dispatchReserved_ = false; }
    bool pauseIfExhausted(bool pending, bool workerBusy, Time now) noexcept {
        if (pending && requested_ && !paused_ && !workerBusy && budget_.exhausted(now)) {
            pause(); return true;
        }
        return false;
    }
    void scheduleRetry(Target target, Time now, bool exiting = false) noexcept {
        retryAt_ = now + (target == Target::Paired ? std::chrono::seconds(2) : std::chrono::seconds(exiting ? 1 : 10));
        retryScheduled_ = true;
    }
    bool retryScheduled() const noexcept { return retryScheduled_; }
    bool retryDue(Time now) const noexcept { return retryScheduled_ && !shutdown_ && !paused_ && now >= retryAt_; }
    Completion sessionCompleted(bool terminated, bool journalCleared, Time now, bool exiting = false) noexcept {
        workerJoined();
        if (terminated && journalCleared) { resolved(); return Completion::Confirmed; }
        if (budget_.exhausted(now)) { pause(); return Completion::Paused; }
        scheduleRetry(Target::Session, now, exiting);
        return Completion::Retry;
    }
private:
    StopRetryBudget budget_;
    bool requested_ = false, paused_ = false, shutdown_ = false;
    bool resetCheckoutChecks_ = false, dispatchReserved_ = false, retryScheduled_ = false;
    Time retryAt_{};
};
}
