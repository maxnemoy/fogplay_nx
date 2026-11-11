#include "core/diagnostics/latency_histogram.hpp"
#include "core/diagnostics/thread_observation.hpp"
#include "core/constants/storage_paths.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"
#include "app/application.hpp"
#include "app/stream_telemetry_logging.hpp"
#include "features/session/presentation/session_rating_text.hpp"
#include "features/settings/presentation/rumble_text.hpp"
#include "features/settings/presentation/controller_text.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {
namespace {
std::optional<fog::RentalSessionCoordinator::PairedDependencies> pairedDependencies(Services& services) {
    auto* provider = services.provider(fog::ProviderId::FogHub);
    if (!provider || !provider->pairing() || !provider->pins()) return std::nullopt;
    return fog::RentalSessionCoordinator::PairedDependencies{provider->rentals(), *provider->pairing(), *provider->pins(),
        provider->pairedJournal(storagePaths::path(storagePaths::PendingPairedRental))};
}
}


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
    screen_ = Screen::Profile; sidebar_ = false; selection_ = rentalSession_.state().pairedPending ? 1 : 0;
    loginProvider_ = rentalSession_.state().pairedPending ? fog::ProviderId::FogHub : fog::ProviderId::Mts;
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

RestoredProviders ApplicationController::restoreProviders(fog::RentalSessionCoordinator::AuthScope scope) {
    // Serialized worker only. Optional profile/balance traffic must never delay
    // recovery of a potentially billable rental belonging to either provider.
    RestoredProviders result;
    const bool pendingPaired = scope == fog::RentalSessionCoordinator::AuthScope::FogHub;
    const bool pendingMts = scope == fog::RentalSessionCoordinator::AuthScope::Mts;
    const auto restoreMts = [this,&result] {
        try {
            result.mts = auth_.restore().restored;
        } catch (...) { result.mtsFailed = true; }
    };
    const auto restorePaired = [this,&result] {
        if (!pairedServices_) return;
        try {
            result.foghub = pairedServices_->auth().restore().restored;
        } catch (...) { result.foghubFailed = true; }
    };
    // The main thread owns the stop-attempt budget. Restore only the provider
    // needed for recovery now; unrelated login/profile traffic can wait.
    if (pendingPaired) { restorePaired(); return result; }
    if (pendingMts) { restoreMts(); return result; }
    restoreMts();
    restorePaired();
    if (result.mts) { try { result.mtsProfile = account_.profile(); } catch (...) {} }
    if (result.foghub) { try { result.foghubProfile = pairedServices_->account().profile(); } catch (...) {} }
    return result;
}

void ApplicationController::startProviderRestore() {
    const auto scope = rentalSession_.recoveryAuthScope();
    start(Task::RestoreAuth,[this,scope] { return restoreProviders(scope); });
}

ApplicationController::ApplicationController(Runtime& runtime, std::unique_ptr<Services> services)
    : rt_(runtime), services_(std::move(services)), auth_(services_->auth()), account_(services_->account()),
      catalogRepository_(services_->catalog()), rentals_(services_->rentals()),
      rentalSession_({rentals_, services_->sessionJournal(storagePaths::path(storagePaths::PendingSession)),
          services_->checkoutJournal(storagePaths::path(storagePaths::PendingOrder))}, pairedDependencies(*services_)),
      playerMediaFactory_(runtime.renderer,[](const fog::media::telemetry::StallEvent& value) {
          diagnostics::diagnosticMediaStall(value);
      }), player_(*this,playerMediaFactory_,*this), ui_(runtime.renderer, runtime.font, runtime.titleFont) {
    player_.setDiagnosticsObserver(this);
    pairedServices_ = providerServices(fog::ProviderId::FogHub);
    if (!recentGames_.load()) diagnostic("Recent games load unavailable; history will remain optional.");
    try {
        videoSettings_ = fog::video::loadSettings(storagePaths::path(storagePaths::VideoSettings));
    } catch (...) {
        videoSettings_ = {};
        videoSaveStatus_ = uiText::Settings::VideoLoadFailed;
        diagnostic("Video settings unreadable or invalid; using defaults.");
    }
    if (videoSettings_.codec == ::stream::VideoCodec::H265 && !playerMediaFactory_.supportsH265()) {
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
    latency_.cancel();
    if (catalogApi_) catalogApi_->cancel();
    // Cancel every worker before joining it or tearing down networking.
    shutdownSession();
    services_->cancel();
    player_.shutdownAfterCancellation();
    if (playStationPending_.valid()) playStationPending_.wait();
    if (pending_.valid()) pending_.wait();
    if (catalogPending_.valid()) catalogPending_.wait();
    latency_.stop();
    flushSettingsSaves();
    if (recentSave_.valid()) recentSave_.wait();
    if (recentSavePending_) recentGames_.save();
    if (captcha_) SDL_DestroyTexture(captcha_);
}

void ApplicationController::run() {
    fog::diagnostics::observeThread("application");
    hidInitializeTouchScreen();
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    screen_ = rentalSession_.state().pending ? Screen::Recovery : Screen::Games;
    startProviderRestore();
    status_ = uiText::Profile::ValidatingSavedAuth;
    for (;;) {
        fog::stream::LoopTiming::Scope loopScope(loopTiming_, fog::stream::LoopTiming::Stage::Loop,
            player_.snapshot().transport || player_.snapshot().recovering);
        fog::stream::LoopTiming::Scope eventsScope(loopTiming_, fog::stream::LoopTiming::Stage::Events);
        const bool systemRunning = appletMainLoop();
        if (!systemRunning) {
            diagnostic("Stop trigger: system exit request; cancelling work without recovery retries.");
            exitAfterStop_ = true;
            rentalSession_.request();
            break;
        }
        const bool focused = appletGetFocusState() == AppletFocusState_InFocus;
        SDL_Event event;
        bool quit = false;
        while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) quit = true;
        padSampledAtUs_ = fog::metrics::steadyNowUs();
        padUpdate(&pad);
        const auto rawDown = padGetButtonsDown(&pad);
        const auto rawHeld = padGetButtons(&pad);
        observeStreamMenuInput(focused,rawHeld,rawDown);
        const auto down = focused ? rawDown : 0;
        auto uiDown = fog::controller::mapUiButtons(down,controllerSettings_,faceMasks);
        const auto held = focused ? rawHeld : 0;
        if (quit || ((down & HidNpadButton_Plus) && (!rentalSession_.state().pending || rentalSession_.paused()))) {
            exitAfterStop_ = true; rentalSession_.request();
            break;
        }
        eventsScope.finish();
        if (!focused || screen_ != Screen::Streaming || rentalSession_.requested()) {
            stopRumble();
            loopTiming_.suspendPresentation();
        }
        try {
            fog::stream::LoopTiming::Scope pollScope(loopTiming_, fog::stream::LoopTiming::Stage::Poll);
            // Accept stop intent before consuming a ready worker result. The worker may
            // still own checkout_/guard_, so queueing only changes main-thread state.
            if (screen_ == Screen::Checkout && (uiDown & HidNpadButton_B)) {
                queueCheckoutStop();
                uiDown &= ~HidNpadButton_B;
            }
            // Queue focus loss before consuming a ready checkout, so a background
            // completion cannot briefly open RTC/media instead of stopping the rental.
            if (rentalSession_.state().pending && !focused) ratingFocusLost_ = true;
            if (rentalSession_.state().pending && !rentalSession_.paused() && !rentalSession_.requested() && !focused) {
                diagnostic("Stop trigger: lost application focus; state=" + std::to_string(static_cast<int>(appletGetFocusState())));
                rentalSession_.request(); if (!pending_.valid()) beginFinish("focus-lost");
            }
            // Preserve repeated delivery of the guard's atomic stop signal
            // while an already admitted provider operation is still running.
            if (rentalSession_.requested()) rentalSession_.request();
            pollRecentSave();
            pollSettingsSaves();
            pollTransportTeardown();
            poll();
            pollCatalog();
            pollPlayStation();
            if (rentalSession_.pauseIfExhausted(pending_.valid(), std::chrono::steady_clock::now()))
                pauseSessionRecovery();
            if (screen_ == Screen::Checkout && !pending_.valid() && (rentalSession_.state().pairedPending ? pairedServices_->auth().authenticated() : auth_.authenticated()) &&
                checkoutAutoPoll_ && (rentalSession_.retryScheduled() ? rentalSession_.retryDue(std::chrono::steady_clock::now()) :
                    std::chrono::steady_clock::now() >= checkoutAt_)) {
                if (rentalSession_.requested()) beginFinish(); else pollCheckout(false);
            }
            pollScope.finish();
            if ((player_.snapshot().transport || player_.snapshot().recovering || player_.snapshot().teardown || player_.snapshot().qualityClosing) && focused && !rentalSession_.requested()) {
                fog::stream::LoopTiming::Scope pumpScope(loopTiming_, fog::stream::LoopTiming::Stage::Pump);
                pumpStream(pad, held, down);
            }
            fog::stream::LoopTiming::Scope inputScope(loopTiming_, fog::stream::LoopTiming::Stage::Input);
            if (!pending_.valid() && focused &&
                (!player_.snapshot().teardown || screen_ == Screen::Streaming || screen_ == Screen::StopConfirm)) input(uiDown);
            updateRumble(pad, focused);
            if (screen_ == Screen::Games && focused && (!sidebar_ || catalog_.items().empty())) requestCatalog();
            if (screen_ == Screen::Stopping && !rentalSession_.paused() && !pending_.valid() && !rentalSession_.state().pairedPending && auth_.authenticated() &&
                rentalSession_.retryDue(std::chrono::steady_clock::now())) beginFinish();
        }
        catch (const std::exception& error) {
            stopRumble();
            status_ = safeText(error.what()); diagnostic(status_);
            if (screen_ == Screen::Checkout && !pending_.valid()) checkoutStatus_ = status_;
            if (rentalSession_.state().pending && (player_.snapshot().ownsRental)) beginFinish("stream-exception");
        }
        if (exitAfterStop_ && !rentalSession_.state().pending && !pending_.valid() && !player_.snapshot().teardown) break;
        try {
            fog::stream::LoopTiming::Scope drawScope(loopTiming_, fog::stream::LoopTiming::Stage::Draw);
            if (focused) draw(); else svcSleepThread(16000000);
        }
        catch (const std::exception& error) {
            diagnostic(safeText(error.what()));
            if (rentalSession_.state().pending && !rentalSession_.requested() && !pending_.valid()) beginFinish("render-exception");
        }
    }
    cancelPlayStation();
    shutdownSession();
    services_->cancel();
    if (catalogApi_) catalogApi_->cancel();
    diagnostic("Exit requested; cancelling pending request.");
}

bool ApplicationController::start(Task kind, std::function<TaskResult()> work) {
    const bool offer = kind == Task::VerifyMtsOffer || kind == Task::VerifyPairedOffer;
    if (pending_.valid()) {
        if (offer && rentalSession_.cancelLaunch()) disarmExitHandling();
        return false;
    }
    task_ = kind;
    status_ = uiText::Profile::ConnectingService;
    try {
        pending_ = std::async(std::launch::async, [work = std::move(work)]() mutable {
            fog::diagnostics::observeThread("application-task");
            return work();
        });
    } catch (...) {
        if (offer && rentalSession_.cancelLaunch()) disarmExitHandling();
        throw;
    }
    return true;
}

void ApplicationController::poll() {
    if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    using Rental = fog::RentalSessionCoordinator;
    const auto kind = task_;
    TaskResult data;
    try { data = pending_.get(); }
    catch (...) {
        const auto error = std::current_exception();
        if (kind == Task::RentalOperation) {
            const auto effects = rentalSession_.completeJoined(error, rentalAdmission(), std::chrono::steady_clock::now());
            applyRentalEffects(effects);
            std::rethrow_exception(error);
        }
        rentalSession_.externalServiceTaskJoined();
        if (kind == Task::VerifyMtsOffer || kind == Task::VerifyPairedOffer) {
            rentalSession_.cancelLaunch(); checkoutAutoPoll_ = false;
            if (rentalSession_.state().durablePending) { screen_ = Screen::Recovery; if (rentalSession_.requested()) beginFinish("offer-worker-failed"); }
            else {
                disarmExitHandling();
                screen_ = kind == Task::VerifyMtsOffer ? (selectedPlayStation_ ? Screen::PlayStationDetails : Screen::Details) : Screen::PlayStation;
            }
            throw;
        }
        if (kind == Task::SessionRatingLoad || kind == Task::SessionRatingSubmit) {
            if (kind == Task::SessionRatingLoad) rating_.loadFailed(fog::rating::text::LoadFailed);
            else rating_.submitFailed(fog::rating::text::SubmitFailed);
            status_.clear(); diagnostic("FH rating request failed; form and response details omitted."); return;
        }
        if (kind == Task::RestoreAuth) {
            screen_ = Screen::Consent; loginReturn_ = Screen::Games;
            status_ = ui_.controllerHints(uiText::Profile::RestoreFailedHint);
            diagnostic("Saved authentication restore failed; credentials omitted."); return;
        }
        if ((kind == Task::Phone || kind == Task::Profile) && loginAuth().capabilities().method == fog::LoginMethod::Credentials) {
            screen_ = Screen::Profile; diagnostic("Login failed: provider=FH stage=request; details omitted."); throw;
        }
        if (kind == Task::ProviderProfile || kind == Task::ProviderForget) screen_ = Screen::Profile;
        throw;
    }
    if (kind == Task::RentalOperation) {
        const auto effects = rentalSession_.completeJoined(std::get<Rental::WorkerResult>(std::move(data)), rentalAdmission(), std::chrono::steady_clock::now());
        applyRentalEffects(effects);
        return;
    }
    // Authentication/profile/forget tasks share the provider resources. Refresh
    // account authority after EVERY shared join, before another task is admitted.
    rentalSession_.externalServiceTaskJoined();
    if (kind == Task::RestoreAuth) {
        auto restored = std::get<RestoredProviders>(std::move(data));
        mtsProfile_ = std::move(restored.mtsProfile); foghubProfile_ = std::move(restored.foghubProfile);
        authSaveFailed_ = false;
        if (rentalSession_.state().pairedPending) {
            loginProvider_ = fog::ProviderId::FogHub; screen_ = Screen::Recovery;
            if (restored.foghub) recoverSession();
            else { pauseSessionRecovery(); status_ = "FogHub: нужен исходный аккаунт аренды. B — меню; Y — профиль. Проверьте аренду на сайте."; }
        } else if (rentalSession_.state().pending) {
            loginProvider_ = fog::ProviderId::Mts;
            if (restored.mts) recoverSession();
            else { pauseSessionRecovery(); status_ = ui_.controllerHints(uiText::Profile::PreviousRentalSignInHint) + " B — меню."; }
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
    if (kind == Task::SessionRatingLoad) { completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(std::move(data))); return; }
    if (kind == Task::SessionRatingSubmit) { completeSessionRatingSubmit(std::get<fog::SessionRatingResult>(data)); return; }
    if ((kind == Task::VerifyMtsOffer || kind == Task::VerifyPairedOffer) && std::holds_alternative<VerifiedOffer>(data)) {
        if (rentalSession_.state().durablePending) { beginFinish("preflight-journal"); return; }
        if (rentalSession_.requested() || exitAfterStop_ || appletGetFocusState() != AppletFocusState_InFocus) {
            rentalSession_.cancelLaunch(); disarmExitHandling(); screen_ = Screen::PlayStation; status_ = uiText::Server::ConnectionCancelled; return;
        }
        const auto order = std::get<VerifiedOffer>(data).request;
        if (kind == Task::VerifyMtsOffer) {
            if (!selectedPlayStation_ || !auth_.authenticated() || !fog::isPlayStationComputerId(order.computerId) || order.offerId.empty()) {
                rentalSession_.cancelLaunch(); disarmExitHandling(); screen_ = Screen::PlayStation; status_ = uiText::Server::ConnectionNotConfirmed; return;
            }
            checkoutStatus_ = uiText::Server::CreatingOrder;
            startRental(Rental::MtsCreate{order});
        } else {
            checkoutStatus_ = "FogHub: подготовка и автоматическое сопряжение…";
            diagnostic("Paired checkout request: provider=FH action=create");
            startRental(Rental::PairedCreate{order});
        }
        return;
    }
    if ((kind == Task::VerifyMtsOffer || kind == Task::VerifyPairedOffer) && std::holds_alternative<ChangedOffer>(data)) {
        if (rentalSession_.state().durablePending) { beginFinish("offer-change"); return; }
        rentalSession_.cancelLaunch(); disarmExitHandling();
        auto fresh = std::move(std::get<ChangedOffer>(data).computers);
        if (kind == Task::VerifyPairedOffer) {
            playStationComputers_.erase(std::remove_if(playStationComputers_.begin(),playStationComputers_.end(),
                [](const fog::Computer& pc) { return pc.id.provider == fog::ProviderId::FogHub; }),playStationComputers_.end());
            playStationComputers_.insert(playStationComputers_.end(),fresh.begin(),fresh.end());
        } else playStationComputers_ = std::move(fresh);
        playStationLoadedAt_ = std::chrono::steady_clock::now(); screen_ = Screen::PlayStation; selection_ = 0;
        status_ = uiText::Server::TariffChanged; return;
    }
    if (rentalSession_.requested() && rentalSession_.state().pending && !rentalSession_.paused() &&
        kind != Task::Profile && kind != Task::Phone && kind != Task::Captcha && kind != Task::Otp) { beginFinish(); return; }
    if (kind == Task::History) { history_ = std::move(std::get<std::vector<fog::HistoryEntry>>(data)); status_.clear(); return; }
    if (kind == Task::GameDetail) {
        auto result = std::get<fog::GameDetail>(std::move(data)); detail_ = std::move(result.game); autoComputers_ = std::move(result.recommendations); status_.clear(); return;
    }
    if (kind == Task::Computers) { computers_ = std::move(std::get<fog::Page<fog::Computer>>(data).items); selection_ = 0; status_.clear(); return; }
    if (kind == Task::Profile) {
        const auto profile = std::get<fog::UserProfile>(data);
        if (loginProvider_ == fog::ProviderId::FogHub) foghubProfile_ = profile; else mtsProfile_ = profile;
        screen_ = loginReturn_;
        status_ = loginAuth().storageError().empty() ? uiText::Profile::AuthSaved : uiText::Profile::AuthSaveFailed;
        diagnostic("Authenticated profile OK; personal fields not logged.");
        if (rentalSession_.state().pending) { if (rentalSession_.paused()) pauseSessionRecovery(); else recoverSession(); }
        else if (screen_ == Screen::History) loadHistory();
        else if (screen_ == Screen::PlayStation) loadPlayStation();
        else if (screen_ == Screen::Games && catalog_.items().empty()) loadGames();
        return;
    }
    const auto& authResult = std::get<fog::AuthResult>(data);
    authStep_ = authResult.step; screen_ = Screen::Auth;
    if (authStep_ == fog::AuthStep::Success) {
        authSaveFailed_ = !loginAuth().storageError().empty();
        if (captcha_) { SDL_DestroyTexture(captcha_); captcha_ = nullptr; }
        start(Task::Profile, [this] { return loginAccount().profile(); });
    } else if (authStep_ == fog::AuthStep::Captcha) { setCaptcha(authResult); status_.clear(); }
    else if (authStep_ == fog::AuthStep::Otp) status_.clear();
    else {
        if (loginAuth().capabilities().method == fog::LoginMethod::Credentials) { screen_ = Screen::Profile; status_ = ui_.controllerHints("FogHub: вход не завершён. A — повторить вход."); }
        else status_ = ui_.controllerHints(uiText::Profile::SignInIncompleteHint);
        diagnostic("Auth stopped at unsupported/restart response; body omitted.");
    }
}

Application::Application(platform::Runtime& runtime, std::unique_ptr<Services> services)
    : controller_(std::make_unique<ApplicationController>(runtime, std::move(services))) {}
Application::~Application() = default;
void Application::run() { controller_->run(); }

}
