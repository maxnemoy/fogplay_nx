#pragma once
#include "features/session/application/checkout.hpp"
#include "features/session/application/paired_checkout.hpp"
#include "features/session/application/rental_stop_coordinator.hpp"
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace fog {
// Owns guards and journal authority. The application lends its existing one
// service-task slot; this owner never creates a thread/future or exposes a guard.
class RentalSessionCoordinator {
public:
    using Time = RentalStopCoordinator::Time;
    using Clock = PairedCheckoutGuard::Clock;
    struct MtsDependencies {
        RentalRepository& repository;
        std::unique_ptr<SessionJournal> sessionJournal;
        std::unique_ptr<CheckoutJournal> checkoutJournal;
    };
    struct PairedDependencies {
        RentalRepository& repository;
        PairingClient& pairing;
        PairingPinGenerator& pins;
        std::unique_ptr<PairedCheckoutJournal> journal;
    };
    enum class AuthScope { All, Mts, FogHub };
    enum class Operation { ListSessions, Adopt, Recover, MtsCreate, MtsPoll, SessionStop, CheckoutStop,
        ManualResolution, PairedCreate, PairedPoll, PairedStop, TransportInspection, QualityInspection };
    struct Admission { bool busy = false, exiting = false, focused = true; };
    struct ListSessions {};
    struct Adopt { RentalId rental; EntityId computer; };
    struct Recover {};
    struct MtsCreate { StartRentalRequest request; };
    struct MtsPoll { bool manual = false; };
    struct Stop {};
    struct ManualResolution {};
    struct PairedCreate { StartRentalRequest request; };
    struct PairedPoll {};
    struct InspectTransport { RentalId expected; };
    struct InspectQuality { RentalId expected; };
    using Request = std::variant<ListSessions, Adopt, Recover, MtsCreate, MtsPoll, Stop,
        ManualResolution, PairedCreate, PairedPoll, InspectTransport, InspectQuality>;
    struct PairedView {
        PairedCheckoutState state = PairedCheckoutState::Idle;
        RentalId rental;
        bool stopRequested = false, endpointReady = false;
        std::string message;
        PairedPreparationStage preparationStage = PairedPreparationStage::None;
        std::optional<RepositoryErrorKind> preparationError;
        std::optional<RentalId> requiredRating;
    };
    struct State {
        bool pending = false, durablePending = false, launchReserved = false;
        bool pairedSupported = false, pairedPending = false, checkoutPending = false;
        bool sessionPending = false, sessionOwned = false, journalFault = false;
        bool mtsAuthenticated = false, pairedAuthenticated = false;
        CheckoutResult checkout;
        PairedView paired;
    };
    class Work {
    public:
        Operation operation() const noexcept { return operation_; }
    private:
        friend class RentalSessionCoordinator;
        Work(Operation operation, Request request, std::uint64_t serial, bool resetChecks = false)
            : operation_(operation), request_(std::move(request)), serial_(serial), resetChecks_(resetChecks) {}
        Operation operation_;
        Request request_;
        std::uint64_t serial_;
        bool resetChecks_;
    };
    struct Adopted { RentalId rental; EntityId computer; };
    struct Matched { bool matched = false; };
    using Value = std::variant<std::vector<Rental>, Adopted, Matched, CheckoutResult, PairedView, FinishResult>;
    struct WorkerResult {
        std::uint64_t serial;
        Operation operation;
        Value value;
    };
    struct StartGrant { RentalId rental; EntityId computer; bool fromCatalog = false; };
    enum class ResumePurpose { Transport, Quality };
    enum class ResumeStatus { Ready, Retry, Refused };
    struct ResumeVerdict { ResumePurpose purpose; ResumeStatus status; RentalId expected; };
    enum class Flow { None, MtsCheckout, PairedCheckout, Paused, SessionConfirmed, SessionRetry,
        RecoveryUnmatched, ActiveSessions, ManualResolved, LaunchCancelled };
    struct Effects {
        Flow flow = Flow::None;
        Operation operation = Operation::Recover;
        std::optional<StartGrant> start;
        std::optional<ResumeVerdict> resume;
        std::vector<Rental> sessions;
        bool stopRequired = false, recoverNext = false;
        std::exception_ptr error;
    };
    enum class PlanStatus { NoOp, Busy, Paused, Dispatch, NeedsSignIn, Cancelled };
    struct Plan { PlanStatus status = PlanStatus::NoOp; std::optional<Work> work; };

    RentalSessionCoordinator(MtsDependencies, std::optional<PairedDependencies> = std::nullopt,
        Clock = std::chrono::steady_clock::now);
    ~RentalSessionCoordinator();
    RentalSessionCoordinator(const RentalSessionCoordinator&) = delete;
    RentalSessionCoordinator& operator=(const RentalSessionCoordinator&) = delete;
    const State& state() const noexcept;
    AuthScope recoveryAuthScope() const noexcept;
    bool beginLaunch(Admission);
    bool cancelLaunch() noexcept;
    Plan plan(Request, Admission, Time);
    // The returned value owns its data; execute is used only by the shared worker.
    WorkerResult execute(const Work&);
    Effects completeJoined(WorkerResult, Admission, Time);
    Effects completeJoined(std::exception_ptr, Admission, Time);
    void externalServiceTaskJoined();
    void dispatchFailed() noexcept;
    void request() noexcept;
    void shutdown() noexcept;
    void pause() noexcept;
    bool manualRetry() noexcept;
    bool requested() const noexcept;
    bool paused() const noexcept;
    unsigned attempts() const noexcept;
    bool retryScheduled() const noexcept;
    bool retryDue(Time) const noexcept;
    bool pauseIfExhausted(bool workerBusy, Time) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
