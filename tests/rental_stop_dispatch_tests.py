"""Production shared-task/stop adapters with REAL coordinator and blocked transport."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def method(source,name):
    at=source.index('ApplicationController::'+name+'(')
    start=source.rfind('\n',0,at)+1
    brace=source.index('{',at)
    depth,end=1,brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
app=(root/'src/app/application.cpp').read_text()
rental=(root/'src/features/session/presentation/rental_controller.cpp').read_text()
harness=r'''
#include "rental_session_fakes.hpp"
#include "core/constants/ui_text.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
using namespace rental_test;
namespace uiText=fog::constants::ui;
constexpr int AppletFocusState_InFocus=1;
int appletGetFocusState(){return AppletFocusState_InFocus;}
using TaskResult=std::variant<std::monostate,Owner::WorkerResult>;
enum class Task{RentalOperation,VerifyMtsOffer,VerifyPairedOffer};
enum class Screen{Streaming,Recovery,PlayStation,Checkout,Stopping};
struct ApplicationController {
    Fixture fixture;
    Owner& rentalSession_;
    struct PlayerIntent { void cancel() {} } player_;
    static Owner& prepare(Fixture& fixture,int providerCase) {
        if(providerCase==0) fixture.run(Owner::Adopt{id(fog::ProviderId::Mts,"rental"),{}});
        if(providerCase==1) {fixture.mts.noHash=true;fixture.run(Owner::MtsCreate{fixture.order()});}
        if(providerCase==2) fixture.run(Owner::PairedCreate{fixture.order(fog::ProviderId::FogHub)});
        return *fixture.owner;
    }
    explicit ApplicationController(int providerCase=-1):rentalSession_(prepare(fixture,providerCase)) {
        fixture.mts.finishAction=[this]{apiCalled=true;};
        fixture.mts.pollAction=[this]{apiCalled=true;};
        fixture.fh.finishAction=[this]{apiCalled=true;};
    }
    bool exitAfterStop_=false,shutdownDone_=false,checkoutAutoPoll_=true;
    bool exitUnlocked=false,unblock=false,failDispatch=false;
    std::atomic<bool> apiCalled{false},closeEntered{false},closeExited{false},apiCancelled{false};
    std::mutex mutex;
    std::condition_variable condition;
    Screen screen_=Screen::Streaming;
    Task task_=Task::RentalOperation;
    fog::ProviderId loginProvider_=fog::ProviderId::Mts;
    std::string status_,checkoutStatus_;
    std::future<TaskResult> pending_;
    struct Services {ApplicationController* owner;unsigned cancellations=0;void cancel(){++cancellations;owner->apiCancelled=true;}} servicesOwner_{this};
    Services* services_=&servicesOwner_;
    struct Cancel {void cancel(){}} latency_;
    std::shared_ptr<Cancel> catalogApi_;
    void cancelPlayStation(){}
    void diagnostic(const std::string&){}
    void armExitHandling(){exitUnlocked=false;}
    void disarmExitHandling(){exitUnlocked=true;}
    void pauseSessionRecovery();
    void shutdownSession()noexcept;
    Owner::Admission rentalAdmission()const;
    bool startRental(Owner::Request,const char* = "rental-guard");
    void beginFinish(const char*);
    bool startReal(Task,std::function<TaskResult()>);
    bool start(Task kind,std::function<TaskResult()> work) {
        if(failDispatch)throw std::runtime_error("synthetic dispatch failure");
        return startReal(kind,std::move(work));
    }
    void closeStream(){
        assert(rentalSession_.requested());closeEntered=true;
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock,[this]{return unblock;});closeExited=true;
    }
    Owner::Effects joinRental(){
        try {auto result=std::get<Owner::WorkerResult>(pending_.get());return rentalSession_.completeJoined(std::move(result),{},fixture.now);}
        catch(...){return rentalSession_.completeJoined(std::current_exception(),{},fixture.now);}
    }
};
'''
harness+=method(app,'start').replace('ApplicationController::start(','ApplicationController::startReal(')
for name in ['rentalAdmission','startRental','beginFinish','pauseSessionRecovery','shutdownSession']:
    harness+='\n'+method(rental,name)
harness+=r'''
bool waitFor(const std::atomic<bool>& value){const auto limit=std::chrono::steady_clock::now()+2s;while(!value&&std::chrono::steady_clock::now()<limit)std::this_thread::sleep_for(1ms);return value;}
int main(){
    for(int target:{0,1,2}){
        ApplicationController app(target);
        std::thread ui([&]{app.beginFinish("blocked-close");});
        const bool closed=waitFor(app.closeEntered),sent=waitFor(app.apiCalled),stillBlocked=!app.closeExited;
        {std::lock_guard<std::mutex> lock(app.mutex);app.unblock=true;}app.condition.notify_all();ui.join();
        assert(closed&&sent&&stillBlocked&&app.pending_.valid());
        const auto effects=app.joinRental();
        assert(effects.operation==(target==0?Owner::Operation::SessionStop:target==1?Owner::Operation::CheckoutStop:Owner::Operation::PairedStop));
        assert(app.closeExited);
    }
    for(int target:{0,1,2}){
        ApplicationController app(target);app.unblock=true;app.failDispatch=true;
        bool failed=false;try{app.beginFinish("dispatch-throw");}catch(const std::runtime_error&){failed=true;}
        assert(failed&&app.closeEntered&&app.closeExited&&!app.apiCalled&&!app.pending_.valid());
        assert(app.rentalSession_.requested()&&app.rentalSession_.attempts()==1&&app.rentalSession_.state().pending);
        app.failDispatch=false;app.beginFinish("retry");assert(app.pending_.valid());app.joinRental();
    }
    {ApplicationController app;app.unblock=true;app.beginFinish("none");assert(!app.pending_.valid()&&!app.apiCalled&&!app.closeEntered);}
    for(int target:{0,1,2}){
        ApplicationController app(target);app.unblock=true;app.fixture.mts.finishTerminal=false;app.fixture.fh.finishTerminal=false;
        if(target==0)app.fixture.mts.active={{id(fog::ProviderId::Mts,"rental"),"",{}}};
        for(unsigned i=0;i<3;++i){app.beginFinish("budget");assert(app.pending_.valid());app.joinRental();assert(app.rentalSession_.attempts()==i+1);}
        app.apiCalled=false;app.closeEntered=false;app.beginFinish("exhausted");
        assert(app.rentalSession_.paused()&&!app.checkoutAutoPoll_&&app.exitUnlocked&&app.rentalSession_.state().pending);
        assert(!app.pending_.valid()&&!app.apiCalled&&!app.closeEntered&&app.screen_==Screen::Recovery);
    }
    {
        ApplicationController app(0);app.exitAfterStop_=true;app.unblock=true;
        app.pending_=std::async(std::launch::async,[&]()->TaskResult{while(!app.apiCancelled)std::this_thread::sleep_for(1ms);return std::monostate{};});
        const auto began=std::chrono::steady_clock::now();app.shutdownSession();
        assert(app.pending_.wait_for(1s)==std::future_status::ready);app.pending_.get();app.rentalSession_.externalServiceTaskJoined();
        assert(std::chrono::steady_clock::now()-began<1s&&app.exitUnlocked&&app.rentalSession_.state().pending&&!app.apiCalled);
        app.shutdownSession();app.beginFinish("after-shutdown");
        assert(app.servicesOwner_.cancellations==1&&!app.pending_.valid()&&!app.apiCalled&&app.rentalSession_.requested());
    }
    std::cout<<"Real coordinator + production shared-slot adapter: all3 stop targets precede BLOCKED close; dispatch failure consumes attempt/closes/retries; budget/pause, retained journals and sticky OS-exit cancellation/join PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-rental-stop-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    sources=['src/features/session/application/session.cpp','src/features/session/application/checkout.cpp','src/features/session/application/paired_checkout.cpp','src/features/session/application/rental_session_coordinator.cpp']
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(root/'src'),'-I'+str(root/'tests'),*[str(root/file) for file in sources],str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
