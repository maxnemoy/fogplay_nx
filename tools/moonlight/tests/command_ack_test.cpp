#include <Settings.hpp>
#include <Diagnostics.hpp>
#include <client.h>
#include <http.h>
#include <errors.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {
std::string response;
bool expectResume;
int requests;
}
void randomGet(void* target, std::size_t size) { std::memset(target, 0, size); }
const char* LiGetLaunchUrlQueryParameters() { return ""; }
int http_request(const std::string& url, Data* out, HTTPRequestTimeout) {
    ++requests;
    assert((url.find("/resume?") != std::string::npos) == expectResume);
    *out = Data(reinterpret_cast<unsigned char*>(response.data()), response.size());
    moonlight_diagnostics::commandHttp(0, 200, 1, true, false);
    return GS_OK;
}

void check(bool resume, const std::string& body, bool success, int acknowledgement) {
    expectResume = resume;
    response = body;
    requests = 0;
    SERVER_DATA server{};
    server.currentGame = resume ? 7 : 0;
    server.urlAddress = "127.0.0.1";
    server.httpsPort = 47984;
    STREAM_CONFIGURATION config{};
    config.width = 1280; config.height = 720; config.fps = 30;
    config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    Settings::instance().reset_cancel();
    const int result = gs_start_app(&server, &config, 7, false, false, 1);
    if ((result == GS_OK) != success) std::cerr << "Synthetic response mismatch: " << body << " result=" << result << '\n';
    assert((result == GS_OK) == success);
    assert(requests == 1);
    const auto snapshot = moonlight_diagnostics::commandSnapshot();
    assert(snapshot.operation == (resume ? moonlight_diagnostics::CommandOperation::Resume :
        moonlight_diagnostics::CommandOperation::Launch));
    assert(snapshot.gameSession == acknowledgement);
    if (success) {
        assert(server.serverInfo.rtspSessionUrl);
        assert(std::string(server.serverInfo.rtspSessionUrl) == "rtsp://127.0.0.1:48010");
    }
    delete[] server.serverInfo.rtspSessionUrl;
}

int main() {
    const std::string url = "<sessionUrl0>rtsp://127.0.0.1:48010</sessionUrl0>";
    check(false, "<root status_code='200'><gamesession>1</gamesession>"+url+"</root>", true, 1);
    check(true, "<root status_code='200'><resume>1</resume>"+url+"</root>", true, 1);
    check(true, "<root status_code='200'><gamesession>1</gamesession>"+url+"</root>", false, -1);
    check(false, "<root status_code='200'><resume>1</resume>"+url+"</root>", false, -1);
    check(true, "<root status_code='200'><resume>0</resume>"+url+"</root>", false, 0);
    check(true, "<root status_code='200'><resume>11</resume>"+url+"</root>", false, -1);
    check(true, "<root status_code='200'><resume>1</resume></root>", false, 1);
    check(true, "<root status_code='503'><resume>0</resume></root>", false, -1);
    check(true, "<root status_code='200'><resume>1", false, -1);
    std::cout << "Actual GameStream launch/resume acknowledgement tests passed\n";
}
