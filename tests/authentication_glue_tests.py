"""Actual auth coordinator methods with synthetic keyboard/repositories; no network."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
application = (root / 'src/app/application.cpp').read_text()
authentication = (root / 'src/features/auth/presentation/authentication_controller.cpp').read_text()
rental = (root / 'src/features/session/presentation/rental_controller.cpp').read_text()
header = (root / 'src/app/application_controller.hpp').read_text()


def method(source, name):
    start = source.index('void ApplicationController::' + name + '(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def enum(name):
    start = header.index('enum class ' + name + ' {')
    return header[start:header.index(';', start) + 1]


harness = r'''
#include "features/account/domain/account_repository.hpp"
#include "features/session/application/session.hpp"
#include "features/session/application/stop_retry_budget.hpp"
#include "core/constants/ui_text.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "features/session/presentation/session_rating.hpp"
#include "features/session/presentation/session_rating_text.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>
namespace uiText = fog::constants::ui;
std::vector<std::string> logs;
void diagnostic(const std::string& value) {logs.push_back(value);}
std::vector<std::optional<std::string>> answers;
std::size_t keyboardCalls=0;
int failKeyboard=-1;
std::optional<std::string> keyboard(const char* title,const char*,std::size_t limit,bool secret=false) {
    const auto call=keyboardCalls++;
    if(static_cast<int>(call)==failKeyboard) throw std::runtime_error("software keyboard failed (0x123)");
    if(call==0) assert(std::string(title)=="FogHub: login" || std::string(title)=="Phone number +7...");
    if(call==1) assert(std::string(title)=="FogHub: password" && secret && limit==256);
    return answers.at(call);
}
void SDL_DestroyTexture(void*) {}
constexpr int AppletFocusState_InFocus=1;
int appletGetFocusState() {return AppletFocusState_InFocus;}
namespace fogplay {
''' + enum('Screen') + '\n' + enum('Task') + r'''
struct FakeAuth {
    fog::LoginMethod method=fog::LoginMethod::Credentials;
    fog::AuthStep response=fog::AuthStep::Success;
    bool signedIn=false, failValidation=false, failBegin=false;
    int begins=0;
    fog::LoginRequest received;
    fog::AuthCapabilities capabilities() const {return {method};}
    bool authenticated() const {return signedIn;}
    std::string storageError() const {return {};}
    void validateLogin(const fog::LoginRequest&) {
        if(failValidation) throw std::runtime_error("synthetic invalid credentials");
    }
    fog::AuthResult begin(const fog::LoginRequest& request) {
        ++begins;received=request;
        if(failBegin) throw std::runtime_error("synthetic authentication failure");
        signedIn=response==fog::AuthStep::Success;
        fog::AuthResult result;result.step=response;return result;
    }
};
struct FakeAccount {
    bool fail=false;
    fog::UserProfile profile() {
        if(fail) throw std::runtime_error("synthetic profile failure");
        fog::UserProfile result;result.displayName="synthetic account";return result;
    }
};
struct RestoredProviders {
    bool mts=false,foghub=false,mtsFailed=false,foghubFailed=false;
    std::optional<fog::UserProfile> mtsProfile,foghubProfile;
};
struct RecoveryResult {bool matched=false;};
struct FakeOrder {int computerId=0;std::string offerId;};
struct VerifiedOffer {FakeOrder request;};
struct ChangedOffer {std::vector<fog::Computer> computers;};
using TaskResult=std::variant<std::monostate,RestoredProviders,fog::AuthResult,fog::UserProfile,
    fog::GameDetail,fog::Page<fog::Computer>,std::vector<fog::HistoryEntry>,std::vector<fog::Rental>,
    fog::FinishResult,RecoveryResult,VerifiedOffer,ChangedOffer,
    std::optional<fog::SessionRatingForm>,fog::SessionRatingResult>;
struct FakePaired {void create(const FakeOrder&) {} void bindAuthenticatedAccount() {} void stop() {}};
struct FakeCheckout {int result() {return 0;} void create(const FakeOrder&) {}};
struct ApplicationController {
    fog::rating::Presentation rating_;
    FakeAuth auth_;
    FakeAccount account_;
    FakeAuth& loginAuth() {return auth_;}
    FakeAccount& loginAccount() {return account_;}
    fog::ProviderId loginProvider_=fog::ProviderId::FogHub;
    Screen screen_=Screen::Profile,loginReturn_=Screen::Games;
    fog::AuthStep authStep_=fog::AuthStep::Unknown;
    std::future<TaskResult> pending_;
    Task task_=Task::RestoreAuth;
    std::string status_,checkoutStatus_;
    std::size_t selection_=1;
    bool sessionPending_=false,pairedPending_=false,stopAfterTask_=false,exitAfterStop_=false;
    bool recoveryPaused_=false;
    fog::StopRetryBudget stopRetry_;
    bool checkoutAutoPoll_=false,selectedPlayStation_=false,authSaveFailed_=false;
    std::chrono::steady_clock::time_point retryAt_{},checkoutAt_{},playStationLoadedAt_{};
    std::optional<fog::UserProfile> mtsProfile_,foghubProfile_;
    std::vector<fog::Computer> playStationComputers_,computers_;
    std::vector<fog::Rental> sessions_;
    std::vector<fog::HistoryEntry> history_;
    fog::Game detail_;fog::AutomaticComputers autoComputers_;
    struct Catalog {std::vector<int> items() {return {1};}} catalog_;
    struct Ui {std::string controllerHints(const std::string& value) {return value;}} ui_;
    FakeCheckout checkout_;
    std::unique_ptr<FakePaired> paired_=std::make_unique<FakePaired>();
    void* captcha_=nullptr;
    int gameLoads=0;
    int recoveryCalls=0;
    void loadGames() {++gameLoads;}
    void syncPending() {} void disarmExitHandling() {} void armExitHandling() {}
    void pauseSessionRecovery();
    void beginFinish(const char* ="") {} void logCheckoutResult(Task) {} void handleCheckout() {}
    void handlePairedCheckout() {} void recoverSession() {++recoveryCalls;} void loadHistory() {} void loadPlayStation() {}
    void completeSessionRatingLoad(std::optional<fog::SessionRatingForm>) {}
    void completeSessionRatingSubmit(const fog::SessionRatingResult&) {}
    void setCaptcha(const fog::AuthResult&) {}
    void refreshProfile();void beginLogin();void start(Task,std::function<TaskResult()>);void poll();
    void frame() {
        try {poll();}
        catch(const std::exception& error) {status_=error.what();}
    }
    void drain() {while(pending_.valid()) {pending_.wait();frame();}}
};
}
namespace fog {
bool checkoutNeedsAutomaticCheck(int) {return false;}
bool isPlayStationComputerId(int) {return false;}
}
namespace fogplay {
''' + '\n'.join([method(application, 'refreshProfile'), method(application, 'start'), method(rental, 'pauseSessionRecovery'),
                  method(application, 'poll'), method(authentication, 'beginLogin')]) + r'''
}
void reset(std::vector<std::optional<std::string>> values) {
    answers=std::move(values);keyboardCalls=0;failKeyboard=-1;logs.clear();
}
void noSecretsInLogs() {
    for(const auto& line:logs) {
        assert(line.find("identifier-private")==std::string::npos);
        assert(line.find("password-private")==std::string::npos);
    }
}
int main() {
    using fogplay::ApplicationController;using fogplay::Screen;
    {
        ApplicationController app; app.sessionPending_=true;
        app.start(fogplay::Task::Recovery,[]() -> fogplay::TaskResult {throw std::runtime_error("synthetic active-list outage");});
        app.drain();
        assert(app.sessionPending_ && app.recoveryPaused_ && !app.checkoutAutoPoll_ && app.screen_==Screen::Recovery);
        assert(!app.pending_.valid());
    }
    for (const bool paused : {false,true}) {
        ApplicationController app; app.sessionPending_=true; app.pairedPending_=true; app.recoveryPaused_=paused;
        app.start(fogplay::Task::Profile,[]() -> fogplay::TaskResult {return fog::UserProfile{};}); app.drain();
        assert(app.recoveryCalls==(paused?0:1) && !app.pending_.valid());
        assert(app.stopRetry_.attempts()==0); // profile never dispatches an unbudgeted PairedStop
        if(paused) assert(app.recoveryPaused_ && app.screen_==Screen::Recovery);
    }
    {
        reset({"identifier-private","password-private"});ApplicationController app;
        app.refreshProfile();
        assert(keyboardCalls==2 && app.screen_==Screen::Auth && app.pending_.valid());
        app.drain();
        assert(app.screen_==Screen::Profile && app.foghubProfile_ && app.auth_.begins==1);
        assert(app.auth_.received.identifier=="identifier-private" && app.auth_.received.secret=="password-private");
        assert(app.status_==uiText::Profile::AuthSaved);noSecretsInLogs();
    }
    for(int field=0;field<2;++field) for(int empty=0;empty<2;++empty) {
        reset({"identifier-private","password-private"});answers[field]=empty?std::optional<std::string>(""):std::nullopt;
        ApplicationController app;app.screen_=Screen::Consent;app.status_="stale status";app.beginLogin();
        assert(app.screen_==Screen::Profile && !app.status_.empty() && app.status_!="stale status" && !app.pending_.valid());
        assert(keyboardCalls==static_cast<std::size_t>(field+1) && app.auth_.begins==0);
        assert(logs.back().find(field?"field=password":"field=login")!=std::string::npos);
        assert(logs.back().find(empty?"reason=empty":"reason=not_confirmed")!=std::string::npos);
        assert(app.status_.find("identifier-private")==std::string::npos && app.status_.find("password-private")==std::string::npos);
        assert(app.status_.find("A")!=std::string::npos);noSecretsInLogs();
    }
    for(int field=0;field<2;++field) {
        reset({"identifier-private","password-private"});failKeyboard=field;ApplicationController app;
        app.screen_=Screen::Consent;
        try {app.beginLogin();assert(false);} catch(const std::exception& error) {app.status_=error.what();}
        assert(app.screen_==Screen::Profile && !app.pending_.valid() && !app.status_.empty());noSecretsInLogs();
    }
    for(int failure=0;failure<3;++failure) {
        reset({"identifier-private","password-private"});ApplicationController app;
        app.auth_.failValidation=failure==0;app.auth_.failBegin=failure==1;app.account_.fail=failure==2;
        try {app.refreshProfile();app.drain();} catch(const std::exception& error) {app.status_=error.what();}
        assert(app.screen_==Screen::Profile && !app.status_.empty() && !app.pending_.valid());noSecretsInLogs();
        app.auth_.failValidation=false;app.auth_.failBegin=false;app.account_.fail=false;
        // A in Profile refreshes an authenticated profile after profile-load failure;
        // otherwise it starts the credential keyboard again.
        reset({"identifier-private","password-private"});app.refreshProfile();app.drain();
        assert(app.screen_==Screen::Profile && app.foghubProfile_);
        assert(keyboardCalls==(failure==2?0U:2U));noSecretsInLogs();
    }
    for(auto response:{fog::AuthStep::Unknown,fog::AuthStep::Restart}) {
        reset({"identifier-private","password-private"});ApplicationController app;app.auth_.response=response;
        app.refreshProfile();app.drain();assert(app.screen_==Screen::Profile && !app.status_.empty());
        app.auth_.response=fog::AuthStep::Success;reset({"identifier-private","password-private"});
        app.refreshProfile();app.drain();assert(app.foghubProfile_);
    }
    {
        reset({"+71234567890"});ApplicationController app;app.loginProvider_=fog::ProviderId::Mts;
        app.auth_.method=fog::LoginMethod::PhoneCode;
        app.refreshProfile();assert(app.screen_==Screen::Consent && keyboardCalls==0 && !app.pending_.valid());
        app.beginLogin();assert(keyboardCalls==1);app.drain();
        assert(app.screen_==Screen::Profile && app.mtsProfile_ && app.auth_.received.secret.empty());
    }
    {
        reset({std::nullopt});ApplicationController app;app.auth_.method=fog::LoginMethod::PhoneCode;
        app.sessionPending_=true;app.beginLogin();assert(app.screen_==Screen::Recovery && !app.pending_.valid());
    }
    std::cout<<"Auth actual coordinator: direct FH login, login/password non-confirmed and empty input with visible retry status, keyboard/validation/request/profile failure, retry, unsupported response and MTS consent/phone route PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-auth-glue-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-I' + str(root / 'src'), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=15)
