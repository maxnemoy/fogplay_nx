#include "core/constants/ui_text.hpp"
#include "core/constants/error_text.hpp"
#include "app/application.hpp"
#include "app/app_diagnostics.hpp"
#include "platform/switch/switch_runtime.hpp"
#include <exception>
#include <string>

int main(int, char**) {
    // Buffer startup timing before touching SD; keep logging through platform teardown.
    fogplay::diagnostics::LogSession logSession;
    const auto type = appletGetAppletType();
    if (type != AppletType_Application && type != AppletType_SystemApplication) {
        logSession.start();
        fogplay::platform::fatalScreen(fog::constants::error::Internal::FullApplicationModeRequired);
        return 12;
    }
    try {
        fogplay::platform::Runtime runtime;
        runtime.initGraphics();
        runtime.showStartup(fog::constants::ui::Startup::PreparingLogs);
        logSession.start();
        runtime.initServices();
        std::string id;
        { fogplay::diagnostics::StartupStage stage("device_id"); id = fogplay::platform::deviceId(); }
        fogplay::diagnostics::diagnostic("Platform full_application=1 BSD sessions=12 efficiency=8");
        runtime.showStartup(fog::constants::ui::Startup::CheckingStorage);
        try { fogplay::diagnostics::StartupStage stage("sd_preflight"); fogplay::platform::checkStorage(); }
        catch (const std::exception& error) { fogplay::diagnostics::diagnostic(std::string("SD preflight failed; rental creation will be blocked: ") + error.what()); }
        runtime.showStartup(fog::constants::ui::Startup::PreparingInterface);
        std::unique_ptr<fogplay::Application> app;
        {
            fogplay::diagnostics::StartupStage stage("application_setup");
            app = std::make_unique<fogplay::Application>(runtime, id);
        }
        fogplay::diagnostics::diagnostic("Startup entering_ui_loop");
        app->run();
        return 0;
    } catch (const std::exception& error) {
        logSession.start();
        fogplay::platform::fatalScreen(error.what());
        return 1;
    }
}
