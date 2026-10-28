#include "client.h"
#include "Settings.hpp"
#include "FogMoonlightDiagnostics.h"

#ifdef MOONLIGHT_CURL_COEXIST_SMOKE
extern "C" int moonlight_check_app_curl(void);
#endif

// Compile/link proof only: this executable never contacts a host.
int main(int argc, char**) {
    SERVER_DATA server{};
    STREAM_CONFIGURATION configuration{};
    LiInitializeStreamConfiguration(&configuration);
    FogMoonlightDiagnostics diagnostics{};
    FogMoonlightGetDiagnostics(&diagnostics);
    if (argc == 42) {
#ifdef MOONLIGHT_CURL_COEXIST_SMOKE
        if (moonlight_check_app_curl() != 0)
            return 1;
#endif
        gs_init(&server, "127.0.0.1");
        char pin[] = "0000";
        gs_pair(&server, pin);
        PAPP_LIST apps = nullptr;
        gs_applist(&server, &apps);
        gs_start_app(&server, &configuration, 1, false, false, 1);
        LiStartConnection(&server.serverInfo, &configuration, nullptr, nullptr,
                          nullptr, nullptr, 0, nullptr, 0);
        LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0);
        LiStopConnection();
        gs_quit_app(&server);
    }
    return 0;
}
