#include "features/session/application/checkout.hpp"
#include "features/session/infrastructure/rental_journal_storage.hpp"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

fog::EntityId mts(std::string value) { return {fog::ProviderId::Mts, std::move(value)}; }
struct Rental : fog::RentalRepository {
    fog::ProviderId provider = fog::ProviderId::Mts;
    std::string creation = "1", rentalId = "rental-fixture";
    bool authenticated = true;
    fog::EntityId id(const std::string& value) const { return {provider, value}; }
    std::string account = std::string(64, 'a');
    std::uint64_t generation = 1;
    std::vector<fog::Rental> active;
    int creates = 0;
    fog::AuthData auth() const override { return {id(account), authenticated ? fog::AuthState::Authenticated : fog::AuthState::SignedOut, generation}; }
    std::vector<fog::Rental> activeSessions(bool) override { return active; }
    bool finishSession(const fog::RentalId& rental) override {
        require(rental == id(rentalId), "finish only attributed rental");
        active.clear();
        return true;
    }
    fog::CreationReceipt createOrder(const fog::StartRentalRequest&) override {
        ++creates;
        active = {{id(rentalId), "", std::nullopt}};
        return {id(creation), false, std::nullopt};
    }
    fog::RentalPreparation orderStatus(const fog::CreationId& creationId) override {
        require(creationId == id(creation), "poll only recorded order");
        return {id(creation), fog::PreparationState::PaymentConfirmed, fog::CancellationState::Unspecified,
            id(rentalId), 2, true};
    }
};

struct MemorySessionJournal : fog::SessionJournal {
    fog::SessionJournalRecord saved;
    fog::SessionJournalRecord load() override { return saved; }
    void write(const fog::RentalId& rental) override { saved = {rental, true, false, {}}; }
    void clear() override { saved = {}; }
    bool hasMarker() const noexcept override { return saved.persistent; }
};
struct MemoryCheckoutJournal : fog::CheckoutJournal {
    fog::CheckoutJournalLoad saved;
    fog::CheckoutJournalLoad load() override { return saved; }
    void write(const fog::CheckoutJournalRecord& record) override { saved = {record, true, false, {}}; }
    void clear() override { saved = {}; }
};
void opaqueProviderContract() {
    Rental rental;
    rental.provider = fog::ProviderId::FogHub;
    rental.creation = "creation:opaque-uuid";
    rental.rentalId = "rental/opaque/value";
    fog::SessionGuard sessions(rental, std::make_unique<MemorySessionJournal>());
    fog::CheckoutGuard checkout(rental, sessions, std::make_unique<MemoryCheckoutJournal>());
    bool rejected = false;
    try { checkout.create({mts("same-id"), mts("offer"), {}, false}); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && rental.creates == 0 && !checkout.unresolved(), "provider mismatch rejected before create intent");
    require(checkout.create({rental.id("pc:abc"), rental.id("offer:xyz"), {}, false}).state == fog::CheckoutState::Pending,
        "shared checkout accepts opaque non-MTS identifiers");
    for (int i = 0; i < 3; ++i) checkout.poll();
    require(checkout.result().state == fog::CheckoutState::Ready && sessions.pendingRental() == rental.id(rental.rentalId),
        "shared readiness and ownership use provider-qualified identifiers");
    rental.authenticated = false;
    require(!sessions.ownsSession() && !sessions.finishAttempt().terminated, "signed-out state revokes finish authority");
    rental.authenticated = true;
    require(checkout.stop().state == fog::CheckoutState::Idle && !checkout.unresolved(), "shared stop confirms opaque provider rental");

    auto saved = std::make_unique<MemorySessionJournal>();
    saved->saved = {mts(rental.rentalId), true, false, {}};
    fog::SessionGuard legacy(rental, std::move(saved));
    rental.active = {{rental.id(rental.rentalId), "", std::nullopt}};
    require(!legacy.recoverMatchingSession() && !legacy.ownsSession(), "identical values across providers cannot restore ownership");
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("fog-boundary-port-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    try {
        opaqueProviderContract();
        Rental rental;
        fog::SessionGuard sessions(rental, fog::makeMtsSessionJournal((directory / "session.json").string()));
        fog::CheckoutGuard checkout(rental, sessions, fog::makeMtsCheckoutJournal((directory / "checkout.json").string()));
        require(checkout.create({mts("1"), mts("2"), mts("3"), false}).state == fog::CheckoutState::Pending, "create through injected service");
        require(rental.creates == 1, "create sent exactly once");
        for (int i = 0; i < 3; ++i) checkout.poll();
        require(checkout.result().state == fog::CheckoutState::Ready, "three readiness observations permit connection");
        require(sessions.ownsSession(), "ready rental adopted");
        ++rental.generation;
        require(!sessions.ownsSession(), "authentication change invalidates ownership");
        require(checkout.stop().state == fog::CheckoutState::Idle, "stop through injected service");
        require(!checkout.unresolved() && sessions.pendingRental().value.empty(), "confirmed stop clears recovery markers");
        std::filesystem::remove_all(directory);
        std::cout << "Rental service boundary tests passed: " << assertions << " assertions. No HTTP adapter linked.\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << "Rental service boundary test failed: " << error.what() << '\n';
        return 1;
    }
}
