#pragma once
#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fog {
using Query = std::vector<std::pair<std::string, std::string>>;
struct HttpResponse { long status = 0; std::string body; };
struct RequestOptions { long timeoutMs = 45000; long connectTimeoutMs = 15000; };
std::string base64Encode(std::string_view input);
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpResponse request(const std::string& method, const std::string& path,
        const Query& query = {}, const std::string& jsonBody = "", const std::string& token = "",
        RequestOptions options = {});
    void cancel() noexcept;
private:
    std::atomic_bool cancelled_{false};
};
}
