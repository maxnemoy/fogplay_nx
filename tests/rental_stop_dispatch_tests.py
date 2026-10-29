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
#include "core/diagnostics/thread_observation.hpp"
#include <variant>
#include "features/session/application/session.hpp"
#include "features/session/application/stop_retry_budget.hpp"
#include "features/session/application/paired_checkout.hpp"
#include "features/session/application/checkout.hpp"
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
using TaskResult = std::variant<std::monostate,fog::FinishResult>;
namespace uiText = fog::constants::ui;
using namespace std::chrono_literals;
struct ApplicationController {
    enum class Task { None, Finish, CheckoutStop, PairedStop, PairedPoll, CheckoutPoll, RestoreAuth };
    enum class Screen { Streaming, Stopping, Checkout, Recovery, PlayStation };
    struct Checkout {
        ApplicationController* owner;
        bool unresolved_ = false;
        fog::CheckoutResult result_{};
        bool unresolved() { return unresolved_; }
        const fog::CheckoutResult& result() { return result_; }
        void stop(bool = false) { owner->apiCalled = true; }
        void poll(bool) { owner->apiCalled = true; }
    } checkout_{this};
    struct Guard {
        ApplicationController* owner;
        using Result = fog::FinishResult;
        Result finishAttempt() { owner->apiCalled = true; return {true,true,"done"}; }
    } guard_{this};
    struct Paired {
        ApplicationController* owner;
        fog::PairedCheckoutResult result_{};
        const fog::PairedCheckoutResult& result() {return result_;}
        void requestStop() { owner->stopAfterTask_ = true; }
        void bindAuthenticatedAccount() {}
        void stop() { owner->apiCalled = true; }
        void poll() { owner->apiCalled = true; }
    } pairedOwner_{this};
    Paired* paired_ = &pairedOwner_;
    bool pairedPending_ = false, checkoutAutoPoll_ = true;
    std::future<TaskResult> pending_;
    Task task_ = Task::None;
    Screen screen_ = Screen::Streaming;
    bool sessionPending_ = true, stopAfterTask_ = false, exitAfterStop_ = false;
    bool shutdownDone_ = false, recoveryPaused_ = false, resetStopChecks_ = false;
    bool exitUnlocked = false;
    fog::StopRetryBudget stopRetry_;
    std::string status_, checkoutStatus_;
    std::chrono::steady_clock::time_point retryAt_, checkoutAt_;
    std::atomic<bool> apiCalled{false}, closeEntered{false}, closeExited{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool unblock = false;
    std::atomic<bool> apiCancelled{false};
    struct Services {
        ApplicationController* owner;
        int cancellations = 0;
        void cancel() { ++cancellations; owner->apiCancelled = true; }
    } servicesOwner_{this};
    Services* services_ = &servicesOwner_;
    struct PairedServices {
        struct Pairing { std::string diagnostics() {return {};} } pairing_;
        Pairing* pairing() {return &pairing_;}
        bool bindRental(const fog::RentalId&) {return true;}
    } pairedServicesOwner_;
    PairedServices* pairedServices_ = &pairedServicesOwner_;
    struct Auth {bool authenticated() {return true;}} auth_;
    struct Cancellable { void cancel() {} } latency_;
    Cancellable* catalogApi_ = nullptr;
    void cancelPlayStation() {}
    void disarmExitHandling() { exitUnlocked = true; }
    void syncPending() {}
    bool completePairedTransportReconnect() {return false;}
    bool completePairedVideoReconnect() {return false;}
    bool offerSessionRating(const fog::PairedCheckoutResult&) {return false;}
    void recoverSession() {}
    std::monostate restoreProviders() {return {};}
    void startStream(const fog::RentalId&, bool) {}
    std::string safeText(const std::string& text) {return text;}
    void diagnostic(const std::string&) {}
    void closeStream() {
        assert(stopAfterTask_);
        closeEntered = true;
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock,[this]{return unblock;});
        closeExited = true;
    }
    void start(Task kind, std::function<TaskResult()> work);
    void beginFinish(const char* reason);
    void pauseSessionRecovery();
    void shutdownSession() noexcept;
    void handlePairedCheckout();
    void pollCheckout(bool);
    void retrySessionRecovery() {stopRetry_.reset(); recoveryPaused_=false; beginFinish("manual");}
};
'''
harness += method((root/'src/app/application.cpp').read_text(), 'start') + '\n'
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'beginFinish')
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'pauseSessionRecovery')
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'shutdownSession')
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'handlePairedCheckout')
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'pollCheckout')
harness += r'''
bool waitFor(const std::atomic<bool>& flag) {
    const auto limit = std::chrono::steady_clock::now()+2s;
    while (!flag && std::chrono::steady_clock::now()<limit) std::this_thread::sleep_for(1ms);
    return flag;
}
int main() {
    for (const int providerCase : {0,1,2}) {
        const bool checkout = providerCase == 1;
        ApplicationController app;
        app.checkout_.unresolved_ = checkout;
        app.pairedPending_ = providerCase == 2;
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
        assert(app.task_ == (app.pairedPending_ ? ApplicationController::Task::PairedStop : checkout ? ApplicationController::Task::CheckoutStop : ApplicationController::Task::Finish));
    }
    ApplicationController noRental;
    noRental.sessionPending_ = false;
    noRental.beginFinish("test");
    assert(!noRental.apiCalled && !noRental.closeEntered);
    {
        ApplicationController app; app.unblock = true;
        for (int attempt = 0; attempt < 3; ++attempt) {
            app.beginFinish("budget-test"); assert(app.pending_.valid()); app.pending_.get();
        }
        app.apiCalled = false; app.closeEntered = false;
        app.beginFinish("exhausted");
        assert(!app.pending_.valid() && !app.apiCalled && !app.closeEntered);
        assert(app.recoveryPaused_ && !app.checkoutAutoPoll_ && app.exitUnlocked && app.sessionPending_);
        assert(app.screen_ == ApplicationController::Screen::Recovery);
    }
    {
        fog::StopRetryBudget budget;
        const auto now = fog::StopRetryBudget::Clock::time_point{};
        assert(budget.begin(now));
        assert(!budget.begin(now + 30s) && budget.exhausted(now + 30s));
        budget.reset(); assert(budget.begin(now + 31s) && budget.attempts() == 1);
    }
    {
        ApplicationController app; app.pairedPending_=true; app.unblock=true;
        app.paired_->result_.state=fog::PairedCheckoutState::RecoveryRequired;
        app.paired_->result_.stopRequested=true; // provider failPaid initiated stop without main-thread intent
        app.handlePairedCheckout();
        assert(app.stopAfterTask_ && app.stopRetry_.attempts()==1 && app.checkoutAutoPoll_);
        app.pollCheckout(false); assert(app.task_==ApplicationController::Task::PairedStop); app.pending_.get();
        app.handlePairedCheckout(); assert(app.stopRetry_.attempts()==2);
        app.pollCheckout(false); app.pending_.get(); app.handlePairedCheckout();
        assert(app.stopRetry_.attempts()==3 && app.recoveryPaused_ && !app.checkoutAutoPoll_);
        app.apiCalled=false; app.pollCheckout(false);
        assert(!app.pending_.valid() && !app.apiCalled);
    }
    {
        ApplicationController app; app.exitAfterStop_ = true; app.unblock = true;
        app.pending_ = std::async(std::launch::async,[&]() -> TaskResult {
            while (!app.apiCancelled) std::this_thread::sleep_for(1ms);
            return std::monostate{};
        });
        const auto began = std::chrono::steady_clock::now();
        app.shutdownSession();
        assert(app.pending_.wait_for(1s) == std::future_status::ready); app.pending_.get();
        assert(std::chrono::steady_clock::now()-began < 1s && app.exitUnlocked);
        assert(app.sessionPending_ && !app.apiCalled && app.servicesOwner_.cancellations == 1);
        app.shutdownSession(); app.beginFinish("after-exit");
        assert(!app.apiCalled && !app.pending_.valid() && app.servicesOwner_.cancellations == 1);
    }
    std::cout << "Rental stop dispatch precedes blocked transport cleanup: MTS session/order and paired FH PASS (real glue, fake API/transport)\n";
    std::cout << "Stop retry cap/deadline, retained warning UI, OS-exit cancellation/unlock/join with pending worker PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-rental-stop-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'src'),'-I'+str(root/'third_party'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
