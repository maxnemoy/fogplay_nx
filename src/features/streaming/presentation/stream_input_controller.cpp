#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {

SDL_Rect ApplicationController::videoRect() const {
    if (!media_ || media_->videoWidth() <= 0 || media_->videoHeight() <= 0) return {0,0,1280,720};
    const double scale = std::min(1280.0/media_->videoWidth(),720.0/media_->videoHeight());
    const int width = static_cast<int>(media_->videoWidth()*scale);
    const int height = static_cast<int>(media_->videoHeight()*scale);
    return {(1280-width)/2,(720-height)/2,width,height};
}

bool ApplicationController::sendRemote(const Json& command) {
    return !transportReconnect_.recovering() && stream_ && stream_->ready() && stream_->sendControl(command.dump());
}

void ApplicationController::releaseMouse() noexcept {
    try {
        if (stream_) {
            if (mouseLeft_) stream_->sendControl(fog::remote::mouseButton(false,0).dump());
            if (mouseRight_) stream_->sendControl(fog::remote::mouseButton(false,2).dump());
        }
    } catch (...) { /* Best effort before the transport itself is closed. */ }
    mouseLeft_ = mouseRight_ = touchDrag_ = false;
}

void ApplicationController::neutralizeStreamInput() {
    releasePlayStationInput();
    psTouchLastSubmitted_ = false;
    releaseMouse();
    if (stream_) stream_->sendGamepad(stream::Gamepad{});
    touchWasDown_ = false;
    mouseAt_ = overlayHintAt_ = std::chrono::steady_clock::now();
}

void ApplicationController::suspendVideoInput() {
    neutralizeStreamInput();
    streamInputGate_.suspendVideo();
}

void ApplicationController::overlayTransition() {
    neutralizeStreamInput();
    streamInputGate_.overlayTransition();
}

void ApplicationController::overlayAction(const fog::stream::OverlayAction& action) {
    using Action = fog::stream::OverlayActionType;
    if (action.type == Action::None) return;
    if (action.type == Action::StopSession) { beginFinish("stream-menu"); return; }
    if (action.type == Action::SettingsChanged) {
        changeVideoSettings(overlay_.videoSettings(), true); return;
    }
    if (action.type == Action::RequestKeyframe) {
        const bool sent = stream_ && stream_->requestVideoKeyframe();
        overlayNotice_ = sent ? uiText::Stream::KeyframeRequested : uiText::Stream::KeyframeRequestFailed;
        overlayNoticeAt_ = std::chrono::steady_clock::now();
        return;
    }
    if (action.type == Action::SendText) {
        // Text is transient and never included in diagnostics or saved files.
        const bool sent = sendRemote(fog::remote::unicodeText(action.text));
        overlayNotice_ = sent ? uiText::Stream::TextSent : uiText::Stream::TextSendFailed;
        overlayNoticeAt_ = std::chrono::steady_clock::now();
        return;
    }
    const char* code = action.type == Action::Enter ? "Enter" : action.type == Action::Tab ? "Tab" :
                       action.type == Action::Backspace ? "Backspace" : "Escape";
    const bool pressed = sendRemote(fog::remote::key(true,code));
    // Always attempt key-up, even if the channel changed while sending.
    const bool released = sendRemote(fog::remote::key(false,code));
    overlayNotice_ = pressed && released ? uiText::Stream::KeySent : uiText::Stream::ControlUnavailable;
    overlayNoticeAt_ = std::chrono::steady_clock::now();
}

bool ApplicationController::handleStreamOverlay(PadState& pad, u64 held, u64 down) {
    held = fog::controller::mapUiButtons(held,controllerSettings_,faceMasks);
    down = fog::controller::mapUiButtons(down,controllerSettings_,faceMasks);
    using Mode = fog::stream::OverlayMode;
    HidTouchScreenState touch{};
    hidGetTouchScreenStates(&touch,1);
    const bool touching = touch.count > 0;
    if ((held & HidNpadButton_Plus) && (held & HidNpadButton_Minus) &&
        (down & (HidNpadButton_Plus | HidNpadButton_Minus))) {
        overlay_.openMenu(); overlayTransition(); return true;
    }
    if (streamInputGate_.suppressOverlay(held != 0,touching)) {
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
    if (streamInputGate_.videoSuspended() || videoRecovery_.active() || videoLossWaiting_ || !stream_ || !stream_->ready() || !media_ || !media_->videoFrames()) return;
    const auto rect = videoRect();
    const auto stick = padGetStickPos(&pad,0);
    pointerX_ += axis(stick.x)*900.0f*elapsed;
    pointerY_ -= axis(stick.y)*900.0f*elapsed;
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
    const bool buttonsChanged = left != mouseLeft_ || right != mouseRight_;
    if (buttonsChanged || now-lastPointerSent_ >= std::chrono::milliseconds(30)) {
        sendRemote(fog::remote::absoluteMouse(static_cast<int>(pointerX_)-rect.x,static_cast<int>(pointerY_)-rect.y,
            rect.w,rect.h,media_->videoWidth(),media_->videoHeight()));
        lastPointerSent_ = now;
    }
    if (left != mouseLeft_) { sendRemote(fog::remote::mouseButton(left,0)); mouseLeft_ = left; }
    if (right != mouseRight_) { sendRemote(fog::remote::mouseButton(right,2)); mouseRight_ = right; }
    if (down & HidNpadButton_L) sendRemote(fog::remote::wheel(-32));
    if (down & HidNpadButton_R) sendRemote(fog::remote::wheel(32));
}

float ApplicationController::axis(int value) {
    const float normalized = std::clamp(value / 32767.0f,-1.0f,1.0f);
    return std::abs(normalized) < 0.12f ? 0.0f : normalized;
}

}
