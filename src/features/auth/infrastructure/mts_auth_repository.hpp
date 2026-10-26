#pragma once
#include "features/auth/domain/auth_repository.hpp"
#include "features/session/infrastructure/api.hpp"
#include "features/session/infrastructure/mts_operation.hpp"
namespace fog {
class MtsAuthRepository final : public AuthRepository {
public:
    explicit MtsAuthRepository(Api& api) : api_(api) {}
    bool authenticated() const noexcept override { return api_.loggedIn(); }
    AuthData auth() const override {
        return {{ProviderId::Mts,api_.accountIdentity()}, api_.loggedIn() ? AuthState::Authenticated : AuthState::SignedOut, api_.authGeneration()};
    }
    AuthCapabilities capabilities() const noexcept override { return {LoginMethod::PhoneCode}; }
    void validateLogin(const LoginRequest& request) const override {
        mtsOperation([&]{ normalizePhone(request.identifier); });
    }
    AuthResult begin(const LoginRequest& request) override { return mtsOperation([&]{ return map(api_.submitPhone(normalizePhone(request.identifier))); }); }
    AuthResult answer(const AuthAnswer& answer) override {
        const auto step = answer.step;
        const auto& value = answer.value;
        if (step == AuthStep::Captcha) return mtsOperation([&]{ return map(api_.submitCaptcha(value)); });
        if (step == AuthStep::Otp) return mtsOperation([&]{ return map(api_.submitOtp(value)); });
        throw std::invalid_argument("Unsupported authentication answer");
    }
    RestoreAuthResult restore() override { return mtsOperation([&]{ return RestoreAuthResult{api_.restoreAuth()}; }); }
    void forget() override {
        try { api_.forgetAuth(); }
        catch (const std::runtime_error& error) { throw RepositoryError(RepositoryErrorKind::StorageFailure,error.what()); }
    }
    std::string storageError() const override { return api_.authStorageError(); }
private:
    AuthResult map(const Json& response) {
        AuthResult result{authStep(response),auth(),{}};
        if (result.step == AuthStep::Captcha) {
            if (!response.contains("image") || !response["image"].is_string())
                throw std::runtime_error("Captcha image missing.");
            result.captchaImage = decodeCaptcha(response["image"].get<std::string>());
        }
        return result;
    }
    Api& api_;
};
}
