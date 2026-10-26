#define _POSIX_C_SOURCE 200809L
#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "host_client.hpp"
#include "probe_buffers.hpp"
#include <Diagnostics.hpp>
#include "features/streaming/infrastructure/media.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using Clock=std::chrono::steady_clock;
constexpr auto Title="FogPlay Moonlight Local Test " FOG_MOONLIGHT_PROBE_VERSION;
FILE* logFile=nullptr;
std::mutex logMutex;
Clock::time_point epoch=Clock::now();
void log(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    if(!logFile)return;
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-epoch).count();
    std::fprintf(logFile,"[%lld ms] %s\n",static_cast<long long>(ms),message.c_str());std::fflush(logFile);
}
void check(Result rc,const char* step) {if(R_FAILED(rc))throw std::runtime_error(std::string(step)+" failed code="+std::to_string(rc));}
struct Platform {
    bool sockets=false,nifm=false,pl=false,sdl=false,ttf=false;
    SDL_Window* window=nullptr; SDL_Renderer* renderer=nullptr; TTF_Font* font=nullptr;
    void init() {
        if(appletGetAppletType()!=AppletType_Application && appletGetAppletType()!=AppletType_SystemApplication)
            throw std::runtime_error("Use full application mode: hold R while launching a game");
        appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);
        check(plInitialize(PlServiceType_User),"Fonts");pl=true;
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER))throw std::runtime_error("SDL init failed");
        sdl=true;
        if(TTF_Init())throw std::runtime_error("TTF init failed");
        ttf=true;
        window=SDL_CreateWindow(Title,0,0,1280,720,SDL_WINDOW_SHOWN);
        if(!window)throw std::runtime_error("Window init failed");
        renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if(!renderer)throw std::runtime_error("Renderer init failed");
        SDL_RenderSetLogicalSize(renderer,1280,720);
        PlFontData data{};check(plGetSharedFontByType(&data,PlSharedFontType_Standard),"Font data");
        font=TTF_OpenFontRW(SDL_RWFromConstMem(data.address,static_cast<int>(data.size)),1,22);
        if(!font)throw std::runtime_error("Font load failed");
        SDL_SetRenderDrawColor(renderer,12,16,24,255);SDL_RenderClear(renderer);
        text("Preparing local Moonlight test...",30);SDL_RenderPresent(renderer);
        auto config=*socketGetDefaultInitConfig();config.num_bsd_sessions=12;config.sb_efficiency=8;
        check(socketInitialize(&config),"BSD");sockets=true;
        check(nifmInitialize(NifmServiceType_User),"NIFM");nifm=true;
    }
    void text(const std::string& value,int y,SDL_Color color={235,239,250,255},int x=20) {
        auto* surface=TTF_RenderUTF8_Blended(font,value.c_str(),color);if(!surface)return;
        auto* texture=SDL_CreateTextureFromSurface(renderer,surface);
        SDL_Rect rect{x,y,surface->w,surface->h};
        if(texture){SDL_RenderCopy(renderer,texture,nullptr,&rect);SDL_DestroyTexture(texture);}SDL_FreeSurface(surface);
    }
    ~Platform(){
        if(font)TTF_CloseFont(font);
        if(renderer)SDL_DestroyRenderer(renderer);
        if(window)SDL_DestroyWindow(window);
        if(ttf)TTF_Quit();
        if(sdl)SDL_Quit();
        if(pl)plExit();
        if(nifm)nifmExit();
        if(sockets)socketExit();
        log("Platform cleanup complete");
    }
};
enum class Phase {Ready,Host,Pair,Launch,Connect,Stream,Stop,Done,Failed};
struct Shared {
    std::atomic<Phase> phase{Phase::Ready};
    std::atomic_bool stop{false},done{false},streaming{false},starting{false},terminated{false};
    std::atomic<int> stage{0},error{0};
    std::atomic<unsigned> pin{0},inputErrors{0};
    std::mutex apiMutex, messageMutex, cancelMutex;
    bool cleaningUp=false; // Protected by cancelMutex; never held across network calls.
    std::string message;
    moonlight_probe::AudioQueue audio;
    void status(Phase value,const std::string& text){
        {std::lock_guard<std::mutex> lock(messageMutex);message=text;}phase=value;log(text);
    }
    std::string statusText(){std::lock_guard<std::mutex> lock(messageMutex);return message;}
};
Shared* active=nullptr; // Bound before starting core; cleared only after all callbacks stop.
void stageStarting(int stage){active->stage=stage;log("Moonlight stage="+std::to_string(stage));}
void stageFailed(int stage,int error){active->error=error;log("Moonlight stage failed="+std::to_string(stage)+" code="+std::to_string(error));}
void terminated(int error){active->error=error;active->terminated=true;log("Moonlight terminated code="+std::to_string(error));}
int videoSetup(int format,int width,int height,int fps,void*,int){
    log("Video setup format="+std::to_string(format)+" size="+std::to_string(width)+"x"+std::to_string(height)+" fps="+std::to_string(fps));
    return format==VIDEO_FORMAT_H264 && width>0 && width<=1920 && height>0 && height<=1080 ? 0 : -1;
}
int audioInit(int config,OPUS_MULTISTREAM_CONFIGURATION* opus,void*,int){
    if(!opus)return -1;
    log("Audio setup rate="+std::to_string(opus->sampleRate)+" channels="+std::to_string(opus->channelCount)+
        " streams="+std::to_string(opus->streams)+" coupled="+std::to_string(opus->coupledStreams)+" samples="+std::to_string(opus->samplesPerFrame));
    return config==AUDIO_CONFIGURATION_STEREO && opus->sampleRate==48000 && opus->channelCount==2 &&
        opus->streams==1 && opus->coupledStreams==1 && opus->mapping[0]==0 && opus->mapping[1]==1 &&
        (opus->samplesPerFrame==240 || opus->samplesPerFrame==480) ? 0 : -1;
}
void audioSample(char* data,int size){try{active->audio.push(data,size);}catch(...){active->error=-900;active->terminated=true;}}
void ignoreCoreLog(const char*,...){ /* Core messages can include protocol secrets. */ }
std::string randomPin(){unsigned value=0;randomGet(&value,sizeof(value));value%=10000;char result[5];std::snprintf(result,sizeof(result),"%04u",value);return result;}
bool good(Shared& state,const moonlight_probe::HostResult& result){
    if(result.ok)return true;
    state.error=result.code;state.status(Phase::Failed,result.message);return false;
}
void worker(Shared& state,moonlight_probe::HostClient& host,moonlight_probe::ProbeConfig config){
    bool started=false;bool failed=false;
    try {
        state.status(Phase::Host,"Reading local Sunshine host");
        if(!good(state,host.initialize(config)))failed=true;
        if(!failed)log(std::string("Host initialized; paired=")+(host.paired()?"1":"0"));
        if(!failed && !state.stop && !host.paired()){
            const auto pin=randomPin();state.pin=static_cast<unsigned>(std::stoul(pin));
            state.status(Phase::Pair,"Pairing: enter the displayed PIN in Sunshine on the PC");
            if(!good(state,host.pair(pin)))failed=true;
            if(!failed)log("Pairing complete; authenticated HTTPS confirmed");
            state.pin=0;
        }
        std::vector<moonlight_probe::App> apps;
        if(!failed && !state.stop && !good(state,host.apps(apps)))failed=true;
        int appId=config.appId;
        if(!failed && !state.stop){
            if(!appId)for(const auto& app:apps)if(app.name=="Desktop"){appId=app.id;break;}
            if(!appId){state.status(Phase::Failed,"Desktop not found: configure app_id in moonlight-test.json");failed=true;}
            else {
                state.status(Phase::Launch,"Starting selected local application");
                if(!good(state,host.launch(appId)))failed=true;
            }
        }
        if(!failed && !state.stop){
            DECODER_RENDERER_CALLBACKS video{};LiInitializeVideoCallbacks(&video);
            video.setup=videoSetup;video.capabilities=CAPABILITY_PULL_RENDERER;
            AUDIO_RENDERER_CALLBACKS audio{};LiInitializeAudioCallbacks(&audio);
            audio.init=audioInit;audio.decodeAndPlaySample=audioSample;audio.capabilities=CAPABILITY_SLOW_OPUS_DECODER;
            CONNECTION_LISTENER_CALLBACKS listeners{};LiInitializeConnectionCallbacks(&listeners);
            listeners.stageStarting=stageStarting;listeners.stageFailed=stageFailed;listeners.connectionTerminated=terminated;listeners.logMessage=ignoreCoreLog;
            state.status(Phase::Connect,"Starting Moonlight transport");state.starting=true;
            const int result=LiStartConnection(&host.serverInfo(),&host.streamConfig(),&listeners,&video,&audio,nullptr,0,nullptr,0);
            state.starting=false;
            if(result){state.error=result;state.status(Phase::Failed,"Moonlight connection failed code="+std::to_string(result));failed=true;}
            else {
                started=true;
                LiSendControllerArrivalEvent(0,1,LI_CTYPE_XBOX,0xffff,0);
                state.streaming=true;state.status(Phase::Stream,"Local stream active");
                while(!state.stop && !state.terminated)std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if(state.terminated){failed=true;state.status(Phase::Failed,"Host disconnected code="+std::to_string(state.error.load()));}
            }
        }
    }catch(...){state.status(Phase::Failed,"Local probe worker failed; check numeric diagnostics");failed=true;}
    state.streaming=false;state.starting=false;
    {std::lock_guard<std::mutex> lock(state.cancelMutex);state.cleaningUp=true;}
    const auto failureMessage=state.statusText();
    state.status(Phase::Stop,"Stopping local stream and owned application");
    if(started){
        std::lock_guard<std::mutex> lock(state.apiMutex);
        LiSendMultiControllerEvent(0,0,0,0,0,0,0,0,0);LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE,BUTTON_MOUSE_LEFT);
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE,BUTTON_MOUSE_RIGHT);
        LiStopConnection();log("Moonlight transport cleanup complete");
    }
    try {
        const auto stopped=host.stopOwnedApp();
        if(!stopped.ok){state.status(Phase::Failed,stopped.message);failed=true;}
        else if(host.launchUncertain()){state.status(Phase::Failed,"Launch outcome unknown: check and close the session in Sunshine");failed=true;}
        else if(failed)state.status(Phase::Failed,failureMessage);
        else state.status(Phase::Done,"Local session stopped. Press + to exit");
    }catch(...){state.status(Phase::Failed,"Stop not confirmed: check Sunshine on the PC");}
    state.done=true;
}
class Run {
public:
    Shared shared;moonlight_probe::HostClient host;std::thread thread;
    void start(const moonlight_probe::ProbeConfig& config){active=&shared;thread=std::thread(worker,std::ref(shared),std::ref(host),config);}
    void stop(){
        if(!shared.stop.exchange(true)){
            std::lock_guard<std::mutex> lock(shared.cancelMutex);
            if(!shared.cleaningUp && !shared.done)host.cancel();
        }
        if(shared.starting)LiInterruptConnection();
    }
    ~Run(){stop();if(thread.joinable())thread.join();active=nullptr;}
};
int buttons(u64 held){
    int result=0;
    const std::pair<u64,int> mapping[]={{HidNpadButton_B,A_FLAG},{HidNpadButton_A,B_FLAG},{HidNpadButton_Y,X_FLAG},{HidNpadButton_X,Y_FLAG},
        {HidNpadButton_Up,UP_FLAG},{HidNpadButton_Down,DOWN_FLAG},{HidNpadButton_Left,LEFT_FLAG},{HidNpadButton_Right,RIGHT_FLAG},
        {HidNpadButton_L,LB_FLAG},{HidNpadButton_R,RB_FLAG},{HidNpadButton_Plus,PLAY_FLAG},{HidNpadButton_Minus,BACK_FLAG},
        {HidNpadButton_StickL,LS_CLK_FLAG},{HidNpadButton_StickR,RS_CLK_FLAG}};
    for(const auto& item:mapping)if(held&item.first)result|=item.second;
    return result;
}
void sendInput(Shared& state,PadState& pad,u64 held,bool mouse,bool& mouseLeft,bool& mouseRight){
    const auto left=padGetStickPos(&pad,0),right=padGetStickPos(&pad,1);
    const int result=LiSendMultiControllerEvent(0,1,mouse ? 0 : buttons(held),mouse ? 0 : (held&HidNpadButton_ZL ? 255 : 0),
        mouse ? 0 : (held&HidNpadButton_ZR ? 255 : 0),mouse ? 0 : left.x,mouse ? 0 : left.y,mouse ? 0 : right.x,mouse ? 0 : right.y);
    if(result)++state.inputErrors;
    if(mouse){
        const auto dx=std::abs(left.x)>4000 ? left.x/2500 : 0,dy=std::abs(left.y)>4000 ? -left.y/2500 : 0;
        if(dx || dy)LiSendMouseMoveEvent(static_cast<short>(dx),static_cast<short>(dy));
        const bool nextLeft=held&HidNpadButton_ZR,nextRight=held&HidNpadButton_ZL;
        if(nextLeft!=mouseLeft)LiSendMouseButtonEvent(nextLeft ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,BUTTON_MOUSE_LEFT);
        if(nextRight!=mouseRight)LiSendMouseButtonEvent(nextRight ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,BUTTON_MOUSE_RIGHT);
        mouseLeft=nextLeft;mouseRight=nextRight;
    }
}
std::optional<std::string> keyboard(){
    SwkbdConfig config{};
    if(R_FAILED(swkbdCreate(&config,0)))return {};
    swkbdConfigMakePresetDefault(&config);swkbdConfigSetHeaderText(&config,"Text for local PC");swkbdConfigSetStringLenMax(&config,255);
    char buffer[1024]{};const auto result=swkbdShow(&config,buffer,sizeof(buffer));swkbdClose(&config);
    if(R_SUCCEEDED(result))return std::string(buffer);
    return {};
}
void run(Platform& platform){
    moonlight_probe::ProbeConfig config;const auto loaded=moonlight_probe::HostClient::loadConfig(config);
    if(!loaded.ok)throw std::runtime_error(loaded.message);
    fog::Media media(platform.renderer,::stream::VideoCodec::H264,true);
    PadState pad{};padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);hidInitializeTouchScreen();
    Run client;auto& state=client.shared;
    bool started=false,exitWanted=false,menu=false,mouse=false,mouseLeft=false,mouseRight=false;
    bool minusPending=false,minusConsumed=false;
    bool requestKeyboard=false;
    u64 swallowed=0;
    std::optional<std::string> pendingText;
    int selected=0;
    std::uint64_t decoded=0,decodeErrors=0,invalidFrames=0,inputs=0;auto lastLog=Clock::now();
    std::vector<std::byte> frame;
    while(true){
        const bool appRunning=appletMainLoop();
        const bool focused=appletGetFocusState()==AppletFocusState_InFocus;
        if(!appRunning || !focused){exitWanted=true;if(started)client.stop();else break;}
        if(state.done && exitWanted)break;
        if(!focused || !appRunning){std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
        SDL_Event event;while(SDL_PollEvent(&event))if(event.type==SDL_QUIT){exitWanted=true;if(started)client.stop();}
        padUpdate(&pad);const u64 held=padGetButtons(&pad),down=padGetButtonsDown(&pad);
        swallowed&=held;
        if(!started && (down&HidNpadButton_Plus))break;
        if(!started && (down&HidNpadButton_A)){client.start(config);started=true;}
        if(started && !state.streaming && (down&HidNpadButton_B))client.stop();
        if(state.done && ((down&HidNpadButton_Plus)||exitWanted))break;
        if(state.stop && state.starting)LiInterruptConnection();
        SDL_SetRenderDrawColor(platform.renderer,12,16,24,255);SDL_RenderClear(platform.renderer);
        if(state.streaming){
            std::unique_lock<std::mutex> lock(state.apiMutex,std::try_to_lock);
            if(lock.owns_lock() && state.streaming){
                if(pendingText && !state.stop){LiSendUtf8TextEvent(pendingText->data(),static_cast<unsigned>(pendingText->size()));pendingText.reset();}
                const bool chordHandled=(held&HidNpadButton_Minus) && (down&HidNpadButton_Plus);
                if(chordHandled){
                    menu=!menu;selected=0;LiSendMultiControllerEvent(0,1,0,0,0,0,0,0,0);
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE,BUTTON_MOUSE_LEFT);LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE,BUTTON_MOUSE_RIGHT);
                    mouseLeft=false;mouseRight=false;minusConsumed=true;swallowed|=held;
                }
                if(menu){
                    if(down&HidNpadButton_Up)selected=(selected+3)%4;
                    if(down&HidNpadButton_Down)selected=(selected+1)%4;
                    if(down&HidNpadButton_B){menu=false;swallowed|=held;}
                    if(down&HidNpadButton_A){
                        swallowed|=held;
                        if(selected==0){mouse=!mouse;menu=false;}
                        if(selected==1){requestKeyboard=true;menu=false;}
                        if(selected==2)client.stop();
                        if(selected==3)menu=false;
                    }
                }else if(!state.stop){
                    if(!chordHandled && (down&HidNpadButton_Minus)){minusPending=true;minusConsumed=false;}
                    if(minusPending && !(held&HidNpadButton_Minus)){
                        if(!minusConsumed && !mouse)LiSendMultiControllerEvent(0,1,buttons(held)|BACK_FLAG,0,0,0,0,0,0);
                        minusPending=false;minusConsumed=false;
                    }
                    sendInput(state,pad,held&~swallowed&~((held&HidNpadButton_Minus) ? HidNpadButton_Plus|HidNpadButton_Minus : 0),mouse,mouseLeft,mouseRight);++inputs;
                    if(mouse){HidTouchScreenState touch{};if(hidGetTouchScreenStates(&touch,1)>0 && touch.count)
                        LiSendMousePositionEvent(static_cast<short>(touch.touches[0].x),static_cast<short>(touch.touches[0].y),1280,720);}
                }
                for(int count=0;count<3 && state.streaming && !state.stop;++count){
                    VIDEO_FRAME_HANDLE handle{};PDECODE_UNIT unit=nullptr;
                    if(!LiPollNextVideoFrame(&handle,&unit))break;
                    int result=DR_OK;
                    try {
                        if(!unit || !moonlight_probe::copyFrame(*unit,frame)){++invalidFrames;result=DR_NEED_IDR;}
                        else {media.video(frame.data(),frame.size());++decoded;}
                    }catch(const fog::VideoDecodeError&){
                        ++decodeErrors;result=DR_NEED_IDR;
                        try{media.resetVideoDecoder();}catch(...){state.error=-901;state.terminated=true;}
                    }catch(...){result=DR_NEED_IDR;state.error=-902;state.terminated=true;}
                    LiCompleteVideoFrame(handle,result);
                }
                auto audio=state.audio.take();for(const auto& packet:audio)media.audio(packet.data(),packet.size());
                media.preparePresentation();
            }
        }
        if(requestKeyboard){
            requestKeyboard=false;
            if(state.streaming && !state.stop){pendingText=keyboard();padUpdate(&pad);swallowed|=padGetButtons(&pad);}
        }
        if(!media.draw())platform.text("Local PC -> Switch | H.264 + Opus + gamepad",150);
        SDL_SetRenderDrawBlendMode(platform.renderer,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(platform.renderer,10,15,25,210);
        SDL_Rect header{0,0,1280,90},footer{0,625,1280,95};SDL_RenderFillRect(platform.renderer,&header);SDL_RenderFillRect(platform.renderer,&footer);
        platform.text(Title,12);
        platform.text(started ? state.statusText() : "A: connect to configured local host. +: exit. No FogHub rentals.",46,{110,220,205,255});
        if(state.phase==Phase::Pair){char pin[80];std::snprintf(pin,sizeof(pin),"Sunshine PIN: %04u",state.pin.load());platform.text(pin,220);}
        platform.text("Frames="+std::to_string(media.presentedFrames())+" Opus="+std::to_string(media.audioPackets())+
            " Input="+std::to_string(inputs)+" Errors="+std::to_string(decodeErrors)+" Decoder="+(media.hardwareVideo()?"NVTEGRA":"software"),635);
        platform.text(state.done ? "+: exit. Restart this probe to test reconnection." :
            "Minus + Plus: menu | Mouse: left stick / touch, ZR click, ZL right click",673);
        if(menu){
            SDL_SetRenderDrawColor(platform.renderer,25,28,48,245);SDL_Rect box{300,200,720,260};SDL_RenderFillRect(platform.renderer,&box);
            const std::string items[]={std::string("Mouse: ")+(mouse?"ON":"OFF"),"Send keyboard text","Stop local session","Close menu"};
            for(int i=0;i<4;++i)platform.text(std::string(selected==i?"> ":"  ")+items[i],220+i*50,{235,239,250,255},330);
        }
        SDL_RenderPresent(platform.renderer);media.presented();
        if(Clock::now()-lastLog>std::chrono::seconds(5)){
            log("Stats decoded="+std::to_string(decoded)+" displayed="+std::to_string(media.presentedFrames())+
                " opus="+std::to_string(media.audioPackets())+" audioLost="+std::to_string(state.audio.lost())+
                " input="+std::to_string(inputs)+" inputErrors="+std::to_string(state.inputErrors.load())+
                " badFrames="+std::to_string(invalidFrames)+" decoderErrors="+std::to_string(decodeErrors));
            log(media.diagnostics());log(media.audioDiagnostics());lastLog=Clock::now();
        }
    }
    if(started)client.stop();
}
}
int main(){
    mkdir("sdmc:/switch",0777);mkdir("sdmc:/switch/fogplay_nx",0777);
    logFile=std::fopen("sdmc:/switch/fogplay_nx/moonlight-test.log","w");log("Local Moonlight probe " FOG_MOONLIGHT_PROBE_VERSION "; no account or rental requests");
    moonlight_diagnostics::sink.store(log);
    try {Platform platform;platform.init();run(platform);}
    catch(const std::exception& error){
        log(std::string("Probe failure: ")+error.what());consoleInit(nullptr);std::printf("Moonlight local test failed:\n%s\nPress + to exit\n",error.what());
        PadState pad{};padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
        while(appletMainLoop()){padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_Plus)break;consoleUpdate(nullptr);}consoleExit(nullptr);
    }
    moonlight_diagnostics::sink.store(nullptr);
    log("Exit complete");if(logFile){std::fflush(logFile);fsync(fileno(logFile));std::fclose(logFile);logFile=nullptr;}return 0;
}
