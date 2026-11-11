#include "app/application_controller.hpp"
#include "core/constants/stage1_options.hpp"
#include "core/diagnostics/diagnostic_profile.hpp"
#include "app/app_diagnostics.hpp"
#include "features/playstation/application/input_telemetry.hpp"

namespace fogplay {
namespace {
std::uint64_t clockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
void ApplicationController::samplePlayStationInput(PadState& pad) {
    if (!streamPlayStation_) return;
    const auto now=clockMs();
    const auto& sample=psSensors_.update(pad,now);
    const bool suppressed=overlay_.mode()!=fog::stream::OverlayMode::Closed || player_.snapshot().recovering ||
        player_.snapshot().videoRecovering || player_.snapshot().lossWaiting || !player_.inputSink() || !player_.snapshot().ready ||
        (fog::stage1::inputFreezeGate && player_.snapshot().suspension.suspended());
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
}
void ApplicationController::dispatchPlayStationInput(bool allowed) {
    if (!streamPlayStation_) return;
    const bool wasInitialized=psNativeInput_.contactsInitialized();
    psNativeInput_.update(psSensors_.snapshot(),clockMs(),allowed,[this](const fog::remote::Input& command) {
        return player_.inputSink() && player_.inputSink()->sendInput(command);
    });
    if (!wasInitialized && psNativeInput_.contactsInitialized())
        diagnostic("PS touch reset: both contacts UP queued before native input; scope=transport ack=unavailable");
}
void ApplicationController::releasePlayStationInput() {
    if (!streamPlayStation_) return;
    psNativeInput_.suspend([this](const fog::remote::Input& command) { return player_.inputSink() && player_.inputSink()->sendInput(command); });
}
void ApplicationController::logPlayStationInput(bool final) {
    if (!streamPlayStation_) return;
    const auto now=clockMs();
    if (!final && (now-psLogAtMs_<1000 || !player_.periodicDiagnosticsHaveRoom())) return;
    psLogAtMs_=now;
    if (!final && !fog::metrics::collects(fog::metrics::DiagnosticClass::PeriodicDetail)) return;
    fog::playstation::telemetry::Snapshot value;
    value.sensor=psSensors_.snapshot();value.touch=psTouchTrace_;
    const auto& n=psNativeInput_.counters();
    value.native={n.down,n.move,n.up,n.accel,n.gyro,n.rejected,n.taps,n.suppressed,n.resetUpQueued,n.resetRejected};
    value.elapsedMs=diagnostics::diagnosticElapsedMs();value.final=final;
    value.gamepadAccepted=psGamepadAccepted_;value.gamepadRejected=psGamepadRejected_;
    value.motionMovedFrames=psMotionMovedFrames_;value.gyroPeak=psGyroPeak_;value.accelPeak=psAccelPeak_;
    value.contactsInitialized=psNativeInput_.contactsInitialized();
    if (const auto* source=player_.telemetrySource(); player_.inputSink() && source) {
        value.hasTransport=true;value.transport=source->captureProtocolTelemetry();
    }
    if(final) {emitPendingFinalPsTelemetry();pendingFinalPsTelemetry_=value;}
    else fog::playstation::telemetry::enqueue(runLog,value);
}
}
