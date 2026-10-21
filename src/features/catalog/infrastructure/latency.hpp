#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fog {
// Estimated TCP connect time (excluding DNS/TLS/HTTP) to a shared
// MTS node + the PC-to-node measurement published by MTS. It is not ICMP or
// the final WebRTC path latency; unknown/unreachable routes remain unknown.
class LatencyService {
public:
    using Cancelled = std::function<bool()>;
    using EndpointLoader = std::function<std::vector<std::string>(const Cancelled&)>;
    using Probe = std::function<std::optional<int>(const std::string&, const Cancelled&)>;
    explicit LatencyService(EndpointLoader loader = {}, Probe probe = {});
    ~LatencyService();
    LatencyService(const LatencyService&) = delete;
    LatencyService& operator=(const LatencyService&) = delete;
    // Safe on the render thread. Reuses successful node samples for 60 seconds.
    // Transfers have 5-second attempt limits; the Switch synchronous DNS backend
    // uses the platform resolver deadline and cannot be interrupted by curl.
    // stop() must join workers before socketExit; cancel() does not block the UI.
    void request();
    void cancel() noexcept;
    void stop() noexcept;
    bool busy() const;
    std::optional<int> estimate(const nlohmann::json& computer) const;
    static bool validEndpoint(const std::string& endpoint);
    static std::optional<int> connectionMilliseconds(double lookupSeconds, double connectSeconds);
    static std::string routeKey(const std::string& endpoint);
private:
    struct Sample { int milliseconds; std::uint64_t at; };
    void run();
    EndpointLoader loader_;
    Probe probe_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::unordered_map<std::string,Sample> samples_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic_bool stopped_{false};
    bool pending_ = false, busy_ = false;
};
}
