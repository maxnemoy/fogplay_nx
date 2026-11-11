#pragma once
// Test wiring only: real Player, reaper, rental coordinator and paired guard.
// No recovery, quality or ownership decision is implemented in this adapter.
#include "player_fakes.hpp"
#include "rental_session_fakes.hpp"
#include <iostream>

namespace player_adapter_test {
using Player = fog::stream::Player;
using Rental = fog::RentalSessionCoordinator;
using fog::ProviderId;
using namespace std::chrono_literals;
inline unsigned assertions = 0;
inline void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
struct Fixture : player_test::Fixture {
    rental_test::Fixture rental;
    Player::Admission admission;
    fog::EntityId identity;
    std::future<Rental::WorkerResult> pending;
    std::optional<Rental::Request> requestedInspection;
    std::vector<Player::StopReason> stops;
    unsigned dispatches = 0;
    bool dispatchFails = false;

    void begin(ProviderId provider = ProviderId::FogHub, int fps = 30,
        fog::connection::Mode mode = fog::connection::Mode::Normal) {
        const auto grant = provider == ProviderId::FogHub ?
            rental.run(Rental::PairedCreate{rental.order(provider)}) :
            rental.run(Rental::Adopt{{provider,"rental"},{}});
        require(bool(grant.start), "real rental owner grants initial playback");
        identity = grant.start->rental;
        auto start = request(provider); start.rental = identity; start.video.framesPerSecond = fps;
        start.connection.mode = mode;
        require(owner.start(start,{}) == Player::StartResult::Started, "actual Player accepts initial transport");
    }
    Player::Admission currentAdmission() const {
        auto value = admission;
        value.stopRequested = value.stopRequested || rental.owner->requested();
        value.rentalPending = value.rentalPending && rental.owner->state().pending;
        value.pairedPending = value.pairedPending && rental.owner->state().pairedPending;
        value.serviceBusy = value.serviceBusy || pending.valid();
        return value;
    }
    void effects() {
        while (const auto effect = owner.takeEffect()) {
            if (const auto* stop = std::get_if<Player::StopRequired>(&*effect)) {
                stops.push_back(stop->reason);
                rental.owner->request();
            } else {
                const auto inspect = std::get<Player::InspectionNeeded>(*effect);
                require(!pending.valid(), "Player emits at most one outstanding inspection");
                requestedInspection = inspect.purpose == Player::Purpose::Transport ?
                    Rental::Request{Rental::InspectTransport{inspect.expected}} :
                    Rental::Request{Rental::InspectQuality{inspect.expected}};
                auto plan = rental.owner->plan(*requestedInspection,{},rental.now);
                ++dispatches;
                if (!plan.work || dispatchFails) {
                    if (plan.work) rental.owner->dispatchFailed();
                    owner.inspectionDispatchFailed();
                    continue;
                }
                pending = std::async(std::launch::deferred,[this,work=*plan.work] {
                    return rental.owner->execute(work);
                });
            }
        }
    }
    Player::BeforeMedia step() {
        owner.observe(now);
        const auto result = owner.beforeMedia(currentAdmission(),now);
        effects();
        return result;
    }
    Rental::Effects join() {
        require(pending.valid(), "inspection owns the deferred service slot");
        auto result = pending.get();
        return rental.owner->completeJoined(std::move(result),{},rental.now);
    }
    // Typed shell translation only; the production coordinator supplies verdicts.
    void deliver(const Rental::Effects& result) {
        if (!owner.snapshot().inspection) return;
        if (!rental.owner->state().pairedPending) { owner.closeResolvedRental(); return; }
        auto purpose = result.operation == Rental::Operation::TransportInspection ? Player::Purpose::Transport : Player::Purpose::Quality;
        Player::ResumeVerdict verdict{purpose,Player::Verdict::Refused,{}};
        if (result.resume) {
            verdict.expected = result.resume->expected;
            verdict.purpose = result.resume->purpose == Rental::ResumePurpose::Transport ? Player::Purpose::Transport : Player::Purpose::Quality;
            verdict.verdict = result.resume->status == Rental::ResumeStatus::Ready ? Player::Verdict::Ready :
                result.resume->status == Rental::ResumeStatus::Retry ? Player::Verdict::Retry : Player::Verdict::Refused;
        }
        owner.resumeVerdict(verdict);
    }
    void verifyRequest(Player::Purpose purpose) {
        require(requestedInspection.has_value(), "inspection request recorded");
        if (purpose == Player::Purpose::Transport) {
            const auto* value = std::get_if<Rental::InspectTransport>(&*requestedInspection);
            require(value && value->expected == identity, "transport uses InspectTransport for exact rental");
        } else {
            const auto* value = std::get_if<Rental::InspectQuality>(&*requestedInspection);
            require(value && value->expected == identity, "quality uses InspectQuality for exact rental");
        }
    }
    unsigned starts() const {
        unsigned count = 0;
        for (const auto& state : transports.created) count += state->starts.load();
        return count;
    }
    void loss() {
        client()->recovery = ::stream::RecoveryState::RetryableFailure;
        step(); joined(); effects();
    }
    void due() { now += 2s; step(); }
    void completeTransport() {
        if (identity.provider == ProviderId::FogHub) {
            verifyRequest(Player::Purpose::Transport);
            deliver(join()); step();
        }
    }
    void prepare() {
        client()->recovery.reset(); client()->disconnected = false; client()->ready = true;
        if (media()->progress.workers) media()->pending = true;
        else frame();
        step();
        require(owner.media(now).prepared, "real Player consumes fresh prepared frame");
        effects();
    }
    void stopAndClose() { owner.cancel(); owner.closeAfterStopDispatch(); joined(); }
};
}