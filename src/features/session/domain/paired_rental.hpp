#pragma once
#include "features/session/domain/service_models.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace fog {
enum class PairedPreparationStage {
    None, Validation, ComputerPreview, AccountHistory, IdleBaseline, ConnectionPage,
    FreshPairingLink, FreshPairingPage, ConnectionForm, AccountRecheck
};
inline const char* pairedPreparationStageName(PairedPreparationStage stage) {
    switch (stage) {
        case PairedPreparationStage::None: return "none";
        case PairedPreparationStage::Validation: return "validation";
        case PairedPreparationStage::ComputerPreview: return "computer-preview";
        case PairedPreparationStage::AccountHistory: return "account-history";
        case PairedPreparationStage::IdleBaseline: return "idle-baseline";
        case PairedPreparationStage::ConnectionPage: return "connection-page";
        case PairedPreparationStage::FreshPairingLink: return "fresh-pairing-link";
        case PairedPreparationStage::FreshPairingPage: return "fresh-pairing-page";
        case PairedPreparationStage::ConnectionForm: return "connection-form";
        case PairedPreparationStage::AccountRecheck: return "account-recheck";
    }
    return "unknown";
}
// Carries only a fixed stage/category; provider response and exception text are
// deliberately discarded at the preparation boundary.
class PairedPreparationError final : public RepositoryError {
public:
    PairedPreparationError(PairedPreparationStage stage,RepositoryErrorKind kind,
            std::optional<RentalId> requiredRating = std::nullopt)
        : RepositoryError(kind,"Paired rental preparation failed"),stage_(stage),requiredRating_(std::move(requiredRating)) {}
    PairedPreparationStage stage() const noexcept { return stage_; }
    const std::optional<RentalId>& requiredRating() const noexcept { return requiredRating_; }
private:
    PairedPreparationStage stage_;
    std::optional<RentalId> requiredRating_;
};
// In-memory transport address only. Never serialize endpoints or preparation handles.
struct PairedEndpoint {
    std::string host;
    // Zero means undiscovered. Pairing must obtain HTTPS from serverinfo before
    // any HTTPS connection; never derive it arithmetically from the HTTP port.
    std::uint16_t httpsPort = 0;
    // Required explicit base HTTP port from the provider; zero is invalid.
    std::uint16_t httpPort = 0;
};
struct PairedPreparation {
    EntityId id; // Opaque, single-use adapter handle, scoped to this auth generation.
    EntityId computer;
    PairedEndpoint endpoint;
    RentalId rental; // Empty for a new rental; exact known ID for reconnect.
};
struct PairedLaunchReceipt {
    RentalId rental;
    bool reportedError = false; // A partial response can still identify a paid rental.
};
using PairedSubmissionCheck = std::function<bool()>;
// May only be thrown before the mutating request is dispatched. It lets the live
// coordinator clear its own unsubmitted intent without treating a timeout as safe.
class PairedSubmissionCancelled final : public RepositoryError {
public:
    PairedSubmissionCancelled() : RepositoryError(RepositoryErrorKind::InvalidRequest,
        "Rental command cancelled before dispatch") {}
};
enum class RentalInspectionState { Unknown, Preparing, Ready, Terminal };
// Fresh authenticated own-history evidence. Missing rows are Unknown, never Terminal.
// A provider must verify the exact rental ID and account before returning identity fields.
struct RentalInspection {
    RentalId rental;
    AccountId account;
    EntityId computer;
    RentalInspectionState state = RentalInspectionState::Unknown;
    std::optional<PairedEndpoint> endpoint;
};
// Bounded implementations only. begin starts local pairing without waiting for the host.
// begin must discover an unknown HTTPS port from the supplied host/base HTTP port.
// waitReady requires bounded evidence that getservercert was dispatched, followed
// by a short settle interval, BEFORE the provider receives the PIN. GameStream
// does not acknowledge this challenge until PIN submission; this is not proof
// of remote receipt. complete is the authoritative pairing success check.
// complete waits for the pairing result AFTER that provider request.
// cancelAndJoin must always leave no worker or callback accessing the client/PIN.
class PairingClient {
public:
    virtual ~PairingClient() = default;
    virtual bool begin(const PairedEndpoint&, const std::string& pin) = 0;
    virtual bool waitReady() = 0;
    // Non-blocking liveness check used immediately before the paid submission.
    virtual bool submissionReady() const noexcept { return true; }
    virtual bool complete() = 0;
    virtual void cancelAndJoin() noexcept = 0;
    // Fixed phase labels and numeric codes only; no endpoint, identity or PIN.
    virtual std::string diagnostics() const { return {}; }
};
class PairingPinGenerator {
public:
    virtual ~PairingPinGenerator() = default;
    // Production implementations must use a secure random four-digit PIN.
    virtual std::string generate() = 0;
};
}
