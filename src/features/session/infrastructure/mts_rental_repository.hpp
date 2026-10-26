#pragma once
#include "features/session/domain/rental_service.hpp"
#include "features/session/infrastructure/api.hpp"

namespace fog {
class MtsRentalRepository final : public RentalRepository {
public:
    explicit MtsRentalRepository(Api& api) : api_(api) {}
    AuthData auth() const override;
    std::vector<Rental> activeSessions(bool shutdown = false) override;
    bool finishSession(const RentalId& rental) override;
    CreationReceipt createOrder(const StartRentalRequest& request) override;
    RentalPreparation orderStatus(const CreationId& creation) override;
private:
    Api& api_;
};
}
