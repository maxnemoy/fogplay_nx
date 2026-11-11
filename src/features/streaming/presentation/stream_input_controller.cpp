#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"
#include "app/stream_telemetry_logging.hpp"
#include "features/streaming/domain/stick_deadzone.hpp"
#include "core/constants/stage1_options.hpp"
#include "features/streaming/domain/input_experiments.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {
namespace {
const char* menuModeName(fog::stream::OverlayMode mode) {
    using Mode = fog::stream::OverlayMode;
    switch (mode) {
        case Mode::Closed: return "closed";
        case Mode::Menu: return "menu";
        case Mode::Mouse: return "mouse";
        case Mode::Keyboard: return "keyboard";
        case Mode::ImageSettings: return "image-settings";
        case Mode::QualitySelect: return "quality-select";
        case Mode::RumbleSettings: return "rumble-settings";
        case Mode::StopConfirm: return "stop-confirm";
    }
    return "unknown";
}
}

void ApplicationController::traceStreamMenu(const char* event, u64 held, u64 down) {
    const auto elapsed = menuTransitionAt_ == std::chrono::steady_clock::time_point{} ? 0 :
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-menuTransitionAt_).count();
    constexpr u64 chord = HidNpadButton_Plus | HidNpadButton_Minus;
    diagnostic(std::string("Stream menu: event=") + event + " session=" + std::to_string(sessionSequence_) +
        " mode=" + menuModeName(overlay_.mode()) + " screen=" + std::to_string(static_cast<int>(screen_)) +
        " focus=" + std::to_string(menuFocused_) + " held=" + std::to_string(held & chord) +
        " down=" + std::to_string(down & chord) + " polls=" + std::to_string(menuPolls_) +
        " handlers=" + std::to_string(menuHandlers_) + " transitions=" + std::to_string(menuTransitions_) +
        " paints=" + std::to_string(menuPaints_) + " presents=" + std::to_string(menuPresents_) +
        " transition_age_ms=" + std::to_string(elapsed) + " stop=" + std::to_string(rentalSession_.requested()) +
        " worker=" + std::to_string(pending_.valid()) + " task=" + std::to_string(static_cast<int>(task_)) +
        " reconnect=" + std::to_string(player_.snapshot().recovering) +
        " video_recovery=" + std::to_string(player_.snapshot().videoRecovering) + " video_loss=" + std::to_string(player_.snapshot().lossWaiting) +
        " input_suspended=" + std::to_string(streamInputGate_.videoSuspended()) +
        " gate_gamepad_allowed=" + std::to_string(streamInputGate_.gamepadAllowed(false)) +
        " stream=" + std::to_string(static_cast<bool>(player_.inputSink())) + " media=" + std::to_string(static_cast<bool>(player_.mediaView())) +
        " decoded=" + std::to_string(player_.mediaView() ? player_.snapshot().progress.decoded : 0) +
        " uploaded=" + std::to_string(player_.mediaView() ? player_.snapshot().progress.uploaded : 0) +
        " video_presented=" + std::to_string(player_.mediaView() ? player_.snapshot().progress.presented : 0),2048);
}

void ApplicationController::observeStreamMenuInput(bool focused, u64 held, u64 down) {
    const bool active = rentalSession_.state().pending && (screen_ == Screen::Streaming || player_.inputSink() || player_.snapshot().recovering);
    if (!active && !menuObservationActive_) return;
    ++menuPolls_;
    constexpr u64 chord = HidNpadButton_Plus | HidNpadButton_Minus;
    const auto now = std::chrono::steady_clock::now();
    const bool focusChanged = focused != menuFocused_;
    menuFocused_ = focused;
    if (active != menuObservationActive_ || focusChanged || (down & chord) || (held & chord) != menuHeld_)
        traceStreamMenu(!active ? "inactive" : active != menuObservationActive_ ? "active" :
            focusChanged ? "focus-change" : "chord-input",held,down);
    else if (now-menuHeartbeatAt_ >= std::chrono::seconds(5))
        traceStreamMenu("heartbeat",held,down);
    if (active != menuObservationActive_ || focusChanged || (down & chord) || (held & chord) != menuHeld_ ||
        now-menuHeartbeatAt_ >= std::chrono::seconds(5)) menuHeartbeatAt_ = now;
    menuObservationActive_ = active;
    menuHeld_ = held & chord;
}

void ApplicationController::streamMenuPresented(bool videoDrawn) {
    if (screen_ != Screen::Streaming) return;
    ++menuPresents_;
    if (menuPresentedTransition_ == menuTransitions_) return;
    traceStreamMenu(videoDrawn ? "present-complete-video" : "present-complete-no-video");
    menuPresentedTransition_ = menuTransitions_;
}

SDL_Rect ApplicationController::videoRect() const {
    if (!player_.mediaView() || player_.snapshot().progress.width <= 0 || player_.snapshot().progress.height <= 0) return {0,0,1280,720};
    const double scale = std::min(1280.0/player_.snapshot().progress.width,720.0/player_.snapshot().progress.height);
    const int width = static_cast<int>(player_.snapshot().progress.width*scale);
    const int height = static_cast<int>(player_.snapshot().progress.height*scale);
    return {(1280-width)/2,(720-height)/2,width,height};
}

bool ApplicationController::sendRemote(const fog::remote::Input& command) {
    const auto* key = std::get_if<fog::remote::Key>(&command);
    if (key && !key->down) {
        // A video fault between key-down and key-up must never strand the key.
        // Keep a rejected release on this transport until it accepts the retry.
        return keyReleases_.release(key->code,[this](const std::string& code) {
            return player_.inputSink() && player_.snapshot().ready && !player_.snapshot().disconnected &&
                player_.snapshot().connected &&
                player_.inputSink()->sendInput(fog::remote::key(false,code));
        });
    }
    if (key && keyReleases_.pending(key->code)) {
        retryKeyReleases();
        if (keyReleases_.pending(key->code)) return false;
    }
    // Overlay keyboard/mouse actions share the same fresh safety checks as the
    // early gamepad phase. Releases bypass this gate in their retry helpers.
    const bool allowed = screen_ == Screen::Streaming && !rentalSession_.requested() && !exitAfterStop_ &&
        !streamInputGate_.videoSuspended() && player_.snapshot().inputAllowed;
    if (!allowed) return false;
    const bool accepted = player_.inputSink()->sendInput(command);
    if (accepted && key) keyReleases_.pressed(key->code);
    return accepted;
}

void ApplicationController::retryKeyReleases() noexcept {
    keyReleases_.retry([this](const std::string& code) {
        try {
            return player_.inputSink() && player_.snapshot().ready && !player_.snapshot().disconnected &&
                player_.snapshot().connected &&
                player_.inputSink()->sendInput(fog::remote::key(false,code));
        } catch (...) { return false; }
    });
}

void ApplicationController::releaseMouse() noexcept {
    mouseButtons_.releaseAll();
    retryMouseReleases();
    touchDrag_ = false;
}

void ApplicationController::retryMouseReleases() noexcept {
    mouseButtons_.retryReleases([this](bool down, int button) {
        try {
            // Releases must also work while input is suspended or a retry is
            // being scheduled. A failed enqueue remains pending on this stream.
            return player_.inputSink() && player_.snapshot().ready &&
                player_.inputSink()->sendInput(fog::remote::mouseButton(down,button));
        } catch (...) { return false; }
    });
}

void ApplicationController::neutralizeStreamInput() {
    stopRumble();
    retryKeyReleases();
    releasePlayStationInput();
    psTouchLastSubmitted_ = false;
    releaseMouse();
    gamepadSchedule_.suspend([this](const stream::Gamepad& value) {
        return player_.inputSink() && player_.inputSink()->sendGamepad(value);
    });
    touchWasDown_ = false;
    mouseAt_ = overlayHintAt_ = std::chrono::steady_clock::now();
}

void ApplicationController::retryGamepadRelease() noexcept {
    gamepadSchedule_.retryRelease([this](const stream::Gamepad& value) {
        try { return player_.inputSink() && player_.snapshot().ready && player_.inputSink()->sendGamepad(value); }
        catch (...) { return false; }
    });
}

void ApplicationController::stopRumble() noexcept {
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    rumble_.stop(nowMs);
}

void ApplicationController::updateRumble(PadState& pad, bool focused) {
    stream::Rumble feedback;
    const bool received = player_.inputSink() && player_.inputSink()->pollRumble(feedback);
    // Poll can wait for a callback's lock; sample time afterwards so newly
    // received feedback cannot appear to come from the future and be dropped.
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const bool enabled = focused && screen_ == Screen::Streaming && !rentalSession_.requested() &&
        player_.inputSink() && player_.snapshot().ready && !player_.snapshot().disconnected &&
        !player_.snapshot().recovering && !player_.snapshot().videoRecovering && !player_.snapshot().lossWaiting &&
        (!fog::stage1::inputFreezeGate || !player_.snapshot().suspension.suspended()) &&
        streamInputGate_.gamepadAllowed(overlay_.mode() != fog::stream::OverlayMode::Closed);
    rumble_.update(pad, received ? &feedback : nullptr, enabled, nowMs);
}

void ApplicationController::suspendVideoInput() {
    neutralizeStreamInput();
    streamInputGate_.suspendVideo();
}

void ApplicationController::overlayTransition() {
    ++menuTransitions_;
    menuTransitionAt_ = std::chrono::steady_clock::now();
    traceStreamMenu("transition-begin");
    neutralizeStreamInput();
    streamInputGate_.overlayTransition();
    traceStreamMenu("transition-ready");
}

void ApplicationController::overlayAction(const fog::stream::OverlayAction& action) {
    using Action = fog::stream::OverlayActionType;
    if (action.type == Action::None) return;
    if (action.type == Action::StopSession) {
        // An ownership inspection can still own the serialized worker. Queue
        // stop before its result can authorize a quality transport replacement.
        rentalSession_.request();
            beginFinish("stream-menu");
        return;
    }
    if (action.type == Action::SettingsChanged) {
        changeVideoSettings(overlay_.videoSettings(), true); return;
    }
    if (action.type == Action::RumbleSettingsChanged) {
        changeRumbleSettings(overlay_.rumbleSettings()); return;
    }
    if (action.type == Action::RequestKeyframe) {
        const bool sent = player_.inputSink() && player_.requestVideoKeyframe();
        overlayNotice_ = sent ? uiText::Stream::KeyframeRequested : uiText::Stream::KeyframeRequestFailed;
        overlayNoticeAt_ = std::chrono::steady_clock::now();
        return;
    }
    if (action.type == Action::SendText) {
        // Text is transient and never included in diagnostics or saved files.
        // Segments at repeated physical keys are paced by pumpStream(); a new
        // text queues behind any unsent remainder instead of replacing it.
        if (!typedText_.active()) typedTextPresses_ = remotePresses();
        typedText_.appendText(fog::remote::typingSegments(action.text), std::chrono::steady_clock::now());
        pumpTypedText();
        return;
    }
    const char* code = action.type == Action::Enter ? "Enter" : action.type == Action::Tab ? "Tab" :
                       action.type == Action::Backspace ? "Backspace" : "Escape";
    if (typedText_.active()) {
        // Enter/Tab/... must not overtake text that is still being typed.
        typedText_.appendKey(code, std::chrono::steady_clock::now());
        return;
    }
    const bool pressed = sendRemote(fog::remote::key(true,code));
    // Always attempt key-up, even if the channel changed while sending.
    const bool released = sendRemote(fog::remote::key(false,code));
    overlayNotice_ = pressed && released ? uiText::Stream::KeySent : uiText::Stream::ControlUnavailable;
    overlayNoticeAt_ = std::chrono::steady_clock::now();
}

std::uint64_t ApplicationController::remotePresses() const noexcept {
    return gamepadSchedule_.pressEdges() + mousePresses_;
}

void ApplicationController::pumpTypedText() noexcept {
    using Result = fog::stream::TypedText::Result;
    using Kind = fog::stream::TypedText::Kind;
    const auto now = std::chrono::steady_clock::now();
    if (typedText_.active() && remotePresses() != typedTextPresses_) {
        // A gamepad or mouse press already reached the PC and may have moved
        // its focus; never type the remainder into another field or form.
        typedText_.cancel();
        try { overlayNotice_ = uiText::Stream::TextSendCancelled; } catch (...) {}
        overlayNoticeAt_ = now;
        return;
    }
    const auto result = typedText_.pump(now, [this](const fog::stream::TypedText::Item& item) {
        try {
            if (item.kind == Kind::Text) return sendRemote(fog::remote::unicodeText(item.value));
            if (!sendRemote(fog::remote::key(true,item.value))) return false;
            sendRemote(fog::remote::key(false,item.value)); // A refused key-up stays pending in keyReleases_.
            return true;
        } catch (...) { return false; }
    });
    if (result != Result::Completed && result != Result::Failed) return;
    try { overlayNotice_ = result == Result::Completed ? uiText::Stream::TextSent : uiText::Stream::TextSendFailed; }
    catch (...) {}
    overlayNoticeAt_ = now;
}

bool ApplicationController::handleStreamOverlay(PadState& pad, u64 held, u64 down) {
    ++menuHandlers_;
    held = fog::controller::mapUiButtons(held,controllerSettings_,faceMasks);
    down = fog::controller::mapUiButtons(down,controllerSettings_,faceMasks);
    using Mode = fog::stream::OverlayMode;
    HidTouchScreenState touch{};
    hidGetTouchScreenStates(&touch,1);
    const bool touching = touch.count > 0;
    if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus) &&
        (down & (HidNpadButton_Plus | HidNpadButton_Minus))) {
        traceStreamMenu("chord-recognized",held,down);
        if (const auto* media=player_.mediaView()) diagnostics::diagnosticMenuImage(media->captureTelemetry());
        overlay_.openMenu(); overlayTransition(); return true;
    }
    if (streamInputGate_.suppressOverlay(held != 0,touching)) {
        if (menuSuppressedTransition_ != menuTransitions_) {
            traceStreamMenu("release-gate",held,down);
            menuSuppressedTransition_ = menuTransitions_;
        }
        // Avoid click/key/gamepad leakage from the action that opened/closed UI.
        touchWasDown_ = touching;
        mouseAt_ = std::chrono::steady_clock::now();
        return true;
    }
    const auto before = overlay_.mode();
    if (before == Mode::Closed) { touchWasDown_ = touching; return false; }
    if (down & HidNpadButton_B) {
        const auto action = overlay_.back(); overlayAction(action); overlayTransition(); return true;
    }
    if (before == Mode::Mouse) {
        updateMouse(pad,held,down,touch);
    } else {
        if (down & (HidNpadButton_Up|HidNpadButton_StickLUp)) overlay_.up();
        if (down & (HidNpadButton_Down|HidNpadButton_StickLDown)) overlay_.down();
        fog::stream::OverlayAction action;
        if (down & (HidNpadButton_Left|HidNpadButton_StickLLeft)) action = overlay_.left();
        else if (down & (HidNpadButton_Right|HidNpadButton_StickLRight)) action = overlay_.right();
        if (action.type == fog::stream::OverlayActionType::None && touching && !touchWasDown_)
            action = overlay_.touch(static_cast<int>(touch.touches[0].x),static_cast<int>(touch.touches[0].y));
        else if (action.type == fog::stream::OverlayActionType::None && (down & HidNpadButton_A)) action = overlay_.select();
        else if (before == Mode::Keyboard && (down & HidNpadButton_Y)) overlay_.eraseLast();
        overlayAction(action);
    }
    touchWasDown_ = touching;
    if (overlay_.mode() != before) overlayTransition();
    return true;
}

void ApplicationController::updateMouse(PadState& pad,u64 held,u64 down,const HidTouchScreenState& touch) {
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::clamp(std::chrono::duration<float>(now-mouseAt_).count(),0.0f,0.05f);
    mouseAt_ = now;
    if (streamInputGate_.videoSuspended() || player_.snapshot().videoRecovering || player_.snapshot().lossWaiting || !player_.inputSink() || !player_.snapshot().ready ||
        (fog::stage1::inputFreezeGate && player_.snapshot().suspension.suspended()) || !player_.mediaView() || !player_.snapshot().progress.decoded) return;
    const auto rect = videoRect();
    const auto stick = padGetStickPos(&pad,0);
    const auto axes = fog::stream::cursorStick(stick.x,stick.y);
    pointerX_ += axes.x*900.0f*elapsed;
    pointerY_ -= axes.y*900.0f*elapsed;
    if (touch.count > 0) {
        const int x = static_cast<int>(touch.touches[0].x), y = static_cast<int>(touch.touches[0].y);
        const bool inside = x >= rect.x && y >= rect.y && x < rect.x+rect.w && y < rect.y+rect.h;
        if (!touchWasDown_) touchDrag_ = inside;
        if (touchDrag_) { pointerX_ = static_cast<float>(x); pointerY_ = static_cast<float>(y); }
    } else touchDrag_ = false;
    pointerX_ = std::clamp(pointerX_,static_cast<float>(rect.x),static_cast<float>(rect.x+rect.w-1));
    pointerY_ = std::clamp(pointerY_,static_cast<float>(rect.y),static_cast<float>(rect.y+rect.h-1));
    const bool left = (held & (HidNpadButton_A|HidNpadButton_ZR)) || touchDrag_;
    const bool right = (held & HidNpadButton_X) != 0;
    const bool buttonsChanged = mouseButtons_.differs(left,right);
    if (buttonsChanged || fog::input_options::mousePerFrame || now-lastPointerSent_ >= std::chrono::milliseconds(30)) {
        sendRemote(fog::remote::absoluteMouse(static_cast<int>(pointerX_)-rect.x,static_cast<int>(pointerY_)-rect.y,
            rect.w,rect.h,player_.snapshot().progress.width,player_.snapshot().progress.height));
        lastPointerSent_ = now;
    }
    mouseButtons_.update(left,right,[this](bool pressed, int button) {
        const bool sent = sendRemote(fog::remote::mouseButton(pressed,button));
        if (sent && pressed) ++mousePresses_; // Stops pending overlay text (pumpTypedText).
        return sent;
    });
    if (down & HidNpadButton_L) sendRemote(fog::remote::wheel(-32));
    if (down & HidNpadButton_R) sendRemote(fog::remote::wheel(32));
}

}
