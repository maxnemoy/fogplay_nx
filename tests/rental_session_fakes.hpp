#pragma once
#include "features/session/application/rental_session_coordinator.hpp"
#include <cassert>
#include <functional>
#include <future>
#include <stdexcept>

namespace rental_test {
using Owner = fog::RentalSessionCoordinator;
using namespace std::chrono_literals;
inline fog::EntityId id(fog::ProviderId provider, const char* value) { return {provider,value}; }
struct Disk {
    fog::SessionJournalRecord session;
    fog::CheckoutJournalLoad checkout;
    fog::PairedCheckoutLoad paired;
    bool failSessionWrite = false, failSessionClear = false, failCheckoutWrite = false, failPairedWrite = false, failPairedClear = false;
    unsigned sessionWrites = 0, checkoutWrites = 0, pairedWrites = 0;
};
struct SessionJournal final : fog::SessionJournal {
    Disk& disk;
    explicit SessionJournal(Disk& value) : disk(value) {}
    fog::SessionJournalRecord load() override { return disk.session; }
    void write(const fog::RentalId& rental) override {
        ++disk.sessionWrites;
        if (disk.failSessionWrite) throw fog::JournalError("synthetic write failure");
        disk.session = {rental,true,false,{}};
    }
    void clear() override { if (disk.failSessionClear) throw fog::JournalError("synthetic clear failure"); disk.session = {}; }
    bool hasMarker() const noexcept override { return disk.session.persistent; }
};
struct CheckoutJournal final : fog::CheckoutJournal {
    Disk& disk;
    explicit CheckoutJournal(Disk& value) : disk(value) {}
    fog::CheckoutJournalLoad load() override { return disk.checkout; }
    void write(const fog::CheckoutJournalRecord& record) override {
        ++disk.checkoutWrites;
        if (disk.failCheckoutWrite) throw fog::JournalError("synthetic checkout write failure");
        disk.checkout = {record,true,false,{}};
    }
    void clear() override { disk.checkout = {}; }
};
struct PairedJournal final : fog::PairedCheckoutJournal {
    Disk& disk;
    explicit PairedJournal(Disk& value) : disk(value) {}
    fog::PairedCheckoutLoad load() override { return disk.paired; }
    void write(const fog::PairedCheckoutRecord& record) override {
        ++disk.pairedWrites;
        if (disk.failPairedWrite) throw fog::JournalError("synthetic paired write failure");
        disk.paired = {record,true,false};
    }
    void clear() override { if (disk.failPairedClear) throw fog::JournalError("synthetic paired clear failure"); disk.paired = {}; }
};
struct Pair final : fog::PairingClient, fog::PairingPinGenerator {
    unsigned begins = 0, completes = 0, joins = 0;
    bool ready = false, completeOk = true;
    std::function<void()> readyAction;
    bool begin(const fog::PairedEndpoint&, const std::string&) override { ++begins; return true; }
    bool waitReady() override { if (readyAction) readyAction(); ready = true; return true; }
    bool submissionReady() const noexcept override { return ready; }
    bool complete() override { ++completes; return completeOk; }
    void cancelAndJoin() noexcept override { ++joins; }
    std::string generate() override { return "0427"; }
};
struct Repo final : fog::RentalRepository {
    const fog::ProviderId provider;
    Disk& disk;
    fog::AuthData identity;
    std::vector<fog::Rental> active;
    unsigned creates = 0, stops = 0, inspections = 0, polls = 0;
    bool unknownCreate = false, finishAcknowledged = true, finishTerminal = true, activeFailure = false, inspectionFailure = false;
    bool pairedTerminal = false, noHash = false;
    std::function<void()> createAction;
    std::function<void()> finishAction, pollAction;
    explicit Repo(fog::ProviderId value, Disk& storage)
        : provider(value), disk(storage), identity{id(value,"account"),fog::AuthState::Authenticated,1} {}
    fog::AuthData auth() const override { return identity; }
    std::vector<fog::Rental> activeSessions(bool = false) override {
        if (activeFailure) throw fog::RepositoryError(fog::RepositoryErrorKind::NetworkFailure,"synthetic outage");
        return active;
    }
    bool finishSession(const fog::RentalId&) override {
        ++stops;
        if (finishAction) finishAction();
        if (finishTerminal) { active.clear(); pairedTerminal = true; }
        return finishAcknowledged;
    }
    fog::CreationReceipt createOrder(const fog::StartRentalRequest&) override {
        ++creates; assert(disk.checkout.pending);
        if (createAction) createAction();
        if (unknownCreate) throw fog::RepositoryError(fog::RepositoryErrorKind::OutcomeUnknown,"synthetic unknown outcome");
        return {id(provider,"order"),false,std::nullopt};
    }
    fog::RentalPreparation orderStatus(const fog::CreationId& creation) override {
        ++polls;
        if (pollAction) pollAction();
        return {creation,fog::PreparationState::PaymentConfirmed,fog::CancellationState::NotRequested,
            noHash ? fog::RentalId{} : id(provider,"rental"),3,!noHash};
    }
    bool pairedLaunchSupported() const noexcept override { return provider == fog::ProviderId::FogHub; }
    fog::PairedPreparation preparePairedRental(const fog::StartRentalRequest&) override {
        return {id(provider,"prepared"),id(provider,"computer"),{"host.invalid",47984,47989},{}};
    }
    fog::PairedLaunchReceipt createPairedRental(const fog::EntityId&, const std::string&) override {
        ++creates; assert(disk.paired.pending);
        if (createAction) createAction();
        if (unknownCreate) throw fog::RepositoryError(fog::RepositoryErrorKind::OutcomeUnknown,"synthetic unknown outcome");
        return {id(provider,"rental"),false};
    }
    fog::RentalInspection inspectRental(const fog::RentalId& rental) override {
        ++inspections;
        if (inspectionFailure) throw fog::RepositoryError(fog::RepositoryErrorKind::NetworkFailure,"synthetic inspection outage");
        return {rental,identity.account,id(provider,"computer"),
            pairedTerminal ? fog::RentalInspectionState::Terminal : fog::RentalInspectionState::Ready,
            fog::PairedEndpoint{"host.invalid",47984,47989}};
    }
};
struct Fixture {
    Disk disk;
    Pair pair;
    Repo mts{fog::ProviderId::Mts,disk}, fh{fog::ProviderId::FogHub,disk};
    Owner::Time now{};
    std::unique_ptr<Owner> owner;
    explicit Fixture(bool paired = true) { reset(paired); }
    void reset(bool paired = true) {
        std::optional<Owner::PairedDependencies> dependencies;
        if (paired) dependencies.emplace(Owner::PairedDependencies{fh,pair,pair,std::make_unique<PairedJournal>(disk)});
        owner = std::make_unique<Owner>(Owner::MtsDependencies{mts,std::make_unique<SessionJournal>(disk),std::make_unique<CheckoutJournal>(disk)},
            std::move(dependencies),[this] { return now; });
    }
    fog::StartRentalRequest order(fog::ProviderId provider = fog::ProviderId::Mts) const {
        return {id(provider,"computer"),id(provider,"offer"),{},false};
    }
    Owner::Effects run(Owner::Request request, Owner::Admission admission = {}) {
        const auto plan = owner->plan(std::move(request),admission,now);
        assert(plan.work);
        std::optional<Owner::WorkerResult> result;
        std::exception_ptr failure;
        try { result = owner->execute(*plan.work); } catch (...) { failure = std::current_exception(); }
        return failure ? owner->completeJoined(failure,admission,now) : owner->completeJoined(std::move(*result),admission,now);
    }
};
// Deterministic existing-slot analogue: one future shared by rental and external
// auth/service jobs. It never permits a second executor alongside that slot.
struct SerialSlot {
    std::future<void> pending;
    bool busy() const { return pending.valid(); }
    bool start(std::function<void()> work) {
        if (busy()) return false;
        pending = std::async(std::launch::async,std::move(work)); return true;
    }
    void join() { pending.get(); }
};
}
