#pragma once
#include "features/session/domain/service_models.hpp"
#include "features/auth/domain/auth_repository.hpp"
#include <vector>

namespace fog {
// Serial worker access; authentication changes invalidate adopted rental ownership.
class RentalRepository {
public:
    virtual ~RentalRepository() = default;
    virtual AuthData auth() const = 0;
    virtual std::vector<Rental> activeSessions(bool shutdown = false) = 0;
    // Command acknowledgement only. Fresh absence is verified separately by SessionGuard.
    virtual bool finishSession(const RentalId& rental) = 0;
    virtual CreationReceipt createOrder(const StartRentalRequest& request) = 0;
    virtual RentalPreparation orderStatus(const CreationId& creation) = 0;
};
}
