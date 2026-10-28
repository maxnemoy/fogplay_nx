#pragma once
#include <stdexcept>
#include <string>
namespace fog {
enum class RepositoryErrorKind {
    NotAuthenticated, Unavailable, NetworkFailure, InvalidResponse,
    OutcomeUnknown, Unsupported, InvalidRequest, StorageFailure, ReviewRequired,
    ActiveRentalExists, InternalFailure
};
inline const char* repositoryErrorKindName(RepositoryErrorKind kind) {
    switch (kind) {
        case RepositoryErrorKind::NotAuthenticated: return "not-authenticated";
        case RepositoryErrorKind::Unavailable: return "unavailable";
        case RepositoryErrorKind::NetworkFailure: return "network-failure";
        case RepositoryErrorKind::InvalidResponse: return "invalid-response";
        case RepositoryErrorKind::OutcomeUnknown: return "outcome-unknown";
        case RepositoryErrorKind::Unsupported: return "unsupported";
        case RepositoryErrorKind::InvalidRequest: return "invalid-request";
        case RepositoryErrorKind::StorageFailure: return "storage-failure";
        case RepositoryErrorKind::ReviewRequired: return "review-required";
        case RepositoryErrorKind::ActiveRentalExists: return "active-rental-exists";
        case RepositoryErrorKind::InternalFailure: return "internal-failure";
    }
    return "unknown";
}
class RepositoryError : public std::runtime_error {
public:
    RepositoryError(RepositoryErrorKind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    RepositoryErrorKind kind() const noexcept { return kind_; }
private:
    RepositoryErrorKind kind_;
};
}
