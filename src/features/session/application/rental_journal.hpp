#pragma once
#include "features/session/domain/service_models.hpp"
#include <memory>
#include <stdexcept>
#include <string>

namespace fog {
class JournalError : public RepositoryError {
public:
    explicit JournalError(const std::string& message) : RepositoryError(RepositoryErrorKind::StorageFailure, message) {}
};
struct SessionJournalRecord {
    RentalId rental;
    bool persistent = false;
    bool invalid = false;
    std::string error;
};
class SessionJournal {
public:
    virtual ~SessionJournal() = default;
    virtual SessionJournalRecord load() = 0;
    virtual void write(const RentalId& rental) = 0;
    virtual void clear() = 0;
    virtual bool hasMarker() const noexcept = 0;
};
struct CheckoutJournalRecord {
    AccountId account;
    StartRentalRequest selection;
    CreationId creation;
    RentalId rental;
    std::vector<RentalId> before;
    bool stopRequested = false;
};
struct CheckoutJournalLoad {
    CheckoutJournalRecord record;
    bool pending = false;
    bool invalid = false;
    std::string error;
};
class CheckoutJournal {
public:
    virtual ~CheckoutJournal() = default;
    virtual CheckoutJournalLoad load() = 0;
    virtual void write(const CheckoutJournalRecord& record) = 0;
    virtual void clear() = 0;
};
}
