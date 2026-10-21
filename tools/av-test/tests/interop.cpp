#include "features/streaming/infrastructure/media.hpp"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <SDL.h>
#include <opus/opus.h>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Packet { bool video; rtc::binary data; };
struct State {
    std::mutex mutex;
    std::deque<Packet> packets;
    std::deque<Json> signals;
    std::deque<Json> echoes;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    bool failed = false, stopped = false;
};
template<class F> void callback(std::weak_ptr<State> weak, F fn) noexcept {
    if (auto s = weak.lock()) {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->stopped) return;
        try { fn(*s); } catch (...) { s->failed = true; }
    }
}
size_t resources(const char* path) {
    return std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{});
}
struct Connection {
    std::shared_ptr<State> state = std::make_shared<State>();
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    void stop() {
        if (!state) return;
        std::shared_ptr<rtc::DataChannel> dc;
        std::vector<std::shared_ptr<rtc::Track>> tracks;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->stopped = true;
            dc = std::move(state->channel); tracks = std::move(state->tracks);
            state->packets.clear(); state->signals.clear(); state->echoes.clear();
        }
        if (dc) { dc->resetCallbacks(); dc->close(); dc.reset(); }
        for (auto& t : tracks) { t->resetCallbacks(); t->close(); }
        tracks.clear();
        if (pc) { pc->resetCallbacks(); pc->close(); pc.reset(); }
        if (ws) {
            if (ws->isOpen()) ws->send(Json{{"type","stop"}}.dump());
            ws->resetCallbacks(); ws->close(); ws.reset();
        }
        state.reset();
    }
    ~Connection() { try { stop(); } catch (...) {} }
    void open(const std::string& url) {
        rtc::Configuration config; config.disableAutoNegotiation = true;
        config.bindAddress = "127.0.0.1"; config.iceServers.clear();
        config.enableIceTcp = false;
        pc = std::make_shared<rtc::PeerConnection>(config);
        ws = std::make_shared<rtc::WebSocket>();
        std::weak_ptr<State> weak = state;
        pc->onLocalDescription([weak](rtc::Description d) {
            callback(weak,[&](State& s) { s.signals.push_back({{"type",d.typeString()},{"sdp",std::string(d)}}); });
        });
        pc->onLocalCandidate([weak](rtc::Candidate c) {
            callback(weak,[&](State& s) { s.signals.push_back({{"type","candidate"},{"candidate",c.candidate()},{"sdpMid",c.mid()}}); });
        });
        pc->onStateChange([weak](rtc::PeerConnection::State st) {
            if (st == rtc::PeerConnection::State::Failed) callback(weak,[](State& s) { s.failed = true; });
        });
        pc->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
            dc->onMessage([weak](rtc::message_variant msg) {
                callback(weak,[&](State& s) { if (auto text=std::get_if<std::string>(&msg)) s.echoes.push_back(Json::parse(*text)); });
            });
            callback(weak,[&](State& s) { s.channel=dc; });
        });
        pc->onTrack([weak](std::shared_ptr<rtc::Track> track) {
            auto desc=track->description(); desc.setDirection(rtc::Description::Direction::RecvOnly); track->setDescription(desc);
            const bool video=desc.type()=="video";
            std::shared_ptr<rtc::MediaHandler> handler;
            if (video) handler=std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
            else handler=std::make_shared<rtc::OpusRtpDepacketizer>();
            handler->addToChain(std::make_shared<rtc::RtcpReceivingSession>()); track->setMediaHandler(handler);
            track->onFrame([weak,video](rtc::binary bytes, rtc::FrameInfo) {
                callback(weak,[&](State& s) {
                    if (s.packets.size() >= 512 || bytes.size()>4*1024*1024) throw std::runtime_error("media overflow");
                    s.packets.push_back({video,std::move(bytes)});
                });
            });
            callback(weak,[&](State& s) { s.tracks.push_back(track); });
        });
        ws->onMessage([weak](rtc::message_variant msg) {
            callback(weak,[&](State& s) { if (auto text=std::get_if<std::string>(&msg)) s.signals.push_back({{"received",Json::parse(*text)}}); });
        });
        ws->onError([weak](std::string) { callback(weak,[](State& s) { s.failed=true; }); });
        ws->open(url);
    }
};
void require(bool value,const char* error) { if (!value) throw std::runtime_error(error); }

void round(SDL_Renderer* renderer, int index, const std::string& url) {
    fog::Media media(renderer);
    int opusError=0;
    std::unique_ptr<OpusDecoder,decltype(&opus_decoder_destroy)> decoder(opus_decoder_create(48000,2,&opusError),opus_decoder_destroy);
    require(decoder && !opusError,"audio verification decoder failed");
    Connection c; c.open(url);
    std::set<uint64_t> imageHashes;
    std::vector<Uint32> pixels(1280*720);
    std::vector<opus_int16> pcm(5760*2);
    double energy=0; uint64_t samples=0;
    Json wanted;
    unsigned sent=0, echoes=0;
    auto lastSend=Clock::now();
    const auto deadline=Clock::now()+std::chrono::seconds(15);
    while (Clock::now()<deadline) {
        std::deque<Json> signals, received;
        std::deque<Packet> packets;
        std::shared_ptr<rtc::DataChannel> dc;
        {
            std::lock_guard<std::mutex> lock(c.state->mutex);
            require(!c.state->failed,"transport callback failed");
            signals.swap(c.state->signals); packets.swap(c.state->packets); received.swap(c.state->echoes); dc=c.state->channel;
        }
        for (auto& signal: signals) {
            if (signal.contains("received")) {
                const auto& value=signal["received"];
                if (value.value("type","")=="offer") {
                    c.pc->setRemoteDescription(rtc::Description(value.at("sdp").get<std::string>(),"offer"));
                    c.pc->setLocalDescription(rtc::Description::Type::Answer);
                } else if (value.value("type","")=="error") throw std::runtime_error("server signaling error");
            } else c.ws->send(signal.dump());
        }
        for (auto& packet:packets) {
            if (packet.video) {
                media.video(packet.data.data(),packet.data.size());
                SDL_SetRenderDrawColor(renderer,0,0,0,255); SDL_RenderClear(renderer);
                require(media.draw(),"decoded video not drawn");
                require(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_ARGB8888,pixels.data(),1280*4)==0,"pixel read failed");
                uint64_t hash=1469598103934665603ULL;
                for (size_t p=0;p<pixels.size();p+=17) { hash^=pixels[p]; hash*=1099511628211ULL; }
                imageHashes.insert(hash); SDL_RenderPresent(renderer);
            } else {
                media.audio(packet.data.data(),packet.data.size());
                const auto count=opus_decode(decoder.get(),reinterpret_cast<const unsigned char*>(packet.data.data()),packet.data.size(),pcm.data(),5760,0);
                require(count>0,"verification Opus decode failed");
                for (int i=0;i<count*2;++i) { energy+=double(pcm[i])*pcm[i]; ++samples; }
            }
        }
        for (const auto& echo:received) {
            if (echo.value("type","")!="echo") continue;
            if (echo.value("seq",0u)!=sent) continue;
            for (const auto* key:{"seq","buttons","axes","leftTrigger","rightTrigger"}) require(echo.at(key)==wanted.at(key),"input echo differs");
            ++echoes;
        }
        if (dc && dc->isOpen() && (sent==0 || (echoes==sent && sent<3 && Clock::now()-lastSend>std::chrono::milliseconds(200)))) {
            ++sent; lastSend=Clock::now();
            std::vector<int> buttons(18); buttons[sent-1]=1;
            wanted={{"type","input"},{"seq",sent},{"buttons",buttons},{"axes",{0.25,-0.5,0.75,-1.0}},{"leftTrigger",0.4},{"rightTrigger",0.9}};
            dc->send(wanted.dump());
        }
        if (media.videoFrames()>=45 && media.audioPackets()>=60 && imageHashes.size()>=30 && echoes==3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    const auto rms=samples?std::sqrt(energy/samples):0;
    std::cout<<"round="<<index<<" H264_frames="<<media.videoFrames()<<" distinct_rendered_frames="<<imageHashes.size()<<" Opus_packets="<<media.audioPackets()<<" PCM_RMS="<<rms<<" input_echoes="<<echoes<<std::endl;
    require(media.videoFrames()>=45 && imageHashes.size()>=30,"video threshold not reached");
    require(media.audioPackets()>=60 && rms>500,"audible-tone samples missing");
    require(echoes==3,"input echo threshold not reached");
    c.stop();
}
int main(int argc,char** argv) {
    SDL_setenv("SDL_VIDEODRIVER","dummy",1); SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)) return 2;
    auto* window=SDL_CreateWindow("AV interop",0,0,1280,720,0);
    auto* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    int result=0;
    try {
        require(renderer,"SDL renderer unavailable");
        const auto fd=resources("/proc/self/fd"),threads=resources("/proc/self/task");
        for(int index=1;index<=2;++index) {
            round(renderer,index,argc>1?argv[1]:"ws://127.0.0.1:8765/ws");
            auto cleanup=rtc::Cleanup();
            require(cleanup.wait_for(std::chrono::seconds(10))==std::future_status::ready,"cleanup timed out");
            rtc::JoinCleanupThreads(); cleanup.get();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::cout<<"cleanup="<<index<<" FDs="<<resources("/proc/self/fd")<<" baseline="<<fd<<" threads="<<resources("/proc/self/task")<<" baseline="<<threads<<std::endl;
            require(resources("/proc/self/fd")==fd && resources("/proc/self/task")==threads,"resources did not return to baseline");
        }
        std::cout<<"PASS real aiortc -> libdatachannel -> fog::Media H264/Opus + input echo + reconnect cleanup"<<std::endl;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<std::endl; result=1; auto cleanup=rtc::Cleanup(); cleanup.wait(); rtc::JoinCleanupThreads(); }
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return result;
}
