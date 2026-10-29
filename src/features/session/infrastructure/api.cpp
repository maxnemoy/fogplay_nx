#include "core/constants/error_text.hpp"
#include "features/session/infrastructure/api.hpp"
#include "core/domain/repository_error.hpp"
#include "features/auth/infrastructure/auth_storage.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fog {
namespace {
constexpr const char* app = "android:ru.mts.fogplay";
constexpr const char* version = "1.0.159";

// SHA-256 for the fixed 12-byte normalized phone; journal never stores the phone/token.
std::string accountFingerprint(const std::string& phone) {
    constexpr std::array<std::uint32_t, 64> constants{{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}};
    const auto rotate = [](std::uint32_t value, unsigned n) { return (value >> n) | (value << (32 - n)); };
    std::array<std::uint32_t, 64> words{};
    for (unsigned i = 0; i < phone.size(); ++i)
        words[i / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(phone[i])) << (24 - 8 * (i % 4));
    words[phone.size() / 4] |= 0x80u << (24 - 8 * (phone.size() % 4));
    words[15] = static_cast<std::uint32_t>(phone.size() * 8);
    for (unsigned i = 16; i < 64; ++i) {
        const auto x = words[i - 15], y = words[i - 2];
        words[i] = words[i - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) +
            words[i - 7] + (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
    }
    std::array<std::uint32_t, 8> hash{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    auto state = hash;
    for (unsigned i = 0; i < 64; ++i) {
        const auto e = state[4];
        const auto first = state[7] + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
            ((e & state[5]) ^ (~e & state[6])) + constants[i] + words[i];
        const auto a = state[0];
        const auto second = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
            ((a & state[1]) ^ (a & state[2]) ^ (state[1] & state[2]));
        for (unsigned j = 7; j > 0; --j) state[j] = state[j - 1];
        state[4] += first;
        state[0] = first + second;
    }
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) {
        hash[i] += state[i];
        for (int shift = 28; shift >= 0; shift -= 4) result += digits[(hash[i] >> shift) & 15];
    }
    return result;
}

std::string stringField(const Json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
Json responseJson(const HttpResponse& response) {
    if (response.status < 200 || response.status >= 300)
        throw RepositoryError(response.status == 401 || response.status == 403 ? RepositoryErrorKind::NotAuthenticated : RepositoryErrorKind::Unavailable, "HTTP " + std::to_string(response.status) + fog::constants::error::Connection::RequestRejected);
    auto data = Json::parse(response.body, nullptr, false);
    if (data.is_discarded()) throw RepositoryError(RepositoryErrorKind::InvalidResponse, fog::constants::error::Connection::ServerResponseIsNotValidJSON);
    return data;
}
void validatePage(int page, int pageSize) {
    if (page < 1 || page > 1000000 || pageSize < 1 || pageSize > 100)
        throw std::invalid_argument(fog::constants::error::Catalog::InvalidPagination);
}
bool positiveId(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() > 0 &&
        value.get<std::uint64_t>() <= static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
    return value.is_number_integer() && value.get<std::int64_t>() > 0 &&
        value.get<std::int64_t>() <= (std::numeric_limits<int>::max)();
}
bool computerId(const Json& value) {
    if (positiveId(value)) return true;
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 32) return false;
    int id = 0;
    for (const unsigned char c : text) {
        if (c < '0' || c > '9' || id > ((std::numeric_limits<int>::max)() - (c - '0')) / 10)
            return false;
        id = id * 10 + (c - '0');
    }
    return id > 0;
}
Query commonQuery() { return {{"app", app}, {"version", version}}; }
void validateCode(const std::string& code, bool digitsOnly) {
    if (code.empty() || code.size() > 12 || !std::all_of(code.begin(), code.end(),
        [digitsOnly](unsigned char c) { return digitsOnly ? c >= '0' && c <= '9' :
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }))
        throw std::invalid_argument(fog::constants::error::Auth::InvalidAuthenticationCode);
}
}

std::string normalizePhone(const std::string& input) {
    if (input.size() > 64) throw std::invalid_argument(fog::constants::error::Auth::InvalidPhoneNumber);
    std::string number;
    for (const unsigned char c : input) {
        if (c >= '0' && c <= '9') number += static_cast<char>(c);
        else if (c != '+' && c != ' ' && c != '-' && c != '(' && c != ')')
            throw std::invalid_argument(fog::constants::error::Auth::InvalidPhoneNumber);
    }
    if (number.size() == 11 && number.front() == '8') number.front() = '7';
    if (number.size() != 11 || number.front() != '7')
        throw std::invalid_argument(fog::constants::error::Auth::Expected7FollowedBy10Digits);
    return "+" + number;
}
AuthStep authStep(const Json& response) {
    if (!response.is_object()) return AuthStep::Unknown;
    const auto success = response.find("success");
    if (success != response.end() && success->is_boolean() && success->get<bool>() &&
        validAuthToken(stringField(response, "token"))) return AuthStep::Success;
    const auto form = stringField(response, "form");
    if (form == "captcha") return AuthStep::Captcha;
    if (form == "otp") return AuthStep::Otp;
    if (form == "phone" || form == "login") return AuthStep::Restart;
    return AuthStep::Unknown;
}
std::vector<unsigned char> decodeCaptcha(const std::string& encoded) {
    constexpr std::size_t maxEncoded = 4 * 1024 * 1024;
    if (encoded.size() > maxEncoded + 64) throw std::runtime_error(fog::constants::error::Auth::CaptchaIsTooLarge);
    std::string_view input(encoded);
    if (input.substr(0, 11) == "data:image/") {
        const auto marker = input.find(";base64,");
        if (marker == std::string_view::npos || marker > 40)
            throw std::runtime_error(fog::constants::error::Auth::InvalidCaptchaDataURI);
        input.remove_prefix(marker + 8);
    }
    if (input.empty() || input.size() > maxEncoded || input.size() % 4 != 0)
        throw std::runtime_error(fog::constants::error::Auth::InvalidCaptchaBase64Size);
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> result;
    result.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        unsigned value = 0;
        unsigned padding = 0;
        for (unsigned j = 0; j < 4; ++j) {
            const char c = input[i + j];
            if (c == '=') {
                if (i + 4 != input.size() || j < 2) throw std::runtime_error(fog::constants::error::Auth::InvalidCaptchaBase64Padding);
                ++padding;
                value <<= 6;
            } else {
                const auto digit = alphabet.find(c);
                if (digit == std::string_view::npos || padding) throw std::runtime_error(fog::constants::error::Auth::InvalidCaptchaBase64);
                value = (value << 6) | static_cast<unsigned>(digit);
            }
        }
        // Reject noncanonical padding bits instead of silently accepting malformed input.
        if ((padding == 2 && (value & 0xffffu) != 0) || (padding == 1 && (value & 0xffu) != 0))
            throw std::runtime_error(fog::constants::error::Auth::InvalidCaptchaBase64PaddingBits);
        result.push_back(static_cast<unsigned char>((value >> 16) & 255));
        if (padding < 2) result.push_back(static_cast<unsigned char>((value >> 8) & 255));
        if (!padding) result.push_back(static_cast<unsigned char>(value & 255));
    }
    return result;
}

Api::Api(std::string deviceId) : Api(std::move(deviceId), {}) {}
Api::Api(std::string deviceId, Transport transport, OptionsTransport optionsTransport)
    : transport_(std::move(transport)), optionsTransport_(std::move(optionsTransport)), deviceId_(std::move(deviceId)) {
    if (deviceId_.size() != 32 || !std::all_of(deviceId_.begin(), deviceId_.end(),
        [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        throw std::invalid_argument(fog::constants::error::Auth::ExpectedA32CharacterLowercaseHexDeviceID);
}
Api::~Api() { clearToken(); }
HttpResponse Api::request(const std::string& method, const std::string& path,
        const Query& query, const std::string& body, const std::string& token, bool shutdown, bool creation) {
    if (cancelledAll_.load(std::memory_order_acquire))
        throw RepositoryError(RepositoryErrorKind::NetworkFailure, fog::constants::error::Connection::RequestCancelled);
    const RequestOptions options = creation ? RequestOptions{30000, 15000} :
        shutdown ? RequestOptions{4000, 4000} : RequestOptions{};
    try {
        if (optionsTransport_) return optionsTransport_(method, path, query, body, token, options);
        if (transport_) return transport_(method, path, query, body, token);
        return (shutdown ? shutdownHttp_ : http_).request(method, path, query, body, token, options);
    } catch (const RepositoryError&) { throw; }
    catch (const std::runtime_error& error) { throw RepositoryError(RepositoryErrorKind::NetworkFailure,error.what()); }
}
Json Api::games(std::string search, int page, int pageSize, int genreId) {
    validatePage(page, pageSize);
    if (genreId < 0 || genreId > 1000000) throw std::invalid_argument(fog::constants::error::Catalog::InvalidGenreID);
    if (search.size() > 256) throw std::invalid_argument(fog::constants::error::Catalog::SearchTextIsTooLong);
    auto query = commonQuery();
    query.emplace_back("device_manufacturer", "FogPlayNX");
    query.emplace_back("page", std::to_string(page));
    query.emplace_back("pageSize", std::to_string(pageSize));
    if (!search.empty()) query.emplace_back("search", std::move(search));
    if (genreId) query.emplace_back("taxonomy_id", std::to_string(genreId));
    auto data = responseJson(request("GET", "/api/content/games/", query, "", token_));
    if (!data.is_array()) throw std::runtime_error(fog::constants::error::Catalog::UnexpectedCatalogFormat);
    for (const auto& item : data) {
        if (!item.is_object() || !item.contains("id") || !positiveId(item.at("id")) ||
            !item.contains("name") || !item.at("name").is_string())
            throw std::runtime_error(fog::constants::error::Catalog::UnexpectedGameFormat);
    }
    return data;
}
Json Api::computers(int gameId, int page, int pageSize, bool ownSteamAccount) {
    validatePage(page, pageSize);
    if (gameId < 1) throw std::invalid_argument(fog::constants::error::Catalog::InvalidGameID);
    auto query = commonQuery();
    query.emplace_back("game_id", std::to_string(gameId));
    query.emplace_back("steamacc", ownSteamAccount ? "1" : "0");
    query.emplace_back("page", std::to_string(page));
    query.emplace_back("pageSize", std::to_string(pageSize));
    auto data = responseJson(request("GET", "/api/content/computer/", query, "", token_));
    if (!data.is_object() || !data.contains("computers") || !data.at("computers").is_array())
        throw std::runtime_error(fog::constants::error::Catalog::UnexpectedComputerListFormat);
    for (const auto& item : data.at("computers")) {
        if (!item.is_object() || !item.contains("id") || !computerId(item.at("id")))
            throw std::runtime_error(fog::constants::error::Catalog::UnexpectedComputerFormat);
    }
    return data;
}
Json Api::computerAuto(int gameId, bool ownSteamAccount) {
    if (gameId < 1) throw std::invalid_argument(fog::constants::error::Catalog::InvalidGameID);
    auto query = commonQuery();
    query.emplace_back("game_id", std::to_string(gameId));
    query.emplace_back("no_cache", "1");
    query.emplace_back("steamacc", ownSteamAccount ? "1" : "0");
    auto data = responseJson(request("GET", "/api/content/computer-auto/", query, "", token_));
    if (!data.is_object()) throw std::runtime_error(fog::constants::error::Catalog::UnexpectedAutomaticComputerSelectionFormat);
    for (const auto* category : {"min_price", "optimum", "max_power"}) {
        if (!data.contains(category) || !data.at(category).is_array())
            throw std::runtime_error(fog::constants::error::Catalog::UnexpectedAutomaticComputerCategory);
        for (const auto& item : data.at(category))
            if (!item.is_object() || !item.contains("id") || !computerId(item.at("id")) ||
                !item.contains("tariff_id") || !computerId(item.at("tariff_id")))
                throw std::runtime_error(fog::constants::error::Catalog::UnexpectedAutomaticComputerCandidate);
    }
    return data;
}
Json Api::gameDetail(int gameId) {
    if (gameId < 1) throw std::invalid_argument(fog::constants::error::Catalog::InvalidGameID);
    auto query = commonQuery();
    query.emplace_back("game_id", std::to_string(gameId));
    query.emplace_back("gamepad", "1");
    query.emplace_back("device_manufacturer", "FogPlayNX");
    auto data = responseJson(request("GET", "/api/content/games/", query, "", token_));
    if (!data.is_array()) throw std::runtime_error(fog::constants::error::Catalog::UnexpectedGameDetailsFormat);
    for (const auto& item : data)
        if (item.is_object() && item.contains("id") && positiveId(item.at("id")) &&
            item.at("id").get<int>() == gameId && item.contains("name") && item.at("name").is_string())
            return item;
    throw std::runtime_error(fog::constants::error::Catalog::GameDetailsUnavailable);
}
Json Api::orderHistory() {
    if (token_.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    auto data = responseJson(request("GET", "/api/order/history/", {}, "", token_));
    if (!data.is_array() || data.size() > 10000) throw std::runtime_error(fog::constants::error::Session::UnexpectedOrderHistoryFormat);
    for (const auto& item : data)
        if (!item.is_object()) throw std::runtime_error(fog::constants::error::Session::UnexpectedOrderHistoryEntry);
    return data;
}
Json Api::createOrder(const CreateOrderRequest& order) {
    if (token_.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    if (order.computerId < 1 || order.tariffId < 1 || order.gameId < 0)
        throw std::invalid_argument(fog::constants::error::Session::InvalidCheckoutSelection);
    auto query = commonQuery();
    query.emplace_back("computerId", std::to_string(order.computerId));
    query.emplace_back("tariffId", std::to_string(order.tariffId));
    query.emplace_back("device_manufacturer", "FogPlayNX");
    query.emplace_back("device_brand", "Nintendo");
    query.emplace_back("device_model", "Switch");
    query.emplace_back("device_uid", deviceId_);
    query.emplace_back("is_tv", "1");
    query.emplace_back("gamepad_type", "physical");
    query.emplace_back("steamacc", order.ownSteamAccount ? "1" : "0");
    if (order.gameId) query.emplace_back("gameId", std::to_string(order.gameId));
    // One attempt only. CheckoutGuard records intent before this mutation.
    auto data = responseJson(request("POST", "/api/order/create/", query, "", token_, true, true));
    if (!data.is_object()) throw std::runtime_error(fog::constants::error::Session::UnexpectedCreateOrderFormat);
    return data;
}
Json Api::orderStatus(std::int64_t orderId) {
    if (token_.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    if (orderId <= 0) throw std::invalid_argument(fog::constants::error::Session::InvalidOrderID);
    auto data = responseJson(request("GET", "/api/order/status/", {{"id", std::to_string(orderId)}}, "", token_, true));
    if (!data.is_object()) throw std::runtime_error(fog::constants::error::Session::UnexpectedOrderStatusFormat);
    return data;
}
Json Api::authPost(const std::string& endpoint, const Json& body) {
    auto data = responseJson(request("POST", endpoint, {}, body.dump()));
    if (!data.is_object()) throw std::runtime_error(fog::constants::error::Auth::UnexpectedAuthenticationResponse);
    nextStep_ = authStep(data);
    if (nextStep_ == AuthStep::Success) {
        token_ = stringField(data, "token");
        authStorageError_.clear();
        if (!authStoragePath_.empty()) {
            try { saveAuthCredentials(authStoragePath_, {token_, accountIdentity_, deviceId_}); }
            catch (...) { authStorageError_ = "Sign-in works for this run, but could not be saved; sign in again after restart"; }
        }
        loggedIn_.store(true, std::memory_order_release);
    }
    return data;
}
Json Api::submitPhone(std::string phone) {
    phone = normalizePhone(phone);
    forgetAuth(); // Delete the previous account before attempting a valid switch.
    accountIdentity_ = accountFingerprint(phone);
    return authPost("/api/login/mts-phone/", {{"phone", phone}, {"appId", app},
        {"deviceId", deviceId_}, {"agreement", true}, {"subscribe", false}});
}
Json Api::submitCaptcha(std::string code) {
    if (nextStep_ != AuthStep::Captcha) throw std::logic_error(fog::constants::error::Auth::CaptchaIsNotRequested);
    validateCode(code, false);
    return authPost("/api/login/mts-captcha/", {{"code", code}, {"deviceId", deviceId_}, {"appId", app}});
}
Json Api::submitOtp(std::string code) {
    if (nextStep_ != AuthStep::Otp) throw std::logic_error(fog::constants::error::Auth::SMSCodeIsNotRequested);
    validateCode(code, true);
    return authPost("/api/login/mts-otp/", {{"code", code}, {"deviceId", deviceId_},
        {"deviceName", "FogPlay NX Switch"}, {"appId", app}});
}
Json Api::userInfo() {
    if (token_.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    auto data = responseJson(request("GET", "/api/login/user-info/", {}, "", token_));
    if (!data.is_object()) throw std::runtime_error(fog::constants::error::Auth::UnexpectedProfileFormat);
    return data;
}
bool Api::loggedIn() const noexcept { return loggedIn_.load(std::memory_order_acquire); }
Json Api::activeSessions(bool shutdown) {
    if (token_.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    auto data = responseJson(request("GET", "/api/order/get-active-sessions/", {}, "", token_, shutdown));
    if (!data.is_array()) throw std::runtime_error(fog::constants::error::Session::UnexpectedActiveSessionsFormat);
    if (data.size() > 10000) throw std::runtime_error(fog::constants::error::Session::TooManyActiveSessions);
    for (auto& item : data) {
        // Never skip an unidentified entry: doing so could falsely prove absence
        // of the exact rental being stopped. Display metadata is not identity.
        if (!item.is_object() || !validSessionHash(stringField(item, "hash")))
            throw std::runtime_error(fog::constants::error::Session::UnexpectedActiveSessionFormat);
        if (!item.contains("computer_name") || item.at("computer_name").is_null())
            item["computer_name"] = "";
        else if (!item.at("computer_name").is_string())
            throw std::runtime_error(fog::constants::error::Session::UnexpectedActiveSessionDisplayFormat);
    }
    return data;
}
bool Api::finishSession(const std::string& hash) {
    if (token_.empty()) throw std::logic_error(fog::constants::error::Auth::SignInFirst);
    if (!validSessionHash(hash)) throw std::invalid_argument(fog::constants::error::Session::InvalidSessionHash);
    const auto response = request("POST", "/api/order/finish-session/", {{"hash", hash}}, "", token_, true);
    if (response.status >= 300 && response.status < 500) throw FinishRejected();
    auto data = responseJson(response);
    if (!data.is_object() || !data.contains("ok") || !data.at("ok").is_boolean())
        throw std::runtime_error(fog::constants::error::Session::UnexpectedFinishSessionFormat);
    return data.at("ok").get<bool>();
}
void Api::configureAuthStorage(std::string path) {
    if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos)
        throw std::invalid_argument(fog::constants::error::Auth::InvalidSignInStoragePath);
    if (loggedIn()) throw std::logic_error(fog::constants::error::Auth::ConfigureSignInStorageBeforeSigningIn);
    authStoragePath_ = std::move(path);
    authStorageError_.clear();
}
bool Api::restoreAuth() {
    clearToken();
    authStorageError_.clear();
    if (authStoragePath_.empty()) throw std::logic_error(fog::constants::error::Auth::ConfigureSignInStorageBeforeRestoring);
    std::optional<AuthCredentials> saved;
    try { saved = loadAuthCredentials(authStoragePath_); }
    catch (...) {
        forgetAuth();
        authStorageError_ = "Saved sign-in is unreadable or invalid; sign in again";
        return false;
    }
    if (!saved) return false;
    if (saved->deviceId != deviceId_) {
        forgetAuth();
        authStorageError_ = "Saved sign-in belongs to another device; sign in again";
        return false;
    }
    bool authorized = false;
    bool rejected = false;
    try {
        const auto response = request("GET", "/api/login/user-info/", {}, "", saved->token);
        rejected = response.status == 401;
        if (!rejected) {
            const auto data = responseJson(response);
            const auto field = data.is_object() ? data.find("authorized") : data.end();
            if (data.is_object() && field != data.end() && field->is_boolean()) {
                authorized = field->get<bool>();
                rejected = !authorized;
            }
        }
    } catch (...) {
        // Offline/5xx/invalid responses cannot revoke durable credentials.
    }
    if (!authorized) {
        if (rejected) {
            forgetAuth();
            authStorageError_ = "Saved sign-in was rejected; sign in again";
        } else {
            authStorageError_ = "Saved sign-in could not be checked; retained for a later retry";
        }
        return false;
    }
    token_ = std::move(saved->token);
    accountIdentity_ = std::move(saved->account);
    nextStep_ = AuthStep::Success;
    loggedIn_.store(true, std::memory_order_release);
    return true;
}
void Api::forgetAuth() {
    try {
        if (!authStoragePath_.empty()) removeAuthCredentials(authStoragePath_);
        authStorageError_.clear();
    } catch (...) {
        clearToken();
        authStorageError_ = "Could not remove saved sign-in; saved credentials may remain on SD";
        throw std::runtime_error(authStorageError_);
    }
    clearToken();
}
void Api::clearToken() noexcept {
    authGeneration_.fetch_add(1, std::memory_order_release);
    loggedIn_.store(false, std::memory_order_release);
    std::fill(token_.begin(), token_.end(), '\0');
    token_.clear();
    accountIdentity_.clear();
    nextStep_ = AuthStep::Restart;
}
void Api::cancel() noexcept { http_.cancel(); }
void Api::cancelAll() noexcept {
    cancelledAll_.store(true, std::memory_order_release);
    http_.cancel();
    shutdownHttp_.cancel();
}
}
