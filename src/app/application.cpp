#include "app/application_controller.hpp"
#include "app/application.hpp"

namespace fogplay {

ApplicationController::ApplicationController(Runtime& runtime, const std::string& id)
    : rt_(runtime), api_(id), catalogDeviceId_(id), guard_(api_, std::string(dataDir) + "/pending-session.json"),
      checkout_(api_, guard_, std::string(dataDir) + "/pending-order.json"),
      ui_(runtime.renderer, runtime.font, runtime.titleFont) {
    api_.configureAuthStorage(std::string(dataDir) + "/auth.json");
    if (!recentGames_.load()) diagnostic("Recent games load unavailable; history will remain optional.");
    syncPending();
    try {
        videoSettings_ = fog::video::loadSettings(std::string(dataDir) + "/video-settings.json");
        if (videoSettings_.codec == ::stream::VideoCodec::H265 && !fog::Media::supportsH265()) {
            videoSettings_.codec = ::stream::VideoCodec::H264;
            videoSaveStatus_ = "H.265 недоступен в этой сборке. Выбран H.264; файл не изменён.";
        }
    } catch (...) {
        videoSettings_ = {};
        videoSaveStatus_ = "Настройки не прочитаны. Используются стандартные; измените, чтобы сохранить.";
        diagnostic("Video settings unreadable or invalid; using defaults.");
    }
    try { controllerSettings_ = fog::controller::loadSettings(std::string(dataDir) + "/controller-settings.json"); }
    catch (...) { diagnostic("Controller settings unreadable; using Xbox layout default."); }
    ui_.setXboxLayout(controllerSettings_.xbox);
    overlay_.setVideoSettings(videoSettings_);
}

ApplicationController::~ApplicationController() {
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
    status_ = "Проверяем сохранённый вход…";
    for (;;) {
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
        try {
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
            // The shutdown owner retries unresolved stops and provides an explicit escape.
            if (exitAfterStop_ && !pending_.valid()) break;
            if (screen_ == Screen::Checkout && !pending_.valid() && api_.loggedIn() &&
                checkoutAutoPoll_ && std::chrono::steady_clock::now() >= checkoutAt_) {
                if (stopAfterTask_) beginFinish(); else pollCheckout(false);
            }
            if ((stream_ || transportReconnect_.recovering()) && focused && !stopAfterTask_) pumpStream(pad, held, down);
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
        try { if (focused) draw(); else svcSleepThread(16000000); }
        catch (const std::exception& error) {
            diagnostic(safeText(error.what()));
            if (sessionPending_ && !stopAfterTask_ && !pending_.valid()) beginFinish("render-exception");
        }
    }
    shutdownSession();
    api_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    diagnostic("Exit requested; cancelling pending request.");
}

void ApplicationController::start(Task kind, std::function<Json()> work) {
    if (pending_.valid()) return;
    task_ = kind;
    status_ = "Подключение к Fog Play…";
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
            status_ = ui_.controllerHints("Не удалось восстановить вход. A — войти заново.");
            diagnostic("Saved authentication restore failed; credentials omitted.");
            return;
        }
        if (kind == Task::Finish) { screen_ = Screen::Stopping; retryAt_ = std::chrono::steady_clock::now()+std::chrono::seconds(exitAfterStop_ ? 1 : 10); }
        if (kind == Task::Create || kind == Task::CheckoutPoll || kind == Task::CheckoutStop) {
            logCheckoutResult(kind);
            syncPending(); screen_ = sessionPending_ ? Screen::Checkout : Screen::Details;
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
            status_ = ui_.controllerHints("Сохранённый вход не прошёл проверку. A — войти заново.");
            diagnostic("Saved authentication rejected or unreadable; credentials omitted.");
        } else if (sessionPending_) {
            screen_ = Screen::Recovery;
            status_ = ui_.controllerHints("Нужно проверить прежнюю аренду. Y — войти в аккаунт.");
        } else loadGames();
        return;
    }
    if (kind == Task::Create || kind == Task::CheckoutPoll || kind == Task::CheckoutStop || kind == Task::ManualResolution) {
        logCheckoutResult(kind);
        handleCheckout();
        if (kind == Task::ManualResolution && !sessionPending_) status_ = "Локальная отметка снята после вашей проверки аккаунта.";
        return;
    }
    if (stopAfterTask_ && sessionPending_ && kind != Task::Finish && kind != Task::Recovery && kind != Task::Profile && kind != Task::Phone && kind != Task::Captcha && kind != Task::Otp) {
        beginFinish(); return;
    }
    if (kind == Task::History) { history_ = std::move(data); status_ = "История сессий"; return; }
    if (kind == Task::GameDetail) {
        detail_ = std::move(data.at("game")); autoComputers_ = std::move(data.at("auto"));
        status_.clear(); return;
    }
    if (kind == Task::Finish) {
        if (data.value("terminated",false) && data.value("cleared",false)) {
            sessionPending_ = false; stopAfterTask_ = false; screen_ = Screen::Sessions; sessions_ = Json::array(); selection_ = 0;
            status_ = "Сервер подтвердил завершение сессии.";
            diagnostic("Session finish confirmed; active-session absence checked; journal cleared.");
            disarmExitHandling();
        } else {
            screen_ = Screen::Stopping;
            status_ = ui_.controllerHints("Остановка НЕ подтверждена. Повторяем. Y — повторный вход.");
            retryAt_ = std::chrono::steady_clock::now()+std::chrono::seconds(exitAfterStop_ ? 1 : 10);
            diagnostic("Session stop unresolved; journal retained.");
        }
        return;
    }
    if (kind == Task::Recovery) {
        if (data.value("matched",false)) { armExitHandling(); beginFinish(); }
        else { screen_ = Screen::Recovery; status_ = "Прежняя сессия не сопоставлена. Проверьте её на сайте Fog Play."; }
        return;
    }
    if (kind == Task::Sessions) {
        sessions_ = std::move(data); screen_ = Screen::Sessions; selection_ = 0;
        status_ = ui_.controllerHints("A — подключиться • X — обновить");
        diagnostic("Active sessions loaded; identifiers omitted.");
        return;
    }
    if (kind == Task::Computers) {
        computers_ = std::move(data.at("computers")); selection_ = 0;
        status_.clear();
        diagnostic("Computers OK, count=" + std::to_string(computers_.size()));
    } else if (kind == Task::Profile) {
        screen_ = loginReturn_;
        status_ = api_.authStorageError().empty() ? "Аккаунт подключён. Вход сохранён на SD." :
            "Вход выполнен, но сохранить его на SD не удалось.";
        diagnostic("Authenticated profile OK; personal fields not logged.");
        if (sessionPending_) recoverSession();
        else if (screen_ == Screen::History) loadHistory();
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
            status_ = ui_.controllerHints("Введите текст с картинки. A — открыть клавиатуру.");
        } else if (authStep_ == fog::AuthStep::Otp) {
            status_ = ui_.controllerHints("Введите полученный SMS-код кнопкой A.");
        } else {
            status_ = ui_.controllerHints("Вход не завершён. B — назад, Y — новая попытка.");
            diagnostic("Auth stopped at unsupported/restart response; body omitted.");
        }
    }
}

Application::Application(platform::Runtime& runtime, const std::string& deviceId)
    : controller_(std::make_unique<ApplicationController>(runtime, deviceId)) {}
Application::~Application() = default;
void Application::run() { controller_->run(); }

}
