#pragma once
#include <stdexcept>
#include <string>
namespace fog {
enum class RepositoryErrorKind {
    NotAuthenticated, Unavailable, NetworkFailure, InvalidResponse,
    OutcomeUnknown, Unsupported, InvalidRequest, StorageFailure
};
class RepositoryError : public std::runtime_error {
public:
    RepositoryError(RepositoryErrorKind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {}
    RepositoryErrorKind kind() const noexcept { return kind_; }
private:
    RepositoryErrorKind kind_;
};
}
