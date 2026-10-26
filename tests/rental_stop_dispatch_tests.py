"""Run actual Switch glue method bodies against isolated lifecycle test doubles."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]

def method(source, name):
    start = source.index('void ApplicationController::' + name + '(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

harness = r'''
#include "core/constants/ui_text.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
using Json = nlohmann::json;
namespace uiText = fog::constants::ui;
using namespace std::chrono_literals;
struct ApplicationController {
    enum class Task { None, Finish, CheckoutStop };
    enum class Screen { Streaming, Stopping, Checkout };
    struct Checkout {
        ApplicationController* owner;
        bool unresolved_ = false;
        bool unresolved() { return unresolved_; }
        void stop() { owner->apiCalled = true; }
    } checkout_{this};
    struct Guard {
        ApplicationController* owner;
        struct Result { bool terminated, journalCleared; std::string message; };
        Result finishAttempt() { owner->apiCalled = true; return {true,true,"done"}; }
    } guard_{this};
    std::future<Json> pending_;
    Task task_ = Task::None;
    Screen screen_ = Screen::Streaming;
    bool sessionPending_ = true, stopAfterTask_ = false, exitAfterStop_ = false;
    std::string status_, checkoutStatus_;
    std::chrono::steady_clock::time_point retryAt_;
    std::atomic<bool> apiCalled{false}, closeEntered{false}, closeExited{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool unblock = false;
    void diagnostic(const std::string&) {}
    void closeStream() {
        assert(stopAfterTask_);
        closeEntered = true;
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock,[this]{return unblock;});
        closeExited = true;
    }
    void start(Task kind, std::function<Json()> work);
    void beginFinish(const char* reason);
};
'''
harness += method((root/'src/app/application.cpp').read_text(), 'start') + '\n'
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'beginFinish')
harness += r'''
bool waitFor(const std::atomic<bool>& flag) {
    const auto limit = std::chrono::steady_clock::now()+2s;
    while (!flag && std::chrono::steady_clock::now()<limit) std::this_thread::sleep_for(1ms);
    return flag;
}
int main() {
    for (const bool checkout : {false,true}) {
        ApplicationController app;
        app.checkout_.unresolved_ = checkout;
        std::thread mainLoop([&] { app.beginFinish("test"); });
        const bool closeStarted = waitFor(app.closeEntered);
        const bool requestStarted = waitFor(app.apiCalled);
        const bool cleanupStillBlocked = !app.closeExited;
        { std::lock_guard<std::mutex> lock(app.mutex); app.unblock = true; }
        app.condition.notify_all(); mainLoop.join();
        assert(closeStarted && requestStarted && cleanupStillBlocked);
        assert(app.pending_.valid());
        app.pending_.get();
        assert(app.closeExited && app.sessionPending_);
        assert(app.task_ == (checkout ? ApplicationController::Task::CheckoutStop : ApplicationController::Task::Finish));
    }
    ApplicationController noRental;
    noRental.sessionPending_ = false;
    noRental.beginFinish("test");
    assert(!noRental.apiCalled && !noRental.closeEntered);
    std::cout << "Rental stop dispatch precedes blocked transport cleanup: session/order PASS (real glue, fake API/transport)\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-rental-stop-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'src'),'-I'+str(root/'third_party'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
