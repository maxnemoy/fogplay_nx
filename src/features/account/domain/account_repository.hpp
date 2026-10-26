#pragma once
#include "features/auth/domain/auth_repository.hpp"
#include "features/catalog/domain/models.hpp"
namespace fog {
struct UserProfile { AccountId account; };
class AccountRepository {
public:
    virtual ~AccountRepository() = default;
    virtual UserProfile profile() = 0;
    virtual std::vector<HistoryEntry> history() = 0;
};
}
