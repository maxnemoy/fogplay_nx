#include <rtc/rtc.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
using namespace std::chrono_literals;
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool open=false, closed=false, failed=false;
    int echoes=0;
    std::shared_ptr<rtc::WebSocket> remote;
};
void require(bool value, const char* text) { if(!value) throw std::runtime_error(text); }
bool wait(const std::shared_ptr<State>& s, auto predicate, std::chrono::milliseconds duration=3000ms) {
    std::unique_lock lock(s->mutex);
    return s->cv.wait_for(lock,duration,[&]{return predicate(*s);});
}
void cleanup() {
    auto future=rtc::Cleanup();
    require(future.wait_for(5s)==std::future_status::ready,"Cleanup timed out");
    rtc::JoinCleanupThreads(); future.get();
}
void cycle(const std::string& cert, const std::string& key, const std::string& ca, int cycle) {
    auto s=std::make_shared<State>(); std::weak_ptr<State> weak=s;
    rtc::WebSocketServer::Configuration sc;
    sc.bindAddress="127.0.0.1"; sc.port=0; sc.enableTls=true;
    sc.certificatePemFile=cert; sc.keyPemFile=key;
    rtc::WebSocketServer server(sc);
    server.onClient([weak](std::shared_ptr<rtc::WebSocket> remote){
        if(auto state=weak.lock()) {
            { std::lock_guard lock(state->mutex); state->remote=remote; }
            remote->onMessage([socket=std::weak_ptr<rtc::WebSocket>(remote)](rtc::message_variant msg){
                if(auto ws=socket.lock()) ws->send(msg);
            });
        }
    });
    rtc::WebSocket::Configuration cc;
    cc.caCertificatePemFile=ca;
    if(cycle!=3) cc.pingInterval=150ms; // Cycle 3 uses the unmodified 10-second default.
    cc.maxOutstandingPings=3; // Exercise multiple idle TCP notifications, not timeout suppression.
    auto client=std::make_shared<rtc::WebSocket>(cc);
    client->onOpen([weak]{if(auto state=weak.lock()){std::lock_guard lock(state->mutex); state->open=true; state->cv.notify_all();}});
    client->onClosed([weak]{if(auto state=weak.lock()){std::lock_guard lock(state->mutex); state->closed=true; state->cv.notify_all();}});
    client->onError([weak](std::string error){
        std::fprintf(stderr,"WSS error: %s\n",error.c_str());
        if(auto state=weak.lock()){std::lock_guard lock(state->mutex); state->failed=true; state->cv.notify_all();}
    });
    client->onMessage([weak](rtc::message_variant msg){
        if(auto text=std::get_if<std::string>(&msg); text && *text=="after-idle")
            if(auto state=weak.lock()){std::lock_guard lock(state->mutex); ++state->echoes; state->cv.notify_all();}
    });
    client->open("wss://localhost:"+std::to_string(server.port())+"/");
    require(wait(s,[](const State& x){return x.open||x.failed||x.closed;}),"WSS connect timeout");
        {std::lock_guard lock(s->mutex); require(s->open&&!s->failed&&!s->closed,"Trusted WSS failed to open");}
        const int steps=cycle==3?1:3;
        for(int step=1;step<=steps;++step) {
            // No application messages; automatic WebSocket ping/pong must keep TLS alive.
            const bool ended=wait(s,[](const State& x){return x.closed||x.failed;},cycle==3?11000ms:650ms);
            std::printf("WSS cycle=%d idle=%d premature_close=%d\n",cycle,step,ended); std::fflush(stdout);
            require(!ended,"Trusted WSS prematurely closed after idle notification");
            client->send(std::string("after-idle"));
            require(wait(s,[step](const State& x){return x.echoes>=step||x.closed||x.failed;}),"Echo timeout after idle");
            {std::lock_guard lock(s->mutex); require(s->echoes>=step,"Post-idle echo not delivered");}
        }
        std::shared_ptr<rtc::WebSocket> remote;
        {std::lock_guard lock(s->mutex); remote=s->remote;}
        require(bool(remote),"Server connection missing");
        if(cycle==2) remote->forceClose(); else remote->close();
        require(wait(s,[](const State& x){return x.closed;}),"Real remote close not delivered");
        std::printf("WSS cycle=%d remote_close PASS echoes=%d graceful=%d\n",cycle,steps,cycle!=2);
    client->close(); server.stop();
    {std::lock_guard lock(s->mutex); s->remote.reset();}
}
void rejectCycle(const std::string& ca, const std::string& port) {
    auto state=std::make_shared<State>(); std::weak_ptr<State> weak=state;
    rtc::WebSocket::Configuration config; config.caCertificatePemFile=ca;
    auto client=std::make_shared<rtc::WebSocket>(config);
    client->onOpen([weak]{if(auto s=weak.lock()){std::lock_guard lock(s->mutex);s->open=true;s->cv.notify_all();}});
    client->onClosed([weak]{if(auto s=weak.lock()){std::lock_guard lock(s->mutex);s->closed=true;s->cv.notify_all();}});
    client->onError([weak](std::string error){
        std::fprintf(stderr,"Expected untrusted WSS error: %s\n",error.c_str());
        if(auto s=weak.lock()){std::lock_guard lock(s->mutex);s->failed=true;s->cv.notify_all();}
    });
    // External Python SSL server retains credentials until its handshake exits.
    // libdatachannel's separate server borrowed-credential lifetime issue must
    // not interfere with testing this client's required certificate verification.
    client->open("wss://localhost:"+port+"/");
    require(wait(state,[](const State& x){return x.open||(x.failed&&x.closed);}),"Untrusted TLS rejection timeout");
    {std::lock_guard lock(state->mutex); require(!state->open&&state->failed&&state->closed,"Untrusted certificate accepted or not closed");}
    client->close(); client.reset();
    std::printf("WSS certificate rejection and client close PASS\n");
}
int main(int argc,char** argv) {
    if(argc!=5) return 2;
    rtc::SetThreadPoolSize(2); rtc::InitLogger(rtc::LogLevel::Debug);
    int result=0;
    try {
        for(int i=1;i<=3;++i){cycle(argv[1],argv[2],argv[1],i);cleanup();}
        rejectCycle(argv[3],argv[4]); cleanup();
        std::printf("PASS verified WSS idle/ping/echo/reconnect/remote-close/certificate-rejection/cleanup\n");
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL %s\n",error.what()); result=1;
        try{cleanup();}catch(const std::exception& e){std::fprintf(stderr,"Cleanup failure: %s\n",e.what());}
    }
    return result;
}
