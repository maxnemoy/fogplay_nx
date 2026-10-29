#include "features/session/infrastructure/api.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int assertions = 0;
void require(bool condition, const char* label) {
    ++assertions;
    if (!condition) throw std::runtime_error(label);
}
template<class Action> void rejects(Action action, const char* label) {
    bool failed = false;
    try { action(); } catch (const std::exception& error) {
        failed = true;
        require(std::string(error.what()).find("PRIVATE") == std::string::npos,
            "error must not expose raw response");
    }
    require(failed, label);
}
struct RecordedRequest {
    std::string method, path, body, token;
    fog::Query query;
};
struct Fake {
    std::vector<RecordedRequest> requests;
    fog::HttpResponse next{200, "[]"};
    fog::HttpResponse operator()(const std::string& method, const std::string& path,
        const fog::Query& query, const std::string& body, const std::string& token) {
        requests.push_back({method, path, body, token, query});
        return next;
    }
};
std::string parameter(const fog::Query& query, const std::string& key) {
    for (const auto& item : query) if (item.first == key) return item.second;
    return {};
}
}
int main() {
    try {
        using fog::Json;
        using fog::AuthStep;
        const std::string device = "11111111111141118111111111111111";
        require(fog::base64Encode("") == "", "empty base64");
        require(fog::base64Encode("f") == "Zg==", "one-byte base64");
        require(fog::base64Encode("fo") == "Zm8=", "two-byte base64");
        require(fog::base64Encode("foo") == "Zm9v", "raw token Basic base64");
        require(fog::normalizePhone("8 (999) 123-45-67") == "+79991234567", "phone normalize");
        rejects([] { fog::normalizePhone("PRIVATE"); }, "reject invalid phone without echo");
        rejects([] { fog::Api bad("bad-id"); }, "reject invalid device");
        require(fog::authStep(Json{{"success", true}}) == AuthStep::Unknown, "missing token");
        require(fog::authStep(Json{{"success", false}, {"token", "PRIVATE"}}) == AuthStep::Unknown, "false success");
        require(fog::authStep(Json{{"success", true}, {"token", "bad\r\ntoken"}}) == AuthStep::Unknown, "invalid token");
        require(fog::authStep(Json{{"success", true}, {"token", "fixture"}}) == AuthStep::Success, "valid success");
        require(fog::authStep(Json{{"form", "captcha"}}) == AuthStep::Captcha, "captcha stage");
        require(fog::authStep(Json{{"form", "otp"}}) == AuthStep::Otp, "otp stage");
        require(fog::authStep(Json{{"form", "phone"}}) == AuthStep::Restart, "restart stage");
        require(fog::authStep(Json{{"form", "login"}}) == AuthStep::Restart, "restart from actual login form");
        require(fog::decodeCaptcha("Zm9v") == std::vector<unsigned char>({'f','o','o'}), "captcha base64");
        require(fog::decodeCaptcha("data:image/png;base64,Zg==") == std::vector<unsigned char>({'f'}), "captcha URI");
        rejects([] { fog::decodeCaptcha("Zm=9"); }, "base64 broken padding");
        rejects([] { fog::decodeCaptcha("Zh=="); }, "base64 padding bits");
        rejects([] { fog::decodeCaptcha("data:image/png,Zm9v"); }, "data URI no base64");
        rejects([] { fog::decodeCaptcha(std::string(4 * 1024 * 1024 + 65, 'A')); }, "captcha oversized");
        Fake fake;
        fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
        fake.next.body = R"([{"id":1,"name":"Fixture"}])";
        require(api.games("a & b", 2).size() == 1, "catalog parse");
        const auto& catalogRequest = fake.requests.back();
        require(catalogRequest.method == "GET" && catalogRequest.path == "/api/content/games/", "catalog endpoint");
        require(parameter(catalogRequest.query, "search") == "a & b", "search serialization");
        require(parameter(catalogRequest.query, "page") == "2", "page serialization");
        require(parameter(catalogRequest.query, "pageSize") == "8", "page size serialization");
        require(parameter(catalogRequest.query, "app") == "android:ru.mts.fogplay", "app profile");
        require(api.games("racing search",3,16,13).size()==1,"filtered catalog parses");
        require(parameter(fake.requests.back().query,"taxonomy_id")=="13","verified web taxonomy parameter");
        require(parameter(fake.requests.back().query,"page")=="3" && parameter(fake.requests.back().query,"search")=="racing search","genre survives search and pagination");
        api.games("",1,16);
        require(parameter(fake.requests.back().query,"taxonomy_id").empty(),"all genres omits filter");
        rejects([&] { api.games("",1,16,-1); },"negative genre rejected");
        fake.next.body = R"({"computers":[{"id":2}]})";
        require(api.computers(1, 3)["computers"].size() == 1, "computer parse");
        require(parameter(fake.requests.back().query, "game_id") == "1", "game id query");
        // Public server fixture from the verified Windows probe, 2026-10-13.
        // Preserve string IDs: the computer endpoint and game endpoint have different schemas.
        fake.next.body = R"({"computers":[{"id":"3587","name":"3dc5X1","gpu":"NVIDIA GeForce RTX 5090","tariff_id":"41199"}]})";
        const auto actualPcs = api.computers(1, 1);
        require(actualPcs["computers"][0]["id"] == "3587", "real string computer ID retained");
        for (const char* invalidId : {"", "0", "00", "3587x", "PRIVATE", "2147483648", "99999999999999999999999999999999"}) {
            fake.next.body = Json{{"computers", Json::array({Json{{"id", invalidId}}})}}.dump();
            rejects([&] { api.computers(1, 1); }, "invalid string computer ID");
        }
        rejects([&] { api.games("", 0); }, "reject zero page");
        rejects([&] { api.games("", 1, 101); }, "reject excessive page size");
        rejects([&] { api.computers(0, 1); }, "reject zero game id");
        fake.next.body = R"({"token":"PRIVATE"})";
        rejects([&] { api.games("", 1); }, "wrong catalog schema");
        fake.next.body = R"([{"id":18446744073709551615,"name":"x"}])";
        rejects([&] { api.games("", 1); }, "reject overflowing game id");
        fake.next.body = R"({"computers":[{"id":"PRIVATE"}]})";
        rejects([&] { api.computers(1, 1); }, "wrong computer schema");
        fake.next = {401, R"({"token":"PRIVATE"})"};
        rejects([&] { api.games("", 1); }, "private HTTP response suppressed");
        fake.next = {200, "<html>PRIVATE</html>"};
        rejects([&] { api.games("", 1); }, "non-JSON response suppressed");
        rejects([&] { api.submitOtp("1111"); }, "otp cannot skip phone stage");
        fake.next = {200, R"({"form":"captcha","image":"Zg=="})"};
        require(fog::authStep(api.submitPhone("89991234567")) == AuthStep::Captcha, "phone requests captcha");
        auto phone = Json::parse(fake.requests.back().body);
        require(phone["phone"] == "+79991234567" && phone["deviceId"] == device,
            "phone and device serialized");
        require(phone["agreement"] == true && phone["subscribe"] == false,
            "consent request and no marketing subscription");
        require(!api.loggedIn() && fake.requests.back().token.empty(), "phone no prior token");
        fake.next.body = R"({"form":"otp"})";
        api.submitCaptcha("aB12");
        require(fake.requests.back().path == "/api/login/mts-captcha/", "captcha endpoint");
        rejects([&] { api.submitOtp("PRIVATE"); }, "invalid otp code suppressed");
        fake.next.body = R"({"success":false,"form":"otp","token":"PRIVATE"})";
        api.submitOtp("1111");
        require(!api.loggedIn(), "failed otp never authorized");
        fake.next.body = R"({"success":true,"token":"fixture-token"})";
        api.submitOtp("1111");
        require(api.loggedIn(), "successful otp authorized");
        const auto otp = Json::parse(fake.requests.back().body);
        require(otp["deviceName"] == "FogPlay NX Switch" && otp["appId"] == "android:ru.mts.fogplay", "otp profile");
        fake.next.body = "{}";
        api.userInfo();
        require(fake.requests.back().token == "fixture-token", "profile token forwarding");
        rejects([&] { api.submitPhone("PRIVATE"); }, "invalid account switch phone");
        require(api.loggedIn(), "invalid phone preserves current authorization");
        const auto beforeFinalCancel = fake.requests.size();
        api.cancelAll();
        rejects([&] { api.finishSession("fixture-rental"); }, "final cancellation prevents independent finish transport");
        rejects([&] { api.activeSessions(true); }, "final cancellation prevents independent verification transport");
        rejects([&] { api.userInfo(); }, "final cancellation prevents normal transport");
        require(fake.requests.size() == beforeFinalCancel, "no request reaches transport after final exit cancellation");
        api.clearToken();
        rejects([&] { api.userInfo(); }, "logged-out profile cannot request");
        fog::HttpClient http;
        rejects([&] { http.request("GET", "//bad-host/"); }, "absolute host rejected");
        rejects([&] { http.request("GET", "/api/?secret"); }, "query in path rejected");
        http.cancel();
        rejects([&] { http.request("GET", "/api/content/games/"); }, "cancel prevents next request");
        std::cout << "API offline tests passed: " << assertions << " assertions. No network, SMS or orders.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "API test failed: " << error.what() << '\n';
        return 1;
    }
}
