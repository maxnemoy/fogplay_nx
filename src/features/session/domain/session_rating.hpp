#pragma once
#include "features/session/domain/service_models.hpp"
#include <cstddef>

namespace fog {
// A validated provider blocker identifies the exact session requiring review.
// The URL, CSRF and raw page remain inside infrastructure.
class SessionReviewRequired final : public RepositoryError {
public:
    explicit SessionReviewRequired(RentalId rental)
        : RepositoryError(RepositoryErrorKind::ReviewRequired,"A previous session requires a review"),rental_(std::move(rental)) {}
    const RentalId& rental() const noexcept { return rental_; }
private:
    RentalId rental_;
};
struct SessionRatingForm {
    RentalId rental;
    EntityId preparation;
    std::string computerName;
    bool alreadyRated = false;
    std::optional<int> existingScore;
    bool commentSupported = false;
    std::size_t commentMaxLength = 0;
    std::string sessionDate;
};
struct SessionRatingRequest {
    RentalId rental;
    EntityId preparation;
    int score = 0;
    std::string comment;
};
struct SessionRatingResult { RentalId rental; int confirmedScore = 0; };
}
