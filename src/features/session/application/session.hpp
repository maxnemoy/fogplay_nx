#pragma once
#include "features/session/domain/rental_service.hpp"
#include "features/session/application/rental_journal.hpp"
#include <string>

namespace fog {
struct FinishResult {
    bool terminated = false;
    bool journalCleared = false;
    std::string message;
};

// One serial API worker owns this object. No destructor network calls.
// Never connect the live stream until adopt() succeeds.
class SessionGuard {
public:
    SessionGuard(RentalRepository& api, std::unique_ptr<SessionJournal> journal);
    const std::string& journalError() const noexcept { return journalError_; }
    const RentalId& pendingRental() const noexcept { return pendingRental_; }
    bool ownsSession() const {
        const auto auth = api_.auth();
        return owned_ && auth.authenticated() && authGeneration_ == auth.generation && ownedAccount_ == auth.account;
    }
    bool persistent() const noexcept { return persistent_; }
    // observedActive requires a fresh authenticated active-list match, not an
    // order-status hash: the latter may exist while the rental is still preparing.
    void adopt(const RentalId& rental, bool observedActive = false);
    // On restart, absence cannot establish account ownership or termination.
    bool recoverMatchingSession();
    FinishResult finishAttempt();
private:
    void loadJournal();
    void writeJournal();
    void clearJournal();
    RentalRepository& api_;
    std::unique_ptr<SessionJournal> journal_;
    RentalId pendingRental_;
    AccountId ownedAccount_;
    std::string journalError_;
    bool owned_ = false;
    bool persistent_ = false;
    bool invalidJournal_ = false;
    bool terminationEvidence_ = false;
    bool observedActive_ = false;
    std::uint64_t authGeneration_ = 0;
};
}
