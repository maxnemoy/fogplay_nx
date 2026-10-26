#include "features/providers/infrastructure/foghub_transport.hpp"

#include "core/constants/app_info.hpp"
#include "core/constants/resources.hpp"
#include "core/network/curl_runtime.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <string_view>

namespace fog::foghub {
namespace {
constexpr std::size_t maxBodyBytes = 8 * 1024 * 1024;
constexpr std::size_t maxHeaderBytes = 64 * 1024;
constexpr std::size_t maxCookieBytes = 64 * 1024;
constexpr std::size_t maxCookies = 128;
constexpr std::size_t maxRequestBytes = 64 * 1024;

const char* errorMessage(TransportFailure kind) noexcept {
    switch (kind) {
    case TransportFailure::InvalidRequest: return "Invalid FogHub request";
    case TransportFailure::CookieRejected: return "Invalid FogHub cookies";
    case TransportFailure::Cancelled: return "FogHub request cancelled";
    case TransportFailure::Network: return "FogHub HTTPS request failed";
    case TransportFailure::ResponseTooLarge: return "FogHub response limit exceeded";
    case TransportFailure::Internal: return "Cannot initialize FogHub HTTPS transport";
    }
    return "FogHub transport failed";
}

[[noreturn]] void fail(TransportFailure kind) { throw TransportError(kind); }

void check(CURLcode result) {
    if (result != CURLE_OK) fail(TransportFailure::Internal);
}

bool control(unsigned char c) { return c < 32 || c == 127; }

bool validText(std::string_view value) {
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return control(c); });
}

bool unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

std::string encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (unreserved(byte)) result += c;
        else {
            result += '%';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    return result;
}

int hexValue(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decodeQuery(std::string_view value) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        auto c = static_cast<unsigned char>(value[index]);
        if (c == '%') {
            if (value.size() - index < 3) fail(TransportFailure::InvalidRequest);
            const auto high = hexValue(value[index + 1]);
            const auto low = hexValue(value[index + 2]);
            if (high < 0 || low < 0) fail(TransportFailure::InvalidRequest);
            c = static_cast<unsigned char>((high << 4) | low);
            index += 2;
        } else if (c == '+') c = ' ';
        // Reject nested escaping and URI/parser delimiters that could be
        // interpreted differently by a later server-side URL decoder.
        if (control(c) || c == '\\' || c == '%') fail(TransportFailure::InvalidRequest);
        result += static_cast<char>(c);
    }
    return result;
}

bool validFieldName(std::string_view name) {
    return !name.empty() && name.size() <= 128 &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return unreserved(c) || c == '[' || c == ']';
        });
}

std::string lowerAscii(std::string_view value) {
    std::string result(value);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}

struct Transfer {
    WebResponse response;
    std::size_t headerBytes = 0;
    bool tooLarge = false;
    bool failed = false;
};

bool byteCount(std::size_t size, std::size_t count, std::size_t& bytes) {
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) return false;
    bytes = size * count;
    return true;
}

std::size_t bodyCallback(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& transfer = *static_cast<Transfer*>(userdata);
    std::size_t bytes = 0;
    if (!byteCount(size, count, bytes) || bytes > maxBodyBytes - transfer.response.body.size()) {
        transfer.tooLarge = true;
        return 0;
    }
    try { transfer.response.body.append(data, bytes); }
    catch (...) { transfer.failed = true; return 0; }
    return bytes;
}

std::size_t headerCallback(char* data, std::size_t size, std::size_t count, void* userdata) noexcept {
    auto& transfer = *static_cast<Transfer*>(userdata);
    std::size_t bytes = 0;
    if (!byteCount(size, count, bytes) || bytes > maxHeaderBytes - transfer.headerBytes) {
        transfer.tooLarge = true;
        return 0;
    }
    transfer.headerBytes += bytes;
    try {
        std::string_view line(data, bytes);
        if (line.substr(0, 5) == "HTTP/") {
            transfer.response.location.clear();
            transfer.response.contentType.clear();
        }
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto name = lowerAscii(line.substr(0, colon));
            if (name == "location" || name == "content-type") {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
                if (!validText(value)) { transfer.failed = true; return 0; }
                if (name == "location") transfer.response.location = std::string(value);
                else transfer.response.contentType = std::string(value);
            }
        }
    } catch (...) { transfer.failed = true; return 0; }
    return bytes;
}

int progressCallback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<std::atomic_bool*>(userdata)->load(std::memory_order_relaxed) ? 1 : 0;
}

using CookieList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

class CurlWebTransport final : public WebTransport {
public:
    CurlWebTransport() : handle_(createHandle(), &curl_easy_cleanup) {
        check(curl_easy_setopt(handle_.get(), CURLOPT_COOKIEFILE, ""));
    }

    WebResponse get(const std::string& relativePath) override {
        const auto path = detail::canonicalReadPath(relativePath);
        std::lock_guard<std::mutex> lock(mutex_);
        return request(path, nullptr);
    }

    WebResponse submitLogin(const Query& fields) override {
        if (fields.empty() || fields.size() > 64) fail(TransportFailure::InvalidRequest);
        std::string form;
        for (const auto& field : fields) {
            if (!validFieldName(field.first) || field.second.size() > maxRequestBytes / 2 ||
                !validText(field.second)) fail(TransportFailure::InvalidRequest);
            if (!form.empty()) form += '&';
            form += encode(field.first) + '=' + encode(field.second);
            if (form.size() > maxRequestBytes) fail(TransportFailure::InvalidRequest);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return request("/site/login", &form);
    }

    std::vector<std::string> cookies() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return collectCookies();
    }

    void restoreCookies(const std::vector<std::string>& cookies) override {
        if (cookies.size() > maxCookies) fail(TransportFailure::CookieRejected);
        std::vector<std::string> normalized;
        std::size_t bytes = 0;
        for (const auto& cookie : cookies) {
            if (cookie.size() > maxCookieBytes - bytes) fail(TransportFailure::CookieRejected);
            bytes += cookie.size();
            normalized.push_back(detail::normalizeCookie(cookie));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        replaceCookies(normalized);
    }

    void clearCookies() override {
        std::lock_guard<std::mutex> lock(mutex_);
        check(curl_easy_setopt(handle_.get(), CURLOPT_COOKIELIST, "ALL"));
    }

    void cancel() noexcept override { cancelled_.store(true, std::memory_order_relaxed); }

private:
    static CURL* createHandle() {
        try { fog::detail::ensureCurlRuntime(); }
        catch (...) { fail(TransportFailure::Internal); }
        auto* handle = curl_easy_init();
        if (!handle) fail(TransportFailure::Internal);
        return handle;
    }

    std::vector<std::string> collectCookies() {
        curl_slist* raw = nullptr;
        check(curl_easy_getinfo(handle_.get(), CURLINFO_COOKIELIST, &raw));
        CookieList list(raw, &curl_slist_free_all);
        std::vector<std::string> result;
        std::size_t bytes = 0;
        for (auto* item = raw; item; item = item->next) {
            if (!item->data) continue;
            try {
                auto cookie = detail::normalizeCookie(item->data);
                if (result.size() >= maxCookies || cookie.size() > maxCookieBytes - bytes)
                    fail(TransportFailure::ResponseTooLarge);
                bytes += cookie.size();
                result.push_back(std::move(cookie));
            } catch (const TransportError& error) {
                if (error.kind() != TransportFailure::CookieRejected) throw;
                // Ignore foreign/invalid cookies received from the server.
            }
        }
        return result;
    }

    void replaceCookies(const std::vector<std::string>& cookies) {
        check(curl_easy_setopt(handle_.get(), CURLOPT_COOKIELIST, "ALL"));
        for (const auto& cookie : cookies)
            check(curl_easy_setopt(handle_.get(), CURLOPT_COOKIELIST, cookie.c_str()));
    }

    WebResponse request(const std::string& path, const std::string* form) {
        if (cancelled_.load(std::memory_order_relaxed)) fail(TransportFailure::Cancelled);
        // Rebuild the engine with host-only, secure cookies before every call.
        replaceCookies(collectCookies());
        curl_easy_reset(handle_.get());
        Transfer transfer;
        const std::string url = "https://foghub.ru" + path;
        check(curl_easy_setopt(handle_.get(), CURLOPT_URL, url.c_str()));
        check(curl_easy_setopt(handle_.get(), CURLOPT_COOKIEFILE, ""));
        check(curl_easy_setopt(handle_.get(), CURLOPT_USERAGENT, constants::common::UserAgent));
#if LIBCURL_VERSION_NUM >= 0x075500
        check(curl_easy_setopt(handle_.get(), CURLOPT_PROTOCOLS_STR, "https"));
#else
        check(curl_easy_setopt(handle_.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS));
#endif
#ifdef __SWITCH__
        check(curl_easy_setopt(handle_.get(), CURLOPT_CAINFO, constants::resources::CertificateBundle));
#endif
        check(curl_easy_setopt(handle_.get(), CURLOPT_SSL_VERIFYPEER, 1L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_SSL_VERIFYHOST, 2L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_FOLLOWLOCATION, 0L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_MAXREDIRS, 0L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_CONNECTTIMEOUT_MS, 15000L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_TIMEOUT_MS, 45000L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_NOSIGNAL, 1L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_NOPROGRESS, 0L));
        check(curl_easy_setopt(handle_.get(), CURLOPT_XFERINFOFUNCTION, &progressCallback));
        check(curl_easy_setopt(handle_.get(), CURLOPT_XFERINFODATA, &cancelled_));
        check(curl_easy_setopt(handle_.get(), CURLOPT_WRITEFUNCTION, &bodyCallback));
        check(curl_easy_setopt(handle_.get(), CURLOPT_WRITEDATA, &transfer));
        check(curl_easy_setopt(handle_.get(), CURLOPT_HEADERFUNCTION, &headerCallback));
        check(curl_easy_setopt(handle_.get(), CURLOPT_HEADERDATA, &transfer));
        if (form) {
            check(curl_easy_setopt(handle_.get(), CURLOPT_POST, 1L));
            check(curl_easy_setopt(handle_.get(), CURLOPT_POSTFIELDS, form->data()));
            check(curl_easy_setopt(handle_.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(form->size())));
            // libcurl uses application/x-www-form-urlencoded for POSTFIELDS.
        } else check(curl_easy_setopt(handle_.get(), CURLOPT_HTTPGET, 1L));
        const auto result = curl_easy_perform(handle_.get());
        // Remove any foreign-domain cookies before exposing or using the jar.
        replaceCookies(collectCookies());
        if (transfer.tooLarge) fail(TransportFailure::ResponseTooLarge);
        if (transfer.failed) fail(TransportFailure::Internal);
        if (cancelled_.load(std::memory_order_relaxed) || result == CURLE_ABORTED_BY_CALLBACK)
            fail(TransportFailure::Cancelled);
        if (result != CURLE_OK) fail(TransportFailure::Network);
        check(curl_easy_getinfo(handle_.get(), CURLINFO_RESPONSE_CODE, &transfer.response.status));
        return std::move(transfer.response);
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle_;
    std::atomic_bool cancelled_{false};
    std::mutex mutex_;
};
}

TransportError::TransportError(TransportFailure kind) : std::runtime_error(errorMessage(kind)), kind_(kind) {}

std::unique_ptr<WebTransport> makeWebTransport() { return std::make_unique<CurlWebTransport>(); }

namespace detail {
std::string canonicalReadPath(const std::string& input) {
    if (input.empty() || input.size() > maxRequestBytes || input.front() != '/' ||
        input.find_first_of("\\#") != std::string::npos || !validText(input))
        fail(TransportFailure::InvalidRequest);
    const auto question = input.find('?');
    const auto path = input.substr(0, question);
    static constexpr std::array<std::string_view, 8> allowed = {
        "/site/login", "/user/info", "/user/balance", "/session/list",
        "/game/list", "/game/preview", "/pc/list", "/pc/preview"
    };
    if (std::find(allowed.begin(), allowed.end(), path) == allowed.end()) fail(TransportFailure::InvalidRequest);
    if (question == std::string::npos) return path;
    if (question + 1 == input.size()) fail(TransportFailure::InvalidRequest);
    std::string result = path;
    std::size_t start = question + 1;
    std::size_t count = 0;
    while (start < input.size()) {
        if (++count > 64) fail(TransportFailure::InvalidRequest);
        const auto end = input.find('&', start);
        const auto parameter = std::string_view(input).substr(start, end == std::string::npos ? input.size() - start : end - start);
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) fail(TransportFailure::InvalidRequest);
        const auto name = decodeQuery(parameter.substr(0, equals));
        const auto value = decodeQuery(parameter.substr(equals + 1));
        if (!validFieldName(name)) fail(TransportFailure::InvalidRequest);
        const auto lower = lowerAscii(name);
        if (lower == "_method" || lower == "method" || lower == "action") fail(TransportFailure::InvalidRequest);
        result += count == 1 ? '?' : '&';
        result += encode(name) + '=' + encode(value);
        if (result.size() > maxRequestBytes) fail(TransportFailure::InvalidRequest);
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == input.size()) fail(TransportFailure::InvalidRequest);
    }
    return result;
}

std::string normalizeCookie(const std::string& input) {
    if (input.empty() || input.size() > 8192 || input.find_first_of("\r\n") != std::string::npos ||
        input.find('\0') != std::string::npos) fail(TransportFailure::CookieRejected);
    std::string_view line(input);
    const bool httpOnly = line.substr(0, 10) == "#HttpOnly_";
    if (httpOnly) line.remove_prefix(10);
    std::array<std::string_view, 7> fields;
    std::size_t start = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto end = line.find('\t', start);
        if ((index < 6 && end == std::string_view::npos) || (index == 6 && end != std::string_view::npos))
            fail(TransportFailure::CookieRejected);
        fields[index] = line.substr(start, end == std::string_view::npos ? line.size() - start : end - start);
        if (!validText(fields[index])) fail(TransportFailure::CookieRejected);
        start = end == std::string_view::npos ? line.size() : end + 1;
    }
    if ((fields[0] != "foghub.ru" && fields[0] != ".foghub.ru") ||
        (fields[1] != "TRUE" && fields[1] != "FALSE") ||
        (fields[3] != "TRUE" && fields[3] != "FALSE") ||
        fields[2].empty() || fields[2].front() != '/' || fields[2].size() > 2048 ||
        fields[2].find_first_of("\\;?") != std::string_view::npos ||
        fields[4].empty() || fields[4].size() > 19 ||
        !std::all_of(fields[4].begin(), fields[4].end(), [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
        fields[5].empty() || fields[5].size() > 256 ||
        !std::all_of(fields[5].begin(), fields[5].end(), [](unsigned char c) {
            return c > 32 && c < 127 && std::string_view("()<>@,;:\\\"/[]?={}").find(c) == std::string_view::npos;
        }) || fields[6].find(';') != std::string_view::npos)
        fail(TransportFailure::CookieRejected);
    // Persist only the exact HTTPS origin even when Set-Cookie used Domain=.
    return std::string(httpOnly ? "#HttpOnly_foghub.ru" : "foghub.ru") + "\tFALSE\t" +
        std::string(fields[2]) + "\tTRUE\t" + std::string(fields[4]) + "\t" +
        std::string(fields[5]) + "\t" + std::string(fields[6]);
}
}
}
