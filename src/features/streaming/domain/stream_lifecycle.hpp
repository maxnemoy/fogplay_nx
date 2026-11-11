#pragma once
#include "features/streaming/domain/stream_connection.hpp"
#include "features/settings/domain/video_settings.hpp"
#include <string>

namespace stream {
enum class RecoveryState { Connected, Disconnected, RetryableFailure, TerminalFailure };
// Application-owner transport lifecycle. Raw readiness/error/retry semantics
// remain provider-defined; this contract performs no rental operations.
class StreamLifecycle {
public:
    virtual ~StreamLifecycle() = default;
    virtual bool start(const StreamConnection& connection) = 0;
    // Preferences are retained across start(); codec changes require a new connection.
    virtual bool configureVideo(const fog::video::Settings& settings) = 0;
    // Pre-start transport preference, retained across start(). Relay never falls back to direct.
    virtual bool configureRelay(bool relay) = 0;
    virtual bool updateVideoSettings(const fog::video::Settings& settings) = 0;
    virtual void pump() = 0;
    virtual bool ready() const = 0;
    virtual std::string status() const = 0;
    virtual std::string error() const = 0;
    virtual bool retryableError() const = 0; // Fixed transport failures/timeouts only; persists after teardown.
    virtual bool disconnected() const = 0; // Live peer or ICE Disconnected; application applies its grace period.
    // A transient disconnection can recover on the current transport during
    // grace. Failed/terminated adapters override when callbacks precede error text.
    virtual RecoveryState recoveryState() const {
        if (!error().empty()) return retryableError() ? RecoveryState::RetryableFailure : RecoveryState::TerminalFailure;
        return disconnected() ? RecoveryState::Disconnected : RecoveryState::Connected;
    }
    virtual bool disconnectForRetry() = 0; // False if pending terminal evidence forbids retry; otherwise release transports.
    virtual void close() = 0;
};
}
