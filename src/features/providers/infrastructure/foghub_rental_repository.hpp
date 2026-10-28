#pragma once
#include "features/providers/infrastructure/foghub_repositories.hpp"
#include "features/session/domain/rental_service.hpp"
#include <chrono>
#include <optional>
#include <set>

namespace fog::foghub {
// Dedicated paired capability. Paid creation is single use; unknown outcomes
// require reconciliation, and closing always validates fresh own-history action.
class Rentals final : public fog::RentalRepository {
public:
    explicit Rentals(Session& session) : session_(session),scope_(session.auth()) {}
    AuthData auth() const override { return session_.auth(); }
    bool pairedLaunchSupported() const noexcept override { return true; }
    bool sessionRatingSupported() const noexcept override { return true; }
    std::optional<SessionRatingForm> ratingForm(const std::optional<RentalId>& = std::nullopt) override;
    SessionRatingResult submitRating(const SessionRatingRequest&) override;
    PairedPreparation preparePairedRental(const StartRentalRequest&) override;
    PairedLaunchReceipt createPairedRental(const EntityId& preparation,const std::string& pin) override;
    PairedLaunchReceipt createPairedRental(const EntityId&,const std::string&,
        const PairedSubmissionCheck&) override;
    RentalInspection inspectRental(const RentalId&) override;
    std::vector<Rental> activeSessions(bool shutdown = false) override;
    bool finishSession(const RentalId&) override;
    bool finishSession(const RentalId&,const PairedSubmissionCheck&) override;
    CreationReceipt createOrder(const StartRentalRequest&) override;
    RentalPreparation orderStatus(const CreationId&) override;
private:
    struct Pending {
        EntityId handle,computer;
        AuthData owner;
        ConnectForm form;
        std::set<std::string> baseline;
        std::chrono::steady_clock::time_point expires;
    };
    void synchronize();
    static void validateId(const EntityId&);
    static std::set<std::string> idleBaseline(const Session::OwnHistory&);
    static const HistoryRow* find(const Session::OwnHistory&,const RentalId&);
    static bool terminal(const HistoryRow&);
    RentalInspection inspection(const Session::OwnHistory&,const RentalId&);
    Session& session_;
    AuthData scope_;
    std::optional<Pending> pending_;
    struct RatingPending { RentalId rental; EntityId handle,computer; AuthData owner; };
    std::optional<RatingPending> ratingPending_;
    bool unresolvedCreate_ = false;
    std::optional<RentalId> unresolvedRental_;
    std::set<std::string> closeAttempts_;
    std::map<std::string,EntityId> observedComputers_;
    std::map<std::string,PairedEndpoint> pairedEndpoints_;
};
}
