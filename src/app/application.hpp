#pragma once
#include <memory>
#include "app/services.hpp"
#include <string>

namespace fogplay {
namespace platform { struct Runtime; }
class ApplicationController;

class Application {
public:
    Application(platform::Runtime& runtime, std::unique_ptr<Services> services);
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    void run();
private:
    std::unique_ptr<ApplicationController> controller_;
};
}
