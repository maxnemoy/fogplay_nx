#pragma once
#include "features/auth/domain/auth_repository.hpp"
#include "features/account/domain/account_repository.hpp"
#include "features/catalog/domain/models.hpp"
#include "features/session/domain/rental_service.hpp"
#include <memory>
#include <string>
namespace fog { class SessionJournal; class CheckoutJournal; }
namespace stream { class StreamingClient; }
namespace fogplay {
// Composition owns provider resources; controllers use only domain contracts.
class Services {
public:
    virtual ~Services() = default;
    // Auth/account/catalog/rentals share one serialized worker; publicCatalog owns isolated cancellable transport.
    virtual fog::AuthRepository& auth() = 0;
    virtual fog::AccountRepository& account() = 0;
    virtual fog::CatalogRepository& catalog() = 0;
    virtual fog::RentalRepository& rentals() = 0;
    virtual std::shared_ptr<fog::CatalogRepository> publicCatalog() = 0;
    virtual std::unique_ptr<fog::SessionJournal> sessionJournal(const std::string& path) = 0;
    virtual std::unique_ptr<fog::CheckoutJournal> checkoutJournal(const std::string& path) = 0;
    virtual std::unique_ptr<stream::StreamingClient> stream() = 0;
    virtual void cancel() noexcept = 0;
};
}
