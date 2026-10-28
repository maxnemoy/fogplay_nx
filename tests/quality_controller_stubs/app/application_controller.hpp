#pragma once
#include "core/constants/ui_text.hpp"
#include "features/session/application/paired_checkout.hpp"
#include "features/settings/domain/video_settings.hpp"
#include "features/streaming/domain/transport_reconnect.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

// Narrow port for compiling the production settings controller without libnx,
// SDL, media workers or network. The real paired coordinator is tested separately.
namespace fogplay {
enum class Screen { Streaming, Recovery };
enum class Task { PairedPoll, Other };
using TaskResult = std::variant<std::monostate>;
inline void diagnostic(const std::string&) {}
struct TestStream {
    bool active = true, lost = false, retryable = false;
    std::string failure;
    std::optional<stream::RecoveryState> reportedState;
    int updates = 0;
    bool ready() const { return active; }
    bool disconnected() const { return lost; }
    std::string error() const { return failure; }
    bool retryableError() const { return retryable; }
    stream::RecoveryState recoveryState() const {
        if (reportedState) return *reportedState;
        if (!failure.empty()) return retryable ? stream::RecoveryState::RetryableFailure : stream::RecoveryState::TerminalFailure;
        return lost ? stream::RecoveryState::Disconnected : stream::RecoveryState::Connected;
    }
    bool updateVideoSettings(const fog::video::Settings&) { ++updates; return true; }
};
struct TestPaired {
    fog::PairedCheckoutResult value{fog::PairedCheckoutState::Ready,
        {fog::ProviderId::FogHub,"owned"},false,fog::PairedEndpoint{"synthetic.invalid",443,80},{}};
    int polls = 0, stopRequests = 0, resumeInspections = 0;
    const fog::PairedCheckoutResult& result() const { return value; }
    void poll() { ++polls; }
    void inspectForResume() { ++resumeInspections; }
    void requestStop() { ++stopRequests; }
};
struct TestOverlay {
    fog::video::Settings value;
    std::string saved;
    void setVideoSettings(const fog::video::Settings& settings) { value = settings; }
    void setSaveStatus(const std::string& status) { saved = status; }
};
struct TestInputGate { int suspensions = 0; void suspendVideo() { ++suspensions; } };
class ApplicationController {
public:
    void changeVideoSettings(const fog::video::Settings&,bool);
    bool requestPairedVideoReconnect();
    bool completePairedVideoReconnect();
    void requestPairedTransportReconnect();
    bool completePairedTransportReconnect();
    bool transportRecoveryAllowed() const;
    void stopTransportRecovery(const char*);
    void retryTransport(const char*);
    bool handleTransportState(std::chrono::steady_clock::time_point);
    bool advanceTransportRecovery(std::chrono::steady_clock::time_point);
    void requestTransportRecovery();
    void completeTransportRecovery(fog::stream::TransportReconnect::Preparation);
    void recordTransportBreak() { if (streamRental_.provider == fog::ProviderId::Mts) ++relayBreaks; }
    void suspendVideoInput() { streamInputGate_.suspendVideo(); neutralizeStreamInput(); }
    void start(Task task,std::function<TaskResult()> work) {
        ++dispatches; task_ = task;
        if (dispatchThrows) throw std::runtime_error("synthetic dispatch failure");
        pending_ = std::async(std::launch::deferred,std::move(work));
    }
    bool closeTransport(bool final) {
        ++disconnects; finalDisconnect = final;
        if (!disconnectAllowed) return false;
        stream_.reset(); return true;
    }
    bool startTransport(bool resumeOnly = false) {
        ++starts; resumedOnly = resumeOnly; startedRental = streamRental_; startedSettings = videoSettings_;
        if (startThrows) throw std::runtime_error("synthetic start failure");
        if (startAllowed || startRetryable) {
            stream_ = std::make_unique<TestStream>(); stream_->active = false;
            if (startRetryable) { stream_->failure = "synthetic network failure"; stream_->retryable = true; }
        }
        return startAllowed;
    }
    void closeStream() {
        ++streamCloses; pairedVideoReconnectPending_ = pairedTransportReconnectPending_ = false;
        transportReconnect_.cancel(); stream_.reset();
    }
    void beginFinish(const char*) {
        if (pending_.valid() || !sessionPending_) return;
        ++finishes; stopAfterTask_ = true;
        pairedVideoReconnectPending_ = pairedTransportReconnectPending_ = false;
        transportReconnect_.cancel();
    }
    void neutralizeStreamInput() { ++neutralizations; }
    void joinInspection() { pending_.get(); }
    Screen screen_ = Screen::Streaming;
    Task task_ = Task::Other;
    bool sessionPending_ = true, pairedPending_ = true, pairedVideoReconnectPending_ = false;
    bool pairedTransportReconnectPending_ = false;
    bool stopAfterTask_ = false, exitAfterStop_ = false, shutdownDone_ = false;
    bool dispatchThrows = false, disconnectAllowed = true, startAllowed = true, startThrows = false;
    bool startRetryable = false;
    bool resumedOnly = false, finalDisconnect = true;
    int dispatches = 0, disconnects = 0, starts = 0, finishes = 0, streamCloses = 0, neutralizations = 0;
    int relayBreaks = 0;
    std::unique_ptr<TestPaired> paired_ = std::make_unique<TestPaired>();
    TestPaired* pairedServices_ = paired_.get();
    std::unique_ptr<TestStream> stream_ = std::make_unique<TestStream>();
    std::future<TaskResult> pending_;
    fog::RentalId streamRental_{fog::ProviderId::FogHub,"owned"}, startedRental;
    fog::video::Settings videoSettings_, startedSettings;
    TestOverlay overlay_;
    TestInputGate streamInputGate_;
    fog::stream::TransportReconnect transportReconnect_;
    std::string videoSaveStatus_, status_;
};
}
