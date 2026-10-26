#include "features/providers/infrastructure/foghub_transport.hpp"

#include <curl/curl.h>

#include <cassert>
#include <cstdarg>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace fog::foghub;

namespace {
struct RequestCapture {
    std::string url;
    std::string method;
    const char* form = nullptr;
    curl_off_t formSize = 0;
    std::map<CURLoption, long> options;
    curl_write_callback bodyCallback = nullptr;
    curl_write_callback headerCallback = nullptr;
    void* bodyData = nullptr;
    void* headerData = nullptr;
};
RequestCapture request;
std::string capturedForm;
std::string responseHeaders;
std::string responseBody;
std::vector<std::string> responseCookies;
CURLcode responseCode = CURLE_OK;
long responseStatus = 200;
std::size_t performed = 0;
}

// Every perform is intercepted: these tests cannot contact FogHub or log in.
extern "C" CURLcode __real_curl_easy_setopt(CURL*, CURLoption, ...);
extern "C" CURLcode __real_curl_easy_getinfo(CURL*, CURLINFO, ...);

extern "C" CURLcode __wrap_curl_easy_setopt(CURL* handle, CURLoption option, ...) {
    va_list arguments;
    va_start(arguments, option);
    CURLcode result = CURLE_UNKNOWN_OPTION;
    if (option == CURLOPT_WRITEFUNCTION || option == CURLOPT_HEADERFUNCTION) {
        const auto callback = va_arg(arguments, curl_write_callback);
        if (option == CURLOPT_WRITEFUNCTION) request.bodyCallback = callback;
        else request.headerCallback = callback;
        result = __real_curl_easy_setopt(handle, option, callback);
    } else if (option == CURLOPT_XFERINFOFUNCTION) {
        const auto callback = va_arg(arguments, curl_xferinfo_callback);
        result = __real_curl_easy_setopt(handle, option, callback);
    } else if (option == CURLOPT_WRITEDATA || option == CURLOPT_HEADERDATA || option == CURLOPT_XFERINFODATA) {
        auto* data = va_arg(arguments, void*);
        if (option == CURLOPT_WRITEDATA) request.bodyData = data;
        if (option == CURLOPT_HEADERDATA) request.headerData = data;
        result = __real_curl_easy_setopt(handle, option, data);
    } else if (option == CURLOPT_POSTFIELDSIZE_LARGE) {
        const auto size = va_arg(arguments, curl_off_t);
        request.formSize = size;
        result = __real_curl_easy_setopt(handle, option, size);
    } else if (option < CURLOPTTYPE_OBJECTPOINT) {
        const auto value = va_arg(arguments, long);
        request.options[option] = value;
        if (option == CURLOPT_HTTPGET && value == 1L) request.method = "GET";
        if (option == CURLOPT_POST && value == 1L) request.method = "POST";
        result = __real_curl_easy_setopt(handle, option, value);
    } else {
        assert(option < CURLOPTTYPE_FUNCTIONPOINT);
        const auto* value = va_arg(arguments, const char*);
        if (option == CURLOPT_URL) {
            request = {};
            request.url = value;
        }
        if (option == CURLOPT_POSTFIELDS) request.form = value;
        result = __real_curl_easy_setopt(handle, option, value);
    }
    va_end(arguments);
    return result;
}

extern "C" CURLcode __wrap_curl_easy_getinfo(CURL* handle, CURLINFO info, ...) {
    va_list arguments;
    va_start(arguments, info);
    CURLcode result = CURLE_UNKNOWN_OPTION;
    if (info == CURLINFO_RESPONSE_CODE) {
        auto* status = va_arg(arguments, long*);
        *status = responseStatus;
        result = CURLE_OK;
    } else {
        assert(info == CURLINFO_COOKIELIST);
        auto** cookies = va_arg(arguments, curl_slist**);
        result = __real_curl_easy_getinfo(handle, info, cookies);
    }
    va_end(arguments);
    return result;
}

extern "C" CURLcode __wrap_curl_easy_perform(CURL* handle) {
    ++performed;
    assert(request.url.rfind("https://foghub.ru/", 0) == 0);
    assert(request.options.at(CURLOPT_SSL_VERIFYPEER) == 1L);
    assert(request.options.at(CURLOPT_SSL_VERIFYHOST) == 2L);
    assert(request.options.at(CURLOPT_FOLLOWLOCATION) == 0L);
    assert(request.options.at(CURLOPT_CONNECTTIMEOUT_MS) == 15000L);
    assert(request.options.at(CURLOPT_TIMEOUT_MS) == 45000L);
    capturedForm = request.form ? std::string(request.form, static_cast<std::size_t>(request.formSize)) : "";
    for (const auto& cookie : responseCookies)
        assert(__real_curl_easy_setopt(handle, CURLOPT_COOKIELIST, cookie.c_str()) == CURLE_OK);
    if (!responseHeaders.empty() && request.headerCallback(responseHeaders.data(), 1, responseHeaders.size(), request.headerData) != responseHeaders.size())
        return CURLE_WRITE_ERROR;
    if (!responseBody.empty() && request.bodyCallback(responseBody.data(), 1, responseBody.size(), request.bodyData) != responseBody.size())
        return CURLE_WRITE_ERROR;
    return responseCode;
}

template<class Function>
void expectFailure(TransportFailure kind, Function function) {
    bool failed = false;
    try { function(); }
    catch (const TransportError& error) {
        assert(error.kind() == kind);
        assert(std::string(error.what()).find("secret") == std::string::npos);
        assert(std::string(error.what()).find("https://") == std::string::npos);
        failed = true;
    }
    assert(failed);
}

void paths() {
    const std::vector<std::string> allowed = {
        "/site/login", "/user/info", "/user/balance", "/session/list",
        "/game/list", "/game/preview", "/pc/list", "/pc/preview"
    };
    for (const auto& path : allowed) assert(detail::canonicalReadPath(path) == path);
    assert(detail::canonicalReadPath("/game/list?search=hello+world&tag=a%26b") ==
        "/game/list?search=hello%20world&tag=a%26b");
    assert(detail::canonicalReadPath("/pc/preview?id=42&empty=") == "/pc/preview?id=42&empty=");
    assert(detail::canonicalReadPath("/game/list?filter%5Bname%5D=%D0%B8%D0%B3%D1%80%D0%B0") ==
        "/game/list?filter%5Bname%5D=%D0%B8%D0%B3%D1%80%D0%B0");
    std::vector<std::string> invalid = {
        "", "https://foghub.ru/user/info", "//foghub.ru/user/info", "/user/info/", "/USER/info",
        "/user/../user/info", "/user/%2e%2e/info", "/%2fuser/info", "/user/info%00", "/user/info#fragment",
        "/user\\info", "/pc/play", "/session/close", "/pc/favorite", "/site/logout", "/promo/test",
        "/site/index", "/", "/pc/preview?", "/pc/preview?id", "/pc/preview?id=1&",
        "/pc/preview?id=1&&id=2", "/pc/preview?=1", "/pc/preview?id=%", "/pc/preview?id=%gg",
        "/pc/preview?id=%0Asecret", "/pc/preview?id=%00secret", "/pc/preview?id=%5Csecret",
        "/pc/preview?id=%252e%252e", "/pc/preview?_method=POST", "/pc/preview?%6dethod=POST",
        "/pc/preview?action=secret", "/pc/preview?id=1\nsecret"
    };
    invalid.emplace_back("/user/info\0secret", 17);
    for (const auto& path : invalid)
        expectFailure(TransportFailure::InvalidRequest, [&] { detail::canonicalReadPath(path); });
    expectFailure(TransportFailure::InvalidRequest, [] { detail::canonicalReadPath("/pc/list?id=" + std::string(65536, 'a')); });
}

void cookieValidation() {
    assert(detail::normalizeCookie(".foghub.ru\tTRUE\t/\tFALSE\t0\tsession\tsecret") ==
        "foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret");
    assert(detail::normalizeCookie("#HttpOnly_.foghub.ru\tTRUE\t/\tTRUE\t0\tsession\tsecret") ==
        "#HttpOnly_foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret");
    assert(detail::normalizeCookie("#HttpOnly_foghub.ru\tFALSE\t/\tFALSE\t0\tPHPSESSID\tsecret") ==
        "#HttpOnly_foghub.ru\tFALSE\t/\tTRUE\t0\tPHPSESSID\tsecret");
    assert(detail::normalizeCookie("#HttpOnly_foghub.ru\tFALSE\t/\tFALSE\t0\t_csrf\tsecret") ==
        "#HttpOnly_foghub.ru\tFALSE\t/\tTRUE\t0\t_csrf\tsecret");
    std::vector<std::string> invalid = {
        "", "Set-Cookie: session=secret; Domain=foghub.ru", "ALL", "# comment",
        "evil.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret",
        "sub.foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret",
        "foghub.ru.evil.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret",
        ".ru\tTRUE\t/\tTRUE\t0\tsession\tsecret",
        "foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret\nALL",
        "foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret\rSet-Cookie: evil=1",
        "foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret\textra",
        "foghub.ru\tFALSE\t/\tTRUE\t-1\tsession\tsecret",
        "foghub.ru\tFALSE\t/\tTRUE\tx\tsession\tsecret",
        "foghub.ru\tFALSE\t/\tTRUE\t0\tbad name\tsecret",
        "foghub.ru\tFALSE\t/\tTRUE\t0\tbad:name\tsecret",
        "foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret; other=1",
        "foghub.ru\tFALSE\trelative\tTRUE\t0\tsession\tsecret",
        "foghub.ru\tFALSE\t/\\evil\tTRUE\t0\tsession\tsecret"
    };
    auto nul = std::string("foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret");
    nul += '\0';
    invalid.push_back(nul);
    for (const auto& cookie : invalid)
        expectFailure(TransportFailure::CookieRejected, [&] { detail::normalizeCookie(cookie); });
}

void cookieEngine() {
    auto transport = makeWebTransport();
    assert(transport->cookies().empty());
    const std::string session = "#HttpOnly_foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret";
    transport->restoreCookies({session});
    assert(transport->cookies() == std::vector<std::string>{session});
    auto otherTransport = makeWebTransport();
    assert(otherTransport->cookies().empty());
    expectFailure(TransportFailure::CookieRejected, [&] {
        transport->restoreCookies({session, "evil.ru\tFALSE\t/\tTRUE\t0\tevil\t1"});
    });
    assert(transport->cookies() == std::vector<std::string>{session});
    expectFailure(TransportFailure::CookieRejected, [&] {
        transport->restoreCookies(std::vector<std::string>(129, session));
    });
    transport->clearCookies();
    assert(transport->cookies().empty());
    transport->restoreCookies({session});
    transport->restoreCookies({});
    assert(transport->cookies().empty());
    transport->cancel();
    expectFailure(TransportFailure::Cancelled, [&] { transport->get("/user/info"); });
    expectFailure(TransportFailure::Cancelled, [&] { transport->submitLogin({{"LoginForm[username]", "secret"}, {"_csrf", "a+/="}}); });
    expectFailure(TransportFailure::InvalidRequest, [&] { transport->get("https://secret.invalid/user/info"); });
    expectFailure(TransportFailure::InvalidRequest, [&] { transport->submitLogin({{"bad\r\nheader", "secret"}}); });
    expectFailure(TransportFailure::InvalidRequest, [&] { transport->submitLogin({{"password", std::string(65536, 'a')}}); });
    expectFailure(TransportFailure::InvalidRequest, [&] { transport->submitLogin({}); });
}

void requestExecution() {
    auto transport = makeWebTransport();
    responseHeaders = "Location: https://evil.invalid/secret\r\n";
    responseBody = "example";
    responseStatus = 302;
    const auto before = performed;
    const auto response = transport->get("/pc/preview?id=a%26b");
    assert(request.url == "https://foghub.ru/pc/preview?id=a%26b");
    assert(request.method == "GET");
    assert(response.status == 302 && response.body == "example");
    assert(response.location == "https://evil.invalid/secret");
    assert(performed == before + 1); // No implicit redirect, including off-origin.
    responseHeaders = "cOnTeNt-TyPe: application/json; charset=utf-8\r\n";
    responseStatus = 200;
    const auto content = transport->submitLogin({{"LoginForm[username]", "test@example.invalid"}, {"LoginForm[password]", "secret&=+"}, {"_csrf", "+/="}});
    assert(content.contentType == "application/json; charset=utf-8");
    assert(request.url == "https://foghub.ru/site/login" && request.method == "POST");
    assert(capturedForm == "LoginForm%5Busername%5D=test%40example.invalid&LoginForm%5Bpassword%5D=secret%26%3D%2B&_csrf=%2B%2F%3D");
    responseHeaders.clear();
    transport->get("/user/info");
    assert(request.method == "GET" && capturedForm.empty()); // POST state reset.
    responseCookies = {"evil.ru\tFALSE\t/\tTRUE\t0\tforeign\tsecret", "#HttpOnly_foghub.ru\tFALSE\t/\tFALSE\t0\tPHPSESSID\tsecret"};
    transport->get("/user/info");
    assert(transport->cookies() == std::vector<std::string>{"#HttpOnly_foghub.ru\tFALSE\t/\tTRUE\t0\tPHPSESSID\tsecret"});
    responseCookies.clear();
    responseCode = CURLE_COULDNT_RESOLVE_HOST;
    expectFailure(TransportFailure::Network, [&] { transport->get("/user/info"); });
    responseCode = CURLE_OK;
    responseBody.assign(8 * 1024 * 1024 + 1, 'x');
    expectFailure(TransportFailure::ResponseTooLarge, [&] { transport->get("/game/list"); });
    responseBody.clear();
    responseHeaders.assign(64 * 1024 + 1, 'x');
    expectFailure(TransportFailure::ResponseTooLarge, [&] { transport->get("/user/info"); });
    responseHeaders.clear();
    responseCode = CURLE_ABORTED_BY_CALLBACK;
    expectFailure(TransportFailure::Cancelled, [&] { transport->get("/user/info"); });
    responseCode = CURLE_OK;
}

int main() {
    paths();
    cookieValidation();
    cookieEngine();
    requestExecution();
    std::cout << "FogHub transport offline tests passed\n";
}
