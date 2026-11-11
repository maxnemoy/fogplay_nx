#include "features/session/application/rental_session_coordinator.hpp"
#include <stdexcept>
#include <utility>

namespace fog {
namespace {
RentalSessionCoordinator::PairedView pairedView(const PairedCheckoutResult& value) {
    return {value.state, value.rental, value.stopRequested, value.endpoint.has_value(), value.message,
        value.preparationStage, value.preparationError, value.requiredRating};
}
}
struct RentalSessionCoordinator::Impl {
    RentalRepository& mts;
    SessionGuard session;
    CheckoutGuard checkout;
    RentalRepository* pairedRepository = nullptr;
    std::unique_ptr<PairedCheckoutGuard> paired;
    RentalStopCoordinator stop;
    State snapshot;
    bool launch = false;
    std::uint64_t serial = 0;
    std::optional<Work> active;
    Impl(MtsDependencies dependencies, std::optional<PairedDependencies> pair, Clock clock)
        : mts(dependencies.repository), session(mts, std::move(dependencies.sessionJournal)),
          checkout(mts, session, std::move(dependencies.checkoutJournal)) {
        if (pair) {
            pairedRepository = &pair->repository;
            paired = std::make_unique<PairedCheckoutGuard>(pair->repository, pair->pairing, pair->pins,
                std::move(pair->journal), std::move(clock));
        }
        refresh();
    }
    void publishPending() noexcept {
        snapshot.launchReserved = launch;
        snapshot.pending = snapshot.durablePending || launch;
    }
    // Initial construction or after the ONLY shared task joined. No worker
    // writes this UI snapshot, and UI never samples a live guard during work.
    void refresh() {
        snapshot.pairedSupported = bool(paired);
        snapshot.pairedPending = paired && paired->unresolved();
        snapshot.checkoutPending = checkout.unresolved();
        snapshot.sessionPending = session.persistent() || !session.pendingRental().empty() || !session.journalError().empty();
        snapshot.journalFault = !session.journalError().empty();
        snapshot.sessionOwned = session.ownsSession();
        snapshot.mtsAuthenticated = mts.auth().authenticated();
        snapshot.pairedAuthenticated = pairedRepository && pairedRepository->auth().authenticated();
        snapshot.checkout = checkout.result();
        snapshot.paired = paired ? pairedView(paired->result()) : PairedView{};
        snapshot.durablePending = snapshot.pairedPending || snapshot.checkoutPending || snapshot.sessionPending;
        publishPending();
    }
    Plan reserve(Operation operation, Request request, bool reset = false) {
        Work work(operation, std::move(request), ++serial, reset);
        active = work;
        return {PlanStatus::Dispatch, std::move(work)};
    }
    void signalStop() noexcept { stop.request(); if (paired) paired->requestStop(); }
    Effects finish(Operation operation, const Value* value, std::exception_ptr error, Admission admission, Time now) {
        const auto command = active;
        active.reset(); stop.workerJoined();
        refresh();
        Effects effects; effects.operation = operation; effects.error = error;
        if (snapshot.pending && (!admission.focused || admission.exiting)) signalStop();
        const bool canStart = admission.focused && !admission.exiting && !stop.shutdownRequested() && !stop.requested() && !stop.paused();
        if (operation == Operation::ListSessions) {
            effects.flow = Flow::ActiveSessions;
            if (!error && value) effects.sessions = std::get<std::vector<Rental>>(*value);
            return effects;
        }
        if (operation == Operation::Adopt) {
            launch = false; publishPending();
            if (!error && value && canStart && snapshot.sessionOwned && !snapshot.journalFault) {
                const auto& adopted = std::get<Adopted>(*value);
                effects.start = StartGrant{adopted.rental, adopted.computer, false};
            } else if (snapshot.durablePending) {
                effects.stopRequired = snapshot.sessionOwned;
                effects.flow = effects.stopRequired ? Flow::None : Flow::Paused;
                if (effects.stopRequired) signalStop(); else stop.pause();
            } else effects.flow = Flow::LaunchCancelled;
            return effects;
        }
        if (operation == Operation::Recover) {
            if (!error && value && std::get<Matched>(*value).matched) { signalStop(); effects.stopRequired = true; }
            else { stop.pause(); effects.flow = Flow::RecoveryUnmatched; }
            return effects;
        }
        if (operation == Operation::SessionStop) {
            FinishResult result;
            if (!error && value) result = std::get<FinishResult>(*value);
            const auto completion = stop.sessionCompleted(result.terminated, result.journalCleared, now, admission.exiting);
            if (completion == RentalStopCoordinator::Completion::Confirmed) effects.flow = Flow::SessionConfirmed;
            else if (completion == RentalStopCoordinator::Completion::Paused) effects.flow = Flow::Paused;
            else effects.flow = Flow::SessionRetry;
            return effects;
        }
        const bool isPaired = operation == Operation::PairedCreate || operation == Operation::PairedPoll ||
            operation == Operation::PairedStop || operation == Operation::TransportInspection || operation == Operation::QualityInspection;
        if (isPaired) {
            launch = false; publishPending();
            effects.flow = Flow::PairedCheckout;
            if (!snapshot.pairedPending) {
                stop.resolved();
                effects.recoverNext = snapshot.durablePending;
                return effects;
            }
            if (snapshot.paired.stopRequested) stop.observedPairedStop(now);
            if (error) {
                if (operation == Operation::PairedStop) { stop.pause(); effects.flow = Flow::Paused; }
                else effects.stopRequired = stop.requested();
                return effects;
            }
            if ((operation == Operation::TransportInspection || operation == Operation::QualityInspection) && command) {
                const auto expected = operation == Operation::TransportInspection ?
                    std::get<InspectTransport>(command->request_).expected : std::get<InspectQuality>(command->request_).expected;
                auto status = ResumeStatus::Refused;
                if (canStart && !snapshot.paired.stopRequested && snapshot.paired.rental == expected) {
                    if (snapshot.paired.state == PairedCheckoutState::Ready &&
                        (operation == Operation::QualityInspection || snapshot.paired.endpointReady)) status = ResumeStatus::Ready;
                    else if (operation == Operation::TransportInspection && snapshot.paired.state == PairedCheckoutState::Preparing) status = ResumeStatus::Retry;
                }
                effects.resume = ResumeVerdict{operation == Operation::TransportInspection ? ResumePurpose::Transport : ResumePurpose::Quality, status, expected};
                return effects;
            }
            if (snapshot.paired.state == PairedCheckoutState::Ready && canStart && !snapshot.paired.stopRequested)
                effects.start = StartGrant{snapshot.paired.rental, {}, false};
            else if (stop.requested() && !snapshot.paired.stopRequested) effects.stopRequired = true;
            else if (stop.requested() || snapshot.paired.stopRequested) {
                if (stop.exhausted(now)) { stop.pause(); effects.flow = Flow::Paused; }
                else stop.scheduleRetry(RentalStopCoordinator::Target::Paired, now);
            }
            return effects;
        }
        launch = false; publishPending();
        effects.flow = operation == Operation::ManualResolution ? Flow::ManualResolved : Flow::MtsCheckout;
        if (!snapshot.durablePending) { stop.resolved(); return effects; }
        if (error) {
            if (stop.requested() || snapshot.checkout.stopRequested)
                stop.scheduleRetry(RentalStopCoordinator::Target::Checkout, now, admission.exiting);
            effects.stopRequired = stop.requested() && operation != Operation::CheckoutStop;
            return effects;
        }
        if (snapshot.checkout.state == CheckoutState::Ready && canStart && !snapshot.checkout.stopRequested) {
            if (snapshot.sessionOwned && !snapshot.journalFault) effects.start = StartGrant{snapshot.checkout.rentalId, {}, true};
            else { stop.pause(); effects.flow = Flow::Paused; }
        }
        else if ((stop.requested() || snapshot.checkout.stopRequested) && stop.exhausted(now)) {
            stop.pause(); effects.flow = Flow::Paused;
        } else if (snapshot.checkout.stopRequested) stop.scheduleRetry(RentalStopCoordinator::Target::Checkout, now, admission.exiting);
        else if (stop.requested() || (snapshot.checkout.state == CheckoutState::RecoveryRequired && !snapshot.checkout.rentalId.empty())) {
            signalStop(); effects.stopRequired = true;
        }
        return effects;
    }
};
RentalSessionCoordinator::RentalSessionCoordinator(MtsDependencies mts, std::optional<PairedDependencies> paired, Clock clock)
    : impl_(std::make_unique<Impl>(std::move(mts), std::move(paired), std::move(clock))) {}
RentalSessionCoordinator::~RentalSessionCoordinator() = default;
const RentalSessionCoordinator::State& RentalSessionCoordinator::state() const noexcept { return impl_->snapshot; }
RentalSessionCoordinator::AuthScope RentalSessionCoordinator::recoveryAuthScope() const noexcept {
    if (state().pairedPending) return AuthScope::FogHub;
    if (state().checkoutPending || state().sessionPending) return AuthScope::Mts;
    return AuthScope::All;
}
bool RentalSessionCoordinator::beginLaunch(Admission admission) {
    auto& m = *impl_;
    if (admission.busy || admission.exiting || !admission.focused || m.active || m.snapshot.pending || m.stop.shutdownRequested()) return false;
    m.stop.newRental(); m.launch = true; m.publishPending(); return true;
}
bool RentalSessionCoordinator::cancelLaunch() noexcept {
    auto& m = *impl_;
    if (m.active || m.snapshot.durablePending) return false;
    m.launch = false; m.stop.clearRequest(); m.publishPending(); return true;
}
RentalSessionCoordinator::Plan RentalSessionCoordinator::plan(Request request, Admission admission, Time now) {
    auto& m = *impl_;
    if (admission.busy || m.active) return {PlanStatus::Busy, std::nullopt};
    if (admission.exiting || m.stop.shutdownRequested()) return {};
    if (std::holds_alternative<Stop>(request)) {
        const auto decision = m.stop.next({m.snapshot.pending, m.snapshot.pairedPending, m.snapshot.checkoutPending}, false, admission.exiting, now);
        if (decision.action == RentalStopCoordinator::Action::Pause) return {PlanStatus::Paused, std::nullopt};
        if (decision.action != RentalStopCoordinator::Action::Dispatch) return {};
        const auto operation = decision.target == RentalStopCoordinator::Target::Paired ? Operation::PairedStop :
            decision.target == RentalStopCoordinator::Target::Checkout ? Operation::CheckoutStop : Operation::SessionStop;
        if (m.paired) m.paired->requestStop();
        return m.reserve(operation, std::move(request), decision.resetCheckoutChecks);
    }
    if (std::holds_alternative<Recover>(request)) {
        if (m.stop.paused()) return {PlanStatus::Paused, std::nullopt};
        if (m.snapshot.pairedPending || m.snapshot.checkoutPending) {
            if (m.snapshot.pairedPending ? !m.snapshot.pairedAuthenticated : !m.snapshot.mtsAuthenticated)
                return {PlanStatus::NeedsSignIn, std::nullopt};
            m.signalStop(); return plan(Stop{}, admission, now);
        }
        if (m.snapshot.sessionOwned) { m.signalStop(); return plan(Stop{}, admission, now); }
        return m.reserve(Operation::Recover, std::move(request));
    }
    if (std::holds_alternative<MtsCreate>(request) || std::holds_alternative<PairedCreate>(request)) {
        if (m.snapshot.durablePending || m.stop.requested() || !admission.focused) {
            cancelLaunch(); return {PlanStatus::Cancelled, std::nullopt};
        }
        if (!m.launch && !beginLaunch(admission)) return {};
        if (std::holds_alternative<PairedCreate>(request) && !m.paired) { cancelLaunch(); return {}; }
        return m.reserve(std::holds_alternative<MtsCreate>(request) ? Operation::MtsCreate : Operation::PairedCreate, std::move(request));
    }
    if (std::holds_alternative<Adopt>(request)) {
        if (m.snapshot.pending || !admission.focused) return {};
        m.stop.newRental(); m.launch = true; m.publishPending();
        return m.reserve(Operation::Adopt, std::move(request));
    }
    if (std::holds_alternative<ListSessions>(request)) {
        if (m.snapshot.pending) return {};
        return m.reserve(Operation::ListSessions, std::move(request));
    }
    if (std::holds_alternative<MtsPoll>(request)) {
        if (m.snapshot.pairedPending || !m.snapshot.checkoutPending) return {};
        return m.reserve(Operation::MtsPoll, std::move(request));
    }
    if (std::holds_alternative<ManualResolution>(request)) {
        if (m.snapshot.pairedPending || !m.snapshot.checkoutPending) return {};
        return m.reserve(Operation::ManualResolution, std::move(request));
    }
    if (!m.paired || !m.snapshot.pairedPending) return {};
    if (std::holds_alternative<PairedPoll>(request)) return m.reserve(Operation::PairedPoll, std::move(request));
    if (m.stop.requested() || !admission.focused) return {PlanStatus::Cancelled, std::nullopt};
    return m.reserve(std::holds_alternative<InspectTransport>(request) ? Operation::TransportInspection : Operation::QualityInspection, std::move(request));
}
RentalSessionCoordinator::WorkerResult RentalSessionCoordinator::execute(const Work& work) {
    auto& m = *impl_;
    const auto& request = work.request_;
    switch (work.operation_) {
        case Operation::ListSessions: return {work.serial_, work.operation_, m.mts.activeSessions()};
        case Operation::Adopt: { const auto& value = std::get<Adopt>(request); m.session.adopt(value.rental); return {work.serial_, work.operation_, Adopted{value.rental, value.computer}}; }
        case Operation::Recover: return {work.serial_, work.operation_, Matched{m.session.recoverMatchingSession()}};
        case Operation::MtsCreate: return {work.serial_, work.operation_, m.checkout.create(std::get<MtsCreate>(request).request)};
        case Operation::MtsPoll: return {work.serial_, work.operation_, m.checkout.poll(std::get<MtsPoll>(request).manual)};
        case Operation::CheckoutStop: return {work.serial_, work.operation_, m.checkout.stop(work.resetChecks_)};
        case Operation::ManualResolution: return {work.serial_, work.operation_, m.checkout.acknowledgeManualResolution()};
        case Operation::SessionStop: return {work.serial_, work.operation_, m.session.finishAttempt()};
        default: break;
    }
    if (!m.paired) throw std::logic_error("Paired operation is unavailable");
    switch (work.operation_) {
        case Operation::PairedCreate: return {work.serial_, work.operation_, pairedView(m.paired->create(std::get<PairedCreate>(request).request))};
        case Operation::PairedPoll: case Operation::QualityInspection: return {work.serial_, work.operation_, pairedView(m.paired->poll())};
        case Operation::TransportInspection: return {work.serial_, work.operation_, pairedView(m.paired->inspectForResume())};
        case Operation::PairedStop: m.paired->bindAuthenticatedAccount(); return {work.serial_, work.operation_, pairedView(m.paired->stop())};
        default: throw std::logic_error("Unknown rental operation");
    }
}
RentalSessionCoordinator::Effects RentalSessionCoordinator::completeJoined(WorkerResult result, Admission admission, Time now) {
    auto& m = *impl_;
    if (!m.active || m.active->serial_ != result.serial || m.active->operation_ != result.operation)
        throw std::logic_error("Rental completion does not match admitted work");
    return m.finish(result.operation, &result.value, {}, admission, now);
}
RentalSessionCoordinator::Effects RentalSessionCoordinator::completeJoined(std::exception_ptr error, Admission admission, Time now) {
    auto& m = *impl_;
    if (!m.active) throw std::logic_error("Rental failure has no admitted work");
    return m.finish(m.active->operation_, nullptr, error, admission, now);
}
void RentalSessionCoordinator::externalServiceTaskJoined() {
    if (impl_->active) throw std::logic_error("External task joined during rental operation");
    impl_->refresh();
}
void RentalSessionCoordinator::dispatchFailed() noexcept {
    const bool unsubmittedLaunch = impl_->active && (impl_->active->operation_ == Operation::MtsCreate ||
        impl_->active->operation_ == Operation::PairedCreate || impl_->active->operation_ == Operation::Adopt);
    impl_->active.reset(); impl_->stop.dispatchFailed();
    if (unsubmittedLaunch && !impl_->snapshot.durablePending) {
        impl_->launch = false; impl_->stop.clearRequest(); impl_->publishPending();
    }
}
void RentalSessionCoordinator::request() noexcept { impl_->signalStop(); }
void RentalSessionCoordinator::shutdown() noexcept { impl_->stop.shutdown(); if (impl_->paired) impl_->paired->requestStop(); }
void RentalSessionCoordinator::pause() noexcept { impl_->stop.pause(); }
bool RentalSessionCoordinator::manualRetry() noexcept { return !impl_->active && impl_->stop.manualRetry(); }
bool RentalSessionCoordinator::requested() const noexcept { return impl_->stop.requested(); }
bool RentalSessionCoordinator::paused() const noexcept { return impl_->stop.paused(); }
unsigned RentalSessionCoordinator::attempts() const noexcept { return impl_->stop.attempts(); }
bool RentalSessionCoordinator::retryScheduled() const noexcept { return impl_->stop.retryScheduled(); }
bool RentalSessionCoordinator::retryDue(Time now) const noexcept { return impl_->stop.retryDue(now); }
bool RentalSessionCoordinator::pauseIfExhausted(bool busy, Time now) noexcept { return impl_->stop.pauseIfExhausted(state().pending, busy, now); }
}
