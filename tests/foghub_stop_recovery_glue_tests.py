"""Real FH guard/journal/coordinator recovery; UI blocked-close adapter is tested separately."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
harness=r'''
#include "rental_session_fakes.hpp"
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

using Owner=fog::RentalSessionCoordinator;
struct ApplicationController {
    Disk disk; Repo repo; Pair pair; Pins pins;
    rental_test::Disk mtsDisk;
    rental_test::Repo mts{fog::ProviderId::Mts,mtsDisk};
    std::unique_ptr<Owner> rental;
    std::future<Owner::WorkerResult> pending_;
    bool checkoutAutoPoll_=false,exitUnlocked=false;
    ApplicationController(){reset();}
    void reset(){
        rental=std::make_unique<Owner>(Owner::MtsDependencies{mts,std::make_unique<rental_test::SessionJournal>(mtsDisk),std::make_unique<rental_test::CheckoutJournal>(mtsDisk)},
            Owner::PairedDependencies{repo,pair,pins,std::make_unique<Journal>(disk)});
    }
    void syncPending(){rental->externalServiceTaskJoined();}
    void submit(Owner::Request request){
        const auto plan=rental->plan(std::move(request),{pending_.valid(),false,true},Owner::Time{});
        if(plan.status==Owner::PlanStatus::Paused){checkoutAutoPoll_=false;exitUnlocked=true;}
        if(plan.work){const auto work=*plan.work;pending_=std::async(std::launch::async,[this,work]{return rental->execute(work);});}
    }
    void recoverSession(){submit(Owner::Recover{});}
    void pollCheckout(bool){submit(rental->requested()?Owner::Request{Owner::Stop{}}:Owner::Request{Owner::PairedPoll{}});}
    void retrySessionRecovery(){assert(rental->manualRetry());recoverSession();}
    void joinStop(){
        assert(pending_.valid());
        Owner::Effects effects;
        try{auto result=pending_.get();effects=rental->completeJoined(std::move(result),{},Owner::Time{});}
        catch(...){effects=rental->completeJoined(std::current_exception(),{},Owner::Time{});}
        checkoutAutoPoll_=rental->state().pending&&!rental->paused();exitUnlocked=rental->paused()||!rental->state().pending;
        assert(!effects.start);
    }
    void exhaust(){
        syncPending();recoverSession();joinStop();
        for(unsigned attempt=1;attempt<3;++attempt){pollCheckout(false);joinStop();}
        assert(rental->paused()&&rental->attempts()==3&&!checkoutAutoPoll_&&exitUnlocked);
        assert(rental->state().pending&&rental->state().pairedPending&&disk.saved.pending);
        const auto reads=repo.inspections,stops=repo.closes;
        for(unsigned tick=0;tick<100;++tick)pollCheckout(false);
        assert(!pending_.valid()&&repo.inspections==reads&&repo.closes==stops);
    }
};
int main() {
    // Unknown/lost history after failed launch pauses after three bounded checks.
    // The user finishes on the site; explicit A rechecks the exact ended row.
    {
        ApplicationController app;app.exhaust();
        assert(app.repo.closes==0 && app.repo.creates==0 && app.pair.pairs==0);
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        app.retrySessionRecovery();app.joinStop();
        assert(!app.rental->state().pending && !app.rental->state().pairedPending && !app.rental->paused() && !app.rental->requested());
        assert(!app.disk.saved.pending && app.disk.clears==1 && !app.rental->state().pairedPending);
        assert(app.rental->attempts()==0 && app.repo.closes==0 && app.repo.creates==0 && app.pair.pairs==0);
    }
    // Site-end observed after restart needs no close replay, even if old stop
    // dispatch was persisted and authentication was restored with a new epoch.
    {
        ApplicationController app;app.repo.owner.generation=5;
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        app.syncPending();app.recoverSession();app.joinStop();
        assert(!app.rental->state().pending && !app.disk.saved.pending && app.repo.closes==0 && app.pair.pairs==0);
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
        app.reset();
        app.repo.evidence.state=fog::RentalInspectionState::Preparing;
        app.repo.closeFails=lostReply;app.exhaust();
        assert(app.repo.closes==1 && app.disk.saved.record.stopAttempted);
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;
        app.retrySessionRecovery();app.joinStop();
        assert(!app.rental->state().pending && !app.disk.saved.pending && app.repo.closes==1);
    }
    {
        ApplicationController app;app.disk.failClear=true;
        app.repo.evidence.state=fog::RentalInspectionState::Terminal;app.exhaust();
        assert(app.repo.closes==0 && app.disk.saved.pending);
        app.disk.failClear=false;app.retrySessionRecovery();app.joinStop();
        assert(!app.rental->state().pending && !app.disk.saved.pending && app.repo.closes==0);
    }
    std::cout<<"FogHub REAL coordinator stop/recovery PASS: three-attempt pause, no background retry, external end/restart reconciliation clears admission gate, network/missing/wrong-owner retention, close ACK/lost-reply no replay, SD cleanup retry\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-fh-stop-recovery-') as directory:
    target=Path(directory);(target/'test.cpp').write_text(harness)
    sources=['session.cpp','checkout.cpp','paired_checkout.cpp','rental_session_coordinator.cpp']
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'src'),'-I'+str(root/'tests'),*[str(root/'src/features/session/application'/file) for file in sources],str(target/'test.cpp'),'-o',str(target/'test')],check=True)
    subprocess.run([str(target/'test')],check=True,timeout=20)
