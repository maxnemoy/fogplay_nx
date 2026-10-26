#pragma once
#include "core/domain/identity.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fog {
enum class AuthState { SignedOut, Authenticated };
struct AuthData {
    AccountId account;
    AuthState state = AuthState::SignedOut;
    std::uint64_t generation = 0;
    bool authenticated() const noexcept { return state == AuthState::Authenticated; }
};
enum class AuthStep { Captcha, Otp, Success, Restart, Unknown };
struct AuthResult {
    AuthStep step = AuthStep::Unknown;
    AuthData auth;
    std::vector<unsigned char> captchaImage;
};
struct RestoreAuthResult { bool restored = false; };
enum class LoginMethod { PhoneCode, Credentials };
struct AuthCapabilities { LoginMethod method = LoginMethod::PhoneCode; };
struct LoginRequest { std::string identifier, secret; };
struct AuthAnswer { AuthStep step; std::string value; };
class AuthRepository {
public:
    virtual ~AuthRepository() = default;
    // Full snapshots belong to the serialized repository owner; only authenticated() is read concurrently by UI.
    virtual AuthData auth() const = 0;
    virtual bool authenticated() const noexcept = 0;
    virtual AuthCapabilities capabilities() const noexcept = 0;
    // Local validation only: no state mutation or network request.
    virtual void validateLogin(const LoginRequest& request) const = 0;
    virtual AuthResult begin(const LoginRequest& request) = 0;
    virtual AuthResult answer(const AuthAnswer& answer) = 0;
    virtual RestoreAuthResult restore() = 0;
    virtual void forget() = 0;
    virtual std::string storageError() const = 0;
};
}
