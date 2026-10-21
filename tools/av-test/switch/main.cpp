#include <switch.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include "features/streaming/infrastructure/media.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static FILE* logfile = nullptr;
static std::mutex logMutex;
static void log(const std::string& message, bool durable=true) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (!logfile) return;
    std::fprintf(logfile, "%llu %s\n", static_cast<unsigned long long>(armGetSystemTick()), message.c_str());
    std::fflush(logfile);
    if (durable) { fsync(fileno(logfile)); fsdevCommitDevice("sdmc"); }
}
static void check(Result value, const char* name) {
    if (R_FAILED(value)) throw std::runtime_error(std::string(name) + " initialization failed: " + std::to_string(value));
}

struct Platform {
    bool sockets=false, nifm=false, pl=false, sdl=false, ttf=false;
    SDL_Window* window=nullptr;
    SDL_Renderer* renderer=nullptr;
    TTF_Font* font=nullptr;
    void init() {
        SocketInitConfig config = *socketGetDefaultInitConfig();
        config.num_bsd_sessions=12; config.sb_efficiency=8;
        check(socketInitialize(&config), "BSD sockets"); sockets=true;
        check(nifmInitialize(NifmServiceType_User), "NIFM"); nifm=true;
        check(plInitialize(PlServiceType_User), "Shared fonts"); pl=true;
        if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_TIMER)!=0) throw std::runtime_error(SDL_GetError());
        sdl=true;
        if (TTF_Init()!=0) throw std::runtime_error(TTF_GetError());
        ttf=true;
        window=SDL_CreateWindow("FogPlay AV Test 0.1.0", 0,0,1280,720,SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());
        renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer,1280,720);
        PlFontData data{}; check(plGetSharedFontByType(&data,PlSharedFontType_Standard),"System font");
        font=TTF_OpenFontRW(SDL_RWFromConstMem(data.address,static_cast<int>(data.size)),1,22);
        if (!font) throw std::runtime_error(TTF_GetError());
    }
    ~Platform() {
        if (font) TTF_CloseFont(font);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (ttf) TTF_Quit();
        if (sdl) SDL_Quit();
        if (pl) plExit();
        if (nifm) { log("nifmExit begin"); nifmExit(); log("nifmExit complete"); }
        if (sockets) { log("socketExit begin"); socketExit(); log("socketExit complete"); }
    }
    void text(const std::string& value,int y,SDL_Color color={230,235,245,255}) {
        auto* surface=TTF_RenderUTF8_Blended(font,value.c_str(),color);
        if (!surface) return;
        auto* texture=SDL_CreateTextureFromSurface(renderer,surface);
        SDL_Rect rect{18,y,surface->w,surface->h};
        if (texture) { SDL_RenderCopy(renderer,texture,nullptr,&rect); SDL_DestroyTexture(texture); }
        SDL_FreeSurface(surface);
    }
};

static std::string readEndpoint() {
    std::ifstream file("sdmc:/switch/fogplay_av_test/server.txt");
    std::string url; std::getline(file,url);
    while (!url.empty() && (url.back()=='\r' || url.back()==' ' || url.back()=='\t')) url.pop_back();
    const std::string prefix="ws://", suffix=":8765/ws";
    if (url.size()<=prefix.size()+suffix.size() || url.compare(0,prefix.size(),prefix)!=0 ||
        url.compare(url.size()-suffix.size(),suffix.size(),suffix)!=0)
        throw std::runtime_error("server.txt must contain ws://LAN-IPv4:8765/ws");
    const auto host=url.substr(prefix.size(),url.size()-prefix.size()-suffix.size());
    in_addr address{};
    if (inet_pton(AF_INET,host.c_str(),&address)!=1) throw std::runtime_error("Numeric IPv4 required in server.txt");
    const auto value=ntohl(address.s_addr);
    const auto a=value>>24, b=(value>>16)&255;
    if (!(a==10 || a==127 || (a==172 && b>=16 && b<=31) || (a==192 && b==168)))
        throw std::runtime_error("Endpoint must use a private LAN IPv4 address");
    return url;
}

enum class EventKind { Text, State, Channel, Track, Failure };
struct AvEvent {
    EventKind kind;
    std::string text;
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::Track> track;
};
struct Packet { bool video; rtc::binary bytes; };
struct Shared {
    std::mutex mutex;
    bool live=true, overflow=false;
    std::deque<AvEvent> events;
    std::deque<Packet> packets;
    std::size_t queuedBytes=0;
    void event(AvEvent value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!live) return;
        if (events.size()>=128) { overflow=true; return; }
        events.push_back(std::move(value));
    }
    void packet(bool video,rtc::binary value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!live) return;
        if (value.empty() || value.size()>(video ? 4*1024*1024u : 65536u) ||
            packets.size()>=192 || queuedBytes+value.size()>8*1024*1024u) { overflow=true; return; }
        queuedBytes+=value.size(); packets.push_back({video,std::move(value)});
    }
};
template<class F> static void callback(std::weak_ptr<Shared> weak,F&& fn) noexcept {
    try { if (auto shared=weak.lock()) fn(*shared); }
    catch (const std::exception& e) { try { if (auto shared=weak.lock()) shared->event({EventKind::Failure,e.what(),{}, {}}); } catch (...) {} }
    catch (...) { try { if (auto shared=weak.lock()) shared->event({EventKind::Failure,"Unknown RTC callback error",{}, {}}); } catch (...) {} }
}

class Client {
    std::shared_ptr<Shared> shared=std::make_shared<Shared>();
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    bool offered=false;
    std::uint64_t sequence=0;
    Clock::time_point started=Clock::now(), lastSend=Clock::now(),lastEcho=Clock::now();
    Clock::time_point connectedAt=Clock::now(),lastVideo=Clock::now(),lastAudio=Clock::now();
    std::uint64_t previousVideo=0,previousAudio=0;
    void message(const std::string& value) {
        const auto json=Json::parse(value);
        const auto type=json.at("type").get<std::string>();
        if (type=="offer") {
            if (offered) throw std::runtime_error("Unexpected second offer");
            const auto sdp=json.at("sdp").get<std::string>();
            if (sdp.size()>192*1024) throw std::runtime_error("SDP too large");
            peer->setRemoteDescription(rtc::Description(sdp,"offer"));
            peer->setLocalDescription(rtc::Description::Type::Answer);
            offered=true; status="ICE / DTLS connecting"; log("Remote offer accepted");
        } else if (type=="candidate") {
            const auto candidate=json.at("candidate").get<std::string>();
            if (candidate.size()>8192) throw std::runtime_error("ICE candidate too large");
            peer->addRemoteCandidate(rtc::Candidate(candidate,json.at("sdpMid").get<std::string>()));
        } else if (type=="error") throw std::runtime_error(json.value("message","Server rejected connection"));
        else if (type=="echo") {
            echoed=json.at("seq").get<std::uint64_t>();
            if (echoed>sequence) throw std::runtime_error("Invalid controller echo sequence");
            ++echoes; lastEcho=Clock::now();
            lastButtons=json.value("buttons",Json::array()).dump();
        }
    }
public:
    std::string status="Preparing RTC", lastButtons="[]";
    std::uint64_t echoes=0,echoed=0;
    bool connected=false;
    explicit Client(const std::string& url) {
        const std::weak_ptr<Shared> weak=shared;
        rtc::Configuration config; config.disableAutoNegotiation=true; config.maxMessageSize=16384;
        peer=std::make_shared<rtc::PeerConnection>(config);
        rtc::WebSocket::Configuration socketConfig;
        socketConfig.maxMessageSize=256*1024;
        socketConfig.connectionTimeout=std::chrono::seconds(10);
        ws=std::make_shared<rtc::WebSocket>(socketConfig);
        const std::weak_ptr<rtc::WebSocket> weakWs=ws;
        peer->onLocalDescription([weak,weakWs](rtc::Description description) {
            callback(weak,[&](Shared&) { if (auto socket=weakWs.lock()) socket->send(Json{{"type","answer"},{"sdp",std::string(description)}}.dump()); });
        });
        peer->onLocalCandidate([weak,weakWs](rtc::Candidate candidate) {
            callback(weak,[&](Shared&) { if (auto socket=weakWs.lock()) socket->send(Json{{"type","candidate"},{"candidate",candidate.candidate()},{"sdpMid",candidate.mid()}}.dump()); });
        });
        peer->onStateChange([weak](rtc::PeerConnection::State state) {
            callback(weak,[&](Shared& shared) { shared.event({EventKind::State,std::to_string(static_cast<int>(state)),{}, {}}); });
        });
        peer->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> value) {
            callback(weak,[&](Shared& shared) {
                if (value->label()!="control") throw std::runtime_error("Unexpected data channel");
                value->onMessage([weak](rtc::message_variant message) {
                    callback(weak,[&](Shared& state) {
                        if (!std::holds_alternative<std::string>(message)) throw std::runtime_error("Non-text controller echo");
                        auto text=std::get<std::string>(std::move(message));
                        if (text.size()>16384) throw std::runtime_error("Controller echo too large");
                        state.event({EventKind::Text,std::move(text),{}, {}});
                    });
                });
                value->onError([weak](std::string error) { callback(weak,[&](Shared& state) { state.event({EventKind::Failure,"Data channel: "+error,{}, {}}); }); });
                shared.event({EventKind::Channel,{},value,{}});
            });
        });
        peer->onTrack([weak](std::shared_ptr<rtc::Track> track) {
            callback(weak,[&](Shared& shared) {
                auto description=track->description();
                if (description.isRemoved()) return;
                description.setDirection(rtc::Description::Direction::RecvOnly); track->setDescription(description);
                const bool video=description.type()=="video";
                std::shared_ptr<rtc::MediaHandler> handler;
                if (video) handler=std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
                else if (description.type()=="audio") handler=std::make_shared<rtc::OpusRtpDepacketizer>();
                else return;
                handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
                track->setMediaHandler(handler);
                track->onFrame([weak,video](rtc::binary bytes,rtc::FrameInfo) {
                    callback(weak,[&](Shared& state) { state.packet(video,std::move(bytes)); });
                });
                track->onError([weak](std::string error) { callback(weak,[&](Shared& state) { state.event({EventKind::Failure,"Track: "+error,{}, {}}); }); });
                shared.event({EventKind::Track,description.type(),{},track});
            });
        });
        ws->onOpen([weak] { callback(weak,[](Shared& state) { state.event({EventKind::State,"ws-open",{}, {}}); }); });
        ws->onMessage([weak](rtc::message_variant message) {
            callback(weak,[&](Shared& state) {
                if (!std::holds_alternative<std::string>(message)) throw std::runtime_error("Non-text signaling message");
                state.event({EventKind::Text,std::get<std::string>(std::move(message)),{}, {}});
            });
        });
        ws->onError([weak](std::string error) { callback(weak,[&](Shared& state) { state.event({EventKind::Failure,"WebSocket: "+error,{}, {}}); }); });
        ws->onClosed([weak] { callback(weak,[](Shared& state) { state.event({EventKind::Failure,"PC server disconnected",{}, {}}); }); });
        status="Connecting PC WebSocket"; log("WebSocket open "+url); ws->open(url);
    }
    ~Client() { stop(); }
    void stop() noexcept {
        if (!shared) return;
        std::deque<AvEvent> abandonedEvents; std::deque<Packet> abandonedPackets;
        { std::lock_guard<std::mutex> lock(shared->mutex); shared->live=false; abandonedEvents.swap(shared->events); abandonedPackets.swap(shared->packets); }
        try { if (channel) channel->close(); } catch (...) {}
        for (auto& track:tracks) { try { track->close(); } catch (...) {} }
        try { if (peer) peer->close(); } catch (...) {}
        try { if (ws) ws->forceClose(); } catch (...) {}
        channel.reset(); tracks.clear(); peer.reset(); ws.reset(); shared.reset(); log("Client RTC handles released");
    }
    void tick(fog::Media& media,u64 buttons,const std::array<float,4>& axes) {
        std::deque<AvEvent> events; std::deque<Packet> packets;
        { std::lock_guard<std::mutex> lock(shared->mutex);
          if (shared->overflow) throw std::runtime_error("Receive queue overflow; PC bitrate too high");
          events.swap(shared->events); packets.swap(shared->packets); shared->queuedBytes=0; }
        for (auto& event:events) {
            if (event.kind==EventKind::Failure) throw std::runtime_error(event.text);
            if (event.kind==EventKind::Text) message(event.text);
            else if (event.kind==EventKind::Channel) { channel=event.channel; log("Control data channel received"); }
            else if (event.kind==EventKind::Track) { tracks.push_back(event.track); log("Track received: "+event.text); }
            else if (event.kind==EventKind::State) {
                if (event.text=="ws-open") { status="Waiting PC offer"; log("WebSocket open complete"); }
                else {
                    const auto state=static_cast<rtc::PeerConnection::State>(std::stoi(event.text));
                    std::ostringstream stream; stream<<state; status=stream.str(); log("Peer state "+status);
                    connected=state==rtc::PeerConnection::State::Connected;
                    if (connected) { connectedAt=lastEcho=lastVideo=lastAudio=Clock::now(); }
                    if (state==rtc::PeerConnection::State::Failed || state==rtc::PeerConnection::State::Closed)
                        throw std::runtime_error("Peer connection "+status);
                }
            }
        }
        for (const auto& packet:packets) {
            if (packet.video) media.video(packet.bytes.data(),packet.bytes.size());
            else media.audio(packet.bytes.data(),packet.bytes.size());
        }
        const auto now=Clock::now();
        if (!connected && now-started>std::chrono::seconds(30)) throw std::runtime_error("Connection timed out after 30 seconds");
        if (channel && channel->isOpen() && now-lastSend>=std::chrono::milliseconds(33)) {
            static constexpr std::array<u64,18> mapping{HidNpadButton_B,HidNpadButton_A,HidNpadButton_Y,HidNpadButton_X,
                HidNpadButton_L,HidNpadButton_R,HidNpadButton_ZL,HidNpadButton_ZR,HidNpadButton_Minus,HidNpadButton_Plus,
                HidNpadButton_StickL,HidNpadButton_StickR,HidNpadButton_Up,HidNpadButton_Down,HidNpadButton_Left,HidNpadButton_Right,0,0};
            std::array<int,18> values{};
            for (std::size_t i=0;i<values.size();++i) values[i]=(buttons&mapping[i])?1:0;
            if (channel->bufferedAmount()>65536) throw std::runtime_error("Controller send queue stalled");
            channel->send(Json{{"type","input"},{"seq",++sequence},{"buttons",values},{"axes",axes},
                {"leftTrigger",values[6]},{"rightTrigger",values[7]}}.dump());
            lastSend=now;
        }
        if (connected && now-lastEcho>std::chrono::seconds(10)) throw std::runtime_error("Controller echo timeout");
        if (media.videoFrames()!=previousVideo) { previousVideo=media.videoFrames(); lastVideo=now; }
        if (media.audioPackets()!=previousAudio) { previousAudio=media.audioPackets(); lastAudio=now; }
        if (connected && now-connectedAt>std::chrono::seconds(20) && (!previousVideo || !previousAudio))
            throw std::runtime_error(!previousVideo ? "No decoded video after 20 seconds" : "No decoded audio after 20 seconds");
        if (connected && previousVideo && now-lastVideo>std::chrono::seconds(10)) throw std::runtime_error("Decoded video stalled for 10 seconds");
        if (connected && previousAudio && now-lastAudio>std::chrono::seconds(10)) throw std::runtime_error("Decoded audio stalled for 10 seconds");
    }
};

static bool cleanupRtc(Platform& platform) {
    try {
        log("RTC cleanup begin");
        auto future=rtc::Cleanup();
        bool warned=false;
        while (future.wait_for(std::chrono::milliseconds(20))!=std::future_status::ready) {
            if (!warned) { log("RTC cleanup waiting"); warned=true; }
            appletMainLoop();
            if (appletGetFocusState()==AppletFocusState_InFocus) {
                SDL_SetRenderDrawColor(platform.renderer,12,16,28,255); SDL_RenderClear(platform.renderer);
                platform.text("Stopping RTC workers; please wait",300);
                platform.text("Services stay alive until cleanup completes",340);
                SDL_RenderPresent(platform.renderer);
            } else svcSleepThread(16000000);
        }
        rtc::JoinCleanupThreads(); future.get(); log("RTC cleanup complete; threads joined");
        return true;
    } catch (const std::exception& e) { log(std::string("Unsafe RTC cleanup failure: ")+e.what()); return false; }
    catch (...) { log("Unsafe unknown RTC cleanup failure"); return false; }
}

int main() {
    fsdevMountSdmc(); mkdir("sdmc:/switch",0777); mkdir("sdmc:/switch/fogplay_av_test",0777);
    logfile=std::fopen("sdmc:/switch/fogplay_av_test/client.log","w");
    log("FogPlay AV Test 0.1.0: LAN video/audio/control, no account or rental");
    const auto type=appletGetAppletType();
    if (type!=AppletType_Application && type!=AppletType_SystemApplication) {
        consoleInit(nullptr); std::printf("Full application required.\nHold R while starting an installed game.\nThen launch FogPlay AV Test in hbmenu.\nPress + to exit.\n");
        log("Skipped: Album/applet mode"); PadState pad; padConfigureInput(1,HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
        while (appletMainLoop()) { padUpdate(&pad); if (padGetButtonsDown(&pad)&HidNpadButton_Plus) break; consoleUpdate(nullptr); svcSleepThread(16000000); }
        consoleExit(nullptr); if (logfile) std::fclose(logfile); return 12;
    }
    int result=0;
    {
        Platform platform;
        std::unique_ptr<Client> client;
        std::unique_ptr<fog::Media> media;
        std::string error,url;
        bool stopping=false, retry=false, initialized=false;
        try { platform.init(); initialized=true; rtc::InitLogger(rtc::LogLevel::Warning,[](rtc::LogLevel,std::string text) { log("RTC warning: "+text.substr(0,512),false); }); }
        catch (const std::exception& e) { log(std::string("Platform failure: ")+e.what()); result=10; }
        if (initialized) {
            PadState pad; padConfigureInput(1,HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
            auto start=[&] {
                url=readEndpoint(); media=std::make_unique<fog::Media>(platform.renderer);
                client=std::make_unique<Client>(url); log("Decoder and RTC client initialized");
            };
            try { start(); } catch (const std::exception& e) { error=e.what(); log("Start failure: "+error); client.reset(); media.reset(); retry=true; }
            padUpdate(&pad);
            bool plusArmed=(padGetButtons(&pad)&HidNpadButton_Plus)==0;
            auto lastLog=Clock::now();
            while (appletMainLoop() && !stopping) {
                padUpdate(&pad); const auto down=padGetButtonsDown(&pad); const auto held=padGetButtons(&pad);
                SDL_Event event; while (SDL_PollEvent(&event)) if (event.type==SDL_QUIT) stopping=true;
                if (!(held&HidNpadButton_Plus)) plusArmed=true;
                if (plusArmed && (down&HidNpadButton_Plus)) { log("Plus pressed: stopping"); stopping=true; }
                if (stopping) break;
                if (appletGetFocusState()!=AppletFocusState_InFocus) {
                    if (client) {
                        log("Focus lost: disconnecting PC stream; X retries after returning");
                        client.reset(); media.reset(); error="Paused after HOME / focus loss. Press X to reconnect."; retry=true;
                    }
                    svcSleepThread(16000000); continue;
                }
                if (!client && retry && (down&HidNpadButton_X)) {
                    if (!cleanupRtc(platform)) { error="RTC cleanup failed; use HOME to close application"; retry=false; }
                    else { try { start(); error.clear(); } catch (const std::exception& e) { error=e.what(); log("Retry failure: "+error); client.reset(); media.reset(); retry=true; } }
                }
                if (client) {
                    const auto left=padGetStickPos(&pad,0),right=padGetStickPos(&pad,1);
                    const std::array<float,4> axes{left.x/32768.0f,-left.y/32768.0f,right.x/32768.0f,-right.y/32768.0f};
                    try { client->tick(*media,held,axes); }
                    catch (const std::exception& e) { error=e.what(); log("Connection failure: "+error); client.reset(); media.reset(); retry=true; }
                }
                if (appletGetFocusState()!=AppletFocusState_InFocus) { svcSleepThread(16000000); continue; }
                SDL_SetRenderDrawColor(platform.renderer,12,16,28,255); SDL_RenderClear(platform.renderer);
                if (client && media) {
                    try { media->draw(); } catch (const std::exception& e) { error=e.what(); log("Rendering failure: "+error); client.reset(); media.reset(); retry=true; }
                }
                SDL_SetRenderDrawBlendMode(platform.renderer,SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(platform.renderer,10,14,24,220);
                SDL_Rect top{0,0,1280,115},bottom{0,575,1280,145}; SDL_RenderFillRect(platform.renderer,&top); SDL_RenderFillRect(platform.renderer,&bottom);
                platform.text("FogPlay AV Test 0.1.0  |  PC -> Switch H.264 + Opus + gamepad",12);
                platform.text(error.empty() ? (client ? client->status+"  "+url : "Disconnected") : error.substr(0,100),48,{120,225,190,255});
                if (client && media) {
                    auto counters="Frames="+std::to_string(media->videoFrames())+"  Opus="+std::to_string(media->audioPackets())+
                        "  Echo="+std::to_string(client->echoes)+"  Seq="+std::to_string(client->echoed)+"  Decoder="+(media->hardwareVideo()?"NVTEGRA":"software");
                    platform.text(counters,590); platform.text("Echo buttons: "+client->lastButtons,625);
                    if (Clock::now()-lastLog>std::chrono::seconds(5)) { log(counters); lastLog=Clock::now(); }
                }
                platform.text(client ? "Buttons and sticks control the PC test scene. + = stop and exit" :
                    (retry ? "X = retry connection   + = stop and exit" : "Cleanup failed. Keep services alive; HOME -> Close Software"),665);
                SDL_RenderPresent(platform.renderer);
            }
            client.reset(); media.reset();
            if (!cleanupRtc(platform)) {
                // Never return to the loader or tear down BSD while RTC may still own workers.
                for (;;) {
                    appletMainLoop();
                    if (appletGetFocusState()==AppletFocusState_InFocus) {
                        SDL_SetRenderDrawColor(platform.renderer,12,16,28,255); SDL_RenderClear(platform.renderer);
                        platform.text("RTC cleanup failed. Use HOME -> Close Software.",300);
                        platform.text("Application stays alive to protect active worker threads.",340); SDL_RenderPresent(platform.renderer);
                    } else svcSleepThread(16000000);
                }
            }
            log("Media released, RTC cleanup verified; platform teardown next");
        }
    }
    log("Return to hbmenu; diagnostic "+std::to_string(result));
    if (logfile) { std::fflush(logfile); fsync(fileno(logfile)); fsdevCommitDevice("sdmc"); std::fclose(logfile); logfile=nullptr; }
    return result;
}
