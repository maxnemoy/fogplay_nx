#pragma once
#include "features/session/domain/service_models.hpp"
#include <cstdint>

namespace fog {
// Serial worker access; authentication changes invalidate adopted session ownership.
class RentalService {
public:
    virtual ~RentalService() = default;
    virtual bool loggedIn() const noexcept = 0;
    virtual std::uint64_t authGeneration() const noexcept = 0;
    virtual const std::string& accountIdentity() const noexcept = 0;
    virtual Json activeSessions(bool shutdown = false) = 0;
    virtual bool finishSession(const std::string& hash) = 0;
    virtual Json createOrder(const CreateOrderRequest& order) = 0;
    virtual Json orderStatus(std::int64_t orderId) = 0;
};
}
