#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {

void ApplicationController::armExitHandling() {
    if (exitAfterStop_ || shutdownDone_) return;
    checkResult(appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend), "Background stop support");
    if (!exitLocked_) { checkResult(appletLockExit(), "Session exit guard"); exitLocked_ = true; }
    checkResult(appletSetAutoSleepDisabled(true), "Session autosleep guard");
}

void ApplicationController::disarmExitHandling() {
    if (exitLocked_) { appletUnlockExit(); exitLocked_ = false; }
    appletSetAutoSleepDisabled(false);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
}

void ApplicationController::shutdownSession() noexcept {
    if (shutdownDone_) return;
    shutdownDone_ = true;
    rentalSession_.shutdown();
    diagnostic("Application shutdown: cancelling work; unresolved recovery records retained.");
    // Do not start/retry network requests after the OS requests exit. A worker
    // still owns the guards until it is joined; retain its durable intent and
    // cancel every HTTP client before any transport or future join.
    services_->cancel();
    cancelPlayStation();
    latency_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    disarmExitHandling();
    closeStream();
}

void ApplicationController::pauseSessionRecovery() {
    rentalSession_.pause();
    checkoutAutoPoll_ = false;
    screen_ = Screen::Recovery;
    status_ = checkoutStatus_ = uiText::Server::RecoveryPaused;
    disarmExitHandling();
    diagnostic("Session recovery paused; attempts=" + std::to_string(rentalSession_.attempts()) + "; records retained; official account check required.");
}

void ApplicationController::retrySessionRecovery() {
    if (pending_.valid() || exitAfterStop_ || shutdownDone_) return;
    if (!rentalSession_.manualRetry()) return;
    recoverSession();
}

fog::RentalSessionCoordinator::Admission ApplicationController::rentalAdmission() const {
    return {pending_.valid(), exitAfterStop_ || shutdownDone_, appletGetFocusState() == AppletFocusState_InFocus};
}

bool ApplicationController::startRental(fog::RentalSessionCoordinator::Request request, const char* reason) {
    using Rental = fog::RentalSessionCoordinator;
    const auto plan = rentalSession_.plan(std::move(request), rentalAdmission(), std::chrono::steady_clock::now());
    if (plan.status == Rental::PlanStatus::Paused) { pauseSessionRecovery(); return false; }
    if (plan.status == Rental::PlanStatus::NeedsSignIn) {
        loginProvider_ = rentalSession_.state().pairedPending ? fog::ProviderId::FogHub : fog::ProviderId::Mts;
        screen_ = Screen::Recovery;
        status_ = uiText::Profile::PreviousRentalSignInHint;
        return false;
    }
    if (plan.status == Rental::PlanStatus::Cancelled) {
        if (!rentalSession_.state().pending) { disarmExitHandling(); screen_ = Screen::PlayStation; status_ = uiText::Server::ConnectionCancelled; }
        return false;
    }
    if (!plan.work) return false;
    const auto work = *plan.work;
    const bool stopping = work.operation() == Rental::Operation::SessionStop || work.operation() == Rental::Operation::CheckoutStop ||
        work.operation() == Rental::Operation::PairedStop;
    try {
        if (stopping) {
            diagnostic("Rental stop begin; reason=" + std::string(reason) + " screen=" + std::to_string(static_cast<int>(screen_)));
            if (work.operation() == Rental::Operation::PairedStop) { screen_ = Screen::Recovery; checkoutAutoPoll_ = false; }
            else screen_ = work.operation() == Rental::Operation::CheckoutStop ? Screen::Checkout : Screen::Stopping;
            status_ = checkoutStatus_ = uiText::Server::RequestingStop;
        }
        if (!start(Task::RentalOperation, [this,work]() -> TaskResult { return rentalSession_.execute(work); })) {
            rentalSession_.dispatchFailed();
            if (!rentalSession_.state().pending) disarmExitHandling();
            if (stopping) closeStream();
            return false;
        }
    } catch (...) {
        rentalSession_.dispatchFailed();
        if (!rentalSession_.state().pending) disarmExitHandling();
        if (stopping) closeStream();
        throw;
    }
    // The independent rental task is admitted before potentially blocking RTC
    // teardown. Both use the existing shared service slot / player lifecycle.
    if (stopping) closeStream();
    return true;
}

void ApplicationController::logCheckoutResult(Task phase) {
    // Only called after get() joins the worker. Log presence/counters, never identifiers.
    const auto& result = rentalSession_.state().checkout;
    diagnostic("Checkout phase=" + std::to_string(static_cast<int>(phase)) +
        " state=" + std::to_string(static_cast<int>(result.state)) +
        " serverStatus=" + std::to_string(result.serverStatus) +
        " orderPresent=" + std::to_string(!result.creationId.empty()) +
        " hashPresent=" + std::to_string(!result.rentalId.value.empty()) +
        " serverHashPresent=" + std::to_string(result.serverHashPresent) +
        " stopRequested=" + std::to_string(result.stopRequested) +
        " stopQueued=" + std::to_string(rentalSession_.requested()) +
        " pollCount=" + std::to_string(result.pollCount) +
        " readyObservations=" + std::to_string(result.readyObservations));
    diagnostic("Checkout detail: " + result.message);
}

void ApplicationController::queueCheckoutStop() {
    diagnostic("Stop trigger: user pressed Back during checkout.");
    rentalSession_.request();

    checkoutStatus_ = uiText::Server::StopAfterResponse;
    status_ = checkoutStatus_;
    if (!pending_.valid()) beginFinish();
}

void ApplicationController::applyRentalEffects(const fog::RentalSessionCoordinator::Effects& effects) {
    using Rental = fog::RentalSessionCoordinator;
    const auto& state = rentalSession_.state();
    if (completePlayerInspection(effects)) return;
    if (effects.start) {
        const auto& grant = *effects.start;
        if (grant.rental.provider == fog::ProviderId::FogHub) {
            if (!pairedServices_ || !pairedServices_->bindRental(grant.rental)) { beginFinish("paired-stream-binding-failed"); return; }
            startStream(grant.rental,false,grant.computer);
        } else if (effects.operation == Rental::Operation::Adopt) startStream(grant.rental,false,grant.computer);
        else startStream(grant.rental,!selectedPlayStation_,selectedComputerId_);
        return;
    }
    if (effects.recoverNext) {
        if (!auth_.authenticated()) startProviderRestore(); else recoverSession();
        return;
    }
    if (effects.flow == Rental::Flow::Paused) { pauseSessionRecovery(); return; }
    if (effects.flow == Rental::Flow::LaunchCancelled) {
        disarmExitHandling(); screen_ = Screen::Sessions; status_ = uiText::Server::ConnectionNotConfirmed; return;
    }
    if (effects.flow == Rental::Flow::RecoveryUnmatched) {
        pauseSessionRecovery(); status_ = uiText::Server::PreviousRentalNotMatched + std::string(" B — меню; Y — профиль."); return;
    }
    if (effects.flow == Rental::Flow::SessionConfirmed) {
        screen_ = Screen::Sessions; sessions_.clear(); selection_ = 0;
        status_ = uiText::Server::TerminationConfirmed;
        diagnostic("Session finish confirmed; active-session absence checked; journal cleared.");
        disarmExitHandling(); return;
    }
    if (effects.flow == Rental::Flow::SessionRetry) {
        screen_ = Screen::Stopping; status_ = ui_.controllerHints(uiText::Server::TerminationRetrySignInHint);
        diagnostic("Session stop unresolved; journal retained."); return;
    }
    if (effects.flow == Rental::Flow::ActiveSessions) {
        if (!effects.error) { sessions_ = effects.sessions; screen_ = Screen::Sessions; selection_ = 0; status_.clear(); }
        return;
    }
    if (effects.flow == Rental::Flow::PairedCheckout) {
        const auto& result = state.paired;
        diagnostic("Paired checkout: provider=FH state=" + std::to_string(static_cast<int>(result.state)) +
            " rentalPresent=" + std::to_string(!result.rental.empty()) + " stopRequested=" + std::to_string(result.stopRequested) +
            " unresolved=" + std::to_string(state.pairedPending) + " preparationStage=" + fog::pairedPreparationStageName(result.preparationStage) +
            " preparationError=" + (result.preparationError ? fog::repositoryErrorKindName(*result.preparationError) : "none") +
            " message=" + safeText(result.message));
        checkoutAutoPoll_ = false;
        status_ = checkoutStatus_ = "FogHub: " + result.message;
        if (!state.pairedPending) {
            disarmExitHandling(); screen_ = Screen::PlayStation;
            if (result.state == fog::PairedCheckoutState::Terminal) status_ = "FogHub: завершение аренды подтверждено.";
            offerSessionRating(result);
        } else if (effects.error) screen_ = Screen::Recovery;
        else if ((rentalSession_.requested() || result.stopRequested) && !effects.stopRequired) {
            screen_ = Screen::Checkout; checkoutAutoPoll_ = true;
        } else if (result.state == fog::PairedCheckoutState::Preparing && !rentalSession_.requested()) {
            screen_ = Screen::Checkout; checkoutAutoPoll_ = true; checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        } else screen_ = Screen::Recovery;
    } else if (effects.flow == Rental::Flow::MtsCheckout || effects.flow == Rental::Flow::ManualResolved) {
        const auto& result = state.checkout;
        logCheckoutResult(effects.operation == Rental::Operation::MtsCreate ? Task::Create :
            effects.operation == Rental::Operation::CheckoutStop ? Task::CheckoutStop :
            effects.operation == Rental::Operation::ManualResolution ? Task::ManualResolution : Task::CheckoutPoll);
        checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(effects.error ? 10 : 2);
        checkoutAutoPoll_ = state.pending && fog::checkoutNeedsAutomaticCheck(result);
        if (!state.pending) {
            disarmExitHandling();
            screen_ = effects.error ? (selectedPlayStation_ ? Screen::PlayStationDetails : Screen::Details) :
                selectedPlayStation_ ? Screen::PlayStation : Screen::Games;
            status_ = effects.flow == Rental::Flow::ManualResolved ? uiText::Server::LocalBlockCleared :
                result.state == fog::CheckoutState::Rejected ? uiText::Server::OrderRejected : uiText::Server::RentalFinished;
        } else {
            screen_ = Screen::Checkout;
            if (result.stopRequested || rentalSession_.requested()) checkoutStatus_ = uiText::Server::WaitingStopData;
            else if (result.state == fog::CheckoutState::RecoveryRequired) checkoutStatus_ = ui_.controllerHints(uiText::Server::OrderRecoveryHint);
            else if (result.pollCount >= 30) checkoutStatus_ = ui_.controllerHints(uiText::Server::AutoCheckPausedHint);
            else if (result.readyObservations > 0) checkoutStatus_ = uiText::Server::ReadyObservationsPrefix + std::to_string(result.readyObservations) + "/3)…";
            else if (result.preparation == fog::PreparationState::PaymentConfirmed && !result.serverHashPresent) checkoutStatus_ = uiText::Server::PaymentConfirmedWaitingAddress;
            else if (result.preparation == fog::PreparationState::Preparing) checkoutStatus_ = result.serverHashPresent ? uiText::Server::PreparingConnection : uiText::Server::WaitingAddress;
            else if (result.preparation == fog::PreparationState::Pending) checkoutStatus_ = uiText::Server::OrderCreated;
            else if (result.state == fog::CheckoutState::PaymentRequired) checkoutStatus_ = uiText::Server::PaymentRequired;
            else checkoutStatus_ = uiText::Server::OrderRecorded;
            status_ = checkoutStatus_;
        }
    }
    if (effects.stopRequired) {
        if (effects.operation == Rental::Operation::Recover) armExitHandling();
        beginFinish("rental-result-stop");
    }
}



void ApplicationController::pollCheckout(bool manual) {
    if (pending_.valid()) return;
    if (manual && (rentalSession_.requested() || rentalSession_.paused() || rentalSession_.state().checkout.stopRequested)) {
        retrySessionRecovery(); return;
    }
    if (rentalSession_.requested()) { beginFinish("stop-poll"); return; }
    if (rentalSession_.state().pairedPending) startRental(fog::RentalSessionCoordinator::PairedPoll{});
    else startRental(fog::RentalSessionCoordinator::MtsPoll{manual});
}

void ApplicationController::createPairedSession(const fog::Computer& pc) {
    if (!rentalSession_.state().pairedSupported || !pairedServices_ || !pc.listed || !pc.launchable || !pc.hourlyPrice ||
        pc.availability != fog::Availability::Online) { status_ = uiText::Server::TariffUnconfirmedRefreshHint; return; }
    if (!pairedServices_->auth().authenticated()) {
        loginProvider_ = fog::ProviderId::FogHub; loginReturn_ = Screen::PlayStationDetails; screen_ = Screen::Consent; return;
    }
    checkStorage(); armExitHandling();
    if (!rentalSession_.beginLaunch(rentalAdmission())) { if (!rentalSession_.state().pending) disarmExitHandling(); return; }
    ratingFocusLost_ = false;
    selectedPlayStation_ = false;
    screen_ = Screen::Checkout; checkoutStatus_ = "FogHub: проверяем доступность и тариф…";
    fog::StartRentalRequest request; request.computerId = pc.id;
    start(Task::VerifyPairedOffer,[this,request,offered=pc]() -> TaskResult {
        auto fresh = pairedServices_->catalog().playStationComputers();
        const auto found = std::find_if(fresh.begin(),fresh.end(),[&request](const fog::Computer& computer) {
            return computer.id == request.computerId;
        });
        if (found == fresh.end() || !found->listed || !found->launchable || found->availability != fog::Availability::Online ||
            !fog::sameOffer(*found,offered) || (!found->hourlyPrice || !offered.hourlyPrice || found->hourlyPrice->minorUnits != offered.hourlyPrice->minorUnits || found->hourlyPrice->currency != offered.hourlyPrice->currency))
            return ChangedOffer{std::move(fresh)};
        return VerifiedOffer{request};
    });
    return;
}

void ApplicationController::createSession() {
    const bool playStation = screen_ == Screen::PlayStationDetails;
    if (screen_ != Screen::Details && !playStation) return;
    const auto& computers = playStation ? playStationComputers_ : computers_;
    if (rentalSession_.state().pending) { screen_ = Screen::Recovery; status_ = uiText::Server::RecoveryPaused; return; }
    if (selection_ >= computers.size()) return;
    const auto& pc = computers[selection_];
    if (pc.id.provider == fog::ProviderId::FogHub) { createPairedSession(pc); return; }
    if (playStation && (playStationLoadedAt_ == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now()-playStationLoadedAt_ > std::chrono::seconds(60))) {
        loadPlayStation();
        status_ = uiText::Server::RefreshingTariff;
        return;
    }
    fog::StartRentalRequest order;
    order.computerId = pc.id;
    order.offerId = pc.offerId;
    if (playStation && (!pc.listed || !pc.launchable ||
        !fog::isPlayStationComputerId(order.computerId) || order.offerId.empty())) {
        status_ = uiText::PlayStation::TariffUnconfirmedHint;
        return;
    }
    loginProvider_ = fog::ProviderId::Mts;
    if (!auth_.authenticated()) {
        loginReturn_ = playStation ? Screen::PlayStationDetails : Screen::Details;
        screen_ = Screen::Consent; return;
    }
    // Verify the same SD replacement path before any billable request.
    checkStorage();
    selectedPlayStation_ = playStation;
    selectedComputerId_ = pc.id;
    order.gameId = playStation ? fog::EntityId{} : selectedGameId_;
    order.ownAccount = playStation ? false : ownSteam_;
    recentLaunch_.prepare(playStation ? std::nullopt : selectedRecentGame_);
    // Lock focus/exit before the worker can send a billable create request.
    armExitHandling();
    if (!rentalSession_.beginLaunch(rentalAdmission())) { if (!rentalSession_.state().pending) disarmExitHandling(); return; }
    screen_ = Screen::Checkout;
    checkoutStatus_ = playStation ? uiText::PlayStation::CheckingTariff : uiText::Server::CreatingOrder;
    if (!playStation) { startRental(fog::RentalSessionCoordinator::MtsCreate{order}); return; }
    start(Task::VerifyMtsOffer, [this, order, playStation, offeredComputer = pc]() -> TaskResult {
        if (playStation) {
            auto current = catalogRepository_.playStationComputers();
            const auto offer = std::find_if(current.begin(),current.end(),[&order](const fog::Computer& candidate) {
                return candidate.id == order.computerId;
            });
            if (offer == current.end() || !offer->listed || !offer->launchable ||
                !fog::sameOffer(*offer,offeredComputer)) {
                return ChangedOffer{std::move(current)};
            }
            // Main-thread lifecycle checks must accept this result before billing.
            return VerifiedOffer{order};
        }
        return VerifiedOffer{order};
    });
}

void ApplicationController::loadSessions() {
    if (rentalSession_.state().pending) { recoverSession(); return; }
    loginProvider_ = fog::ProviderId::Mts;
    if (!auth_.authenticated()) { screen_ = Screen::Consent; return; }
    screen_ = Screen::Sessions; selection_ = 0; sessions_.clear();
    startRental(fog::RentalSessionCoordinator::ListSessions{});
}

void ApplicationController::recoverSession() {
    if (pending_.valid()) return;
    screen_ = Screen::Recovery;
    loginProvider_ = rentalSession_.state().pairedPending ? fog::ProviderId::FogHub : fog::ProviderId::Mts;
    const auto& state = rentalSession_.state();
    if (!rentalSession_.paused() && ((state.pairedPending && state.pairedAuthenticated) ||
        (state.checkoutPending && state.mtsAuthenticated) || state.sessionOwned)) armExitHandling();
    startRental(fog::RentalSessionCoordinator::Recover{});
}

void ApplicationController::connectSession() {
    if (selection_ >= sessions_.size() || rentalSession_.state().pending) return;
    const auto& rental = sessions_[selection_];
    armExitHandling();
    if (!startRental(fog::RentalSessionCoordinator::Adopt{rental.id, rental.computerId.value_or(fog::EntityId{})}) &&
        !rentalSession_.state().pending) disarmExitHandling();
}

void ApplicationController::beginFinish(const char* reason) {
    rentalSession_.request();
    player_.cancel();
    startRental(fog::RentalSessionCoordinator::Stop{}, reason);
}

}
