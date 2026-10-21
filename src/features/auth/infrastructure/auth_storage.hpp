#pragma once
#include <optional>
#include <string>

namespace fog {
struct AuthCredentials {
    std::string token;
    std::string account;
    std::string deviceId;
};
bool validAuthToken(const std::string& token) noexcept;
// No network requests; diagnostics never include paths or credential data.
std::optional<AuthCredentials> loadAuthCredentials(const std::string& path);
void saveAuthCredentials(const std::string& path, const AuthCredentials& credentials);
void removeAuthCredentials(const std::string& path);
}
