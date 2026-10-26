#include "core/constants/error_text.hpp"
#include "features/session/application/checkout.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fog {
namespace {
bool hasRental(const std::vector<Rental>& sessions, const RentalId& rental) {
    for (const auto& session : sessions) if (session.id == rental) return true;
    return false;
}
}
CheckoutGuard::CheckoutGuard(RentalRepository& api, SessionGuard& sessions, std::unique_ptr<CheckoutJournal> journal)
    : api_(api), sessions_(sessions), journal_(std::move(journal)) {
    if (!journal_) throw std::invalid_argument("Missing checkout journal");
    load();
}
CheckoutResult CheckoutGuard::recovery(const std::string& message) {
    result_.state = CheckoutState::RecoveryRequired;
    result_.message = message;
    return result_;
}
CheckoutResult CheckoutGuard::waiting(const std::string& message) {
    result_.state = result_.paymentUrl.empty() ? CheckoutState::Pending : CheckoutState::PaymentRequired;
    result_.message = polls_ >= 30 ?
        "Automatic checks paused; order remains unresolved. Check again or request stop" : message;
    return result_;
}
void CheckoutGuard::load() {
    const auto saved = journal_->load();
    pending_ = saved.pending;
    invalid_ = saved.invalid;
    if (!pending_) return;
    account_ = saved.record.account;
    selection_ = saved.record.selection;
    before_ = saved.record.before;
    result_.creationId = saved.record.creation;
    result_.rentalId = saved.record.rental;
    result_.stopRequested = saved.record.stopRequested;
    recovery(saved.error);
}
void CheckoutGuard::persist() {
    journal_->write({account_, selection_, result_.creationId, result_.rentalId, before_, result_.stopRequested});
}
void CheckoutGuard::clear() {
    try { journal_->clear(); }
    catch (...) { try { persist(); } catch (...) {} throw; }
    pending_ = false;
    invalid_ = false;
    result_ = {};
    account_ = {};
    before_.clear();
    polls_ = 0;
}
bool CheckoutGuard::sameAccount() const {
    const auto auth = api_.auth();
    return auth.authenticated() && !account_.empty() && account_ == auth.account;
}
CheckoutResult CheckoutGuard::create(const StartRentalRequest& order) {
    if (pending_ || !sessions_.pendingRental().empty() || !sessions_.journalError().empty())
        throw std::logic_error(fog::constants::error::Session::ResolvePendingCheckoutOrRentalBeforeCreatingAnother);
    const auto auth = api_.auth();
    if (!auth.authenticated() || auth.account.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    if (!validRentalId(order.computerId) || !validRentalId(order.offerId) ||
        order.computerId.provider != auth.account.provider || order.offerId.provider != auth.account.provider ||
        (!order.gameId.empty() && (!validRentalId(order.gameId) || order.gameId.provider != auth.account.provider)))
        throw std::invalid_argument(fog::constants::error::Session::InvalidCheckoutSelection);
    const auto active = api_.activeSessions(true); // Before-create snapshot, never infer a new hash.
    if (active.size() > 64) throw std::runtime_error(fog::constants::error::Session::TooManySessionsForSafeCheckoutRecovery);
    selection_ = order;
    account_ = auth.account;
    before_.clear();
    for (const auto& session : active) before_.push_back(session.id);
    result_ = {};
    result_.state = CheckoutState::Pending;
    result_.message = "Creating selected rental once";
    pending_ = true;
    try { persist(); }
    catch (const JournalError& error) {
        return recovery(std::string(fog::constants::error::Session::CannotSaveCreateIntentNoCreateRequestSentPrefix) + error.what());
    }
    catch (...) { return recovery(fog::constants::error::Session::CannotSaveCreateIntentNoCreateRequestSentRecoveryRequired); }
    try {
        const auto response = api_.createOrder(order);
        result_.creationId = response.id;
        if (!result_.creationId.empty()) persist(); // Before interpreting payment state or making more requests.
        if (response.reportedError) {
            if (!result_.creationId.empty()) return recovery(fog::constants::error::Session::CreateReturnedAnErrorWithAnOrderIDCheckTheRecordedOrder);
            // No cancellation/idempotency contract establishes that an error cannot
            // follow partial creation. Preserve even an explicit no-ID rejection.
            return recovery(fog::constants::error::Session::ServerReportedACreateErrorWithoutOrderIDVerifyOfficialAccountBeforeAnotherRental);
        }
        if (result_.creationId.empty()) return recovery(fog::constants::error::Session::CreateOutcomeUnknownCheckOfficialAccountDoNotCreateAgain);
        if (response.payment) {
            result_.paymentUrl = response.payment->url;
            result_.paymentAmount = response.payment->amount;
            result_.state = CheckoutState::PaymentRequired;
            result_.message = "Payment required on official page; order remains pending";
        } else result_.message = "Order recorded; checking payment and session readiness";
        return result_;
    } catch (const JournalError& error) {
        return recovery(std::string(fog::constants::error::Session::CreateOutcomeUncertainMarkerRetainedPrefix) + error.what());
    } catch (...) { return recovery(fog::constants::error::Session::CreateOutcomeUncertainMarkerRetainedNeverRepeatCreationAutomatically); }
}
CheckoutResult CheckoutGuard::poll(bool manualRetry) {
    if (!pending_) return result_;
    if (invalid_) return recovery(fog::constants::error::Session::InvalidCheckoutMarkerPreserveItAndVerifyOfficialAccountManually);
    if (!sameAccount()) return recovery(fog::constants::error::Auth::SignInToTheOriginalAccountBeforeCheckingOrStoppingTheOrder);
    if (result_.creationId.empty()) return recovery(fog::constants::error::Session::CreateOutcomeUnknownOfficialAccountCheckRequiredBeforeAnotherRental);
    if (!manualRetry && polls_ >= 30) return waiting("");
    if (manualRetry) polls_ = 0;
    ++polls_;
    result_.pollCount = polls_;
    result_.preparation = PreparationState::Unknown;
    result_.serverStatus = -1;
    result_.serverHashPresent = false;
    try {
        const auto status = api_.orderStatus(result_.creationId);
        result_.preparation = status.state;
        result_.serverStatus = status.diagnosticStatus;
        const auto rental = status.rental;
        result_.serverHashPresent = status.diagnosticRentalPresent;
        if (status.id != result_.creationId) {
            result_.readyObservations = 0;
            return recovery(fog::constants::error::Session::OrderStatusIdentifierMismatchMarkerRetained);
        }
        if (status.cancellation == CancellationState::Invalid) {
            result_.readyObservations = 0;
            return recovery(fog::constants::error::Session::OrderCancellationStatusIsUnknownMarkerRetained);
        }
        if (status.cancellation == CancellationState::Requested) {
            result_.readyObservations = 0;
            return recovery(fog::constants::error::Session::OrderCancellationReportedVerifyRentalTerminationOnTheOfficialAccount);
        }
        if (status.state != PreparationState::Preparing && status.state != PreparationState::PaymentConfirmed) {
            result_.readyObservations = 0;
            if (status.state == PreparationState::Unpaid) return recovery(fog::constants::error::Session::OrderNotPaidThisDoesNotProveRentalTerminationCheckOfficialAccount);
            if (status.state != PreparationState::Pending) return recovery(fog::constants::error::Session::OrderStatusUnavailableOrUnknownMarkerRetained);
            result_.state = result_.paymentUrl.empty() ? CheckoutState::Pending : CheckoutState::PaymentRequired;
            result_.message = result_.stopRequested ? "Stop requested; waiting for session hash. Order is not cancelled" :
                "Waiting for payment or rental readiness";
            return result_;
        }
        if (!validRentalId(rental) || rental.provider != account_.provider) {
            result_.readyObservations = 0;
            result_.state = result_.paymentUrl.empty() ? CheckoutState::Pending : CheckoutState::PaymentRequired;
            result_.message = result_.stopRequested ? "Stop requested; waiting for session hash. Order is not cancelled" :
                "Waiting for a valid rental session hash";
            return result_;
        }
        if (std::find(before_.begin(), before_.end(), rental) != before_.end()) {
            result_.readyObservations = 0;
            return recovery(fog::constants::error::Session::OrderPointsToAPreExistingRentalManualAccountCheckRequired);
        }
        if (!result_.rentalId.empty() && result_.rentalId != rental) {
            result_.readyObservations = 0;
            return recovery(fog::constants::error::Session::OrderSessionChangedMarkerRetained);
        }
        result_.rentalId = rental;
        // Persist the first attributed hash before any connection or cleanup action.
        // Observations are deliberately not persisted: restart requires fresh checks.
        try { persist(); }
        catch (...) { if (!result_.stopRequested) throw; }
        if (!result_.stopRequested && sessions_.pendingRental() == rental &&
            (!sessions_.persistent() || !sessions_.journalError().empty()))
            return recovery(fog::constants::error::Session::SessionJournalIsNotDurableConnectionBlockedStopTheRecordedRental);
        if (result_.readyObservations < 3) ++result_.readyObservations;
        if (!result_.stopRequested && result_.readyObservations < 3) {
            result_.state = CheckoutState::Pending;
            result_.message = "Rental session identified; confirming readiness";
            return result_;
        }
        bool observedActive = false;
        if (!result_.stopRequested) {
            observedActive = hasRental(api_.activeSessions(true), rental);
            if (!observedActive)
                return waiting("Recorded rental is not active yet; checking again before connecting");
        }
        if (sessions_.pendingRental().empty()) {
            try { sessions_.adopt(rental, observedActive); }
            catch (...) {
                if (result_.stopRequested && sessions_.ownsSession() && sessions_.pendingRental() == rental)
                    return finishReady();
                throw;
            }
        }
        else if (sessions_.pendingRental() != rental) return recovery(fog::constants::error::Session::AnotherRentalIsPendingRecoveryResolveItFirst);
        else if (!sessions_.ownsSession() && !sessions_.recoverMatchingSession())
            return recovery(fog::constants::error::Session::CannotEstablishOwnershipOfRecordedRental);
        if (!result_.stopRequested && (!sessions_.persistent() || !sessions_.journalError().empty()))
            return recovery(fog::constants::error::Session::SessionJournalIsNotDurableConnectionBlockedStopTheRecordedRental);
        result_.state = CheckoutState::Ready;
        result_.message = "Rental ready; session recovery journal saved";
        return result_.stopRequested ? finishReady() : result_;
    } catch (const JournalError& error) {
        result_.readyObservations = 0;
        return recovery(std::string(fog::constants::error::Session::CannotSaveRecordedOrderRecoveryMarkerRetainedPrefix) + error.what());
    } catch (...) {
        result_.readyObservations = 0;
        return waiting(fog::constants::error::Session::CannotVerifyRecordedOrderTemporarilyCheckingAgainRecoveryMarkerRetained);
    }
}
CheckoutResult CheckoutGuard::finishReady() {
    const auto finished = sessions_.finishAttempt();
    if (!finished.terminated || !finished.journalCleared) return recovery(finished.message);
    try { clear(); }
    catch (const JournalError& error) {
        return recovery(std::string(fog::constants::error::Session::RentalStoppedButCheckoutMarkerRetainedPrefix) + error.what());
    }
    catch (...) { return recovery(fog::constants::error::Session::RentalStoppedButCheckoutRecoveryMarkerCouldNotBeCleared); }
    result_.message = "Rental termination confirmed";
    return result_;
}
CheckoutResult CheckoutGuard::finishKnown() {
    // The checkout hash was attributed to this account/order before stabilization.
    // Cleanup must not depend on payment status or additional readiness observations.
    try {
        if (sessions_.pendingRental().empty()) {
            try { sessions_.adopt(result_.rentalId); }
            catch (...) {
                if (!sessions_.ownsSession() || sessions_.pendingRental() != result_.rentalId) throw;
            }
        } else if (sessions_.pendingRental() != result_.rentalId) {
            return recovery(fog::constants::error::Session::AnotherRentalIsPendingRecoveryResolveItFirst);
        } else if (!sessions_.ownsSession() && !sessions_.recoverMatchingSession()) {
            return recovery(fog::constants::error::Session::CannotEstablishOwnershipOfRecordedRental);
        }
        return finishReady();
    } catch (const JournalError& error) {
        return recovery(std::string(fog::constants::error::Session::CannotStopRecordedRentalRecoveryMarkerRetainedPrefix) + error.what());
    } catch (...) { return recovery(fog::constants::error::Session::CannotStopRecordedRentalRecoveryMarkerRetained); }
}
CheckoutResult CheckoutGuard::stop() {
    if (!pending_) return result_;
    if (invalid_) return recovery(fog::constants::error::Session::InvalidCheckoutMarkerVerifyOfficialAccountManually);
    result_.stopRequested = true;
    try { persist(); }
    catch (const JournalError& error) {
        if (sameAccount() && !result_.rentalId.empty()) return finishKnown();
        if (sameAccount() && !result_.creationId.empty()) return poll(true);
        return recovery(std::string(fog::constants::error::Session::CannotSaveStopIntentOrderRemainsUnresolvedPrefix) + error.what());
    }
    catch (...) {
        // Already-owned rentals remain stoppable when the SD becomes unwritable.
        if (sameAccount() && !result_.rentalId.empty()) return finishKnown();
        if (sameAccount() && !result_.creationId.empty()) return poll(true);
        return recovery(fog::constants::error::Session::CannotSaveStopIntentOrderRemainsUnresolved);
    }
    if (sameAccount() && !result_.rentalId.empty()) return finishKnown();
    return poll(true);
}
CheckoutResult CheckoutGuard::acknowledgeManualResolution() {
    if (!sessions_.pendingRental().empty() || !sessions_.journalError().empty())
        throw std::logic_error(fog::constants::error::Session::ResolveSessionRecoveryBeforeAcknowledgingCheckout);
    if (!pending_) return result_;
    if (!invalid_ && !sameAccount()) throw std::logic_error(fog::constants::error::Auth::SignInToTheOriginalAccountFirst);
    clear();
    result_.message = "Local marker cleared after your account check; no server cancellation was sent";
    return result_;
}
}
