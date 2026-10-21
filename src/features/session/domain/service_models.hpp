#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace fog {
using Json = nlohmann::json;

struct CreateOrderRequest {
    int computerId = 0;
    int tariffId = 0;
    int gameId = 0; // Zero omits the optional game.
    bool ownSteamAccount = false;
};

enum class AuthStep { Captcha, Otp, Success, Restart, Unknown };
std::string normalizePhone(const std::string& input);
AuthStep authStep(const Json& response);
std::vector<unsigned char> decodeCaptcha(const std::string& encoded);
inline bool validSessionHash(const std::string& hash) noexcept {
    // Opaque server identifier; query escaping handles punctuation. Reject control bytes and unbounded input.
    return !hash.empty() && hash.size() <= 1024 && std::all_of(hash.begin(), hash.end(),
        [](unsigned char c) { return c > 32 && c != 127; });
}

class FinishRejected : public std::runtime_error {
public:
    FinishRejected() : std::runtime_error("Session finish was rejected") {}
};
}
