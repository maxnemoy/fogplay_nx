#include "core/constants/storage_paths.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"
#include "app/application.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

ApplicationController::ApplicationController(Runtime& runtime, const std::string& id)
    : rt_(runtime), api_(id), catalogDeviceId_(id), guard_(api_, storagePaths::path(storagePaths::PendingSession)),
      checkout_(api_, guard_, storagePaths::path(storagePaths::PendingOrder)),
      ui_(runtime.renderer, runtime.font, runtime.titleFont) {
    api_.configureAuthStorage(storagePaths::path(storagePaths::Auth));
    if (!recentGames_.load()) diagnostic("Recent games load unavailable; history will remain optional.");
    syncPending();
    try {
        videoSettings_ = fog::video::loadSettings(storagePaths::path(storagePaths::VideoSettings));
    } catch (...) {
        videoSettings_ = {};
        videoSaveStatus_ = uiText::Settings::VideoLoadFailed;
        diagnostic("Video settings unreadable or invalid; using defaults.");
    }
    if (videoSettings_.codec == ::stream::VideoCodec::H265 && !fog::Media::supportsH265()) {
        videoSettings_.codec = ::stream::VideoCodec::H264;
        videoSaveStatus_ = uiText::Settings::H265FallbackPreserved;
    }
    try { connectionSettings_ = fog::connection::loadSettings(storagePaths::path(storagePaths::ConnectionSettings)); }
    catch (...) {
        connectionSaveStatus_ = uiText::Settings::ConnectionLoadFailed;
        diagnostic("Connection settings unreadable or invalid; using normal mode.");
    }
    try { controllerSettings_ = fog::controller::loadSettings(storagePaths::path(storagePaths::ControllerSettings)); }
    catch (...) { diagnostic("Controller settings unreadable; using Xbox layout default."); }
    ui_.setXboxLayout(controllerSettings_.xbox);
    overlay_.setVideoSettings(videoSettings_);
}

ApplicationController::~ApplicationController() {
    cancelPlayStation();
    if (playStationPending_.valid()) playStationPending_.wait();
    latency_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    // Last attempt precedes permanent cancellation and networking teardown.
    shutdownSession();
    api_.cancel();
    if (pending_.valid()) pending_.wait();
    if (catalogPending_.valid()) catalogPending_.wait();
    latency_.stop();
    if (recentSave_.valid()) recentSave_.wait();
    if (recentSavePending_) recentGames_.save();
    if (captcha_) SDL_DestroyTexture(captcha_);
}

void ApplicationController::run() {
    hidInitializeTouchScreen();
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    screen_ = sessionPending_ ? Screen::Recovery : Screen::Games;
    start(Task::RestoreAuth,[this] { return Json{{"restored",api_.restoreAuth()}}; });
    status_ = uiText::Profile::ValidatingSavedAuth;
    for (;;) {
        fog::stream::LoopTiming::Scope loopScope(loopTiming_, fog::stream::LoopTiming::Stage::Loop,
            stream_ || transportReconnect_.recovering());
        fog::stream::LoopTiming::Scope eventsScope(loopTiming_, fog::stream::LoopTiming::Stage::Events);
        const bool systemRunning = appletMainLoop();
        if (!systemRunning) {
            if (!sessionPending_) break;
            if (!exitAfterStop_) diagnostic("Stop trigger: system exit request; OS grace period is limited.");
            exitAfterStop_ = true;
            stopAfterTask_ = true;
        }
        const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
        SDL_Event event;
        bool quit = false;
        while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) quit = true;
        padUpdate(&pad);
        const auto down = focused ? padGetButtonsDown(&pad) : 0;
        auto uiDown = fog::controller::mapUiButtons(down,controllerSettings_,faceMasks);
        const auto held = focused ? padGetButtons(&pad) : 0;
        if (quit || ((down & HidNpadButton_Plus) && !sessionPending_)) {
            if (!sessionPending_) break;
            exitAfterStop_ = true; stopAfterTask_ = true;
        }
        eventsScope.finish();
        if (!focused || screen_ != Screen::Streaming || stopAfterTask_)
            loopTiming_.suspendPresentation();
        try {
            fog::stream::LoopTiming::Scope pollScope(loopTiming_, fog::stream::LoopTiming::Stage::Poll);
            if (exitAfterStop_ && sessionPending_ && !pending_.valid() && !stopStartedOnExit_) {
                stopStartedOnExit_ = true;
                beginFinish("application-exit");
            }
            // Accept stop intent before consuming a ready worker result. The worker may
            // still own checkout_/guard_, so queueing only changes main-thread state.
            if (screen_ == Screen::Checkout && (uiDown & HidNpadButton_B)) {
                queueCheckoutStop();
                uiDown &= ~HidNpadButton_B;
            }
            // Queue focus loss before consuming a ready checkout, so a background
            // completion cannot briefly open RTC/media instead of stopping the rental.
            if (sessionPending_ && !stopAfterTask_ && !focused) {
                diagnostic("Stop trigger: lost application focus; state=" + std::to_string(static_cast<int>(appletGetFocusState())));
                stopAfterTask_ = true; if (!pending_.valid()) beginFinish("focus-lost");
            }
            pollRecentSave();
            poll();
            pollCatalog();
            pollPlayStation();
            // The shutdown owner retries unresolved stops and provides an explicit escape.
            if (exitAfterStop_ && !pending_.valid()) break;
            if (screen_ == Screen::Checkout && !pending_.valid() && api_.loggedIn() &&
                checkoutAutoPoll_ && std::chrono::steady_clock::now() >= checkoutAt_) {
                if (stopAfterTask_) beginFinish(); else pollCheckout(false);
            }
            pollScope.finish();
            if ((stream_ || transportReconnect_.recovering()) && focused && !stopAfterTask_) {
                fog::stream::LoopTiming::Scope pumpScope(loopTiming_, fog::stream::LoopTiming::Stage::Pump);
                pumpStream(pad, held, down);
            }
            fog::stream::LoopTiming::Scope inputScope(loopTiming_, fog::stream::LoopTiming::Stage::Input);
            if (!pending_.valid() && focused) input(uiDown);
            if (screen_ == Screen::Games && focused && (!sidebar_ || catalog_.items().empty())) requestCatalog();
            if (screen_ == Screen::Stopping && !pending_.valid() && api_.loggedIn() &&
                std::chrono::steady_clock::now() >= retryAt_) beginFinish();
        }
        catch (const std::exception& error) {
            status_ = safeText(error.what()); diagnostic(status_);
            if (screen_ == Screen::Checkout && !pending_.valid()) checkoutStatus_ = status_;
            if (sessionPending_ && (stream_ || !streamHash_.empty() || transportReconnect_.recovering())) beginFinish("stream-exception");
        }
        if (exitAfterStop_ && !sessionPending_ && !pending_.valid()) break;
        try {
            fog::stream::LoopTiming::Scope drawScope(loopTiming_, fog::stream::LoopTiming::Stage::Draw);
            if (focused) draw(); else svcSleepThread(16000000);
        }
        catch (const std::exception& error) {
            diagnostic(safeText(error.what()));
            if (sessionPending_ && !stopAfterTask_ && !pending_.valid()) beginFinish("render-exception");
        }
    }
    cancelPlayStation();
    shutdownSession();
    api_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    diagnostic("Exit requested; cancelling pending request.");
}

void ApplicationController::start(Task kind, std::function<Json()> work) {
    if (pending_.valid()) return;
    task_ = kind;
    status_ = uiText::Profile::ConnectingService;
    pending_ = std::async(std::launch::async, std::move(work));
}

void ApplicationController::poll() {
    if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto kind = task_;
    Json data;
    try { data = pending_.get(); }
    catch (...) {
        if (kind == Task::RestoreAuth) {
            screen_ = Screen::Consent; loginReturn_ = Screen::Games;
            status_ = ui_.controllerHints(uiText::Profile::RestoreFailedHint);
            diagnostic("Saved authentication restore failed; credentials omitted.");
            return;
        }
        if (kind == Task::Finish) { screen_ = Screen::Stopping; retryAt_ = std::chrono::steady_clock::now()+std::chrono::seconds(exitAfterStop_ ? 1 : 10); }
        if (kind == Task::Create || kind == Task::CheckoutPoll || kind == Task::CheckoutStop) {
            logCheckoutResult(kind);
            syncPending(); screen_ = sessionPending_ ? Screen::Checkout : selectedPlayStation_ ? Screen::PlayStationDetails : Screen::Details;
            checkoutAutoPoll_ = sessionPending_ && fog::checkoutNeedsAutomaticCheck(checkout_.result());
            checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
            if (!sessionPending_) disarmExitHandling();
            else if (stopAfterTask_ && kind != Task::CheckoutStop) beginFinish();
        }
        if (stopAfterTask_ && sessionPending_ && !pending_.valid() &&
            kind != Task::Finish && kind != Task::CheckoutStop) beginFinish();
        throw;
    }
    if (kind == Task::RestoreAuth) {
        if (data.value("restored",false)) {
            authSaveFailed_ = false;
            diagnostic("Saved authentication validated; credentials omitted.");
            if (sessionPending_) recoverSession(); else loadGames();
        } else if (!api_.authStorageError().empty()) {
            screen_ = Screen::Consent; loginReturn_ = Screen::Games;
            status_ = ui_.controllerHints(uiText::Profile::ValidationFailedHint);
            diagnostic("Saved authentication rejected or unreadable; credentials omitted.");
        } else if (sessionPending_) {
            screen_ = Screen::Recovery;
            status_ = ui_.controllerHints(uiText::Profile::PreviousRentalSignInHint);
        } else loadGames();
        return;
    }
    if (kind == Task::Create && data.value("playstation_offer_verified",false)) {
        // The public fetch can outlive Back, HOME or shutdown intent. Only the
        // main thread reads these flags and authorizes the separate paid worker.
        syncPending();
        if (sessionPending_) { beginFinish("playstation-preflight-journal"); return; }
        if (stopAfterTask_ || exitAfterStop_ || appletGetFocusState() != AppletFocusState_InFocus) {
            disarmExitHandling(); stopAfterTask_ = false;
            screen_ = Screen::PlayStation;
            status_ = uiText::Server::ConnectionCancelled;
            return;
        }
        fog::CreateOrderRequest order;
        order.computerId = data.at("computer_id").get<int>();
        order.tariffId = data.at("tariff_id").get<int>();
        order.gameId = 0; order.ownSteamAccount = false;
        if (!selectedPlayStation_ || !api_.loggedIn() ||
            !fog::isPlayStationComputerId(order.computerId) || order.tariffId <= 0) {
            disarmExitHandling();
            screen_ = Screen::PlayStation;
            status_ = uiText::Server::ConnectionNotConfirmed;
            return;
        }
        sessionPending_ = true;
        checkoutStatus_ = uiText::Server::CreatingOrder;
        start(Task::Create,[this,order] { checkout_.create(order); return Json::object(); });
        return;
    }
    if (kind == Task::Create && data.value("playstation_offer_changed",false)) {
        syncPending();
        if (sessionPending_) { beginFinish("playstation-offer-change"); return; }
        disarmExitHandling(); stopAfterTask_ = false;
        playStationComputers_ = std::move(data.at("computers"));
        playStationLoadedAt_ = std::chrono::steady_clock::now();
        screen_ = Screen::PlayStation; selection_ = 0;
        status_ = uiText::Server::TariffChanged;
        return;
    }
    if (kind == Task::Create || kind == Task::CheckoutPoll || kind == Task::CheckoutStop || kind == Task::ManualResolution) {
        logCheckoutResult(kind);
        handleCheckout();
        if (kind == Task::ManualResolution && !sessionPending_) status_ = uiText::Server::LocalBlockCleared;
        return;
    }
    if (stopAfterTask_ && sessionPending_ && kind != Task::Finish && kind != Task::Recovery && kind != Task::Profile && kind != Task::Phone && kind != Task::Captcha && kind != Task::Otp) {
        beginFinish(); return;
    }
    if (kind == Task::History) { history_ = std::move(data); status_.clear(); return; }
    if (kind == Task::GameDetail) {
        detail_ = std::move(data.at("game")); autoComputers_ = std::move(data.at("auto"));
        status_.clear(); return;
    }
    if (kind == Task::Finish) {
        if (data.value("terminated",false) && data.value("cleared",false)) {
            sessionPending_ = false; stopAfterTask_ = false; screen_ = Screen::Sessions; sessions_ = Json::array(); selection_ = 0;
            status_ = uiText::Server::TerminationConfirmed;
            diagnostic("Session finish confirmed; active-session absence checked; journal cleared.");
            disarmExitHandling();
        } else {
            screen_ = Screen::Stopping;
            status_ = ui_.controllerHints(uiText::Server::TerminationRetrySignInHint);
            retryAt_ = std::chrono::steady_clock::now()+std::chrono::seconds(exitAfterStop_ ? 1 : 10);
            diagnostic("Session stop unresolved; journal retained.");
        }
        return;
    }
    if (kind == Task::Recovery) {
        if (data.value("matched",false)) { armExitHandling(); beginFinish(); }
        else { screen_ = Screen::Recovery; status_ = uiText::Server::PreviousRentalNotMatched; }
        return;
    }
    if (kind == Task::Sessions) {
        sessions_ = std::move(data); screen_ = Screen::Sessions; selection_ = 0;
        status_.clear();
        diagnostic("Active sessions loaded; identifiers omitted.");
        return;
    }
    if (kind == Task::Computers) {
        computers_ = std::move(data.at("computers")); selection_ = 0;
        status_.clear();
        diagnostic("Computers OK, count=" + std::to_string(computers_.size()));
    } else if (kind == Task::Profile) {
        screen_ = loginReturn_;
        status_ = api_.authStorageError().empty() ? uiText::Profile::AuthSaved :
            uiText::Profile::AuthSaveFailed;
        diagnostic("Authenticated profile OK; personal fields not logged.");
        if (sessionPending_) recoverSession();
        else if (screen_ == Screen::History) loadHistory();
        else if (screen_ == Screen::PlayStation) loadPlayStation();
        else if (screen_ == Screen::Games && catalog_.items().empty()) loadGames();
    } else {
        authStep_ = fog::authStep(data);
        screen_ = Screen::Auth;
        if (authStep_ == fog::AuthStep::Success) {
            authSaveFailed_ = !api_.authStorageError().empty();
            if (captcha_) { SDL_DestroyTexture(captcha_); captcha_ = nullptr; }
            start(Task::Profile, [this] { return api_.userInfo(); });
        } else if (authStep_ == fog::AuthStep::Captcha) {
            setCaptcha(data);
            status_.clear();
        } else if (authStep_ == fog::AuthStep::Otp) {
            status_.clear();
        } else {
            status_ = ui_.controllerHints(uiText::Profile::SignInIncompleteHint);
            diagnostic("Auth stopped at unsupported/restart response; body omitted.");
        }
    }
}

Application::Application(platform::Runtime& runtime, const std::string& deviceId)
    : controller_(std::make_unique<ApplicationController>(runtime, deviceId)) {}
Application::~Application() = default;
void Application::run() { controller_->run(); }

}
