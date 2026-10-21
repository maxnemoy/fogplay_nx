#include "app/application.hpp"
#include "app/app_diagnostics.hpp"
#include "platform/switch/switch_runtime.hpp"
#include <exception>
#include <string>

int main(int, char**) {
    // Keep the log alive through Runtime teardown and the outer fatal handler.
    fogplay::diagnostics::LogSession logSession;
    const auto type = appletGetAppletType();
    if (type != AppletType_Application && type != AppletType_SystemApplication) {
        fogplay::platform::fatalScreen("Full application mode required. Album mode is not supported.");
        return 12;
    }
    try {
        fogplay::platform::Runtime runtime;
        runtime.init();
        const auto id = fogplay::platform::deviceId();
        fogplay::diagnostics::diagnostic("Platform full_application=1 BSD sessions=12 efficiency=8");
        try { fogplay::platform::checkStorage(); }
        catch (const std::exception& error) { fogplay::diagnostics::diagnostic(std::string("SD preflight failed; rental creation will be blocked: ") + error.what()); }
        fogplay::Application app(runtime, id);
        app.run();
        return 0;
    } catch (const std::exception& error) {
        fogplay::platform::fatalScreen(error.what());
        return 1;
    }
}
