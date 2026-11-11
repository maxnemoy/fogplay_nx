#include "player_adapter_fixture.hpp"
#include <algorithm>
using namespace player_adapter_test;
using State = ::stream::RecoveryState;

static void commonLifecycle(ProviderId provider) {
    {
        Fixture f; f.begin(provider); auto original = f.client();
        require(f.step().media && f.starts() == 1 && original->retries == 0,
            "healthy provider does not replace transport");
        original->recovery = State::Disconnected; f.step();
        require(f.owner.snapshot().recovering && original->retries == 0 && !f.input.suspensions.empty(),
            "disconnect enters grace and suspends input");
        f.now += 4999ms; f.step();
        require(original->retries == 0, "five second grace retains original transport before boundary");
        f.now += 1ms; f.step(); f.joined();
        require(original->retries == 1 && original->closes == 0 && f.starts() == 1,
            "grace boundary retires transport without final rental close");
        f.now += 1999ms; f.step();
        require(f.starts() == 1 && f.dispatches == 0, "two second backoff does not start early");
        f.now += 1ms; f.step();
        if (provider == ProviderId::FogHub) {
            require(f.pending.valid() && f.starts() == 1, "FH due retry waits for asynchronous ownership inspection");
            const auto inspected = f.rental.fh.inspections;
            f.verifyRequest(Player::Purpose::Transport);
            auto result = f.join();
            require(f.rental.fh.inspections == inspected + 1 && result.resume &&
                result.resume->status == Rental::ResumeStatus::Ready, "real inspectForResume verifies exact rental once");
            f.deliver(result);
            require(f.starts() == 1, "joined verdict is store-only before overlay admission");
            f.step();
        }
        require(f.starts() == 2 && f.client()->connection.resumeOnly && f.client()->connection.rental == f.identity,
            "both providers resume only the original rental");
        require(f.owner.snapshot().connecting && f.owner.snapshot().attempts == 1,
            "accepted start is not successful playback");
        f.prepare();
        require(!f.owner.snapshot().recovering && f.owner.snapshot().attempts == 1,
            "fresh prepared frame ends recovery without clearing rolling budget");
        require(provider == ProviderId::Mts ? f.transports.calls == 1 : f.transports.calls == 2,
            "MTS retains its adapter while FH replaces the joined adapter");
    }
    {
        Fixture f; f.begin(provider); f.client()->recovery = State::Disconnected; f.step();
        f.now += 1s; f.client()->recovery = State::Connected; f.prepare();
        require(!f.owner.snapshot().recovering && f.client()->retries == 0 && f.starts() == 1,
            "fresh playback during grace recovers without replacement");
    }
    {
        Fixture f; f.begin(provider); auto original = f.client();
        original->recovery = State::Disconnected; f.step(); f.now += 5s;
        original->recovery = State::TerminalFailure; f.step();
        require(f.stops == std::vector<Player::StopReason>{Player::StopReason::FatalTransport} &&
            f.starts() == 1 && original->retries == 0 && original->closes == 0,
            "terminal evidence wins over due recovery and final close waits for stop dispatch");
        f.stopAndClose();
        require(original->closes == 1, "explicit stop-dispatch authorization permits final close");
    }
    {
        Fixture f; f.begin(provider); auto original = f.client(); original->retryOk = false;
        f.loss();
        require(f.stops == std::vector<Player::StopReason>{Player::StopReason::RetryTerminal} &&
            original->destroyed == 0 && original->closes == 0 && f.starts() == 1,
            "rejected retry keeps transport alive until stop dispatch");
        f.stopAndClose();
        require(original->closes == 1 && original->destroyed == 1, "rejected transport closes exactly once after stop");
    }
    for (int intent = 0; intent < 5; ++intent) {
        Fixture f; f.begin(provider); f.loss();
        if (intent == 0) f.admission.stopRequested = true;
        if (intent == 1) f.admission.shuttingDown = true;
        if (intent == 2) f.admission.active = false;
        if (intent == 3) f.admission.rentalPending = false;
        if (intent == 4) f.owner.cancel();
        f.due();
        require(f.starts() == 1 && !f.pending.valid() && !f.owner.snapshot().inputAllowed,
            "stop, shutdown, focus loss, lost ownership or cancel prevents a due start");
    }
}
static void relayProviderGate(ProviderId provider) {
    Fixture f; f.begin(provider,30,fog::connection::Mode::Auto); f.prepare();
    require(!f.client()->relay, "automatic relay starts with ordinary transport for both providers");
    f.loss(); f.due(); f.completeTransport(); f.prepare();
    require(!f.client()->relay, "one visible-playback outage does not trigger relay fallback");
    f.loss(); f.due(); f.completeTransport(); f.prepare();
    require(f.client()->relay == (provider == ProviderId::Mts),
        "two verified playback outages enable automatic relay only on MTS");
}
static void budgetsAndDeadlines(ProviderId provider) {
    for (bool expire : {false,true}) {
        Fixture f; f.begin(provider);
        for (unsigned attempt = 1; attempt <= 3; ++attempt) {
            f.loss(); f.due(); f.completeTransport(); f.prepare();
            require(f.owner.snapshot().attempts == attempt && f.stops.empty(),
                "each successful real recovery consumes a rolling-window attempt");
        }
        if (expire) f.now += 120s;
        f.loss();
        if (!expire) {
            require(f.stops == std::vector<Player::StopReason>{Player::StopReason::Exhausted} && f.starts() == 4,
                "fourth loss inside rolling window stops either provider");
        } else {
            f.due(); f.completeTransport(); f.prepare();
            require(f.stops.empty() && f.starts() == 5 && f.owner.snapshot().attempts == 1,
                "old attempts expire after 120 seconds and admit fresh recovery");
        }
    }
    {
        Fixture f; f.begin(provider); f.client()->recovery = State::Disconnected; f.step();
        f.now += 90s; f.step();
        require(f.stops == std::vector<Player::StopReason>{Player::StopReason::Deadline} && f.starts() == 1,
            "ninety second absolute recovery deadline stops both providers");
    }
    {
        Fixture f; f.begin(provider); f.loss(); f.due(); f.completeTransport();
        f.now += 29999ms; f.step();
        require(f.owner.snapshot().connecting && f.starts() == 2, "resume handshake keeps its thirty second deadline");
        f.now += 1ms; f.step(); f.joined();
        require(!f.owner.snapshot().connecting && f.owner.snapshot().recovering && f.starts() == 2,
            "unprepared resume enters backoff at thirty seconds");
    }
    for (int failure = 0; failure < 3; ++failure) {
        Fixture f; f.begin(provider); f.loss();
        auto next = provider == ProviderId::Mts ? f.client() : std::make_shared<player_test::ClientState>();
        if (provider == ProviderId::FogHub) f.transports.next.push_back(next);
        if (failure == 2) f.medias.fail = true;
        else { next->startOk = false; next->retryable = failure == 0; next->error = "setup failure"; }
        f.due(); f.completeTransport(); f.joined(); f.effects();
        require(failure == 0 ? f.stops.empty() && f.owner.snapshot().recovering && !f.owner.snapshot().connecting :
            f.stops == std::vector<Player::StopReason>{Player::StopReason::StartFatal},
            "retryable start failure backs off; fatal start or construction failure requests owned stop");
    }
}
static void inspect(Fixture& f) {
    f.begin(); f.loss(); f.due(); f.verifyRequest(Player::Purpose::Transport);
    require(f.pending.valid() && f.starts() == 1, "FH transport preflight waits in shared slot");
}
static void inspectionContracts() {
    {
        Fixture f; inspect(f); f.rental.fh.inspectionFailure = true;
        const auto result = f.join();
        require(result.resume && result.resume->status == Rental::ResumeStatus::Retry && f.rental.fh.stops == 0,
            "real transport inspectForResume treats network outage as retry without stopping rental");
        f.deliver(result); f.step();
        require(f.starts() == 1 && !f.owner.snapshot().inspection && f.stops.empty(), "retry verdict returns to common backoff");
        f.rental.fh.inspectionFailure = false; f.due(); f.completeTransport();
        require(f.starts() == 2 && f.rental.fh.creates == 1, "retry inspection never creates a second paid rental");
    }
    for (int invalid = 0; invalid < 8; ++invalid) {
        Fixture f; inspect(f); auto result = f.join();
        if (invalid == 0) result.resume->expected.value = "other";
        if (invalid == 1) result.resume->expected.provider = ProviderId::Mts;
        if (invalid == 2) result.resume->purpose = Rental::ResumePurpose::Quality;
        if (invalid == 3) result.resume->status = Rental::ResumeStatus::Refused;
        if (invalid == 4) result.resume.reset();
        f.deliver(result);
        if (invalid == 5) f.admission.stopRequested = true;
        if (invalid == 6) f.admission.shuttingDown = true;
        if (invalid == 7) f.admission.active = false;
        f.step();
        require(f.starts() == 1 && !f.owner.snapshot().inspection && !f.owner.snapshot().inputAllowed,
            "mismatched identity/purpose, missing/refused verdict or fresh stop admission never resumes");
        if (invalid < 5) require(f.stops.size() == 1, "unverified transport verdict requests rental stop");
    }
    for (bool pumpFirst : {false,true}) {
        Fixture f; inspect(f); f.now += 30s;
        if (pumpFirst) f.step();
        const auto result = f.join(); f.deliver(result); f.step();
        require(f.starts() == 1 && !f.stops.empty(), "late ready inspection cannot undo deadline with or without an intervening pump");
    }
    {
        Fixture f; f.begin(); f.loss(); f.dispatchFails = true; f.due();
        require(f.stops == std::vector<Player::StopReason>{Player::StopReason::InspectionDispatch} && !f.pending.valid(),
            "failed inspection dispatch stops owned rental and clears intent");
    }
    {
        Fixture f; inspect(f); f.rental.fh.pairedTerminal = true;
        auto result = f.join(); require(!f.rental.owner->state().pairedPending, "real coordinator confirms terminal rental");
        f.deliver(result); f.step();
        require(f.starts() == 1 && !f.owner.snapshot().ownsRental && f.stops.empty(),
            "terminal ownership closes resolved playback without another resume or stop request");
    }
    {
        Fixture f; inspect(f); auto result = f.join(); f.owner.cancel(); f.deliver(result); f.step();
        require(f.starts() == 1 && !f.owner.snapshot().inspection, "cancelled Player ignores late inspection delivery");
    }
}
static void blockedClose() {
    for (bool deadline : {false,true}) {
        Fixture f; player_test::Blocker blocker;
        auto original = std::make_shared<player_test::ClientState>();
        original->retryAction = [&] { blocker.block(); }; f.transports.next.push_back(original); f.begin();
        original->recovery = State::RetryableFailure; f.step();
        const bool entered = blocker.wait(); f.now += deadline ? 90s : 3s; f.step();
        const bool held = f.owner.snapshot().teardown && f.starts() == 1 && f.dispatches == 0;
        if (!deadline) f.owner.cancel();
        blocker.release(); f.joined(); f.step();
        require(entered && held && f.starts() == 1, "blocked reaper prevents inspection/resume; join cannot undo cancel or deadline");
        if (deadline) require(f.stops == std::vector<Player::StopReason>{Player::StopReason::Deadline},
            "absolute recovery deadline still applies while close blocks");
        const auto trace = f.trace.copy();
        require(std::find(trace.begin(),trace.end(),"media-worker-join") < std::find(trace.begin(),trace.end(),"retry-close"),
            "media workers stop before asynchronous transport close");
    }
}
int main() {
    try {
        for (auto provider : {ProviderId::Mts,ProviderId::FogHub}) { commonLifecycle(provider); relayProviderGate(provider); budgetsAndDeadlines(provider); }
        inspectionContracts(); blockedClose();
        std::cout << "Player transport adapter: " << assertions << " assertions passed\n";
    } catch (const std::exception& error) { std::cerr << "Player transport adapter failed: " << error.what() << '\n'; return 1; }
}