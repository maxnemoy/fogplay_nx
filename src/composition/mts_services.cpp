#include "composition/mts_services.hpp"
#include "core/constants/resources.hpp"
#include <switch.h>
#include "core/constants/storage_paths.hpp"
#include "features/auth/infrastructure/mts_auth_repository.hpp"
#include "features/account/infrastructure/mts_account_repository.hpp"
#include "features/catalog/infrastructure/mts_catalog_repository.hpp"
#include "features/session/infrastructure/mts_rental_repository.hpp"
#include "features/session/infrastructure/rental_journal_storage.hpp"
#include "features/streaming/infrastructure/stream.hpp"
namespace fogplay {
namespace {
struct PublicCatalog {
    fog::Api api;
    fog::MtsCatalogRepository repository;
    explicit PublicCatalog(const std::string& id) : api(id), repository(api) {}
};
class MtsServices final : public Services {
public:
    explicit MtsServices(std::string id) : id_(std::move(id)), api_(id_), auth_(api_), account_(api_), catalog_(api_), rentals_(api_) {
        api_.configureAuthStorage(fog::constants::storage::path(fog::constants::storage::Auth));
    }
    fog::AuthRepository& auth() override { return auth_; }
    fog::AccountRepository& account() override { return account_; }
    fog::CatalogRepository& catalog() override { return catalog_; }
    fog::RentalRepository& rentals() override { return rentals_; }
    std::shared_ptr<fog::CatalogRepository> publicCatalog() override {
        auto owner = std::make_shared<PublicCatalog>(id_);
        return {owner, &owner->repository};
    }
    std::unique_ptr<fog::SessionJournal> sessionJournal(const std::string& path) override { return fog::makeMtsSessionJournal(path); }
    std::unique_ptr<fog::CheckoutJournal> checkoutJournal(const std::string& path) override { return fog::makeMtsCheckoutJournal(path); }
    std::unique_ptr<stream::StreamingClient> stream() override { std::array<unsigned char,5> random{}; randomGet(random.data(),random.size());
        std::string suffix;
        for (auto value : random) suffix += "0123456789abcdefghijklmnopqrstuvwxyz"[value % 36];
        return std::make_unique<stream::Client>(fog::constants::resources::CertificateBundle,suffix); }
    void cancel() noexcept override { api_.cancel(); }
private:
    std::string id_;
    fog::Api api_;
    fog::MtsAuthRepository auth_;
    fog::MtsAccountRepository account_;
    fog::MtsCatalogRepository catalog_;
    fog::MtsRentalRepository rentals_;
};
}
std::unique_ptr<Services> makeMtsServices(const std::string& deviceId) { return std::make_unique<MtsServices>(deviceId); }
}
