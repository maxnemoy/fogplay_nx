#include "core/constants/error_text.hpp"
#include "features/session/application/session.hpp"
#include <stdexcept>
#include <utility>

namespace fog {
namespace {
bool hasRental(const std::vector<Rental>& sessions, const RentalId& rental) {
    for (const auto& session : sessions) if (session.id == rental) return true;
    return false;
}

}

SessionGuard::SessionGuard(RentalRepository& api, std::unique_ptr<SessionJournal> journal)
    : api_(api), journal_(std::move(journal)) {
    if (!journal_) throw std::invalid_argument("Missing session journal");
    loadJournal();
}
void SessionGuard::loadJournal() {
    const auto saved = journal_->load();
    pendingRental_ = saved.rental;
    persistent_ = saved.persistent;
    invalidJournal_ = saved.invalid;
    journalError_ = saved.error;
}
void SessionGuard::writeJournal() {
    try {
        journal_->write(pendingRental_);
        persistent_ = true;
    } catch (...) {
        if (journal_->hasMarker()) persistent_ = true;
        throw;
    }
}
void SessionGuard::adopt(const RentalId& rental, bool observedActive) {
    if (invalidJournal_ || !journalError_.empty()) throw std::runtime_error(fog::constants::error::Session::SessionJournalNeedsRecovery);
    const auto auth = api_.auth();
    if (!auth.authenticated()) throw std::logic_error(fog::constants::error::Auth::SignInBeforeAdoptingASession);
    if (!validRentalId(rental) || rental.provider != auth.account.provider) throw std::invalid_argument(fog::constants::error::Session::InvalidSessionHash);
    if (!pendingRental_.empty()) throw std::logic_error(fog::constants::error::Session::ResolvePendingSessionBeforeConnectingAnother);
    pendingRental_ = rental;
    observedActive_ = observedActive;
    // An authenticated rental was selected or attributed to the recorded order.
    // Even if the SD write fails, retain authority to stop it; never start media then.
    owned_ = true;
    authGeneration_ = auth.generation;
    ownedAccount_ = auth.account;
    try { writeJournal(); }
    catch (const JournalError& error) {
        journalError_ = std::string(fog::constants::error::Session::SessionJournalWriteFailedLiveConnectionIsBlockedPrefix) + error.what();
        throw;
    }
    catch (...) {
        journalError_ = fog::constants::error::Session::SessionJournalWriteFailedLiveConnectionIsBlocked;
        throw;
    }
}
bool SessionGuard::recoverMatchingSession() {
    const auto auth = api_.auth();
    if (invalidJournal_ || pendingRental_.empty() || !auth.authenticated() ||
        pendingRental_.provider != auth.account.provider) return false;
    if (!hasRental(api_.activeSessions(true), pendingRental_)) return false;
    owned_ = true;
    authGeneration_ = auth.generation;
    ownedAccount_ = auth.account;
    terminationEvidence_ = false;
    observedActive_ = true;
    return true;
}
void SessionGuard::clearJournal() {
    try { if (persistent_) journal_->clear(); }
    catch (...) {
        // Preserve a conservative marker if a durable clear cannot be established.
        try { writeJournal(); } catch (...) {}
        throw;
    }
    persistent_ = false;
    pendingRental_ = {};
    ownedAccount_ = {};
    journalError_.clear();
    owned_ = false;
    terminationEvidence_ = false;
    observedActive_ = false;
}
FinishResult SessionGuard::finishAttempt() {
    if (!ownsSession() || pendingRental_.empty() || invalidJournal_)
        return {false, false, fog::constants::error::Session::PendingSessionMustMatchTheSignedInAccountBeforeStopping};
    bool acknowledged = false;
    try { acknowledged = api_.finishSession(pendingRental_); }
    catch (const FinishRejected&) {}
    catch (const std::logic_error&) {}
    catch (...) {}
    terminationEvidence_ = terminationEvidence_ || acknowledged;
    try {
        const auto sessions = api_.activeSessions(true);
        if (hasRental(sessions, pendingRental_)) {
            observedActive_ = true;
            return {false, false, fog::constants::error::Session::SessionRemainsActiveStopIsUnconfirmedAndCanBeRetried};
        }
        // An attributed checkout hash can precede activation. A failed POST and
        // absence alone never establish that this not-yet-active rental stopped.
        if (!terminationEvidence_ && !observedActive_)
            return {false, false, fog::constants::error::Session::SessionTerminationIsUnconfirmedPendingRecoveryMarkerRetained};
    } catch (...) {
        return {false, false, fog::constants::error::Session::CannotVerifySessionTerminationPendingRecoveryMarkerRetained};
    }
    try { clearJournal(); }
    catch (const JournalError& error) {
        journalError_ = std::string(fog::constants::error::Session::SessionTerminatedButRecoveryJournalCouldNotBeClearedPrefix) + error.what();
        return {true, false, journalError_};
    }
    catch (...) {
        journalError_ = fog::constants::error::Session::SessionTerminatedButRecoveryJournalCouldNotBeCleared;
        return {true, false, journalError_};
    }
    return {true, true, "Session termination confirmed"};
}
}
