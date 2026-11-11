#include "features/session/application/rental_stop_coordinator.hpp"
#include <cassert>
#include <iostream>
#include <utility>

using Stop = fog::RentalStopCoordinator;
using namespace std::chrono_literals;
static constexpr Stop::Snapshot session{true, false, false};
static constexpr Stop::Snapshot checkout{true, false, true};
static constexpr Stop::Snapshot paired{true, true, true};
static void arbitrationAndAdmission() {
    const auto now = Stop::Time{};
    for (const auto snapshot : {session, checkout, paired}) {
        Stop owner;
        assert(owner.next({}, false, false, now).action == Stop::Action::None);
        assert(owner.next(snapshot, true, false, now).action == Stop::Action::None && owner.attempts() == 0);
        assert(owner.next(snapshot, false, true, now).action == Stop::Action::None && owner.attempts() == 0);
        const auto first = owner.next(snapshot, false, false, now);
        assert(first.action == Stop::Action::Dispatch && owner.requested() && owner.attempts() == 1);
        assert(first.target == (snapshot.paired ? Stop::Target::Paired : snapshot.checkout ? Stop::Target::Checkout : Stop::Target::Session));
        // Reservation protects against repeated ticks even before the adapter's
        // shared future becomes visible. No second attempt is consumed.
        owner.request(); owner.request();
        assert(owner.next(snapshot, false, false, now).action == Stop::Action::None && owner.attempts() == 1);
        owner.workerJoined();
        assert(owner.next(snapshot, false, false, now).action == Stop::Action::Dispatch && owner.attempts() == 2);
        owner.dispatchFailed(); // launch failure consumes its attempt.
        assert(owner.attempts() == 2 && owner.requested());
        assert(owner.next(snapshot, false, false, now).action == Stop::Action::Dispatch && owner.attempts() == 3);
        owner.workerJoined();
        assert(owner.next(snapshot, false, false, now).action == Stop::Action::Pause && owner.paused());
        assert(owner.next(snapshot, false, false, now + 24h).action == Stop::Action::Pause && owner.attempts() == 3);
    }
    Stop retainedJournal;
    // Lack of ownership is not proof of termination: a pending corrupt/retained
    // journal with neither checkout nor paired state still selects Session.
    assert(retainedJournal.next(session, false, false, now).target == Stop::Target::Session);
    Stop lateCreate;
    lateCreate.request();
    assert(lateCreate.next(checkout, true, false, now).action == Stop::Action::None);
    const bool readyMayStartPlayer = !lateCreate.requested();
    assert(!readyMayStartPlayer && lateCreate.attempts() == 0);
    lateCreate.workerJoined();
    assert(lateCreate.next(checkout, false, false, now).action == Stop::Action::Dispatch);
}
static void deadlinesAndScheduling() {
    const auto now = Stop::Time{};
    Stop elapsed;
    elapsed.next(session, false, false, now); elapsed.workerJoined();
    assert(!elapsed.exhausted(now + 30s - 1ns));
    assert(elapsed.next(session, false, false, now + 30s).action == Stop::Action::Pause);
    assert(elapsed.attempts() == 1);
    assert(elapsed.manualRetry());
    assert(elapsed.next(session, false, false, now + 31s).action == Stop::Action::Dispatch);
    assert(elapsed.attempts() == 1 && !elapsed.paused());
    Stop timer;
    for (const auto target : {Stop::Target::Paired, Stop::Target::Checkout, Stop::Target::Session}) {
        timer.scheduleRetry(target, now);
        const auto interval = target == Stop::Target::Paired ? 2s : 10s;
        assert(!timer.retryDue(now + interval - 1ns) && timer.retryDue(now + interval));
    }
    timer.scheduleRetry(Stop::Target::Checkout, now, true);
    assert(!timer.retryDue(now + 1s - 1ns) && timer.retryDue(now + 1s));
    timer.pause(); assert(!timer.retryDue(now + 1h));
    assert(timer.manualRetry() && !timer.retryScheduled());
    Stop idleBudget;
    idleBudget.request();
    assert(!idleBudget.pauseIfExhausted(true, false, now + 1h));
    idleBudget.next(session, false, false, now);
    assert(!idleBudget.pauseIfExhausted(true, true, now + 30s));
    idleBudget.workerJoined();
    assert(idleBudget.pauseIfExhausted(true, false, now + 30s) && idleBudget.paused());
}
static void providerHandoffAndManualChecks() {
    const auto now = Stop::Time{};
    Stop provider;
    provider.observedPairedStop(now);
    provider.observedPairedStop(now + 1s);
    assert(provider.attempts() == 1 && provider.requested());
    assert(provider.next(paired, false, false, now + 2s).action == Stop::Action::Dispatch && provider.attempts() == 2);
    provider.workerJoined();
    provider.resolved(); // FH resolved, MTS journal still exists.
    assert(!provider.requested() && !provider.paused() && provider.attempts() == 0);
    assert(provider.next(session, false, false, now + 3s).target == Stop::Target::Session && provider.attempts() == 1);
    Stop manual;
    manual.pause(); assert(manual.manualRetry());
    auto decision = manual.next(paired, false, false, now);
    assert(!decision.resetCheckoutChecks); manual.workerJoined();
    decision = manual.next(session, false, false, now);
    assert(!decision.resetCheckoutChecks); manual.workerJoined();
    decision = manual.next(checkout, false, false, now);
    assert(decision.resetCheckoutChecks); manual.workerJoined();
    manual.resolved();
    assert(!manual.next(checkout, false, false, now).resetCheckoutChecks);
    Stop retainedManual;
    assert(retainedManual.manualRetry());
    retainedManual.newRental(); // Only CheckoutStop consumes the one-shot.
    assert(retainedManual.next(checkout, false, false, now).resetCheckoutChecks);
}
static void confirmationAndShutdown() {
    const auto now = Stop::Time{};
    for (const auto evidence : {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
        Stop owner; owner.next(session, false, false, now);
        assert(owner.sessionCompleted(evidence.first, evidence.second, now) == Stop::Completion::Retry);
        assert(owner.requested() && owner.attempts() == 1 && !owner.retryDue(now + 10s - 1ns));
        assert(owner.retryDue(now + 10s));
        assert(owner.sessionCompleted(true, true, now) == Stop::Completion::Confirmed);
        assert(!owner.requested() && !owner.paused() && owner.attempts() == 0 && !owner.retryScheduled());
    }
    Stop shutdown;
    shutdown.request(); shutdown.shutdown();
    shutdown.clearRequest(); shutdown.resolved(); shutdown.newRental();
    assert(shutdown.requested() && shutdown.shutdownRequested() && !shutdown.manualRetry());
    assert(shutdown.sessionCompleted(true, true, now) == Stop::Completion::Confirmed);
    assert(shutdown.shutdownRequested() && shutdown.requested());
    assert(shutdown.next(paired, false, false, now + 1h).action == Stop::Action::None);
    shutdown.scheduleRetry(Stop::Target::Session, now);
    assert(!shutdown.retryDue(now + 1h));
}
int main() {
    arbitrationAndAdmission(); deadlinesAndScheduling(); providerHandoffAndManualChecks(); confirmationAndShutdown();
    std::cout << "PASS rental stop owner: joined targets/reservation, sticky intent/shutdown, exact budgets, provider timers/implicit attempt/handoff, manual one-shot, journal confirmation\n";
}
