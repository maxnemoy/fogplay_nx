#pragma once
#include "app/app_diagnostics.hpp"
#include <Diagnostics.hpp>

namespace fogplay {
// The sink captures no service/session object. Its code and the application
// logger live for the process; service teardown joins all Moonlight workers
// before LogSession closes the logger. Replacing/delivering the sink is atomic.
class MoonlightDiagnosticsBridge {
public:
    MoonlightDiagnosticsBridge() { moonlight_diagnostics::sink.store(&forward); }
private:
    static void forward(const std::string& message) {
        diagnostics::diagnostic(message, 2048);
    }
};
}
