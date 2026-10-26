#include "features/providers/infrastructure/foghub_rental_repository.hpp"
#include "features/session/application/paired_checkout.hpp"
#include <cassert>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>

using namespace fog;
using namespace fog::foghub;
namespace {
std::string fixture(const std::string& name) {
    std::ifstream input("tests/fixtures/foghub/"+name+".html");
    assert(input.good());
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::string replace(std::string value,const std::string& from,const std::string& to) {
    const auto offset = value.find(from); assert(offset != std::string::npos);
    value.replace(offset,from.size(),to); return value;
}
std::string all(std::string value,const std::string& from,const std::string& to) {
    std::size_t offset = 0;
    while ((offset=value.find(from,offset)) != std::string::npos) {
        value.replace(offset,from.size(),to); offset += to.size();
    }
    return value;
}
std::string history(bool active) {
    auto page = all(fixture(active ? "history-active" : "history-terminal"),"9000002","9000042");
    return all(std::move(page),"1836","42");
}
std::string baseline() {
    auto page = history(false);
    const auto start = page.find("<tr data-key=\"9000042\""); assert(start != std::string::npos);
    const auto end = page.find("</tr>",start); assert(end != std::string::npos);
    page.erase(start,end+5-start);
    return replace(std::move(page),"1–2 из 2","1–1 из 1");
}
WebResponse html(std::string body) { return {200,std::move(body),{},"text/html; charset=UTF-8"}; }
struct FakeWeb final : WebTransport {
    std::string profile = fixture("account"), currentHistory = baseline();
    std::string connect = fixture("connect-metadata-synthetic");
    std::string afterCreate = history(true), afterClose = history(false);
    std::deque<std::string> queuedHistory;
    WebResponse submitted{302,{},"/pc/play-success?id=42&withResume=0",{}};
    WebResponse success = html(fixture("launch-success-metadata-synthetic"));
    WebResponse closed{302,{},"/session/list",{}};
    unsigned posts = 0, closes = 0, successReads = 0, historyReads = 0;
    unsigned failHistoryAt = 0;
    bool loseCreate = false, loseClose = false, failCreatedHistory = false;
    bool previousClient = false, freshSubmitted = false;
    Query fields;
    std::function<void()> historyReadAction;
    std::function<void(const Query&)> submitAction;
    WebResponse get(const std::string& path) override {
        if (path == "/user/info") return html(profile);
        if (path == "/user/balance") return {200,"{\"balance\":\"250.00\"}",{},"application/json"};
        if (path == "/session/list") {
            ++historyReads;
            if (historyReadAction) historyReadAction();
            if (historyReads==failHistoryAt) throw TransportError(TransportFailure::Network);
            if (failCreatedHistory && posts) throw TransportError(TransportFailure::Network);
            if (queuedHistory.empty()) return html(currentHistory);
            auto value=queuedHistory.front(); queuedHistory.pop_front(); return html(value);
        }
        if (path == "/pc/preview?id=42") return html(all(fixture("computer-preview-card-excerpt"),"1838","42"));
        if (path == "/pc/play?id=42") {
            if (previousClient) return html("<!doctype html><html><body><form method='post' action='/pc/play?id=42'><button>Продолжить</button></form><a href='/pc/play?id=42&amp;resume=0'>подключите его снова</a></body></html>");
            return html(connect);
        }
        if (path == "/pc/play?id=42&resume=0") {
            assert(previousClient);
            return html(all(connect,"/pc/play?id=42","/pc/play?id=42&amp;resume=0"));
        }
        if (path == "/pc/play-success?id=42&withResume=0") {
            ++successReads; return success;
        }
        assert(false && "Unexpected request; no arbitrary redirect or close GET is allowed");
        return {};
    }
    WebResponse submitLogin(const Query&) override { assert(false); return {}; }
    WebResponse submitPairing(const std::string& pc,const Query& value) override {
        if (submitAction) submitAction(value);
        assert(pc == "42"); ++posts; fields=value; currentHistory=afterCreate;
        if (loseCreate) throw TransportError(TransportFailure::Network);
        return submitted;
    }
    WebResponse submitPairing(const std::string& pc,const Query& value,bool freshPair) override {
        assert(freshPair==previousClient); freshSubmitted=freshPair;
        return submitPairing(pc,value);
    }
    WebResponse closeRental(const std::string& rental) override {
        assert(rental == "9000042"); ++closes;
        if (loseClose) throw TransportError(TransportFailure::Network);
        currentHistory=afterClose; return closed;
    }
    std::vector<std::string> cookies() override { return {"synthetic-cookie"}; }
    void restoreCookies(const std::vector<std::string>&) override {}
    void clearCookies() override {}
    void cancel() noexcept override {}
};
struct Store final : CookieStore {
    std::optional<SavedCookies> load() override { return SavedCookies{"login:fixture-user",{"synthetic-cookie"}}; }
    void save(const SavedCookies&) override {}
    void clear() override {}
};
struct Setup {
    FakeWeb web;
    Store store;
    Session session{web,store};
    Rentals rentals{session};
    Setup() { assert(session.restore().restored); }
    PairedPreparation prepare() { return rentals.preparePairedRental({{ProviderId::FogHub,"42"},{},{},false}); }
    PairedLaunchReceipt create() { return rentals.createPairedRental(prepare().id,"0123"); }
};
template<class Action> void rejects(RepositoryErrorKind kind,Action&& action) {
    bool rejected=false;
    try { action(); }
    catch (const RepositoryError& error) {
        rejected=true; assert(error.kind()==kind);
        const std::string message=error.what();
        assert(message.find("synthetic-csrf")==std::string::npos && message.find("192.0.2.42")==std::string::npos &&
            message.find("0123")==std::string::npos);
    }
    assert(rejected);
}
void happyAndExactIds() {
    Setup s;
    const auto prepared=s.prepare();
    const auto historyBeforeCreate=s.web.historyReads;
    assert(prepared.id.provider==ProviderId::FogHub && prepared.id.value!="42" && prepared.rental.empty());
    assert(prepared.endpoint.host=="192.0.2.42" && prepared.endpoint.httpPort==47989 && prepared.endpoint.httpsPort==0);
    const auto receipt=s.rentals.createPairedRental(prepared.id,"0123");
    assert(receipt.rental==EntityId({ProviderId::FogHub,"9000042"}) && !receipt.reportedError && s.web.posts==1);
    // Exactly the pre-POST safety read; no history request delays journaling ID.
    assert(s.web.historyReads==historyBeforeCreate+1);
    assert(s.web.fields==Query({{"_csrf","synthetic-csrf-placeholder"},{"moonlight_url","192.0.2.42:47989"},{"Pc[pin]","0123"}}));
    auto inspected=s.rentals.inspectRental(receipt.rental);
    assert(inspected.state==RentalInspectionState::Ready && inspected.endpoint && inspected.endpoint->host==prepared.endpoint.host &&
        inspected.account==s.session.auth().account && inspected.computer==prepared.computer);
    assert(s.rentals.activeSessions().size()==1);
    assert(s.rentals.finishSession(receipt.rental) && s.web.closes==1);
    inspected=s.rentals.inspectRental(receipt.rental);
    assert(inspected.state==RentalInspectionState::Terminal && !inspected.endpoint);
    assert(s.rentals.finishSession(receipt.rental) && s.web.closes==1);
    // A verified terminal receipt releases the adapter-local creation guard.
    assert(!s.prepare().id.empty());
    rejects(RepositoryErrorKind::Unsupported,[&]{s.rentals.createOrder({});});
    rejects(RepositoryErrorKind::Unsupported,[&]{s.rentals.orderStatus({});});
    rejects(RepositoryErrorKind::Unsupported,[&]{s.rentals.preparePairedReconnect(receipt.rental);});
}
void preparationGuards() {
    Setup expiredPair; const auto expired=expiredPair.prepare();
    rejects(RepositoryErrorKind::InvalidRequest,[&] {
        expiredPair.rentals.createPairedRental(expired.id,"0123",[] { return false; });
    });
    assert(expiredPair.web.posts==0 && !expiredPair.prepare().id.empty());
    Setup callbackThrows; const auto throwing=callbackThrows.prepare();
    rejects(RepositoryErrorKind::InvalidRequest,[&] {
        callbackThrows.rentals.createPairedRental(throwing.id,"0123",[]() -> bool { throw std::runtime_error("synthetic failure"); });
    });
    assert(callbackThrows.web.posts==0);
    Setup custom; custom.web.connect=all(custom.web.connect,"192.0.2.42:47989","192.0.2.42:54321");
    const auto customPort=custom.prepare();
    assert(customPort.endpoint.httpPort==54321 && customPort.endpoint.httpsPort==0 && custom.web.posts==0);
    Setup active; active.web.currentHistory=history(true);
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{active.prepare();}); assert(active.web.posts==0);
    Setup unknown; unknown.web.currentHistory=replace(history(true),"<small>Активна","<small>Unverified");
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{unknown.prepare();}); assert(unknown.web.posts==0);
    Setup incomplete; incomplete.web.currentHistory=replace(baseline(),"1–1 из 1","1–1 из 2");
    rejects(RepositoryErrorKind::InvalidResponse,[&]{incomplete.prepare();}); assert(incomplete.web.posts==0);
    Setup replaced; const auto first=replaced.prepare(); const auto second=replaced.prepare();
    rejects(RepositoryErrorKind::InvalidRequest,[&]{replaced.rentals.createPairedRental(first.id,"0123");});
    rejects(RepositoryErrorKind::InvalidRequest,[&]{replaced.rentals.createPairedRental(second.id,"12345");});
    assert(replaced.web.posts==0);
    Setup generation; const auto old=generation.prepare(); generation.session.forget(); assert(generation.session.restore().restored);
    rejects(RepositoryErrorKind::InvalidRequest,[&]{generation.rentals.createPairedRental(old.id,"0123");}); assert(generation.web.posts==0);
    Setup account; const auto prepared=account.prepare(); account.web.profile=all(account.web.profile,"fixture-user","different-user");
    rejects(RepositoryErrorKind::NotAuthenticated,[&]{account.rentals.createPairedRental(prepared.id,"0123");});
    assert(account.web.posts==0 && !account.session.authenticated());
    Setup changed; const auto handle=changed.prepare(); changed.web.currentHistory=history(true);
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{changed.rentals.createPairedRental(handle.id,"0123");}); assert(changed.web.posts==0);
}
void unknownCreateNeverRetries() {
    Setup s; const auto prepared=s.prepare(); s.web.loseCreate=true;
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{s.rentals.createPairedRental(prepared.id,"0123");});
    rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.createPairedRental(prepared.id,"0123");});
    s.web.currentHistory=baseline();
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{s.prepare();});
    assert(s.web.posts==1 && s.web.successReads==0);
    for (const auto status : {302L,307L,308L,500L,200L}) {
        Setup redirect;
        redirect.web.submitted={status,{},status==302 ? "https://foreign.invalid/pc/play-success?id=42&withResume=0" : "/pc/play-success?id=42&withResume=0",{}};
        rejects(RepositoryErrorKind::OutcomeUnknown,[&]{redirect.create();});
        assert(redirect.web.posts==1 && redirect.web.successReads==0);
    }
    Setup existing;
    existing.web.success.body=all(existing.web.success.body,"9000042","9000001");
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{existing.create();});
    assert(existing.web.posts==1 && existing.web.successReads==1);
    Setup changedSuccess;
    changedSuccess.web.success={302,{},"/pc/play-success?id=43&withResume=0",{}};
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{changedSuccess.create();});
    assert(changedSuccess.web.posts==1 && changedSuccess.web.successReads==1);
}
void partialReceiptAndOwnership() {
    Setup vanished; const auto known=vanished.create();
    assert(vanished.rentals.inspectRental(known.rental).state==RentalInspectionState::Ready);
    vanished.web.currentHistory=baseline();
    assert(vanished.rentals.inspectRental(known.rental).state==RentalInspectionState::Unknown);
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{vanished.prepare();}); assert(vanished.web.posts==1);
    Setup failed; failed.web.failCreatedHistory=true;
    const auto receipt=failed.create(); assert(!receipt.reportedError && receipt.rental.value=="9000042");
    assert(failed.rentals.inspectRental(receipt.rental).state==RentalInspectionState::Unknown);
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{failed.prepare();});
    Setup missing; missing.web.afterCreate=baseline();
    const auto missingReceipt=missing.create(); assert(!missingReceipt.reportedError && missingReceipt.rental.value=="9000042");
    assert(missing.rentals.inspectRental(missingReceipt.rental).state==RentalInspectionState::Unknown);
    Setup wrong; wrong.web.afterCreate=all(history(true),"/pc/preview?id=42","/pc/preview?id=43");
    const auto wrongReceipt=wrong.create(); assert(!wrongReceipt.reportedError);
    assert(wrong.rentals.inspectRental(wrongReceipt.rental).state==RentalInspectionState::Unknown);
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{wrong.rentals.finishSession(wrongReceipt.rental);}); assert(wrong.web.closes==0);
    Setup adopted; adopted.web.currentHistory=history(true);
    assert(adopted.rentals.inspectRental({ProviderId::FogHub,"9000042"}).state==RentalInspectionState::Preparing);
    assert(adopted.rentals.inspectRental({ProviderId::FogHub,"42"}).state==RentalInspectionState::Unknown);
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{adopted.rentals.finishSession({ProviderId::FogHub,"42"});}); assert(adopted.web.closes==0);
    rejects(RepositoryErrorKind::InvalidRequest,[&]{adopted.rentals.inspectRental({ProviderId::Mts,"9000042"});});
}
void closeFreshnessAndNoReplay() {
    Setup missing; missing.web.currentHistory=baseline();
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{missing.rentals.finishSession({ProviderId::FogHub,"9000042"});}); assert(missing.web.closes==0);
    Setup malformed; malformed.web.currentHistory=replace(history(true),"href=\"/session/close","data-method='post' href=\"/session/close");
    rejects(RepositoryErrorKind::InvalidResponse,[&]{malformed.rentals.finishSession({ProviderId::FogHub,"9000042"});}); assert(malformed.web.closes==0);
    Setup lost; lost.web.currentHistory=history(true); lost.web.loseClose=true;
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{lost.rentals.finishSession({ProviderId::FogHub,"9000042"});});
    rejects(RepositoryErrorKind::OutcomeUnknown,[&]{lost.rentals.finishSession({ProviderId::FogHub,"9000042"});}); assert(lost.web.closes==1);
    lost.web.currentHistory=history(false);
    assert(lost.rentals.finishSession({ProviderId::FogHub,"9000042"}) && lost.web.closes==1);
    Setup unstable; unstable.web.queuedHistory={history(false),history(true)};
    assert(!unstable.rentals.finishSession({ProviderId::FogHub,"9000042"}) && unstable.web.closes==0);
    Setup account; const auto receipt=account.create();
    account.web.profile=all(account.web.profile,"fixture-user","different-user");
    assert(account.rentals.inspectRental(receipt.rental).state==RentalInspectionState::Unknown);
    rejects(RepositoryErrorKind::NotAuthenticated,[&]{account.rentals.finishSession(receipt.rental);});
    assert(account.web.closes==0);
}
struct CheckoutDisk {
    PairedCheckoutLoad saved;
    bool failWrites = false, failClear = false;
};
struct CheckoutJournal final : PairedCheckoutJournal {
    CheckoutDisk& disk;
    explicit CheckoutJournal(CheckoutDisk& value) : disk(value) {}
    PairedCheckoutLoad load() override { return disk.saved; }
    void write(const PairedCheckoutRecord& record) override {
        if (disk.failWrites) throw JournalError("synthetic storage failure");
        disk.saved = {record,true,false};
    }
    void clear() override {
        if (disk.failClear) throw JournalError("synthetic storage failure");
        disk.saved = {};
    }
};
struct AutoPin final : PairingPinGenerator {
    std::string generate() override { return "0123"; }
};
struct LocalPair final : PairingClient {
    FakeWeb& web;
    CheckoutDisk& disk;
    std::string pin;
    bool dispatched = false, alive = true, completeOk = true;
    unsigned begins = 0, completes = 0;
    LocalPair(FakeWeb& w,CheckoutDisk& d) : web(w),disk(d) {}
    bool begin(const PairedEndpoint& endpoint,const std::string& value) override {
        assert(endpoint.host=="192.0.2.42" && endpoint.httpPort==47989 && endpoint.httpsPort==0);
        ++begins; pin=value; return true;
    }
    bool waitReady() override { dispatched=true; return true; }
    bool submissionReady() const noexcept override { return dispatched && alive; }
    bool complete() override {
        ++completes;
        assert(web.posts==1 && disk.saved.record.rental.value=="9000042");
        return completeOk;
    }
    void cancelAndJoin() noexcept override { dispatched=false; }
};
struct CheckoutSetup {
    Setup adapter;
    CheckoutDisk disk;
    AutoPin pins;
    LocalPair pair{adapter.web,disk};
    std::unique_ptr<PairedCheckoutGuard> guard;
    CheckoutSetup() {
        reset();
        adapter.web.submitAction = [&](const Query& fields) {
            assert(pair.dispatched && pair.pin=="0123");
            assert(fields.back()==std::make_pair(std::string("Pc[pin]"),pair.pin));
            assert(disk.saved.pending && disk.saved.record.rental.empty());
        };
    }
    void reset() { guard=std::make_unique<PairedCheckoutGuard>(adapter.rentals,pair,pins,std::make_unique<CheckoutJournal>(disk)); }
    PairedCheckoutResult create() { return guard->create({{ProviderId::FogHub,"42"},{},{},false}); }
};
void automaticCheckoutEndToEnd() {
    { CheckoutSetup s;
      assert(s.create().state==PairedCheckoutState::Ready);
      // stop() first verifies history, then the adapter independently fetches
      // its close-action evidence. Failure in this second read sent no close.
      s.adapter.web.failHistoryAt=s.adapter.web.historyReads+2;
      assert(s.guard->stop().state==PairedCheckoutState::RecoveryRequired && s.adapter.web.closes==0 &&
          s.disk.saved.record.stopRequested && !s.disk.saved.record.stopAttempted);
      assert(s.guard->stop().state==PairedCheckoutState::Terminal && s.adapter.web.closes==1 &&
          s.adapter.web.posts==1 && !s.disk.saved.pending); }
    { CheckoutSetup s; s.adapter.web.previousClient=true;
      assert(s.create().state==PairedCheckoutState::Ready && s.adapter.web.posts==1 && s.adapter.web.freshSubmitted);
      assert(s.guard->stop().state==PairedCheckoutState::Terminal && s.adapter.web.closes==1); }
    { CheckoutSetup s;
      const auto ready=s.create();
      assert(ready.state==PairedCheckoutState::Ready && ready.rental.value=="9000042" && ready.endpoint);
      assert(s.adapter.web.posts==1 && s.pair.begins==1 && s.pair.completes==1 && s.disk.saved.pending);
      const auto reads=s.adapter.web.historyReads;
      s.guard->requestStop();
      assert(s.guard->poll().state==PairedCheckoutState::Terminal && s.adapter.web.closes==1 &&
          s.adapter.web.posts==1 && !s.disk.saved.pending && s.adapter.web.historyReads>=reads+4); }
    { CheckoutSetup s;
      s.adapter.web.historyReadAction = [&] { if (s.adapter.web.historyReads==2) s.pair.alive=false; };
      assert(s.create().state==PairedCheckoutState::Idle && s.adapter.web.posts==0 &&
          !s.disk.saved.pending && !s.guard->unresolved()); }
    { CheckoutSetup s;
      s.adapter.web.historyReadAction = [&] { if (s.adapter.web.historyReads==2) s.guard->requestStop(); };
      assert(s.create().state==PairedCheckoutState::Idle && s.adapter.web.posts==0 &&
          !s.disk.saved.pending && !s.guard->unresolved()); }
    { CheckoutSetup s; s.pair.completeOk=false;
      assert(s.create().state==PairedCheckoutState::Terminal && s.adapter.web.posts==1 && s.adapter.web.closes==1); }
    { CheckoutSetup s; s.adapter.web.loseCreate=true;
      assert(s.create().state==PairedCheckoutState::RecoveryRequired && s.disk.saved.pending);
      s.guard->poll(); s.guard->stop(); s.reset(); s.create();
      assert(s.adapter.web.posts==1 && s.adapter.web.closes==0); }
    { CheckoutSetup s;
      assert(s.create().state==PairedCheckoutState::Ready);
      s.adapter.session.forget(); s.reset();
      assert(s.adapter.session.restore().restored);
      assert(s.guard->bindAuthenticatedAccount().state==PairedCheckoutState::RecoveryRequired);
      assert(s.guard->stop().state==PairedCheckoutState::Terminal && s.adapter.web.posts==1 &&
          s.adapter.web.closes==1 && s.pair.begins==1); }
    { CheckoutSetup s;
      assert(s.create().state==PairedCheckoutState::Ready);
      s.disk.failWrites=true; s.disk.failClear=true; s.adapter.web.loseClose=true;
      assert(s.guard->stop().state==PairedCheckoutState::RecoveryRequired && s.adapter.web.closes==1 && s.disk.saved.pending);
      s.guard->stop(); s.guard->poll();
      assert(s.adapter.web.closes==1 && s.adapter.web.posts==1 && s.guard->unresolved()); }
}
}
int main() {
    happyAndExactIds(); preparationGuards(); unknownCreateNeverRetries(); partialReceiptAndOwnership(); closeFreshnessAndNoReplay(); automaticCheckoutEndToEnd();
    std::cout << "FogHub paired rental adapter offline tests passed\n";
}
