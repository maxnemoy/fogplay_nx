#include "app/application_controller.hpp"

namespace fogplay {

void ApplicationController::armExitHandling() {
    checkResult(appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend), "Background stop support");
    if (!exitLocked_) { checkResult(appletLockExit(), "Session exit guard"); exitLocked_ = true; }
    checkResult(appletSetAutoSleepDisabled(true), "Session autosleep guard");
}

void ApplicationController::disarmExitHandling() {
    if (exitLocked_) { appletUnlockExit(); exitLocked_ = false; }
    appletSetAutoSleepDisabled(false);
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
}

void ApplicationController::shutdownSession() noexcept {
    if (shutdownDone_) return;
    shutdownDone_ = true;
    diagnostic("Application shutdown: checking owned rental cleanup.");
    // Dispatch the owned rental stop before transport shutdown can wait for an
    // in-flight control send. Its worker uses checkout/guard, not the transport.
    if (sessionPending_ && !pending_.valid()) {
        try { beginFinish("application-shutdown"); }
        catch (...) { diagnostic("Shutdown stop dispatch failed; recovery records retained."); }
    }
    closeStream();
    // Join any owner of checkout/guard before inspecting them. Do not permanently
    // cancel API requests yet: an unresolved order can still need its hash polled.
    if (!sessionPending_ && task_ != Task::Create) api_.cancel();
    if (pending_.valid()) {
        try { pending_.get(); } catch (...) {}
    }
    syncPending();
    PadState shutdownPad;
    padInitializeDefault(&shutdownPad);
    bool exitChordReleased = false;
    std::chrono::steady_clock::time_point exitChordAt{};
    bool manualExit = false;
    bool first = true;
    auto retry = std::chrono::steady_clock::now();
    while (sessionPending_) {
        appletMainLoop(); // OS exit grants only a bounded grace period, not immunity.
        if (std::chrono::steady_clock::now() >= retry) {
            if (first) {
                first = false;
                try { armExitHandling(); }
                catch (...) { diagnostic("Cannot re-arm exit guard; still attempting rental stop."); }
            }
            try {
                if (checkout_.unresolved()) checkout_.stop();
                else if (api_.loggedIn()) {
                    if (!guard_.ownsSession()) guard_.recoverMatchingSession();
                    if (guard_.ownsSession()) guard_.finishAttempt();
                }
                syncPending();
            } catch (...) { diagnostic("Shutdown stop attempt failed; recovery records retained."); }
            if (!sessionPending_) break;
            diagnostic("SESSION STOP UNCONFIRMED: retaining network and recovery records; retry pending.");
            retry = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
        }
        if (appletGetFocusState() == AppletFocusState_InFocus) {
            padUpdate(&shutdownPad);
            const auto buttons = padGetButtons(&shutdownPad);
            const auto chord = HidNpadButton_Plus | HidNpadButton_Minus;
            if ((buttons & chord) != chord) { exitChordReleased = true; exitChordAt = {}; }
            else if (exitChordReleased) {
                if (exitChordAt == std::chrono::steady_clock::time_point{}) exitChordAt = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - exitChordAt >= std::chrono::seconds(3)) { manualExit = true; break; }
            }
            try {
                SDL_SetRenderDrawColor(rt_.renderer, 12, 16, 28, 255);
                SDL_RenderClear(rt_.renderer);
                ui_.text("Завершение аренды не подтверждено", 40, 240, 1200);
                ui_.text("Повторяем остановку. Проверьте сессию на fogplay.mts.ru с телефона.", 40, 290, 1200);
                ui_.text("Сначала завершите аренду на сайте. Затем удерживайте − и + 3 секунды для выхода.", 40, 340, 1200);
                ui_.text("Этот выход не подтверждает остановку: журнал восстановления останется на SD.", 40, 390, 1200);
                SDL_RenderPresent(rt_.renderer);
            } catch (...) {}
        }
        svcSleepThread(100000000);
    }
    diagnostic(manualExit ? "Shutdown: explicit manual exit with unresolved rental; recovery records retained." :
        "Shutdown: no unresolved rental remains.");
    api_.cancel();
    disarmExitHandling();
}

void ApplicationController::syncPending() {
    sessionPending_ = checkout_.unresolved() || guard_.persistent() ||
        !guard_.pendingHash().empty() || !guard_.journalError().empty();
}

void ApplicationController::logCheckoutResult(Task phase) {
    // Only called after get() joins the worker. Log presence/counters, never identifiers.
    const auto& result = checkout_.result();
    diagnostic("Checkout phase=" + std::to_string(static_cast<int>(phase)) +
        " state=" + std::to_string(static_cast<int>(result.state)) +
        " serverStatus=" + std::to_string(result.serverStatus) +
        " orderPresent=" + std::to_string(result.orderId > 0) +
        " hashPresent=" + std::to_string(!result.hash.empty()) +
        " serverHashPresent=" + std::to_string(result.serverHashPresent) +
        " stopRequested=" + std::to_string(result.stopRequested) +
        " stopQueued=" + std::to_string(stopAfterTask_) +
        " pollCount=" + std::to_string(result.pollCount) +
        " readyObservations=" + std::to_string(result.readyObservations));
    diagnostic("Checkout detail: " + result.message);
}

void ApplicationController::queueCheckoutStop() {
    diagnostic("Stop trigger: user pressed Back during checkout.");
    stopAfterTask_ = true;
    checkoutStatus_ = "Завершение запрошено. Ждём текущий ответ сервера, затем остановим аренду.";
    status_ = checkoutStatus_;
    if (!pending_.valid()) beginFinish();
}

void ApplicationController::handleCheckout() {
    syncPending();
    const auto& result = checkout_.result();
    checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    checkoutAutoPoll_ = fog::checkoutNeedsAutomaticCheck(result);
    if (result.stopRequested) checkoutAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
    if (result.state == fog::CheckoutState::Ready && !stopAfterTask_ && !result.stopRequested) {
        startStream(result.hash); return;
    }
    if (!sessionPending_) {
        disarmExitHandling(); stopAfterTask_ = false;
        screen_ = Screen::Games;
        status_ = result.state == fog::CheckoutState::Rejected ? "Сервер отклонил заказ. Обновите список ПК." : "Аренда завершена.";
        return;
    }
    screen_ = Screen::Checkout;
    if (result.stopRequested || stopAfterTask_) checkoutStatus_ = "Ожидаем данные для остановки. Заказ пока не отменён.";
    else if (result.state == fog::CheckoutState::RecoveryRequired) checkoutStatus_ = ui_.controllerHints("Заказ требует проверки. A — проверить, B — остановить, Y — вход.");
    else if (result.pollCount >= 30) checkoutStatus_ = ui_.controllerHints("Автопроверка приостановлена. A — проверить ещё раз, B — остановить аренду.");
    else if (result.readyObservations > 0) checkoutStatus_ = "Проверяем готовность сессии (" + std::to_string(result.readyObservations) + "/3)…";
    else if (result.serverStatus == 2 && !result.serverHashPresent) checkoutStatus_ = "Оплата подтверждена. Ожидаем адрес сессии…";
    else if (result.serverStatus == 1) checkoutStatus_ = result.serverHashPresent ? "Сервер готовится к подключению…" : "Ожидаем адрес сессии…";
    else if (result.serverStatus == 0) checkoutStatus_ = "Заказ создан. Ожидаем сервер…";
    else if (result.state == fog::CheckoutState::PaymentRequired) checkoutStatus_ = "Сервис запросил оплату. Проверьте заказ на fogplay.mts.ru.";
    else checkoutStatus_ = "Заказ записан. Ожидаем готовность сервера…";
    status_ = checkoutStatus_;
    if (!result.stopRequested && (stopAfterTask_ ||
        (result.state == fog::CheckoutState::RecoveryRequired && !result.hash.empty()))) {
        diagnostic("Stop trigger: checkout requires recovery after session identification.");
        beginFinish();
    }
}

void ApplicationController::pollCheckout(bool manual) {
    if (pending_.valid()) return;
    start(Task::CheckoutPoll, [this, manual] { checkout_.poll(manual); return Json::object(); });
}

void ApplicationController::createSession() {
    if (sessionPending_ || selection_ >= computers_.size()) return;
    if (!api_.loggedIn()) { loginReturn_ = Screen::Details; screen_ = Screen::Consent; return; }
    // Verify the same SD replacement path before any billable request.
    checkStorage();
    const auto& pc = computers_[selection_];
    fog::CreateOrderRequest order;
    order.computerId = std::stoi(field(pc,"id","0"));
    order.tariffId = std::stoi(field(pc,"tariff_id","0"));
    order.gameId = selectedGameId_; order.ownSteamAccount = ownSteam_;
    recentLaunch_.prepare(selectedRecentGame_);
    // Lock focus/exit before the worker can send a billable create request.
    armExitHandling(); sessionPending_ = true; stopAfterTask_ = false;
    screen_ = Screen::Checkout;
    checkoutStatus_ = "Создаём заказ. Ожидаем ответ Fog Play…";
    start(Task::Create, [this, order] { checkout_.create(order); return Json::object(); });
}

void ApplicationController::loadSessions() {
    if (!api_.loggedIn()) { screen_ = Screen::Consent; return; }
    if (sessionPending_) { recoverSession(); return; }
    screen_ = Screen::Sessions; selection_ = 0; sessions_ = Json::array();
    start(Task::Sessions, [this] { return api_.activeSessions(); });
}

void ApplicationController::recoverSession() {
    if (pending_.valid()) return;
    screen_ = Screen::Recovery;
    if (checkout_.unresolved()) { armExitHandling(); stopAfterTask_ = true; beginFinish(); return; }
    start(Task::Recovery, [this] { return Json{{"matched", guard_.recoverMatchingSession()}}; });
}

void ApplicationController::connectSession() {
    if (selection_ >= sessions_.size() || sessionPending_) return;
    const auto hash = sessions_[selection_].at("hash").get<std::string>();
    // Durable ownership must succeed before any signaling / media operation.
    try { guard_.adopt(hash); }
    catch (...) {
        sessionPending_ = !guard_.pendingHash().empty() || guard_.persistent() || !guard_.journalError().empty();
        if (sessionPending_) {
            screen_ = Screen::Recovery;
            if (guard_.ownsSession()) beginFinish();
        }
        throw;
    }
    startStream(hash,false);
}

void ApplicationController::beginFinish(const char* reason) {
    if (pending_.valid() || !sessionPending_) return;
    diagnostic("Rental stop begin; reason=" + std::string(reason) + " screen=" + std::to_string(static_cast<int>(screen_)));
    stopAfterTask_ = true;
    // The server stop must not wait for a synchronous RTC send already in flight.
    // Join the transport only after the independent rental owner has started.
    try {
        if (checkout_.unresolved()) {
            screen_ = Screen::Checkout;
            start(Task::CheckoutStop,[this] { checkout_.stop(); return Json::object(); });
            checkoutStatus_ = status_ = "Запрашиваем завершение аренды…";
        } else {
            screen_ = Screen::Stopping;
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(exitAfterStop_ ? 1 : 10);
            start(Task::Finish, [this] {
                const auto result = guard_.finishAttempt();
                return Json{{"terminated",result.terminated},{"cleared",result.journalCleared},{"message",result.message}};
            });
            status_ = "Завершаем аренду на сервере. Ждём подтверждение.";
        }
    } catch (...) {
        closeStream();
        throw;
    }
    closeStream();
}

}
