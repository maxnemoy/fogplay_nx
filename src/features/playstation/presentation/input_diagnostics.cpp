#include "app/application_controller.hpp"
#include <iomanip>
#include <sstream>

namespace fogplay {
namespace {
std::uint64_t clockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<std::size_t N> std::string values(const std::array<float,N>& array) {
    std::ostringstream out; out << std::fixed << std::setprecision(4);
    for (std::size_t i=0;i<N;++i) { if(i) out << ','; out << array[i]; }
    return out.str();
}
}
void ApplicationController::samplePlayStationInput(PadState& pad) {
    if (!streamPlayStation_) return;
    const auto now=clockMs();
    const auto& sample=psSensors_.update(pad,now);
    const bool suppressed=overlay_.mode()!=fog::stream::OverlayMode::Closed || transportReconnect_.recovering() ||
        videoRecovery_.active() || videoLossWaiting_ || !stream_ || !stream_->ready();
    psTouchTrace_.observe(sample,now,suppressed);
    if (sample.motion.valid && sample.motion.fresh) {
        float peak=0;
        for (std::size_t i=0;i<3;++i) {
            peak=std::max(peak,std::abs(sample.motion.psGyroRadiansPerSecond[i]));
            psAccelPeak_=std::max(psAccelPeak_,std::abs(sample.motion.psAccelerationG[i]));
        }
        psGyroPeak_=std::max(psGyroPeak_,peak);
        if (peak>0.05f) ++psMotionMovedFrames_;
    }
    if (now-psLogAtMs_>=1000) logPlayStationInput(false);
}
void ApplicationController::dispatchPlayStationInput(bool allowed) {
    if (!streamPlayStation_) return;
    const bool wasInitialized=psNativeInput_.contactsInitialized();
    psNativeInput_.update(psSensors_.snapshot(),clockMs(),allowed,[this](const fog::remote::Input& command) {
        return stream_ && stream_->sendInput(command);
    });
    if (!wasInitialized && psNativeInput_.contactsInitialized())
        diagnostic("PS touch reset: both contacts UP queued before native input; scope=transport ack=unavailable");
}
void ApplicationController::releasePlayStationInput() {
    if (!streamPlayStation_) return;
    psNativeInput_.suspend([this](const fog::remote::Input& command) { return stream_ && stream_->sendInput(command); });
}
void ApplicationController::logPlayStationInput(bool final) {
    if (!streamPlayStation_) return;
    psLogAtMs_=clockMs();
    const auto& s=psSensors_.snapshot(); const auto& c=s.counters; const auto& m=s.motion;
    const std::string prefix=final ? "PS input final: " : "PS input: ";
    diagnostic(prefix+"scope=transport ack=unavailable enabled="+std::to_string(s.enabled)+
        " touchValid="+std::to_string(s.touchValid)+" contacts="+std::to_string(s.touchCount)+
        " hidContacts="+std::to_string(s.hidTouchCount)+" sample="+std::to_string(s.touchSampleNumber)+
        " down="+std::to_string(psTouchTrace_.down)+" up="+std::to_string(psTouchTrace_.up)+
        " moves="+std::to_string(psTouchTrace_.moves)+" swipes="+std::to_string(psTouchTrace_.swipes)+
        " swipeThresholdScreenPx=60 lastFinger="+std::to_string(psTouchTrace_.lastFinger)+
        " lastPS5="+std::to_string(psTouchTrace_.lastX)+","+std::to_string(psTouchTrace_.lastY)+
        " lastGestureScreenDelta="+std::to_string(psTouchTrace_.lastDx)+","+std::to_string(psTouchTrace_.lastDy)+
        " lastGestureMs="+std::to_string(psTouchTrace_.lastDurationMs)+
        " suppressedFrames="+std::to_string(psTouchTrace_.suppressedFrames)+
        " touchStaleReleases="+std::to_string(c.touchStaleReleases)+
        " gamepadQueued="+std::to_string(psGamepadAccepted_)+" gamepadRejected="+std::to_string(psGamepadRejected_),2048);
    diagnostic("PS sensors: npad="+std::to_string(s.npadId)+" style="+std::to_string(s.style)+
        " handles="+std::to_string(s.handleCount)+" bindCount="+std::to_string(c.handleRefreshes)+
        " getResult="+std::to_string(s.lastGetHandlesResult)+" startResult="+std::to_string(s.lastStartResult)+
        " stopResult="+std::to_string(s.lastStopResult)+" startFailures="+std::to_string(c.startFailures)+
        " stopFailures="+std::to_string(c.stopFailures)+" valid="+std::to_string(m.valid)+
        " fresh="+std::to_string(m.fresh)+" source="+std::to_string(m.sourceIndex)+
        " gyroPeakRad="+std::to_string(psGyroPeak_)+" accelPeakG="+std::to_string(psAccelPeak_)+
        " motionMovedFrames="+std::to_string(psMotionMovedFrames_)+" sample="+std::to_string(m.sampleNumber)+
        " samples="+std::to_string(c.motionSamples)+" missing="+std::to_string(c.motionMissing)+
        " invalid="+std::to_string(c.motionInvalid)+" stale="+std::to_string(c.motionStaleReleases)+
        " gyroTurnsPerSecond="+values(m.gyroTurnsPerSecond)+" accelerationG="+values(m.accelerationG)+
        " psGyroRadiansPerSecond="+values(m.psGyroRadiansPerSecond)+" psAccelerationG="+values(m.psAccelerationG),2048);
    const auto& n=psNativeInput_.counters();
    diagnostic("PS native queued: down="+std::to_string(n.down)+" move="+std::to_string(n.move)+
        " up="+std::to_string(n.up)+" accel="+std::to_string(n.accel)+" gyro="+std::to_string(n.gyro)+
        " rejected="+std::to_string(n.rejected)+" taps="+std::to_string(n.taps)+
        " suppressed="+std::to_string(n.suppressed)+
        " resetUpQueued="+std::to_string(n.resetUpQueued)+" resetRejected="+std::to_string(n.resetRejected)+
        " contactsInitialized="+std::to_string(psNativeInput_.contactsInitialized())+" ack=unavailable");
    if(stream_) diagnostic("PS transport input: "+stream_->inputDiagnostics(),3072);
}
}
