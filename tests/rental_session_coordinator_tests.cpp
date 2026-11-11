#include "rental_session_fakes.hpp"
#include <iostream>

using namespace rental_test;
using fog::ProviderId;

static void launchAndUnknownOutcome() {
    Fixture f;
    assert(!f.owner->state().pending && f.owner->recoveryAuthScope() == Owner::AuthScope::All);
    assert(!f.owner->beginLaunch({true,false,true}));
    assert(!f.owner->beginLaunch({false,false,false}));
    assert(f.owner->beginLaunch({}));
    assert(f.owner->state().pending && !f.owner->state().durablePending && f.owner->state().launchReserved);
    assert(!f.owner->beginLaunch({}));
    f.owner->request();
    auto cancelled = f.owner->plan(Owner::MtsCreate{f.order()}, {}, f.now);
    assert(cancelled.status == Owner::PlanStatus::Cancelled && !cancelled.work && !f.owner->state().pending && f.mts.creates == 0);
    f.mts.unknownCreate = true;
    auto uncertain = f.run(Owner::MtsCreate{f.order()});
    assert(!uncertain.start && f.owner->state().durablePending && f.mts.creates == 1 && f.disk.checkout.pending);
    assert(!f.owner->plan(Owner::MtsCreate{f.order()}, {}, f.now).work && f.mts.creates == 1);
    auto resolved = f.run(Owner::ManualResolution{});
    assert(resolved.flow == Owner::Flow::ManualResolved && !f.owner->state().pending && f.mts.stops == 0);
    f.owner->shutdown();
    assert(!f.owner->beginLaunch({}) && !f.owner->manualRetry());
    assert(!f.owner->plan(Owner::MtsCreate{f.order()}, {}, f.now).work);
}
static void adoptAndConfirmation() {
    Fixture f(false);
    const auto rental = id(ProviderId::Mts,"rental");
    f.disk.failSessionWrite = true;
    const auto failed = f.run(Owner::Adopt{rental,id(ProviderId::Mts,"computer")});
    assert(failed.error && !failed.start && failed.stopRequired);
    assert(f.owner->state().pending && f.owner->state().sessionOwned && f.owner->state().journalFault);
    f.disk.failSessionWrite = false;
    const auto stopped = f.run(Owner::Stop{});
    assert(stopped.flow == Owner::Flow::SessionConfirmed && !f.owner->state().pending && f.mts.stops == 1);
    const auto adopted = f.run(Owner::Adopt{rental,{}});
    assert(adopted.start && adopted.start->rental == rental && f.disk.session.persistent);
    f.disk.failSessionClear = true;
    const auto retained = f.run(Owner::Stop{});
    assert(retained.flow == Owner::Flow::SessionRetry && f.owner->state().pending && f.owner->state().journalFault);
    assert(!f.owner->retryDue(f.now + 10s - 1ns) && f.owner->retryDue(f.now + 10s));
    f.disk.failSessionClear = false;
    const auto confirmed = f.run(Owner::Stop{});
    assert(confirmed.flow == Owner::Flow::SessionConfirmed && !f.owner->state().pending && !f.owner->requested());
    const auto plan = f.owner->plan(Owner::Adopt{rental,{}}, {}, f.now);
    assert(plan.work);
    auto value = f.owner->execute(*plan.work);
    f.mts.identity.generation++;
    const auto changed = f.owner->completeJoined(std::move(value),{},f.now);
    assert(!changed.start && f.owner->state().pending && !f.owner->state().sessionOwned && f.owner->paused());
}
static void readyAndCheckBudget() {
    Fixture f(false);
    f.run(Owner::MtsCreate{f.order()});
    f.mts.active = {{id(ProviderId::Mts,"rental"),"",{}}};
    assert(!f.run(Owner::MtsPoll{}).start);
    assert(!f.run(Owner::MtsPoll{}).start);
    const auto ready = f.run(Owner::MtsPoll{});
    assert(ready.start && f.owner->state().sessionOwned && f.owner->state().checkoutPending);
    const auto stop = f.run(Owner::Stop{});
    assert(!stop.start && !f.owner->state().pending && f.mts.stops == 1);
    Fixture waiting(false);
    waiting.mts.noHash = true;
    waiting.run(Owner::MtsCreate{waiting.order()});
    for (unsigned i = 0; i < 30; ++i) waiting.run(Owner::MtsPoll{});
    assert(waiting.owner->state().checkout.pollCount == 30 && waiting.mts.polls == 30);
    waiting.owner->request(); waiting.run(Owner::Stop{});
    assert(waiting.mts.polls == 30 && waiting.owner->state().checkout.stopRequested);
    waiting.owner->pause(); assert(waiting.owner->manualRetry());
    waiting.run(Owner::Stop{});
    assert(waiting.mts.polls == 31 && waiting.owner->state().checkout.pollCount == 1);
}
static void sharedSlotAndJoinedSnapshots() {
    Fixture f(false);
    f.mts.noHash = true;
    SerialSlot slot;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    f.mts.createAction = [&] { entered.set_value(); released.wait(); };
    const auto plan = f.owner->plan(Owner::MtsCreate{f.order()}, {}, f.now);
    assert(plan.work);
    std::optional<Owner::WorkerResult> result;
    assert(slot.start([&] { result = f.owner->execute(*plan.work); }));
    assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    // Guard already wrote its journal on the worker. UI sees only its earlier
    // accepted launch reservation until this SAME shared future is joined.
    assert(f.disk.checkout.pending && f.owner->state().pending && !f.owner->state().checkoutPending);
    assert(!slot.start([] {}) && !f.owner->plan(Owner::Stop{}, {slot.busy(),false,true}, f.now).work);
    f.owner->request();
    assert(f.owner->attempts() == 0);
    release.set_value(); slot.join();
    auto completed = f.owner->completeJoined(std::move(*result),{},f.now);
    assert(completed.stopRequired && !completed.start && f.owner->state().checkoutPending);
    auto stopped = f.run(Owner::Stop{});
    assert(!stopped.start && f.owner->attempts() == 1);
    // Auth work uses the identical future. A thrown external task still joins
    // before the owner refreshes account authority; it never starts executor #2.
    Fixture adopted(false);
    adopted.run(Owner::Adopt{id(ProviderId::Mts,"rental"),{}});
    assert(adopted.owner->state().sessionOwned);
    assert(slot.start([&] { adopted.mts.identity.generation++; throw std::runtime_error("synthetic auth failure"); }));
    bool failed = false; try { slot.join(); } catch (...) { failed = true; }
    adopted.owner->externalServiceTaskJoined();
    assert(failed && !adopted.owner->state().sessionOwned && adopted.owner->state().pending);
}
static void pairedResumeAndHandoff() {
    Fixture f;
    const auto created = f.run(Owner::PairedCreate{f.order(ProviderId::FogHub)});
    assert(created.start && f.owner->state().pairedPending && f.fh.creates == 1 && f.pair.completes == 1);
    const auto rental = created.start->rental;
    const auto transport = f.run(Owner::InspectTransport{rental});
    assert(transport.resume && transport.resume->purpose == Owner::ResumePurpose::Transport && transport.resume->status == Owner::ResumeStatus::Ready);
    const auto quality = f.run(Owner::InspectQuality{rental});
    assert(quality.resume && quality.resume->purpose == Owner::ResumePurpose::Quality && quality.resume->status == Owner::ResumeStatus::Ready);
    const auto wrong = f.run(Owner::InspectTransport{id(ProviderId::FogHub,"different")});
    assert(wrong.resume && wrong.resume->status == Owner::ResumeStatus::Refused && f.fh.creates == 1 && f.pair.completes == 1);
    f.fh.inspectionFailure = true;
    const auto temporary = f.run(Owner::InspectTransport{rental});
    assert(temporary.resume && temporary.resume->status == Owner::ResumeStatus::Retry && !temporary.stopRequired);
    f.fh.inspectionFailure = false;
    f.fh.finishTerminal = false;
    f.owner->request();
    for (unsigned i = 0; i < 3; ++i) {
        const auto stop = f.run(Owner::Stop{});
        assert(!stop.start && f.owner->attempts() == i + 1);
        if (i != 2) assert(!f.owner->retryDue(f.now + 2s - 1ns) && f.owner->retryDue(f.now + 2s));
    }
    assert(f.owner->paused() && f.owner->state().pending && f.disk.paired.pending);
    Fixture handoff;
    handoff.disk.session = {id(ProviderId::Mts,"rental"),true,false,{}};
    handoff.disk.paired = {{ProviderId::FogHub,id(ProviderId::FogHub,"account"),id(ProviderId::FogHub,"computer"),id(ProviderId::FogHub,"rental"),fog::PairedCheckoutPhase::Streaming,false,false},true,false};
    handoff.mts.active = {{id(ProviderId::Mts,"rental"),"",{}}};
    handoff.reset();
    assert(handoff.owner->recoveryAuthScope() == Owner::AuthScope::FogHub);
    const auto finish = handoff.run(Owner::Recover{});
    assert(finish.recoverNext && handoff.owner->state().pending && !handoff.owner->state().pairedPending && handoff.owner->attempts() == 0);
    assert(handoff.owner->recoveryAuthScope() == Owner::AuthScope::Mts);
    assert(handoff.run(Owner::Recover{}).stopRequired);
    assert(handoff.run(Owner::Stop{}).flow == Owner::Flow::SessionConfirmed);
}
static void pairedAtomicStopAndFailedDispatch() {
    Fixture f;
    SerialSlot slot;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    f.pair.readyAction = [&] { entered.set_value(); released.wait(); };
    const auto plan = f.owner->plan(Owner::PairedCreate{f.order(ProviderId::FogHub)}, {}, f.now);
    assert(plan.work);
    std::optional<Owner::WorkerResult> result;
    assert(slot.start([&] { result = f.owner->execute(*plan.work); }));
    assert(entered.get_future().wait_for(2s) == std::future_status::ready);
    f.owner->request(); release.set_value(); slot.join();
    const auto cancelled = f.owner->completeJoined(std::move(*result),{},f.now);
    assert(!cancelled.start && f.fh.creates == 0 && !f.owner->state().pending);
    Fixture stop(false);
    stop.run(Owner::Adopt{id(ProviderId::Mts,"rental"),{}});
    const auto failed = stop.owner->plan(Owner::Stop{}, {}, stop.now);
    assert(failed.work && stop.owner->attempts() == 1);
    stop.owner->dispatchFailed();
    assert(stop.owner->requested() && stop.owner->state().pending && stop.owner->attempts() == 1);
    assert(stop.run(Owner::Stop{}).flow == Owner::Flow::SessionConfirmed);
    stop.owner->shutdown(); stop.owner->externalServiceTaskJoined();
    assert(stop.owner->requested() && !stop.owner->manualRetry() && !stop.owner->plan(Owner::Stop{}, {}, stop.now).work);
}
static void implicitStopAndExceptionalCompletion() {
    Fixture f;
    f.pair.completeOk = false; f.fh.finishTerminal = false;
    const auto paidFailure = f.run(Owner::PairedCreate{f.order(ProviderId::FogHub)});
    assert(!paidFailure.start && f.owner->state().pairedPending && f.owner->requested());
    assert(f.fh.creates == 1 && f.fh.stops == 1 && f.owner->attempts() == 1);
    f.run(Owner::PairedPoll{});
    assert(f.owner->attempts() == 1 && f.fh.stops == 1); // implicit stop counted once, POST not replayed.
    const auto plan = f.owner->plan(Owner::Stop{}, {}, f.now);
    assert(plan.work && f.owner->attempts() == 2);
    f.owner->execute(*plan.work);
    const auto failure = f.owner->completeJoined(std::make_exception_ptr(std::runtime_error("synthetic result failure")),{},f.now);
    assert(failure.flow == Owner::Flow::Paused && f.owner->paused() && f.owner->state().pairedPending);
    Fixture recovery(false);
    recovery.disk.session = {id(ProviderId::Mts,"rental"),true,false,{}};
    recovery.mts.activeFailure = true; recovery.reset(false);
    const auto unavailable = recovery.run(Owner::Recover{});
    assert(unavailable.error && unavailable.flow == Owner::Flow::RecoveryUnmatched && recovery.owner->paused() && recovery.owner->state().pending);
    Fixture focus;
    const auto create = focus.owner->plan(Owner::PairedCreate{focus.order(ProviderId::FogHub)}, {}, focus.now);
    assert(create.work);
    auto result = focus.owner->execute(*create.work);
    const auto hidden = focus.owner->completeJoined(std::move(result),{false,false,false},focus.now);
    assert(!hidden.start && hidden.stopRequired && focus.owner->requested());
}
int main() {
    launchAndUnknownOutcome(); adoptAndConfirmation(); readyAndCheckBudget(); sharedSlotAndJoinedSnapshots();
    pairedResumeAndHandoff(); pairedAtomicStopAndFailedDispatch();
    implicitStopAndExceptionalCompletion();
    std::cout << "PASS real rental coordinator: private guards/journals, shared serial slot, immutable joined snapshots, launch reservation, durable grants/confirmation, auth refresh, distinct FH resume, atomic stop, retry/handoff/shutdown\n";
}
