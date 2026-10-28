#pragma once
#include "features/session/domain/rental_service.hpp"
#include "features/session/application/rental_journal.hpp"
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>

namespace fog {
enum class PairedCheckoutPhase { Intent, RentalKnown, Pairing, Streaming, StopRequested };
struct PairedCheckoutRecord {
    ProviderId provider = ProviderId::FogHub;
    AccountId account;
    EntityId computer;
    RentalId rental;
    PairedCheckoutPhase phase = PairedCheckoutPhase::Intent;
    bool stopRequested = false;
    bool stopAttempted = false;
};
struct PairedCheckoutLoad {
    PairedCheckoutRecord record;
    bool pending = false;
    bool invalid = false;
};
class PairedCheckoutJournal {
public:
    virtual ~PairedCheckoutJournal() = default;
    virtual PairedCheckoutLoad load() = 0;
    // Must durably replace the record with crash recovery, or throw JournalError.
    virtual void write(const PairedCheckoutRecord&) = 0;
    virtual void clear() = 0;
};
enum class PairedCheckoutState { Idle, Preparing, Ready, Terminal, RecoveryRequired };
struct PairedCheckoutResult {
    PairedCheckoutState state = PairedCheckoutState::Idle;
    RentalId rental;
    bool stopRequested = false;
    std::optional<PairedEndpoint> endpoint; // Ephemeral; never journaled.
    std::string message;
    PairedPreparationStage preparationStage = PairedPreparationStage::None;
    std::optional<RepositoryErrorKind> preparationError = std::nullopt;
    std::optional<RentalId> requiredRating = std::nullopt; // Verified blocker target, never logged.
};
// One serialized worker owns this bounded synchronous coordinator.
// No destructor network requests, no create retry, no manual PIN entry.
// Recovery never infers termination from ACK/absence. UI must keep unresolved records.
class PairedCheckoutGuard {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    PairedCheckoutGuard(RentalRepository&, PairingClient&, PairingPinGenerator&,
                        std::unique_ptr<PairedCheckoutJournal>,
                        Clock = std::chrono::steady_clock::now);
    const PairedCheckoutResult& result() const noexcept { return result_; }
    bool unresolved() const noexcept { return pending_ || invalid_; }
    PairedCheckoutResult create(const StartRentalRequest&);
    // Exactly one fresh inspection; a pending host is never paired/charged again.
    PairedCheckoutResult poll();
    // Same verification for an already paired stream. A temporary read-only
    // network failure returns Preparing so the caller can retry within its budget.
    PairedCheckoutResult inspectForResume();
    // Explicit recovery/reconnect. Only a provider's distinct verified reconnect API is used.
    PairedCheckoutResult reconnect();
    PairedCheckoutResult stop();
    // Call on the serialized worker after login/restore. Rebinds only the
    // recorded original account and verifies fresh exact history; never pairs,
    // creates, or closes. A stop intent is then handled by stop()/poll().
    PairedCheckoutResult bindAuthenticatedAccount();
    // Safe from the UI thread. Bounded pairing/network calls observe this before the next step.
    void requestStop() noexcept { stopSignal_.store(true); }
private:
    PairedCheckoutResult pollImpl(bool retryTransient);
    void resetPreparationDiagnostics();
    bool sameAccount() const;
    bool validRecord() const;
    void persist();
    bool inspect(RentalInspection&);
    bool exact(const RentalInspection&) const;
    PairedCheckoutResult recovery(const char*);
    PairedCheckoutResult terminal();
    PairedCheckoutResult failPaid(const char*);
    PairedCheckoutResult cancelUnsubmitted();
    PairedCheckoutResult stopKnown();
    bool startPairing(const PairedPreparation&, std::string&);
    bool validPreparation(const PairedPreparation&, bool reconnect) const;
    RentalRepository& api_;
    PairingClient& pairing_;
    PairingPinGenerator& pins_;
    std::unique_ptr<PairedCheckoutJournal> journal_;
    PairedCheckoutRecord record_;
    PairedCheckoutResult result_;
    std::uint64_t generation_ = 0;
    bool pending_ = false;
    bool invalid_ = false;
    bool paired_ = false;
    Clock clock_;
    std::optional<std::chrono::steady_clock::time_point> preparingDeadline_;
    std::optional<PairedEndpoint> pairedEndpoint_;
    std::atomic_bool stopSignal_{false};
};
}
