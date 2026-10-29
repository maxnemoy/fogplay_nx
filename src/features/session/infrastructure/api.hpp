#pragma once
#include "core/network/http.hpp"
#include "features/session/infrastructure/mts_auth_protocol.hpp"
#include "features/session/domain/service_models.hpp"
#include <atomic>
#include <functional>
#include <string>

namespace fog {
struct CreateOrderRequest {
    int computerId = 0, tariffId = 0, gameId = 0;
    bool ownSteamAccount = false;
};
class Api {
public:
    using Transport = std::function<HttpResponse(const std::string&, const std::string&,
        const Query&, const std::string&, const std::string&)>;
    using OptionsTransport = std::function<HttpResponse(const std::string&, const std::string&,
        const Query&, const std::string&, const std::string&, RequestOptions)>;
    explicit Api(std::string deviceId);
    Api(std::string deviceId, Transport transport, OptionsTransport optionsTransport = {});
    ~Api();
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    Json games(std::string search, int page, int pageSize = 8, int genreId = 0);
    Json computers(int gameId, int page, int pageSize = 5, bool ownSteamAccount = false);
    Json computerAuto(int gameId, bool ownSteamAccount = false);
    Json gameDetail(int gameId);
    Json orderHistory();
    Json createOrder(const CreateOrderRequest& order);
    Json orderStatus(std::int64_t orderId);
    const std::string& accountIdentity() const noexcept { return accountIdentity_; }
    // Called by the UI only after the user accepts the service terms / SMS request.
    Json submitPhone(std::string phone);
    Json submitCaptcha(std::string code);
    Json submitOtp(std::string code);
    Json userInfo();
    Json activeSessions(bool shutdown = false);
    bool finishSession(const std::string& hash);
    bool loggedIn() const noexcept;
    std::uint64_t authGeneration() const noexcept { return authGeneration_.load(std::memory_order_acquire); }
    // Worker-only. Configure before login/restore; parent directory must exist.
    void configureAuthStorage(std::string path);
    bool restoreAuth();
    // Deletes durable credentials and clears RAM even if deletion fails.
    void forgetAuth();
    const std::string& authStorageError() const noexcept { return authStorageError_; }
    // RAM only; destruction and temporary logout never delete saved sign-in.
    void clearToken() noexcept;
    // May be called from the UI thread. Permanent cancellation for shutdown.
    void cancel() noexcept;
    // Final process teardown also cancels the independent rental-cleanup client.
    void cancelAll() noexcept;

private:
    Json authPost(const std::string& endpoint, const Json& body);
    HttpResponse request(const std::string& method, const std::string& path,
        const Query& query = {}, const std::string& body = "", const std::string& token = "",
        bool shutdown = false, bool creation = false);
    HttpClient http_;
    HttpClient shutdownHttp_;
    Transport transport_;
    OptionsTransport optionsTransport_;
    std::string deviceId_;
    std::string token_;
    std::string accountIdentity_;
    std::string authStoragePath_;
    std::string authStorageError_;
    std::atomic_bool loggedIn_{false};
    std::atomic_bool cancelledAll_{false};
    std::atomic<std::uint64_t> authGeneration_{0};
    AuthStep nextStep_ = AuthStep::Restart;
};
// Except loggedIn() / cancel(), use Api from one worker at a time.
}
