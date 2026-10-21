#pragma once
#include <memory>
#include <string>

namespace fogplay {
namespace platform { struct Runtime; }
class ApplicationController;

class Application {
public:
    Application(platform::Runtime& runtime, const std::string& deviceId);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    void run();
private:
    std::unique_ptr<ApplicationController> controller_;
};
}
