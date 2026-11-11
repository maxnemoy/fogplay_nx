"""Actual app dispatch/close adapters with real rental coordinator + Player."""
from pathlib import Path
import subprocess
import tempfile
import os

root=Path(__file__).resolve().parents[1]
def method(source,name):
    at=source.index('ApplicationController::'+name+'(');begin=source.rfind('\n',0,at)+1
    brace=source.index('{',at);depth,end=1,brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]
app=(root/'src/app/application.cpp').read_text()
rental=(root/'src/features/session/presentation/rental_controller.cpp').read_text()
playback=(root/'src/features/streaming/presentation/playback_controller.cpp').read_text()
harness=r'''
#include "player_fakes.hpp"
#include "rental_session_fakes.hpp"
#include "core/constants/ui_text.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include <iostream>
using namespace rental_test;
using Player=fog::stream::Player;
namespace uiText=fog::constants::ui;
constexpr int AppletFocusState_InFocus=1;
int appletGetFocusState(){return AppletFocusState_InFocus;}
enum class Task{Generic,RentalOperation,VerifyMtsOffer,VerifyPairedOffer};
enum class Screen{Streaming,Recovery,PlayStation,Checkout,Stopping};
using TaskResult=std::variant<std::monostate,Owner::WorkerResult>;
struct ApplicationController {
    Fixture rental;
    Owner& rentalSession_=*rental.owner;
    player_test::Fixture playback;
    fog::stream::Player& player_=playback.owner;
    std::future<TaskResult> pending_;
    Task task_=Task::Generic;
    Screen screen_=Screen::Streaming;
    fog::ProviderId loginProvider_=fog::ProviderId::Mts;
    bool exitAfterStop_=false,shutdownDone_=false,checkoutAutoPoll_=false,streamPlayStation_=false;
    std::string status_,checkoutStatus_,overlayNotice_;
    struct Ui {void setPaused(bool){}void close(){}void reset(){}} artwork_,steamArtwork_,recentLaunch_,overlay_,streamInputGate_;
    std::atomic<bool> cancelled{false};
    struct Services{ApplicationController* owner;void cancel(){owner->cancelled=true;}} servicesOwner_{this};
    Services* services_=&servicesOwner_;
    struct Cancel {void cancel(){}} latency_;
    std::shared_ptr<Cancel> catalogApi_;
    void diagnostic(const std::string&){}
    void armExitHandling(){}
    void disarmExitHandling(){}
    void cancelPlayStation(){}
    bool start(Task,std::function<TaskResult()>);
    Owner::Admission rentalAdmission()const;
    bool startRental(Owner::Request,const char* = "rental-guard");
    void beginFinish(const char*);
    void pauseSessionRecovery();
    void shutdownSession()noexcept;
    void closeStream()noexcept;
    bool completePlayerInspection(const Owner::Effects&);
    Owner::Effects join(){
        try{return rentalSession_.completeJoined(std::get<Owner::WorkerResult>(pending_.get()),{},rental.now);}
        catch(...){return rentalSession_.completeJoined(std::current_exception(),{},rental.now);}
    }
    void open(fog::ProviderId provider){
        const auto result=provider==fog::ProviderId::Mts ? rental.run(Owner::Adopt{id(provider,"rental"),{}}) :
            rental.run(Owner::PairedCreate{rental.order(provider)});
        assert(result.start);auto request=playback.request(provider);request.rental=result.start->rental;
        assert(player_.start(request,{})==fog::stream::Player::StartResult::Started);
    }
};
'''
for source,names in ((app,['start']),(rental,['rentalAdmission','startRental','beginFinish','pauseSessionRecovery','shutdownSession']),(playback,['closeStream','completePlayerInspection'])):
    for name in names:harness+='\n'+method(source,name)
harness+=r'''
static void blockedStop(fog::ProviderId provider){
    ApplicationController app;app.open(provider);
    auto client=app.playback.client();player_test::Blocker blocker;
    client->closeAction=[&]{blocker.block();};
    std::promise<void> apiEntered;auto api=apiEntered.get_future();
    auto record=[&]{app.playback.trace.add("rental-stop-api");apiEntered.set_value();};
    app.rental.mts.finishAction=record;app.rental.fh.finishAction=record;
    bool closeEntered=false,stopEntered=false;
    std::thread observer([&]{closeEntered=blocker.wait();stopEntered=api.wait_for(2s)==std::future_status::ready;blocker.release();});
    app.beginFinish("real-blocked-close");observer.join();
    assert(closeEntered&&stopEntered&&app.pending_.valid());
    const auto completion=app.join();app.playback.joined();
    assert(!app.rentalSession_.state().pending&&!app.player_.snapshot().ownsRental);
    assert(completion.flow==Owner::Flow::SessionConfirmed||completion.flow==Owner::Flow::PairedCheckout);
    const auto trace=app.playback.trace.copy();
    const auto workers=std::find(trace.begin(),trace.end(),"media-worker-join");
    const auto release=std::find(workers,trace.end(),"input-release");
    const auto close=std::find(release,trace.end(),"final-close");
    assert(workers<release&&release<close&&close<trace.end());
    assert(client->closes==1&&client->destroyed==1&&app.playback.media()->destroyed==1);
}
int main(){
    blockedStop(fog::ProviderId::Mts);blockedStop(fog::ProviderId::FogHub);
    {
        ApplicationController app;app.open(fog::ProviderId::Mts);auto client=app.playback.client();
        std::promise<void> release;const auto wait=release.get_future().share();
        assert(app.start(Task::Generic,[&]()->TaskResult{wait.wait();return std::monostate{};}));
        app.beginFinish("busy-slot-stop");
        assert(app.rentalSession_.requested()&&app.rentalSession_.attempts()==0&&client->closes==0&&client->destroyed==0);
        assert(app.player_.snapshot().cancelled&&app.player_.snapshot().ownsRental);
        release.set_value();app.pending_.get();app.rentalSession_.externalServiceTaskJoined();
        app.rental.disk.failSessionClear=true;app.beginFinish("joined-slot-stop");
        const auto result=app.join();
        assert(result.flow==Owner::Flow::SessionRetry&&app.rentalSession_.state().pending&&app.rentalSession_.state().journalFault);
        assert(!app.player_.snapshot().ownsRental&&client->closes==1&&app.rentalSession_.attempts()==1);
    }
    {
        ApplicationController app;app.open(fog::ProviderId::FogHub);auto client=app.playback.client();
        player_test::Blocker retry;client->retryOk=false;client->retryAction=[&]{retry.block();};
        client->error="retryable";client->retryable=true;app.playback.before();const bool entered=retry.wait();
        std::promise<void> release;const auto wait=release.get_future().share();
        assert(app.start(Task::Generic,[&]()->TaskResult{wait.wait();return std::monostate{};}));
        app.beginFinish("busy-refused-reaper");retry.release();app.playback.joined();
        assert(entered&&app.rentalSession_.requested()&&app.rentalSession_.attempts()==0&&client->closes==0&&client->destroyed==0&&app.player_.inputSink());
        release.set_value();app.pending_.get();app.rentalSession_.externalServiceTaskJoined();
        app.beginFinish("joined-refused-reaper");app.join();app.playback.joined();
        assert(client->closes==1&&client->destroyed==1&&!app.rentalSession_.state().pending);
    }
    {
        ApplicationController app;app.open(fog::ProviderId::Mts);auto client=app.playback.client();
        client->closeAction=[&]{assert(app.cancelled);};
        app.shutdownSession();
        assert(app.cancelled&&app.rentalSession_.requested()&&app.rentalSession_.state().pending&&!app.pending_.valid());
        assert(app.rental.mts.stops==0&&client->closes==1);app.player_.shutdownAfterCancellation();
    }
    for(const bool cancelPlayer:{false,true}) {
        ApplicationController app;app.open(fog::ProviderId::FogHub);
        auto settings=app.playback.request().video;settings.bitrateMbps++;
        assert(app.player_.changeVideo(settings,{},app.playback.now)==Player::QualityResult::Inspection);
        const auto effect=app.player_.takeEffect();assert(effect&&std::holds_alternative<Player::InspectionNeeded>(*effect));
        const auto needed=std::get<Player::InspectionNeeded>(*effect);
        assert(app.startRental(Owner::InspectQuality{needed.expected}));
        app.pending_.wait(); // Ready, but not joined: focus sampling still precedes completion delivery.
        app.rentalSession_.request(); // Current-frame focus/stop intent while inspection owned the slot.
        if(cancelPlayer)app.player_.cancel(); // Overlay stop can already have cancelled playback while the slot was busy.
        const auto refused=app.join();assert(!app.pending_.valid()&&app.player_.snapshot().inspection==!cancelPlayer);
        assert(app.completePlayerInspection(refused));
        assert(app.pending_.valid()&&app.rentalSession_.attempts()==1&&app.player_.snapshot().cancelled);
        app.join();app.playback.joined();assert(!app.rentalSession_.state().pending&&app.rental.fh.stops==1);
    }
    std::cout<<"PASS actual startRental/beginFinish/close/shutdown bridge: real coordinator + Player + one shared future, blocked MTS/FH close, media/input/client lifetime, busy stop intent, retained journal, cancel-before-close\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-player-rental-') as directory:
    path=Path(directory);(path/'test.cpp').write_text(harness)
    sources=['src/features/session/application/session.cpp','src/features/session/application/checkout.cpp',
        'src/features/session/application/paired_checkout.cpp','src/features/session/application/rental_session_coordinator.cpp',
        'src/features/streaming/application/player.cpp']
    sanitizers=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if os.environ.get('SANITIZE')=='1' else []
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',*sanitizers,'-I'+str(root/'src'),'-I'+str(root/'tests'),
        *[str(root/file) for file in sources],str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=20)
