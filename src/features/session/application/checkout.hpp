#pragma once
#include "features/session/application/session.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fog {
enum class CheckoutState { Idle, Pending, PaymentRequired, Ready, Rejected, RecoveryRequired };
struct CheckoutResult {
    CheckoutState state = CheckoutState::Idle;
    std::int64_t orderId = 0;
    std::string hash;
    std::string paymentUrl;
    std::string paymentAmount;
    std::string message;
    bool stopRequested = false;
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
        (result.stopRequested && (result.orderId > 0 || !result.hash.empty()));
}
// Serial API worker only. No destructor requests and no automatic create retry.
class CheckoutGuard {
public:
    CheckoutGuard(RentalService& api, SessionGuard& sessions, std::string journalPath);
    const CheckoutResult& result() const noexcept { return result_; }
    bool unresolved() const noexcept { return pending_; }
    CheckoutResult create(const CreateOrderRequest& order);
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
    RentalService& api_;
    SessionGuard& sessions_;
    std::string journalPath_;
    std::string account_;
    CreateOrderRequest selection_;
    std::vector<std::string> before_;
    CheckoutResult result_;
    bool pending_ = false;
    bool invalid_ = false;
    unsigned polls_ = 0;
};
}
