#pragma once
#include "features/session/application/session.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fog {
enum class CheckoutState { Idle, Pending, PaymentRequired, Ready, Rejected, RecoveryRequired };
struct CheckoutResult {
    CheckoutState state = CheckoutState::Idle;
    CreationId creationId;
    RentalId rentalId;
    std::string paymentUrl;
    std::string paymentAmount;
    std::string message;
    bool stopRequested = false;
    PreparationState preparation = PreparationState::Unknown;
    int serverStatus = -1;
    bool serverHashPresent = false;
    unsigned pollCount = 0;
    unsigned readyObservations = 0;
};
// Retry a recorded stop even after a transient status failure, before its hash exists.
// This only schedules checking/stopping an existing order, never creation.
inline bool checkoutNeedsAutomaticCheck(const CheckoutResult& result) {
    return ((result.state == CheckoutState::Pending || result.state == CheckoutState::PaymentRequired) &&
        result.pollCount < 30) ||
        (result.stopRequested && (!result.creationId.empty() || !result.rentalId.empty()));
}
// Serial API worker only. No destructor requests and no automatic create retry.
class CheckoutGuard {
public:
    CheckoutGuard(RentalRepository& api, SessionGuard& sessions, std::unique_ptr<CheckoutJournal> journal);
    const CheckoutResult& result() const noexcept { return result_; }
    bool unresolved() const noexcept { return pending_; }
    CheckoutResult create(const StartRentalRequest& order);
    // One bounded GET. UI schedules at least 2 seconds apart; max 30 automatic polls.
    CheckoutResult poll(bool manualRetry = false);
    // Persist stop intent. If hash is unknown, keep polling; this is NOT cancellation.
    CheckoutResult stop();
    // Only after the user verifies on the official account that no rental/charge remains.
    // Does not send cancellation, and never reports server termination.
    CheckoutResult acknowledgeManualResolution();
private:
    void load();
    void persist();
    void clear();
    bool sameAccount() const;
    CheckoutResult recovery(const std::string& message);
    CheckoutResult waiting(const std::string& message);
    CheckoutResult finishReady();
    CheckoutResult finishKnown();
    RentalRepository& api_;
    SessionGuard& sessions_;
    std::unique_ptr<CheckoutJournal> journal_;
    AccountId account_;
    StartRentalRequest selection_;
    std::vector<RentalId> before_;
    CheckoutResult result_;
    bool pending_ = false;
    bool invalid_ = false;
    unsigned polls_ = 0;
};
}
