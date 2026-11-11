"""Real Player plus actual input/presenter callbacks: freeze preserves display anchor."""
from player_test_build import ROOT, run_player
source=(ROOT/'src/features/streaming/presentation/playback_controller.cpp').read_text()
def method(name):
    at=source.index('ApplicationController::'+name+'(');start=source.rfind('\n',0,at)+1;br=source.index('{',at);depth,end=1,br+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
presenter=(ROOT/'src/app/presentation/application_presenter.cpp').read_text()
begin=presenter.index('        loopTiming_.presented(')
present=presenter[begin:presenter.index(';',begin)+1].replace('std::chrono::steady_clock::now()','now')
overlay=presenter[presenter.index('void ApplicationController::drawStreamOverlay('):presenter.index('void ApplicationController::text(')]
assert 'inputSuspended' not in overlay and 'inputFreezeWaiting_' not in overlay
harness=r'''
#include "player_fakes.hpp"
#include "features/session/application/rental_stop_coordinator.hpp"
#include "features/streaming/domain/loop_timing.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include <iostream>
using namespace player_test;
namespace fog::stream { enum class OverlayMode { Closed,Mouse,Menu }; }
struct ApplicationController : fog::stream::PlayerInputEffects {
    Trace trace;
    TransportFactory transports{trace};MediaFactory medias{trace};
    Player::Time now=Player::Time{}+1s;
    Player player_{transports,medias,*this,{false,true,true,true},[this]{return now;}};
    fog::stream::StreamInputGate streamInputGate_;
    fog::stream::LoopTiming loopTiming_;
    fog::RentalStopCoordinator rentalSession_;
    struct Overlay {fog::stream::OverlayMode mode()const{return fog::stream::OverlayMode::Closed;}} overlay_;
    unsigned neutralized=0;
    void suspendVideoInput(){++neutralized;streamInputGate_.suspendVideo();}
    void videoSuspended(fog::stream::VideoSuspension)override;
    void videoPrepared(bool)override;
    void transportStarting(::stream::ControllerProfile,::stream::StreamInputSink&)override{}
    void transportPumped(::stream::StreamInputSink&)override{}
    void beforeTransportClose(::stream::StreamInputSink*)noexcept override{}
    void transportClosed()noexcept override{}
    void tick(){player_.observe(now);player_.beforeMedia({},now);player_.media(now);}
    void frame(){::stream::Packet packet;transports.created.back()->packets.push_back(packet);tick();}
    void present(){auto* view=player_.presentation();view->draw();view->present();player_.presentationCompleted(now);
'''+present+r'''
    }
};
'''+method('videoSuspended')+'\n'+method('videoPrepared')+r'''
int main(){
    for(const bool core:{false,true}){
        ApplicationController app;Player::Start start;start.rental={fog::ProviderId::Mts,"fixture"};app.player_.start(start,{});
        const auto origin=app.now;app.frame();app.present();
        app.transports.created.back()->suspension={core,false,true};app.tick();
        assert(app.neutralized==1&&app.streamInputGate_.videoSuspended()&&!app.player_.snapshot().lossWaiting);
        for(unsigned ms=160;ms<1500;ms+=16){app.now=origin+std::chrono::milliseconds(ms);app.tick();app.present();}
        app.transports.created.back()->suspension={};app.tick();
        assert(app.streamInputGate_.videoSuspended());app.now=origin+1520ms;app.frame();app.present();
        assert(app.loopTiming_.lifetime.newPresentGap.maxUs==1520000&&app.loopTiming_.lifetime.continuityBreaks==0&&app.neutralized==1);
    }
    fog::stream::InputSuspensionStats stats;
    stats.observe({},100);stats.observe({true,false,true},200);stats.observe({true,true,false},350);stats.observe({false,true,false},500);
    assert(stats.count(0)==1&&stats.count(1)==1&&stats.count(2)==1);
    assert(stats.durationMs(0)==300&&stats.durationMs(1)==150&&stats.durationMs(2)==150&&stats.text(550).find("localMs=200")!=std::string::npos);
    stats.observe({},600);stats.observe({false,false,true},650);stats.observe({},750);assert(stats.count(2)==2&&stats.durationMs(2)==250);
    std::cout<<"Input-only freeze: actual Player + production input/present hooks, 1520ms anchor, one neutralization, reason counts/durations PASS\n";
}
'''
run_player(harness)