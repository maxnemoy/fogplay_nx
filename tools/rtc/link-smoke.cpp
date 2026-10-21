#include <rtc/rtc.hpp>
#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#include <curl/curl.h>
#endif
int main() {
#ifdef __SWITCH__
    if (R_FAILED(socketInitializeDefault())) return 10;
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return 11;
    std::printf("curl: %s\n", curl_version());
#endif
    rtc::InitLogger(rtc::LogLevel::Warning);
    {
        rtc::Configuration config;
        config.iceServers.clear();
        rtc::PeerConnection pc(config);
        rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
        video.addH264Codec(96);
        auto track = pc.addTrack(video);
        auto channel = pc.createDataChannel("input");
        rtc::WebSocket ws;
        ws.close();
        pc.close();
    }
    rtc::Cleanup().wait();
#ifdef __SWITCH__
    nifmExit();
    socketExit();
#endif
    std::puts("RTC smoke passed");
    return 0;
}
