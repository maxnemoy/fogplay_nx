#pragma once
#include "features/session/domain/service_models.hpp"
#include "features/session/domain/paired_rental.hpp"
#include "features/session/domain/session_rating.hpp"
#include "features/auth/domain/auth_repository.hpp"
#include <vector>

namespace fog {
// Serial worker access; authentication changes invalidate adopted rental ownership.
class RentalRepository {
public:
    virtual ~RentalRepository() = default;
    virtual AuthData auth() const = 0;
    virtual bool pairedLaunchSupported() const noexcept { return false; }
    virtual bool sessionRatingSupported() const noexcept { return false; }
    // No target discovers the latest owned finished session still awaiting a rating.
    virtual std::optional<SessionRatingForm> ratingForm(const std::optional<RentalId>& = std::nullopt) {
        throw RepositoryError(RepositoryErrorKind::Unsupported,"Session rating is unsupported");
    }
    virtual SessionRatingResult submitRating(const SessionRatingRequest&) {
        throw RepositoryError(RepositoryErrorKind::Unsupported,"Session rating is unsupported");
    }
    virtual PairedPreparation preparePairedRental(const StartRentalRequest&) { throw RepositoryError(RepositoryErrorKind::Unsupported, "Paired launch is unsupported"); }
    virtual PairedLaunchReceipt createPairedRental(const EntityId&, const std::string&) { throw RepositoryError(RepositoryErrorKind::Unsupported, "Paired launch is unsupported"); }
    // The callback is ephemeral, never retained, and must be checked after any
    // provider preflight immediately before dispatching the one paid request.
    virtual PairedLaunchReceipt createPairedRental(const EntityId& id, const std::string& pin,
            const PairedSubmissionCheck& canSubmit) {
        bool allowed = false;
        try { allowed = canSubmit && canSubmit(); } catch (...) {}
        if (!allowed) throw PairedSubmissionCancelled();
        return createPairedRental(id,pin);
    }
    virtual RentalInspection inspectRental(const RentalId&) { throw RepositoryError(RepositoryErrorKind::Unsupported, "Exact rental inspection is unsupported"); }
    virtual PairedPreparation preparePairedReconnect(const RentalId&) { throw RepositoryError(RepositoryErrorKind::Unsupported, "Paired reconnect is unsupported"); }
    virtual void completePairedReconnect(const EntityId&, const std::string&) { throw RepositoryError(RepositoryErrorKind::Unsupported, "Paired reconnect is unsupported"); }
    virtual std::vector<Rental> activeSessions(bool shutdown = false) = 0;
    // Command acknowledgement only. Fresh absence is verified separately by SessionGuard.
    virtual bool finishSession(const RentalId& rental) = 0;
    // Paired-rental owners persist their attempt only at the actual dispatch
    // boundary, after provider ownership/action preflight. Never retain check.
    virtual bool finishSession(const RentalId& rental,const PairedSubmissionCheck& canSubmit) {
        bool allowed = false;
        try { allowed = canSubmit && canSubmit(); } catch (...) {}
        if (!allowed) throw PairedSubmissionCancelled();
        return finishSession(rental);
    }
    virtual CreationReceipt createOrder(const StartRentalRequest& request) = 0;
    virtual RentalPreparation orderStatus(const CreationId& creation) = 0;
};
}
