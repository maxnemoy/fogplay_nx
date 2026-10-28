#include "core/constants/storage_paths.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"
#include "app/application.hpp"
#include "features/session/presentation/session_rating_text.hpp"
#include "features/settings/presentation/rumble_text.hpp"
#include "features/settings/presentation/controller_text.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

std::unique_ptr<fog::PairedCheckoutJournal> Services::pairedJournal(const std::string&) { return {}; }
Services* ApplicationController::providerServices(fog::ProviderId id) const { return services_->provider(id); }
fog::AuthRepository& ApplicationController::loginAuth() {
    if (auto* provider = providerServices(loginProvider_)) return provider->auth();
    throw std::runtime_error("Selected provider unavailable");
}
fog::AccountRepository& ApplicationController::loginAccount() {
    if (auto* provider = providerServices(loginProvider_)) return provider->account();
    throw std::runtime_error("Selected provider unavailable");
}
void ApplicationController::openProfile() {
    screen_ = Screen::Profile; sidebar_ = false; selection_ = pairedPending_ ? 1 : 0;
    loginProvider_ = pairedPending_ ? fog::ProviderId::FogHub : fog::ProviderId::Mts;
    status_.clear();
}
void ApplicationController::refreshProfile() {
    const bool authenticated = loginAuth().authenticated();
    diagnostic(std::string("Profile action: provider=") + (loginProvider_ == fog::ProviderId::FogHub ? "FH" : "MTS") +
        (authenticated ? " action=refresh" : " action=login"));
    if (!authenticated) {
        loginReturn_ = Screen::Profile;
        if (loginAuth().capabilities().method == fog::LoginMethod::Credentials) beginLogin();
        else screen_ = Screen::Consent;
        return;
    }
    start(Task::ProviderProfile,[this] { return loginAccount().profile(); });
}

RestoredProviders ApplicationController::restoreProviders() {
    // Serialized worker only. Optional profile/balance traffic must never delay
    // recovery of a potentially billable rental belonging to either provider.
    RestoredProviders result;
    const bool pendingPaired = paired_ && paired_->unresolved();
    const bool pendingMts = checkout_.unresolved() || guard_.persistent() ||
        !guard_.pendingRental().empty() || !guard_.journalError().empty();
    const auto restoreMts = [this,&result,pendingMts] {
        try {
            result.mts = auth_.restore().restored;
            if (result.mts && pendingMts) {
                if (checkout_.unresolved()) checkout_.stop();
                else if (guard_.ownsSession() || guard_.recoverMatchingSession()) guard_.finishAttempt();
            }
        } catch (...) { result.mtsFailed = true; }
    };
    const auto restorePaired = [this,&result,pendingPaired] {
        if (!pairedServices_) return;
        try {
            result.foghub = pairedServices_->auth().restore().restored;
            if (result.foghub && pendingPaired) {
                paired_->bindAuthenticatedAccount();
                paired_->stop();
            }
        } catch (...) { result.foghubFailed = true; }
    };
    if (pendingPaired) restorePaired();
    restoreMts();
    if (!pendingPaired) restorePaired();
    if (result.mts) { try { result.mtsProfile = account_.profile(); } catch (...) {} }
    if (result.foghub) { try { result.foghubProfile = pairedServices_->account().profile(); } catch (...) {} }
    return result;
}

ApplicationController::ApplicationController(Runtime& runtime, std::unique_ptr<Services> services)
    : rt_(runtime), services_(std::move(services)), auth_(services_->auth()), account_(services_->account()),
      catalogRepository_(services_->catalog()), rentals_(services_->rentals()),
      guard_(rentals_, services_->sessionJournal(storagePaths::path(storagePaths::PendingSession))),
      checkout_(rentals_, guard_, services_->checkoutJournal(storagePaths::path(storagePaths::PendingOrder))),
      ui_(runtime.renderer, runtime.font, runtime.titleFont) {
    pairedServices_ = providerServices(fog::ProviderId::FogHub);
    if (pairedServices_ && pairedServices_->pairing() && pairedServices_->pins())
        paired_ = std::make_unique<fog::PairedCheckoutGuard>(pairedServices_->rentals(),*pairedServices_->pairing(),
            *pairedServices_->pins(),pairedServices_->pairedJournal(storagePaths::path(storagePaths::PendingPairedRental)));
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
    catch (...) {
        controllerSaveStatus_ = fog::controller::text::LoadFailed;
        diagnostic("Controller settings unreadable; using Xbox profile and layout defaults.");
    }
    try { rumbleSettings_ = fog::rumble::loadSettings(storagePaths::path(storagePaths::RumbleSettings)); }
    catch (...) {
        rumbleSaveStatus_ = fog::rumble::text::LoadFailed;
        diagnostic("Rumble settings unreadable or invalid; using balanced mode.");
    }
    const auto rumbleNowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    rumble_.setMode(rumbleSettings_.mode, rumbleNowMs);
    overlay_.setRumbleSettings(rumbleSettings_);
    overlay_.setRumbleStatus(rumbleSaveStatus_);
    ui_.setXboxLayout(controllerSettings_.xbox);
    overlay_.setVideoSettings(videoSettings_);
}

ApplicationController::~ApplicationController() {
    stopRumble();
    cancelPlayStation();
    if (playStationPending_.valid()) playStationPending_.wait();
    latency_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    // Last attempt precedes permanent cancellation and networking teardown.
    shutdownSession();
    services_->cancel();
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
    start(Task::RestoreAuth,[this] { return restoreProviders(); });
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
        const auto rawDown = padGetButtonsDown(&pad);
        const auto rawHeld = padGetButtons(&pad);
        observeStreamMenuInput(focused,rawHeld,rawDown);
        const auto down = focused ? rawDown : 0;
        auto uiDown = fog::controller::mapUiButtons(down,controllerSettings_,faceMasks);
        const auto held = focused ? rawHeld : 0;
        if (quit || ((down & HidNpadButton_Plus) && !sessionPending_)) {
            if (!sessionPending_) break;
            exitAfterStop_ = true; stopAfterTask_ = true;
        }
        eventsScope.finish();
        if (!focused || screen_ != Screen::Streaming || stopAfterTask_) {
            stopRumble();
            loopTiming_.suspendPresentation();
        }
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
            if (sessionPending_ && !focused) ratingFocusLost_ = true;
            if (sessionPending_ && !stopAfterTask_ && !focused) {
                diagnostic("Stop trigger: lost application focus; state=" + std::to_string(static_cast<int>(appletGetFocusState())));
                stopAfterTask_ = true; if (paired_) paired_->requestStop(); if (!pending_.valid()) beginFinish("focus-lost");
            }
            if (stopAfterTask_ && paired_) paired_->requestStop();
            pollRecentSave();
            poll();
            pollCatalog();
            pollPlayStation();
            // The shutdown owner retries unresolved stops and provides an explicit escape.
            if (exitAfterStop_ && !pending_.valid()) break;
            if (screen_ == Screen::Checkout && !pending_.valid() && (pairedPending_ ? pairedServices_->auth().authenticated() : auth_.authenticated()) &&
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
            updateRumble(pad, focused);
            if (screen_ == Screen::Games && focused && (!sidebar_ || catalog_.items().empty())) requestCatalog();
            if (screen_ == Screen::Stopping && !pending_.valid() && !pairedPending_ && auth_.authenticated() &&
                std::chrono::steady_clock::now() >= retryAt_) beginFinish();
        }
        catch (const std::exception& error) {
            stopRumble();
            status_ = safeText(error.what()); diagnostic(status_);
            if (screen_ == Screen::Checkout && !pending_.valid()) checkoutStatus_ = status_;
            if (sessionPending_ && (stream_ || !streamRental_.empty() || transportReconnect_.recovering())) beginFinish("stream-exception");
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
    services_->cancel();
    if (catalogApi_) catalogApi_->cancel();
    diagnostic("Exit requested; cancelling pending request.");
}

void ApplicationController::start(Task kind, std::function<TaskResult()> work) {
    if (pending_.valid()) return;
    task_ = kind;
    status_ = uiText::Profile::ConnectingService;
    pending_ = std::async(std::launch::async, std::move(work));
}

void ApplicationController::poll() {
    if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto kind = task_;
    TaskResult data;
    try { data = pending_.get(); }
    catch (...) {
        if (kind == Task::SessionRatingLoad || kind == Task::SessionRatingSubmit) {
            if (kind == Task::SessionRatingLoad) rating_.loadFailed(fog::rating::text::LoadFailed);
            else rating_.submitFailed(fog::rating::text::SubmitFailed);
            status_.clear();
            diagnostic("FH rating request failed; form and response details omitted.");
            return;
        }
        if (kind == Task::RestoreAuth) {
            screen_ = Screen::Consent; loginReturn_ = Screen::Games;
            status_ = ui_.controllerHints(uiText::Profile::RestoreFailedHint);
            diagnostic("Saved authentication restore failed; credentials omitted.");
            return;
        }
        if (kind == Task::PairedCreate || kind == Task::PairedPoll || kind == Task::PairedStop) {
            syncPending(); screen_ = pairedPending_ ? Screen::Recovery : Screen::PlayStation;
            checkoutAutoPoll_ = false;
            if (!sessionPending_) { disarmExitHandling(); stopAfterTask_ = false; }
            if (pairedPending_ && stopAfterTask_ && kind != Task::PairedStop) beginFinish("paired-worker-failed");
            throw;
        }
        if ((kind == Task::Phone || kind == Task::Profile) &&
            loginAuth().capabilities().method == fog::LoginMethod::Credentials) {
            screen_ = Screen::Profile;
            diagnostic("Login failed: provider=FH stage=request; details omitted.");
            throw;
        }
        if (kind == Task::ProviderProfile || kind == Task::ProviderForget) { screen_ = Screen::Profile; throw; }
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
        auto restored = std::get<RestoredProviders>(std::move(data));
        mtsProfile_ = std::move(restored.mtsProfile); foghubProfile_ = std::move(restored.foghubProfile);
        syncPending();
        authSaveFailed_ = false;
        if (pairedPending_) {
            loginProvider_ = fog::ProviderId::FogHub;
            screen_ = Screen::Recovery;
            status_ = "FogHub: войдите в исходный аккаунт аренды. A — проверить, Y — профиль.";
            if (restored.foghub) handlePairedCheckout();
        } else if (sessionPending_) {
            loginProvider_ = fog::ProviderId::Mts;
            if (restored.mts) recoverSession();
            else { screen_ = Screen::Recovery; status_ = ui_.controllerHints(uiText::Profile::PreviousRentalSignInHint); }
        } else if (!restored.mts && (restored.mtsFailed || !auth_.storageError().empty())) {
            loginProvider_ = fog::ProviderId::Mts; loginReturn_ = Screen::Games; screen_ = Screen::Consent;
            status_ = ui_.controllerHints(restored.mtsFailed ? uiText::Profile::RestoreFailedHint : uiText::Profile::ValidationFailedHint);
        } else {
            loadGames();
            if (restored.foghubFailed) status_ = "FogHub: сохранённый вход не подтверждён. Откройте профиль (Y).";
        }
        return;
    }
    if (kind == Task::ProviderForget) {
        if (loginProvider_ == fog::ProviderId::FogHub) { foghubProfile_.reset(); rating_ = {}; }
        else mtsProfile_.reset();
        screen_ = Screen::Profile; status_ = uiText::Profile::SignedOut; return;
    }
    if (kind == Task::ProviderProfile) {
        const auto profile = std::get<fog::UserProfile>(std::move(data));
        if (loginProvider_ == fog::ProviderId::FogHub) foghubProfile_ = profile; else mtsProfile_ = profile;
        screen_ = Screen::Profile; status_.clear(); return;
    }
    if (kind == Task::SessionRatingLoad) {
        completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(std::move(data))); return;
    }
    if (kind == Task::SessionRatingSubmit) {
        completeSessionRatingSubmit(std::get<fog::SessionRatingResult>(data)); return;
    }
    if (kind == Task::PairedCreate && std::holds_alternative<VerifiedOffer>(data)) {
        syncPending();
        if (sessionPending_ || stopAfterTask_ || exitAfterStop_ || appletGetFocusState() != AppletFocusState_InFocus) {
            if (sessionPending_) beginFinish("paired-preflight-journal");
            else { disarmExitHandling(); stopAfterTask_ = false; screen_ = Screen::PlayStation; status_ = uiText::Server::ConnectionCancelled; }
            return;
        }
        const auto order = std::get<VerifiedOffer>(data).request;
        sessionPending_ = true; pairedPending_ = true;
        checkoutStatus_ = "FogHub: подготовка и автоматическое сопряжение…";
        diagnostic("Paired checkout request: provider=FH action=create");
        start(Task::PairedCreate,[this,order] { paired_->create(order); return std::monostate{}; });
        return;
    }
    if (kind == Task::PairedCreate && std::holds_alternative<ChangedOffer>(data)) {
        syncPending();
        if (sessionPending_) { beginFinish("paired-offer-change"); return; }
        disarmExitHandling(); stopAfterTask_ = false;
        auto fresh = std::move(std::get<ChangedOffer>(data).computers);
        playStationComputers_.erase(std::remove_if(playStationComputers_.begin(),playStationComputers_.end(),
            [](const fog::Computer& pc) { return pc.id.provider == fog::ProviderId::FogHub; }),playStationComputers_.end());
        playStationComputers_.insert(playStationComputers_.end(),fresh.begin(),fresh.end());
        playStationLoadedAt_ = std::chrono::steady_clock::now(); screen_ = Screen::PlayStation; selection_ = 0;
        status_ = uiText::Server::TariffChanged; return;
    }
    if (kind == Task::PairedCreate || kind == Task::PairedPoll || kind == Task::PairedStop) {
        handlePairedCheckout(); return;
    }
    if (kind == Task::Create && std::holds_alternative<VerifiedOffer>(data)) {
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
        const auto order = std::get<VerifiedOffer>(data).request;
        if (!selectedPlayStation_ || !auth_.authenticated() ||
            !fog::isPlayStationComputerId(order.computerId) || order.offerId.empty()) {
            disarmExitHandling();
            screen_ = Screen::PlayStation;
            status_ = uiText::Server::ConnectionNotConfirmed;
            return;
        }
        sessionPending_ = true;
        checkoutStatus_ = uiText::Server::CreatingOrder;
        start(Task::Create,[this,order] { checkout_.create(order); return std::monostate{}; });
        return;
    }
    if (kind == Task::Create && std::holds_alternative<ChangedOffer>(data)) {
        syncPending();
        if (sessionPending_) { beginFinish("playstation-offer-change"); return; }
        disarmExitHandling(); stopAfterTask_ = false;
        playStationComputers_ = std::move(std::get<ChangedOffer>(data).computers);
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
    if (kind == Task::History) { history_ = std::move(std::get<std::vector<fog::HistoryEntry>>(data)); status_.clear(); return; }
    if (kind == Task::GameDetail) {
        auto result = std::get<fog::GameDetail>(std::move(data));
        detail_ = std::move(result.game); autoComputers_ = std::move(result.recommendations);
        status_.clear(); return;
    }
    if (kind == Task::Finish) {
        if (std::get<fog::FinishResult>(data).terminated && std::get<fog::FinishResult>(data).journalCleared) {
            sessionPending_ = false; stopAfterTask_ = false; screen_ = Screen::Sessions; sessions_.clear(); selection_ = 0;
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
        if (std::get<RecoveryResult>(data).matched) { armExitHandling(); beginFinish(); }
        else { screen_ = Screen::Recovery; status_ = uiText::Server::PreviousRentalNotMatched; }
        return;
    }
    if (kind == Task::Sessions) {
        sessions_ = std::move(std::get<std::vector<fog::Rental>>(data)); screen_ = Screen::Sessions; selection_ = 0;
        status_.clear();
        diagnostic("Active sessions loaded; identifiers omitted.");
        return;
    }
    if (kind == Task::Computers) {
        computers_ = std::move(std::get<fog::Page<fog::Computer>>(data).items); selection_ = 0;
        status_.clear();
        diagnostic("Computers OK, count=" + std::to_string(computers_.size()));
    } else if (kind == Task::Profile) {
        const auto profile = std::get<fog::UserProfile>(data);
        if (loginProvider_ == fog::ProviderId::FogHub) foghubProfile_ = profile; else mtsProfile_ = profile;
        screen_ = loginReturn_;
        status_ = loginAuth().storageError().empty() ? uiText::Profile::AuthSaved :
            uiText::Profile::AuthSaveFailed;
        diagnostic("Authenticated profile OK; personal fields not logged.");
        if (pairedPending_) {
            start(Task::PairedStop,[this] { paired_->bindAuthenticatedAccount(); paired_->stop(); return std::monostate{}; });
        } else if (sessionPending_) recoverSession();
        else if (screen_ == Screen::History) loadHistory();
        else if (screen_ == Screen::PlayStation) loadPlayStation();
        else if (screen_ == Screen::Games && catalog_.items().empty()) loadGames();
    } else {
        const auto& authResult = std::get<fog::AuthResult>(data);
        authStep_ = authResult.step;
        screen_ = Screen::Auth;
        if (authStep_ == fog::AuthStep::Success) {
            authSaveFailed_ = !loginAuth().storageError().empty();
            if (captcha_) { SDL_DestroyTexture(captcha_); captcha_ = nullptr; }
            start(Task::Profile, [this] { return loginAccount().profile(); });
        } else if (authStep_ == fog::AuthStep::Captcha) {
            setCaptcha(authResult);
            status_.clear();
        } else if (authStep_ == fog::AuthStep::Otp) {
            status_.clear();
        } else {
            if (loginAuth().capabilities().method == fog::LoginMethod::Credentials) {
                screen_ = Screen::Profile;
                status_ = ui_.controllerHints("FogHub: вход не завершён. A — повторить вход.");
            } else status_ = ui_.controllerHints(uiText::Profile::SignInIncompleteHint);
            diagnostic("Auth stopped at unsupported/restart response; body omitted.");
        }
    }
}

Application::Application(platform::Runtime& runtime, std::unique_ptr<Services> services)
    : controller_(std::make_unique<ApplicationController>(runtime, std::move(services))) {}
Application::~Application() = default;
void Application::run() { controller_->run(); }

}
