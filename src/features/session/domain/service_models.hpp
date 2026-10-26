#pragma once
#include "core/domain/identity.hpp"
#include "core/domain/repository_error.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fog {
using RentalId = EntityId;
using CreationId = EntityId;
struct StartRentalRequest {
    EntityId computerId;
    EntityId offerId;
    EntityId gameId;
    bool ownAccount = false;
};
struct Rental { RentalId id; std::string computerName; std::optional<EntityId> computerId; };
struct PaymentAction { std::string url; std::string amount; };
struct CreationReceipt {
    CreationId id;
    bool reportedError = false;
    std::optional<PaymentAction> payment;
};
enum class PreparationState { Pending, Preparing, PaymentConfirmed, Unpaid, Unknown };
enum class CancellationState { Unspecified, NotRequested, Requested, Invalid };
struct RentalPreparation {
    CreationId id;
    PreparationState state = PreparationState::Unknown;
    CancellationState cancellation = CancellationState::Unspecified;
    RentalId rental;
    int diagnosticStatus = -1;
    bool diagnosticRentalPresent = false;
};
inline bool validSessionHash(const std::string& hash) noexcept {
    return !hash.empty() && hash.size() <= 1024 && std::all_of(hash.begin(), hash.end(),
        [](unsigned char c) { return c > 32 && c != 127; });
}
inline bool validRentalId(const RentalId& id) noexcept { return validSessionHash(id.value); }
class FinishRejected : public RepositoryError {
public:
    FinishRejected() : RepositoryError(RepositoryErrorKind::Unavailable, "Session finish was rejected") {}
};
}
