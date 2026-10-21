#pragma once
namespace fog::stream {
// UI transitions require release; video recovery resumes the next sampled state.
// Neither latch stores controller state or replays pre-recovery input.
class StreamInputGate {
public:
    void reset() { overlaySuppressed_ = videoSuspended_ = false; }
    void overlayTransition() { overlaySuppressed_ = true; }
    bool suppressOverlay(bool held, bool touching) {
        if (!overlaySuppressed_) return false;
        if (!held && !touching) overlaySuppressed_ = false;
        return true; // The release sample itself still belongs to the UI transition.
    }
    void suspendVideo() { videoSuspended_ = true; }
    void videoPrepared(bool waitingForKeyframe) {
        if (!waitingForKeyframe) videoSuspended_ = false;
    }
    bool videoSuspended() const { return videoSuspended_; }
    bool gamepadAllowed(bool overlayHandled) const {
        return !overlayHandled && !overlaySuppressed_ && !videoSuspended_;
    }
private:
    bool overlaySuppressed_ = false, videoSuspended_ = false;
};
}
