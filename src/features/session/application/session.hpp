#pragma once
#include "features/session/domain/rental_service.hpp"
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
    SessionGuard(RentalService& api, std::string journalPath);
    const std::string& pendingHash() const noexcept { return pendingHash_; }
    const std::string& journalError() const noexcept { return journalError_; }
    bool ownsSession() const noexcept { return owned_ && authGeneration_ == api_.authGeneration(); }
    bool persistent() const noexcept { return persistent_; }
    // observedActive requires a fresh authenticated active-list match, not an
    // order-status hash: the latter may exist while the rental is still preparing.
    void adopt(const std::string& hash, bool observedActive = false);
    // On restart, absence cannot establish account ownership or termination.
    bool recoverMatchingSession();
    FinishResult finishAttempt();
private:
    void loadJournal();
    void writeJournal();
    void clearJournal();
    RentalService& api_;
    std::string journalPath_;
    std::string pendingHash_;
    std::string journalError_;
    bool owned_ = false;
    bool persistent_ = false;
    bool invalidJournal_ = false;
    bool terminationEvidence_ = false;
    bool observedActive_ = false;
    std::uint64_t authGeneration_ = 0;
};
}
