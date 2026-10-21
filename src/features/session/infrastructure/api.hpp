#pragma once
#include "core/network/http.hpp"
#include "features/session/domain/rental_service.hpp"
#include <atomic>
#include <functional>
#include <string>

namespace fog {
class Api : public RentalService {
public:
    using Transport = std::function<HttpResponse(const std::string&, const std::string&,
        const Query&, const std::string&, const std::string&)>;
    using OptionsTransport = std::function<HttpResponse(const std::string&, const std::string&,
        const Query&, const std::string&, const std::string&, RequestOptions)>;
    explicit Api(std::string deviceId);
    Api(std::string deviceId, Transport transport, OptionsTransport optionsTransport = {});
    ~Api() override;
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    Json games(std::string search, int page, int pageSize = 8, int genreId = 0);
    Json computers(int gameId, int page, int pageSize = 5, bool ownSteamAccount = false);
    Json computerAuto(int gameId, bool ownSteamAccount = false);
    Json gameDetail(int gameId);
    Json orderHistory();
    Json createOrder(const CreateOrderRequest& order) override;
    Json orderStatus(std::int64_t orderId) override;
    const std::string& accountIdentity() const noexcept override { return accountIdentity_; }
    // Called by the UI only after the user accepts the service terms / SMS request.
    Json submitPhone(std::string phone);
    Json submitCaptcha(std::string code);
    Json submitOtp(std::string code);
    Json userInfo();
    Json activeSessions(bool shutdown = false) override;
    bool finishSession(const std::string& hash) override;
    bool loggedIn() const noexcept override;
    std::uint64_t authGeneration() const noexcept override { return authGeneration_.load(std::memory_order_acquire); }
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
    std::atomic<std::uint64_t> authGeneration_{0};
    AuthStep nextStep_ = AuthStep::Restart;
};
// Except loggedIn() / cancel(), use Api from one worker at a time.
}
