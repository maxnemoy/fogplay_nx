"""Actual FH journal coordinator plus UI recovery methods; no network/mutations."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index('void ApplicationController::' + name + '(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#include "core/constants/ui_text.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "features/session/application/paired_checkout.hpp"
#include "features/session/application/checkout.hpp"
#include "features/session/application/session.hpp"
#include "features/session/application/stop_retry_budget.hpp"
#include <cassert>
#include <future>
#include <iostream>
#include <variant>
namespace uiText = fog::constants::ui;
using namespace std::chrono_literals;
fog::EntityId id(const char* value) {return {fog::ProviderId::FogHub,value};}
struct Disk {
    fog::PairedCheckoutLoad saved{{fog::ProviderId::FogHub,id("owner"),id("123"),id("900"),
        fog::PairedCheckoutPhase::StopRequested,true,true},true,false};
    int clears=0;
    bool failClear=false;
};
struct Journal final : fog::PairedCheckoutJournal {
    Disk& disk;
    explicit Journal(Disk& value):disk(value) {}
    fog::PairedCheckoutLoad load() override {return disk.saved;}
    void write(const fog::PairedCheckoutRecord& value) override {disk.saved={value,true,false};}
    void clear() override {
        ++disk.clears;
        if(disk.failClear) throw fog::JournalError("synthetic storage failure");
        disk.saved={};
    }
};
struct Pair final : fog::PairingClient {
    int pairs=0;
    bool begin(const fog::PairedEndpoint&,const std::string&) override {++pairs;return false;}
    bool waitReady() override {return false;}
    bool complete() override {return false;}
    void cancelAndJoin() noexcept override {}
};
struct Pins final : fog::PairingPinGenerator {std::string generate() override {return "0000";}};
struct Repo final : fog::RentalRepository {
    fog::AuthData owner{id("owner"),fog::AuthState::Authenticated,1};
    fog::RentalInspection evidence{id("900"),id("owner"),id("123"),fog::RentalInspectionState::Unknown,{}};
    int inspections=0, closes=0, creates=0;
    bool networkFailure=false, closeFails=false;
    fog::AuthData auth() const override {return owner;}
    fog::RentalInspection inspectRental(const fog::RentalId& value) override {
        assert(value==id("900"));++inspections;
        if(networkFailure) throw fog::RepositoryError(fog::RepositoryErrorKind::NetworkFailure,"synthetic outage");
        return evidence;
    }
    bool finishSession(const fog::RentalId& value) override {
        assert(value==id("900"));++closes;
        if(closeFails) throw fog::RepositoryError(fog::RepositoryErrorKind::NetworkFailure,"synthetic lost close reply");
        return true; // An ACK never changes the fresh evidence to Terminal.
    }
    std::vector<fog::Rental> activeSessions(bool=false) override {return {};}
    fog::CreationReceipt createOrder(const fog::StartRentalRequest&) override {++creates;return {};}
    fog::RentalPreparation orderStatus(const fog::CreationId&) override {return {};}
};
struct RecoveryResult {bool matched=false;};
using TaskResult=std::variant<std::monostate,fog::FinishResult,RecoveryResult>;
struct ApplicationController {
    enum class Task {None,Finish,CheckoutStop,PairedStop,PairedPoll,CheckoutPoll,RestoreAuth,Recovery};
    enum class Screen {Streaming,Stopping,Checkout,Recovery,PlayStation};
    Disk disk;
    Repo repo;
    Pair pair;
    Pins pins;
    std::unique_ptr<fog::PairedCheckoutGuard> paired_=std::make_unique<fog::PairedCheckoutGuard>(
        repo,pair,pins,std::make_unique<Journal>(disk));
    struct Checkout {
        bool pending=false;
        fog::CheckoutResult snapshot;
        bool unresolved() const {return pending;}
        const fog::CheckoutResult& result() const {return snapshot;}
        void stop(bool=false) {assert(false);}
        void poll(bool=false) {assert(false);}
    } checkout_;
    struct Guard {
        bool persistent() const {return false;}
        std::string pendingRental() const {return {};}
        std::string journalError() const {return {};}
        bool ownsSession() const {return false;}
        bool recoverMatchingSession() {assert(false);return false;}
        fog::FinishResult finishAttempt() {assert(false);return {};}
    } guard_;
    struct Auth {bool authenticated() const {return true;}} auth_;
    struct PairedServices {
        Repo* repo;
        struct Auth {Repo* repo; bool authenticated() const {return repo->owner.authenticated();}};
        Auth auth() const {return {repo};}
        fog::PairingClient* pairing() {return nullptr;}
        bool bindRental(const fog::RentalId&) {assert(false);return false;}
    } pairedServicesOwner_{&repo};
    PairedServices* pairedServices_=&pairedServicesOwner_;
    bool sessionPending_=true,pairedPending_=true,stopAfterTask_=false,checkoutAutoPoll_=false;
    bool recoveryPaused_=false,resetStopChecks_=false,exitAfterStop_=false,shutdownDone_=false;
    bool exitUnlocked=false;
    int closeStreams=0;
    Screen screen_=Screen::Recovery;
    fog::ProviderId loginProvider_=fog::ProviderId::FogHub;
    Task task_=Task::None;
    std::future<TaskResult> pending_;
    fog::StopRetryBudget stopRetry_;
    std::chrono::steady_clock::time_point retryAt_{},checkoutAt_{};
    std::string status_,checkoutStatus_;
    void armExitHandling() {exitUnlocked=false;}
    void disarmExitHandling() {exitUnlocked=true;}
    void closeStream() {++closeStreams;}
    void diagnostic(const std::string&) {}
    std::string safeText(const std::string& value) {return value;}
    bool completePairedTransportReconnect() {return false;}
    bool completePairedVideoReconnect() {return false;}
    bool offerSessionRating(const fog::PairedCheckoutResult&) {return false;}
    std::monostate restoreProviders() {return {};}
    void startStream(const fog::RentalId&,bool) {assert(false);}
    void start(Task,std::function<TaskResult()>);
    void beginFinish(const char* reason="rental-guard");
    void pauseSessionRecovery();
    void retrySessionRecovery();
    void syncPending();
    void recoverSession();
    void handlePairedCheckout();
    void pollCheckout(bool);
    void joinStop() {
        assert(pending_.valid());pending_.get();handlePairedCheckout();
    }
    void exhaust() {
        syncPending();recoverSession();joinStop();
        for(unsigned attempt=1;attempt<3;++attempt) {pollCheckout(false);joinStop();}
        assert(recoveryPaused_ && stopRetry_.attempts()==3 && !checkoutAutoPoll_ && exitUnlocked);
        assert(sessionPending_ && pairedPending_ && disk.saved.pending && paired_->unresolved());
        assert(screen_==Screen::Recovery);
        const auto reads=repo.inspections,stops=repo.closes;
        for(unsigned tick=0;tick<100;++tick) pollCheckout(false);
        assert(!pending_.valid() && repo.inspections==reads && repo.closes==stops);
    }
};
'''
harness += method((root / 'src/app/application.cpp').read_text(), 'start')
source = (root / 'src/features/session/presentation/rental_controller.cpp').read_text()
for name in ('beginFinish', 'pauseSessionRecovery', 'retrySessionRecovery', 'syncPending',
             'recoverSession', 'handlePairedCheckout', 'pollCheckout'):
    harness += '\n' + method(source, name)
harness += r'''
int main() {
    // Unknown/lost history after failed launch pauses after three bounded checks.
    // The user finishes on the site; explicit A rechecks the exact ended row.
    {
        ApplicationController app;app.exhaust();
        assert(app.repo.closes==0 && app.repo.creates==0 && app.pair.pairs==0);
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        app.retrySessionRecovery();app.joinStop();
        assert(!app.sessionPending_ && !app.pairedPending_ && !app.recoveryPaused_ && !app.stopAfterTask_);
        assert(!app.disk.saved.pending && app.disk.clears==1 && !app.paired_->unresolved());
        assert(app.stopRetry_.attempts()==0 && app.repo.closes==0 && app.repo.creates==0 && app.pair.pairs==0);
    }
    // Site-end observed after restart needs no close replay, even if old stop
    // dispatch was persisted and authentication was restored with a new epoch.
    {
        ApplicationController app;app.repo.owner.generation=5;
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        app.syncPending();app.recoverSession();app.joinStop();
        assert(!app.sessionPending_ && !app.disk.saved.pending && app.repo.closes==0 && app.pair.pairs==0);
    }
    for(unsigned failure=0;failure<5;++failure) {
        ApplicationController app;
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        if(failure==0) app.repo.networkFailure=true;
        if(failure==1) app.repo.evidence={}; // Absence is not proof of termination.
        if(failure==2) app.repo.evidence.account=id("different-owner");
        if(failure==3) app.repo.evidence.computer=id("different-computer");
        if(failure==4) app.repo.owner.account=id("different-login");
        app.exhaust();
        assert(app.disk.clears==0 && app.repo.closes==0 && app.repo.creates==0 && app.pair.pairs==0);
    }
    // A close ACK/lost reply while still active preserves the journal. Reads
    // may retry within the UI budget, but dispatch must never be replayed.
    for(bool lostReply:{false,true}) {
        ApplicationController app;
        app.disk.saved.record.stopAttempted=false;
        app.paired_=std::make_unique<fog::PairedCheckoutGuard>(app.repo,app.pair,app.pins,std::make_unique<Journal>(app.disk));
        app.repo.evidence.state=fog::RentalInspectionState::Preparing;
        app.repo.closeFails=lostReply;app.exhaust();
        assert(app.repo.closes==1 && app.disk.saved.record.stopAttempted);
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        app.retrySessionRecovery();app.joinStop();
        assert(!app.sessionPending_ && !app.disk.saved.pending && app.repo.closes==1);
    }
    {
        ApplicationController app;app.disk.failClear=true;
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;app.exhaust();
        assert(app.repo.closes==0 && app.disk.saved.pending);
        app.disk.failClear=false;app.retrySessionRecovery();app.joinStop();
        assert(!app.sessionPending_ && !app.disk.saved.pending && app.repo.closes==0);
    }
    std::cout<<"FogHub stop/recovery production glue PASS: three-attempt pause, no background retry, external end/restart reconciliation clears admission gate, network/missing/wrong-owner retention, close ACK/lost-reply no replay, SD cleanup retry\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-fh-stop-recovery-') as directory:
    target = Path(directory)
    (target / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I' + str(root / 'src'), str(root / 'src/features/session/application/paired_checkout.cpp'),
                    str(target / 'test.cpp'), '-o', str(target / 'test')], check=True)
    subprocess.run([str(target / 'test')], check=True, timeout=15)
