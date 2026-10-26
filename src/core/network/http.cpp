#include "core/constants/resources.hpp"
#include "core/constants/app_info.hpp"
#include "core/constants/error_text.hpp"
#include "core/network/http.hpp"
#include "core/network/curl_runtime.hpp"

#include <curl/curl.h>
#include <limits>
#include <memory>
#include <stdexcept>

namespace fog {
namespace {
constexpr std::size_t maxResponseBytes = 8 * 1024 * 1024;

struct ResponseBuffer {
    std::string body;
    bool tooLarge = false;
    bool allocationFailed = false;
};

std::size_t writeResponse(char* data, std::size_t size, std::size_t count,
                          void* userdata) noexcept {
    auto& buffer = *static_cast<ResponseBuffer*>(userdata);
    if (size != 0 && count > (std::numeric_limits<std::size_t>::max)() / size) {
        buffer.tooLarge = true;
        return 0;
    }
    const auto bytes = size * count;
    if (bytes > maxResponseBytes - buffer.body.size()) {
        buffer.tooLarge = true;
        return 0;
    }
    try {
        buffer.body.append(data, bytes);
    } catch (...) {
        buffer.allocationFailed = true;
        return 0;
    }
    return bytes;
}

int progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return static_cast<std::atomic_bool*>(userdata)->load(std::memory_order_relaxed) ? 1 : 0;
}

void check(CURLcode result) {
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string(fog::constants::error::Connection::HTTPSTransportPrefix) + curl_easy_strerror(result));
    }
}

std::string escape(CURL* handle, const std::string& value) {
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error(fog::constants::error::Connection::QueryParameterIsTooLarge);
    }
    std::unique_ptr<char, decltype(&curl_free)> escaped(
        curl_easy_escape(handle, value.data(), static_cast<int>(value.size())), &curl_free);
    if (!escaped) { throw std::runtime_error(fog::constants::error::Connection::CannotEncodeQueryParameter); }
    return escaped.get();
}
}

std::string base64Encode(std::string_view input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto left = input.size() - offset;
        const auto a = static_cast<unsigned char>(input[offset]);
        const auto b = left > 1 ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto c = left > 2 ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output += alphabet[a >> 2];
        output += alphabet[((a & 3) << 4) | (b >> 4)];
        output += left > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        output += left > 2 ? alphabet[c & 63] : '=';
    }
    return output;
}

HttpClient::HttpClient() {
    detail::ensureCurlRuntime();
}

HttpClient::~HttpClient() = default;

void HttpClient::cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }

HttpResponse HttpClient::request(const std::string& method, const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& query,
    const std::string& jsonBody, const std::string& token, RequestOptions options) {
    if (cancelled_.load(std::memory_order_relaxed)) throw std::runtime_error(fog::constants::error::Connection::RequestCancelled);
    if (options.timeoutMs <= 0 || options.connectTimeoutMs <= 0 ||
        options.connectTimeoutMs > options.timeoutMs)
        throw std::invalid_argument(fog::constants::error::Connection::InvalidHTTPTimeout);
    if (method != "GET" && method != "POST") {
        throw std::invalid_argument(fog::constants::error::Connection::OnlyGETAndPOSTAreSupported);
    }
    if (path.empty() || path.front() != '/' || path.rfind("//", 0) == 0 ||
        path.find_first_of("?#\\\r\n") != std::string::npos ||
        path.find('\0') != std::string::npos) {
        throw std::invalid_argument(fog::constants::error::Connection::ExpectedARelativeFogPlayAPIPath);
    }
    if (token.find_first_of("\r\n") != std::string::npos || token.find('\0') != std::string::npos) {
        throw std::invalid_argument(fog::constants::error::Connection::InvalidAuthenticationToken);
    }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
    if (!handle) { throw std::runtime_error(fog::constants::error::Connection::CannotCreateHTTPSRequest); }
    std::string url = "https://fogplay.mts.ru" + path;
    for (std::size_t index = 0; index < query.size(); ++index) {
        url += index == 0 ? '?' : '&';
        url += escape(handle.get(), query[index].first) + "=" + escape(handle.get(), query[index].second);
    }
    curl_slist* rawHeaders = nullptr;
    const auto appendHeader = [&rawHeaders](const std::string& value) {
        auto* appended = curl_slist_append(rawHeaders, value.c_str());
        if (!appended) { throw std::runtime_error(fog::constants::error::Connection::CannotAllocateHTTPHeaders); }
        rawHeaders = appended;
    };
    try {
        appendHeader("Accept: application/json");
        if (method == "POST") { appendHeader("Content-Type: application/json"); }
        if (!token.empty()) { appendHeader("Authorization: Basic " + base64Encode(token)); }
    } catch (...) {
        curl_slist_free_all(rawHeaders);
        throw;
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(rawHeaders, &curl_slist_free_all);
    ResponseBuffer buffer;
    check(curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str()));
    check(curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get()));
    check(curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, fog::constants::common::UserAgent));
#if LIBCURL_VERSION_NUM >= 0x075500
    check(curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https"));
#else
    check(curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTPS));
#endif
#ifdef __SWITCH__
    check(curl_easy_setopt(handle.get(), CURLOPT_CAINFO, fog::constants::resources::CertificateBundle));
#endif
    check(curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L));
    check(curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L));
    check(curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L));
    check(curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS, options.connectTimeoutMs));
    check(curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, options.timeoutMs));
    check(curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L));
    check(curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L));
    check(curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, &progress));
    check(curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &cancelled_));
    check(curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &writeResponse));
    check(curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &buffer));
    if (method == "POST") {
        check(curl_easy_setopt(handle.get(), CURLOPT_POST, 1L));
        check(curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, jsonBody.data()));
        check(curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                               static_cast<curl_off_t>(jsonBody.size())));
    }
    const auto result = curl_easy_perform(handle.get());
    if (buffer.tooLarge) { throw std::runtime_error(fog::constants::error::Connection::HTTPResponseExceeds8MiB); }
    if (buffer.allocationFailed) { throw std::runtime_error(fog::constants::error::Connection::CannotAllocateHTTPResponseBuffer); }
    if (result == CURLE_ABORTED_BY_CALLBACK) throw std::runtime_error(fog::constants::error::Connection::RequestCancelled);
    check(result);
    HttpResponse response;
    check(curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.status));
    response.body = std::move(buffer.body);
    return response;
}
}
