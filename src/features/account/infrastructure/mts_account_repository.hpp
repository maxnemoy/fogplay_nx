#pragma once
#include "features/account/domain/account_repository.hpp"
#include "features/session/infrastructure/api.hpp"
#include "features/session/infrastructure/mts_operation.hpp"
#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
namespace fog {
class MtsAccountRepository final : public AccountRepository {
public:
    explicit MtsAccountRepository(Api& api) : api_(api) {}
    UserProfile profile() override {
        mtsOperation([&]{ api_.userInfo(); });
        return {{ProviderId::Mts,api_.accountIdentity()}};
    }
    std::vector<HistoryEntry> history() override { return mtsOperation([&]{ return mapMtsHistory(api_.orderHistory()); }); }
private:
    Api& api_;
};
}
