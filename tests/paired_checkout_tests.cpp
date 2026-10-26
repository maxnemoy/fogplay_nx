#include "features/session/application/paired_checkout.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace fog;
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
EntityId id(std::string value) { return {ProviderId::FogHub, std::move(value)}; }
struct Disk {
    PairedCheckoutLoad saved;
    int writes = 0, clears = 0, failWrite = 0;
    bool failAllWrites = false, failClear = false, failLoad = false;
    std::function<void(int)> writeAction;
};
class Journal final : public PairedCheckoutJournal {
public:
    explicit Journal(Disk& disk) : disk_(disk) {}
    PairedCheckoutLoad load() override {
        if (disk_.failLoad) throw JournalError("private SD path");
        return disk_.saved;
    }
    void write(const PairedCheckoutRecord& record) override {
        ++disk_.writes;
        if (disk_.failAllWrites || disk_.writes == disk_.failWrite) throw JournalError("private SD detail");
        disk_.saved = {record, true, false};
        if (disk_.writeAction) disk_.writeAction(disk_.writes);
    }
    void clear() override {
        ++disk_.clears;
        if (disk_.failClear) throw JournalError("private SD detail");
        disk_.saved = {};
    }
private:
    Disk& disk_;
};
struct Pair final : PairingClient {
    bool beginOk = true, readyOk = true, completeOk = true;
    bool begun = false, ready = false;
    bool alive = true;
    int begins = 0, completes = 0, joins = 0;
    std::string pin;
    std::function<void()> readyAction, completeAction, cancelAction;
    bool begin(const PairedEndpoint& endpoint, const std::string& value) override {
        ++begins; begun = true; pin = value;
        require(endpoint.host == "host.invalid", "only prepared endpoint reaches local pair");
        return beginOk;
    }
    bool waitReady() override {
        require(begun, "pair begins before wait");
        ready = readyOk;
        if (readyAction) readyAction();
        return readyOk;
    }
    bool complete() override {
        ++completes;
        if (completeAction) completeAction();
        return completeOk;
    }
    bool submissionReady() const noexcept override { return alive; }
    void cancelAndJoin() noexcept override {
        ++joins; begun = false;
        if (cancelAction) cancelAction();
    }
};
struct Pins final : PairingPinGenerator {
    std::string pin = "0427";
    std::string generate() override { return pin; }
};
struct Repo final : RentalRepository {
    Disk& disk;
    Pair& pair;
    AuthData identity{id("account"), AuthState::Authenticated, 1};
    RentalInspection evidence{id("rental"), id("account"), id("computer"),
        RentalInspectionState::Ready, PairedEndpoint{"host.invalid", 47984, 47989}};
    PairedLaunchReceipt receipt{id("rental"), false};
    bool supported = true, createThrows = false, inspectThrows = false, finishThrows = false;
    bool finishTerminal = true, missingAfterFinish = false, reconnectSupported = false;
    bool badPreparation = false;
    PairedEndpoint preparedEndpoint{"host.invalid", 47984, 47989};
    int creates = 0, inspections = 0, finishes = 0, reconnects = 0;
    std::function<void()> createAction, inspectAction;
    explicit Repo(Disk& d, Pair& p) : disk(d), pair(p) {}
    AuthData auth() const override { return identity; }
    std::vector<Rental> activeSessions(bool = false) override { return {}; }
    CreationReceipt createOrder(const StartRentalRequest&) override { throw std::runtime_error("legacy create must not be called"); }
    RentalPreparation orderStatus(const CreationId&) override { throw std::runtime_error("legacy status must not be called"); }
    bool pairedLaunchSupported() const noexcept override { return supported; }
    PairedPreparation preparePairedRental(const StartRentalRequest&) override {
        return {id("handle"), id(badPreparation ? "other" : "computer"),
                preparedEndpoint, {}};
    }
    PairedLaunchReceipt createPairedRental(const EntityId& prepared, const std::string& pin) override {
        ++creates;
        require(prepared == id("handle"), "only opaque prepared handle submitted");
        require(pair.ready && pair.pin == pin && pin == "0427", "same generated ready PIN submitted");
        require(disk.saved.pending && disk.saved.record.rental.empty() &&
            disk.saved.record.phase == PairedCheckoutPhase::Intent, "durable no-ID intent precedes paid POST");
        if (createAction) createAction();
        if (createThrows) throw RepositoryError(RepositoryErrorKind::OutcomeUnknown, "private URL and PIN");
        return receipt;
    }
    RentalInspection inspectRental(const RentalId& rental) override {
        ++inspections;
        require(rental == id("rental"), "only exact returned rental inspected");
        if (inspectAction) inspectAction();
        if (inspectThrows) throw RepositoryError(RepositoryErrorKind::NetworkFailure, "private detail");
        if (finishes && missingAfterFinish) return {};
        auto result = evidence;
        if (finishes && finishTerminal) result.state = RentalInspectionState::Terminal;
        return result;
    }
    bool finishSession(const RentalId& rental) override {
        ++finishes;
        require(rental == id("rental"), "only exact known rental stopped");
        require(disk.failAllWrites || (disk.saved.record.stopRequested && disk.saved.record.stopAttempted),
                "durable stop intent and attempt precede close unless storage unavailable");
        if (finishThrows) throw RepositoryError(RepositoryErrorKind::NetworkFailure, "private detail");
        return true;
    }
    PairedPreparation preparePairedReconnect(const RentalId& rental) override {
        require(rental == id("rental"), "reconnect exact ID");
        if (!reconnectSupported) throw RepositoryError(RepositoryErrorKind::Unsupported, "unsupported");
        return {id("reconnect"), id("computer"), {"host.invalid", 47984, 47989}, rental};
    }
    void completePairedReconnect(const EntityId& prepared, const std::string& pin) override {
        ++reconnects;
        require(prepared == id("reconnect") && pair.ready && pair.pin == pin, "same-rental reconnect challenge");
    }
};
struct Fixture {
    Disk disk;
    Pair pair;
    Pins pins;
    Repo repo{disk, pair};
    std::chrono::steady_clock::time_point now{};
    std::unique_ptr<PairedCheckoutGuard> guard;
    StartRentalRequest request{id("computer"), {}, {}, false};
    Fixture() { reset(); }
    void reset() { guard = std::make_unique<PairedCheckoutGuard>(repo, pair, pins, std::make_unique<Journal>(disk), [&] { return now; }); }
    PairedCheckoutResult create() { return guard->create(request); }
};
void successAndStop() {
    Fixture f;
    require(f.create().state == PairedCheckoutState::Ready, "successful create pairs and checks own exact history");
    require(f.repo.creates == 1 && f.repo.inspections == 2 && f.pair.completes == 1, "bounded create calls");
    require(f.guard->result().endpoint.has_value() && f.guard->unresolved(), "ready retains durable rental");
    require(f.disk.saved.record.phase == PairedCheckoutPhase::Streaming, "stream phase durable");
    f.create();
    require(f.repo.creates == 1, "ready rental blocks another paid create");
    require(f.guard->stop().state == PairedCheckoutState::Terminal, "exact terminal evidence resolves stop");
    require(!f.guard->unresolved() && !f.disk.saved.pending, "terminal clears journal");
}
void beforeSubmitFailures() {
    { Fixture f; f.pair.cancelAction = [&] { f.guard->requestStop(); }; f.create();
      require(f.repo.creates == 0, "HOME during initial pairing cleanup is never erased"); }
    { Fixture f; f.disk.writeAction = [&](int count) { if (count == 1) f.guard->requestStop(); };
      require(f.create().state == PairedCheckoutState::Idle && f.repo.creates == 0 &&
          !f.disk.saved.pending && !f.guard->unresolved() && f.guard->result().rental.empty(),
          "HOME during intent write clears this known unsubmitted intent"); }
    { Fixture f; f.disk.writeAction = [&](int count) { if (count == 1) ++f.repo.identity.generation; };
      require(f.create().state == PairedCheckoutState::Idle && f.repo.creates == 0 &&
          !f.disk.saved.pending && !f.guard->unresolved() && f.guard->result().rental.empty(),
          "auth change during intent write clears this known unsubmitted intent"); }
    { Fixture f; f.disk.failClear = true;
      f.disk.writeAction = [&](int count) { if (count == 1) f.guard->requestStop(); };
      require(f.create().state == PairedCheckoutState::RecoveryRequired && f.repo.creates == 0 &&
          f.disk.saved.pending && f.guard->unresolved(),
          "failed unsubmitted intent cleanup retains blocking recovery");
      f.disk.failClear = false; f.disk.writeAction = {}; f.reset(); f.create(); f.guard->poll();
      require(f.repo.creates == 0 && f.disk.saved.pending && f.disk.clears == 1,
          "loaded intent cannot reuse prior-process knowledge that POST was not sent"); }
    { Fixture f; f.repo.preparedEndpoint = {"host.invalid", 0, 50123};
      f.repo.evidence.endpoint = f.repo.preparedEndpoint;
      require(f.create().state == PairedCheckoutState::Ready && f.guard->result().endpoint->httpsPort == 0 &&
          f.guard->result().endpoint->httpPort == 50123,
          "unknown HTTPS and explicit custom HTTP port are valid pairing specifications"); }
    { Fixture f; f.repo.preparedEndpoint = {"host.invalid", 0, 50123};
      f.repo.evidence.endpoint = PairedEndpoint{"host.invalid", 55443, 50123};
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "unexplained promotion from unknown HTTPS to a port fails exact specification binding"); }
    { Fixture f; f.repo.preparedEndpoint.httpPort = 0; f.create();
      require(f.repo.creates == 0 && f.pair.begins == 0, "zero base HTTP port is rejected before pairing or charge"); }
    { Fixture f; f.repo.preparedEndpoint = {}; f.create();
      require(f.repo.creates == 0 && f.pair.begins == 0, "default endpoint cannot silently assume provider ports"); }
    { Fixture f; f.disk.failWrite = 1;
      require(f.create().state == PairedCheckoutState::RecoveryRequired, "SD intent failure reported");
      require(f.repo.creates == 0 && f.pair.joins >= 2, "SD failure forbids paid request and joins"); }
    { Fixture f; f.pair.readyOk = false; f.create();
      require(f.repo.creates == 0 && !f.disk.saved.pending, "not-ready pair never charges"); }
    { Fixture f; f.pair.alive = false;
      require(f.create().state == PairedCheckoutState::Idle && f.repo.creates == 0 &&
          !f.disk.saved.pending && !f.guard->unresolved(),
          "pair expired before dispatch clears known unsubmitted intent"); }
    { Fixture f; f.pair.beginOk = false; f.create();
      require(f.repo.creates == 0 && f.pair.joins >= 2, "begin failure joins without charge"); }
    { Fixture f; f.pins.pin = "x427"; f.create();
      require(f.repo.creates == 0 && f.pair.begins == 0, "invalid generated PIN never submitted"); }
    { Fixture f; f.repo.badPreparation = true; f.create();
      require(f.repo.creates == 0 && f.pair.begins == 0, "wrong prepared PC rejected"); }
    { Fixture f; f.repo.supported = false; f.create();
      require(f.repo.creates == 0, "legacy unsupported path never charges"); }
    { Fixture f; f.pair.readyAction = [&] { ++f.repo.identity.generation; }; f.create();
      require(f.repo.creates == 0, "auth generation change before POST prevents charge"); }
    { Fixture f; f.pair.readyAction = [&] { f.guard->requestStop(); }; f.create();
      require(f.repo.creates == 0 && f.pair.joins >= 2, "HOME while waiting readiness cancels before paid POST"); }
}
void unknownAndPartialCreation() {
    { Fixture f; f.repo.createThrows = true;
      require(f.create().state == PairedCheckoutState::RecoveryRequired, "unknown POST stays recovery");
      require(f.guard->unresolved() && f.disk.saved.record.stopRequested, "unknown outcome retains stop intent");
      require(f.guard->result().message.find("private") == std::string::npos, "private provider exception not exposed");
      f.guard->poll(); f.guard->stop(); f.reset(); f.create(); f.guard->reconnect();
      require(f.repo.creates == 1 && f.repo.finishes == 0, "unknown outcome never re-POSTs or guesses ID"); }
    { Fixture f; f.repo.receipt.rental = {};
      f.create(); f.guard->poll();
      require(f.repo.creates == 1 && f.repo.finishes == 0 && f.disk.saved.pending, "missing returned ID retains uncertainty"); }
    { Fixture f; f.repo.receipt.reportedError = true;
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1, "partial known creation is safely stopped"); }
    { Fixture f; f.disk.failWrite = 2;
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "known rental journal failure still stops with durable stop marker"); }
    { Fixture f; f.repo.createAction = [&] { f.guard->requestStop(); };
      require(f.create().state == PairedCheckoutState::Terminal && f.pair.completes == 0, "HOME during POST stops returned rental"); }
    { Fixture f; f.repo.createAction = [&] { f.now += std::chrono::seconds(91); };
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1 && f.pair.completes == 0,
          "creation and receipt time count toward paid preparation deadline"); }
    { Fixture f; f.repo.createAction = [&] { ++f.repo.identity.generation; };
      require(f.create().state == PairedCheckoutState::RecoveryRequired && f.repo.finishes == 0,
          "auth switch after POST forbids wrong-context stop");
      require(f.disk.saved.record.rental == id("rental") && f.disk.saved.record.stopRequested, "auth switch retains exact recovery"); }
}
void ownershipAndPairingFailures() {
    for (int bad = 0; bad < 4; ++bad) {
        Fixture f;
        if (bad == 0) f.repo.evidence.rental = id("other");
        if (bad == 1) f.repo.evidence.account = id("other");
        if (bad == 2) f.repo.evidence.computer = id("other");
        if (bad == 3) f.repo.evidence.state = RentalInspectionState::Unknown;
        require(f.create().state == PairedCheckoutState::RecoveryRequired, "foreign or absent history never streams");
        require(f.repo.finishes == 0 && f.pair.completes == 0 && f.disk.saved.pending, "foreign identity never authorizes finish");
    }
    { Fixture f; f.pair.completeOk = false;
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "pairing rejection stops exact known owned rental"); }
    { Fixture f; f.pair.completeAction = [] { throw std::runtime_error("private pair"); };
      require(f.create().state == PairedCheckoutState::Terminal, "pair exception stops exact rental"); }
    { Fixture f; f.disk.failWrite = 4;
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "stream phase SD failure prevents launch and stops"); }
    { Fixture f; f.repo.inspectThrows = true;
      require(f.create().state == PairedCheckoutState::RecoveryRequired && f.repo.finishes == 0,
          "unverifiable ownership never authorizes finish"); }
    { Fixture f; f.pair.completeAction = [&] { f.guard->requestStop(); };
      require(f.create().state == PairedCheckoutState::Terminal, "HOME during pair completion stops rental"); }
    { Fixture f; f.repo.evidence.endpoint.reset();
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "missing fresh endpoint prevents stream and stops"); }
    { Fixture f; f.disk.writeAction = [&](int count) { if (count == 4) f.guard->requestStop(); };
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "HOME during final journal write prevents stream handoff"); }
    { Fixture f; f.disk.writeAction = [&](int count) { if (count == 4) ++f.repo.identity.generation; };
      require(f.create().state == PairedCheckoutState::RecoveryRequired && !f.guard->result().endpoint &&
          f.repo.finishes == 0 && f.disk.saved.pending,
          "account change during final journal write prevents stream and wrong-account close"); }
    { Fixture f; f.repo.evidence.endpoint->host = "changed.invalid";
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "fresh history host must match the host actually paired"); }
    { Fixture f; f.repo.evidence.endpoint->httpPort = 0;
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "fresh history zero base HTTP port prevents stream and stops known rental"); }
    { Fixture f; f.repo.evidence.endpoint->httpPort = 49999;
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "fresh history base HTTP must exactly match the paired specification"); }
    { Fixture f; f.repo.evidence.endpoint->httpsPort = 49999;
      require(f.create().state == PairedCheckoutState::Terminal && !f.guard->result().endpoint,
          "fresh history ports must match paired endpoint"); }
    { Fixture f; f.repo.evidence.state = RentalInspectionState::Preparing;
      require(f.create().state == PairedCheckoutState::Preparing, "paired preparing rental waits");
      f.repo.evidence.state = RentalInspectionState::Ready;
      require(f.guard->poll().state == PairedCheckoutState::Ready && f.repo.creates == 1 &&
          f.pair.begins == 1, "ready polling never recreates or repairs"); }
    { Fixture f; f.repo.evidence.state = RentalInspectionState::Preparing;
      require(f.create().state == PairedCheckoutState::Preparing, "preparing starts bounded wait");
      f.now += std::chrono::seconds(91);
      require(f.guard->poll().state == PairedCheckoutState::Terminal && f.repo.finishes == 1 &&
          f.repo.creates == 1, "preparing deadline stops exact paid rental without recreate"); }
    { Fixture f; f.pair.completeAction = [&] { f.now += std::chrono::seconds(91); };
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "late pairing completion cannot hand off expired rental"); }
}
void exactTerminationOnly() {
    { Fixture f; f.create();
      f.disk.writeAction = [&](int count) { if (count==6) ++f.repo.identity.generation; };
      require(f.guard->stop().state == PairedCheckoutState::RecoveryRequired && f.repo.finishes == 0 &&
          !f.disk.saved.record.stopAttempted, "auth change during dispatch journal write does not close or consume attempt");
      f.guard->bindAuthenticatedAccount();
      require(f.guard->stop().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "verified account rebind can retry a close proven unsubmitted"); }
    { Fixture f; f.create(); f.repo.finishTerminal = false;
      require(f.guard->stop().state == PairedCheckoutState::RecoveryRequired && f.disk.saved.pending,
          "close ACK while active cannot clear journal");
      f.guard->stop(); f.guard->poll(); f.reset(); f.guard->stop();
      require(f.repo.finishes == 1, "persisted attempted stop forbids blind close retries across restart");
      f.repo.evidence.state = RentalInspectionState::Terminal;
      require(f.guard->stop().state == PairedCheckoutState::Terminal, "later exact terminal evidence resolves"); }
    { Fixture f; f.create(); f.repo.missingAfterFinish = true;
      require(f.guard->stop().state == PairedCheckoutState::RecoveryRequired && f.disk.saved.pending,
          "absence after stop is never terminal evidence"); }
    { Fixture f; f.create(); f.repo.finishThrows = true;
      require(f.guard->stop().state == PairedCheckoutState::Terminal, "timeout can resolve only via exact terminal history"); }
    { Fixture f; f.create(); f.repo.finishThrows = true; f.repo.finishTerminal = false;
      f.guard->stop(); f.guard->stop();
      require(f.repo.finishes == 1 && f.disk.saved.pending, "unknown close outcome does not repeat"); }
    { Fixture f; f.create(); f.disk.failClear = true;
      require(f.guard->stop().state == PairedCheckoutState::RecoveryRequired && f.guard->unresolved(),
          "terminal with failed journal clear retains recovery");
      f.disk.failClear = false;
      require(f.guard->stop().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "cleanup retry needs no additional close"); }
    { Fixture f; f.create(); f.disk.failAllWrites = true;
      require(f.guard->stop().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "known owned rental is stopped despite permanently failed journal writes"); }
    { Fixture f; f.create(); f.disk.failAllWrites = true; f.repo.finishTerminal = false;
      require(f.guard->stop().state == PairedCheckoutState::RecoveryRequired && f.disk.saved.pending &&
          f.repo.finishes == 1, "storage failure preserves pending record when terminal is unverified");
      f.guard->stop(); f.guard->poll();
      require(f.repo.finishes == 1, "in-memory stop attempt prevents replay while storage is unavailable"); }
    { Fixture f; f.repo.createAction = [&] { f.disk.failAllWrites = true; };
      require(f.create().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "receipt remains actionable if every post-create journal write fails"); }
}
void recoveryAndReconnect() {
    { Fixture f; f.create(); f.repo.identity.state = AuthState::SignedOut; f.reset();
      f.repo.identity.state = AuthState::Authenticated; f.repo.identity.generation += 2;
      require(f.guard->bindAuthenticatedAccount().state == PairedCheckoutState::RecoveryRequired &&
          f.repo.finishes == 0 && f.repo.creates == 1 && f.pair.completes == 1,
          "post-construction auth restore verifies original account without pairing or mutation");
      require(f.guard->stop().state == PairedCheckoutState::Terminal && f.repo.finishes == 1,
          "explicit account rebind enables known rental stop without re-pair"); }
    { Fixture f; f.create(); f.repo.identity.state = AuthState::SignedOut; f.reset();
      f.repo.identity.state = AuthState::Authenticated; f.repo.identity.account = id("other");
      f.guard->bindAuthenticatedAccount(); f.guard->stop();
      require(f.repo.finishes == 0 && f.guard->unresolved(), "account bind cannot adopt another account rental"); }
    { Fixture f; f.create(); f.repo.identity.state = AuthState::SignedOut; f.reset();
      f.repo.identity.state = AuthState::Authenticated; ++f.repo.identity.generation;
      f.repo.inspectThrows = true;
      require(f.guard->bindAuthenticatedAccount().state == PairedCheckoutState::RecoveryRequired &&
          f.guard->unresolved() && f.repo.finishes == 0, "account bind preserves recovery if fresh history fails"); }
    { Fixture f; f.create(); f.reset();
      require(f.guard->poll().state == PairedCheckoutState::RecoveryRequired, "restart never assumes prior pairing persists");
      require(f.guard->reconnect().state == PairedCheckoutState::RecoveryRequired && f.repo.creates == 1 &&
          f.repo.finishes == 0, "unsupported reconnect never creates or closes rental"); }
    { Fixture f; f.create(); f.repo.reconnectSupported = true;
      f.pair.cancelAction = [&] { f.guard->requestStop(); };
      require(f.guard->reconnect().state == PairedCheckoutState::Terminal && f.repo.creates == 1 &&
          f.repo.reconnects == 0, "HOME during reconnect cleanup stops known rental"); }
    { Fixture f; f.create(); f.reset(); f.repo.reconnectSupported = true;
      f.pair.readyAction = [&] { f.guard->requestStop(); };
      require(f.guard->reconnect().state == PairedCheckoutState::Terminal && f.repo.reconnects == 0 &&
          f.repo.finishes == 1, "HOME during reconnect readiness stops existing rental"); }
    { Fixture f; f.create(); f.reset(); f.repo.reconnectSupported = true;
      f.disk.writeAction = [&](int count) { if (count == 5) f.guard->requestStop(); };
      require(f.guard->reconnect().state == PairedCheckoutState::Terminal && f.repo.reconnects == 0 &&
          f.repo.finishes == 1, "HOME during reconnect phase journal prevents reconnect mutation and stops"); }
    { Fixture f; f.create(); f.reset(); f.repo.reconnectSupported = true;
      require(f.guard->reconnect().state == PairedCheckoutState::Ready && f.repo.creates == 1 &&
          f.repo.reconnects == 1, "supported same-rental reconnect uses distinct noncreation API"); }
    { Fixture f; f.create(); f.reset(); f.repo.identity.account = id("other");
      require(f.guard->reconnect().state == PairedCheckoutState::RecoveryRequired && f.repo.finishes == 0,
          "recovery refuses another account"); }
    { Fixture f; f.create(); f.reset(); f.repo.reconnectSupported = true; f.pair.completeOk = false;
      require(f.guard->reconnect().state == PairedCheckoutState::Terminal && f.repo.creates == 1,
          "reconnect failure stops known rental without creating"); }
    { Fixture f; f.disk.saved.invalid = true; f.reset(); f.create(); f.guard->stop();
      require(f.repo.creates == 0 && f.repo.finishes == 0 && f.guard->unresolved(),
          "invalid durable journal blocks mutation"); }
    { Fixture f; f.disk.failLoad = true; f.reset(); f.create();
      require(f.repo.creates == 0 && f.guard->unresolved(), "unreadable journal blocks paid create"); }
}
}
int main() {
    try {
        successAndStop(); beforeSubmitFailures(); unknownAndPartialCreation();
        ownershipAndPairingFailures(); exactTerminationOnly(); recoveryAndReconnect();
        std::cout << "Paired checkout: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Paired checkout failed: " << error.what() << "\n";
        return 1;
    }
}
