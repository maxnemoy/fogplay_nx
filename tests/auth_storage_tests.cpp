#include "features/session/infrastructure/api.hpp"
#include "features/auth/infrastructure/auth_storage.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int assertions = 0;
const std::string device = "11111111111141118111111111111111";
const std::string otherDevice = "22222222222242228222222222222222";
void require(bool result, const char* label) {
    ++assertions;
    if (!result) throw std::runtime_error(label);
}
template<class Action> void rejects(Action action, const char* label) {
    bool failed = false;
    try { action(); }
    catch (const std::exception& error) {
        failed = true;
        const std::string text = error.what();
        require(text.find("PRIVATE") == std::string::npos && text.find("79991234567") == std::string::npos,
            "fixed diagnostic never echoes sensitive data");
    }
    require(failed, label);
}
struct Temporary {
    std::filesystem::path directory;
    Temporary() {
        char pattern[] = "/tmp/fogplay-auth-tests-XXXXXX";
        const auto created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("Cannot create test directory");
        directory = created;
    }
    ~Temporary() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    std::string path() const { return (directory / "auth.json").string(); }
};
void writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    if (!file) throw std::runtime_error("Cannot write fixture");
}
std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Fake {
    fog::Api* api = nullptr;
    int authRequests = 0;
    int validationRequests = 0;
    bool offline = false;
    bool otp = false;
    bool anonymousDuringCheck = true;
    std::string forwardedToken;
    fog::HttpResponse profile{200, R"({"authorized":true})"};
    fog::HttpResponse operator()(const std::string& method, const std::string& path,
        const fog::Query&, const std::string& body, const std::string& token) {
        if (path == "/api/login/user-info/") {
            require(method == "GET" && body.empty(), "validation uses GET without SMS body");
            ++validationRequests;
            forwardedToken = token;
            anonymousDuringCheck = anonymousDuringCheck && api && !api->loggedIn();
            if (offline) throw std::runtime_error("PRIVATE transport token or body");
            return profile;
        }
        ++authRequests;
        if (path == "/api/login/mts-phone/" && otp) return {200, R"({"form":"otp"})"};
        return {200, R"({"success":true,"token":"fixture-token"})"};
    }
};
void signIn(fog::Api& api) { api.submitPhone("89991234567"); }
}
int main() {
    try {
        Temporary temporary;
        const auto path = temporary.path();
        std::string account;
        {
            Fake fake;
            fake.otp = true;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            api.submitPhone("89991234567");
            require(!std::filesystem::exists(path), "phone stage never persisted");
            api.submitOtp("1111");
            account = api.accountIdentity();
            require(api.loggedIn() && api.authStorageError().empty(), "fresh login usable and saved");
            require(std::filesystem::exists(path), "token saved before auth method returns");
            require(fake.validationRequests == 0, "fresh save needs no extra live request");
            const auto bytes = readFile(path);
            require(bytes.find("79991234567") == std::string::npos && bytes.find("\"code\"") == std::string::npos,
                "store has no raw phone or SMS code");
            const auto data = fog::Json::parse(bytes);
            require(data.size() == 4 && data["version"] == 1 && data["token"] == "fixture-token" &&
                data["account"] == account && data["deviceId"] == device, "versioned credential fields");
            require(account.size() == 64, "hashed account identity retained");
            struct stat info{};
            require(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "0600 saved file");
            require(!std::filesystem::exists(path + ".tmp"), "atomic write leaves no temporary file");
        }
        require(std::filesystem::exists(path), "destruction preserves persisted auth");
        {
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            fake.api = &api;
            api.configureAuthStorage(path);
            require(api.restoreAuth(), "restart validates persisted token");
            require(api.loggedIn() && api.accountIdentity() == account, "restore same account identity");
            require(fake.anonymousDuringCheck && fake.forwardedToken == "fixture-token" &&
                fake.validationRequests == 1 && fake.authRequests == 0, "validation before logged in, no SMS");
            const auto generation = api.authGeneration();
            api.clearToken();
            require(!api.loggedIn() && api.accountIdentity().empty() && api.authGeneration() > generation,
                "RAM clear invalidates prior auth generation");
            require(std::filesystem::exists(path), "RAM clear leaves token on disk");
            require(api.restoreAuth(), "RAM cleared token can restore");
            api.forgetAuth();
            require(!api.loggedIn() && !std::filesystem::exists(path) && api.authStorageError().empty(),
                "explicit forget removes file and clears RAM");
            const auto calls = fake.validationRequests;
            require(!api.restoreAuth() && api.authStorageError().empty() && calls == fake.validationRequests,
                "missing file guest result without requests");
        }
        const fog::AuthCredentials credentials{"fixture-token", account, device};
        for (const auto& response : {fog::HttpResponse{401, "PRIVATE"}, {200, R"({"authorized":false})"},
            {503, "PRIVATE"}, {200, "PRIVATE malformed"}, {200, "{}"}, {200, R"({"authorized":1})"}}) {
            fog::saveAuthCredentials(path, credentials);
            const auto before = readFile(path);
            Fake fake;
            fake.profile = response;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            fake.api = &api;
            api.configureAuthStorage(path);
            require(!api.restoreAuth(), "unverified credentials cannot log in");
            const bool rejected = response.status == 401 || response.body == R"({"authorized":false})";
            require(!api.loggedIn() && api.accountIdentity().empty() && !api.authStorageError().empty(),
                "failed validation keeps RAM unauthenticated");
            require(std::filesystem::exists(path) != rejected, "only explicit credential rejection deletes saved sign-in");
            if (!rejected) {
                require(readFile(path) == before, "transient validation preserves exact saved credentials");
                fake.profile = {200, R"({"authorized":true})"};
                require(api.restoreAuth() && api.accountIdentity() == account,
                    "later verification recovers original account without SMS");
            }
        }
        {
            fog::saveAuthCredentials(path, credentials);
            Fake fake;
            fake.offline = true;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            fake.api = &api;
            api.configureAuthStorage(path);
            require(!api.restoreAuth() && std::filesystem::exists(path) && !api.loggedIn(),
                "offline verification preserves saved sign-in without authorizing RAM");
            fake.offline = false;
            require(api.restoreAuth() && api.loggedIn() && api.accountIdentity() == account,
                "offline restore can recover original account on next validation");
            require(api.authStorageError().find("PRIVATE") == std::string::npos, "transport detail suppressed");
        }
        {
            fog::saveAuthCredentials(path, credentials);
            Fake fake;
            fog::Api api(otherDevice, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            require(!api.restoreAuth() && fake.validationRequests == 0 && !std::filesystem::exists(path),
                "device mismatch deletes credential without requests");
        }
        for (const auto& bytes : {std::string("PRIVATE malformed"), std::string(65537, 'x'),
            fog::Json{{"version",2},{"token","fixture-token"},{"account",account},{"deviceId",device}}.dump(),
            fog::Json{{"version",1},{"token","bad\r\nPRIVATE"},{"account",account},{"deviceId",device}}.dump(),
            fog::Json{{"version",1},{"token","fixture-token"},{"account","bad"},{"deviceId",device}}.dump(),
            fog::Json{{"version",1},{"token","fixture-token"},{"account",account},{"deviceId",device},{"phone","PRIVATE"}}.dump()}) {
            writeFile(path, bytes);
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            require(!api.restoreAuth() && !api.authStorageError().empty() && fake.validationRequests == 0 &&
                !std::filesystem::exists(path), "bad bounded file removed without network");
        }
        {
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            signIn(api);
            rejects([&] { api.submitPhone("PRIVATE"); }, "invalid account switch phone rejected");
            require(api.loggedIn() && std::filesystem::exists(path), "invalid phone does not discard current sign-in");
            signIn(api);
            fake.otp = true;
            api.submitPhone("89997654321");
            require(!api.loggedIn() && !std::filesystem::exists(path), "incomplete switch cannot roll back account");
        }
        {
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage((temporary.directory / "missing" / "auth.json").string());
            signIn(api);
            require(api.loggedIn() && !api.authStorageError().empty(), "save failure preserves fresh RAM login with warning");
        }
        {
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            signIn(api);
            std::filesystem::remove(path);
            std::filesystem::create_directory(path);
            writeFile(path + "/obstruction", "fixture");
            rejects([&] { api.forgetAuth(); }, "remove error reported");
            require(!api.loggedIn() && !api.authStorageError().empty() && std::filesystem::is_directory(path),
                "failed forget still clears RAM and does not claim SD cleared");
            const auto requests = fake.authRequests;
            rejects([&] { signIn(api); }, "switch blocked when old store cannot be removed");
            require(fake.authRequests == requests, "delete failure prevents new login request");
            std::filesystem::remove_all(path);
        }
        {
            const auto target = (temporary.directory / "target.json").string();
            fog::saveAuthCredentials(target, credentials);
            std::filesystem::create_symlink(target, path);
            rejects([&] { fog::loadAuthCredentials(path); }, "symlink store rejected");
            fog::removeAuthCredentials(path);
            require(std::filesystem::exists(target), "forget symlink never deletes target");
            std::filesystem::create_symlink(target, path + ".tmp");
            rejects([&] { fog::saveAuthCredentials(path, credentials); }, "symlink temporary write rejected");
            require(fog::loadAuthCredentials(target)->token == "fixture-token", "temporary symlink target unchanged");
            fog::removeAuthCredentials(path);
            require(mkfifo(path.c_str(), 0600) == 0, "FIFO fixture");
            rejects([&] { fog::loadAuthCredentials(path); }, "FIFO store rejected without blocking");
            fog::removeAuthCredentials(path);
        }
        {
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            writeFile(path + ".tmp", "interrupted fixture");
            require(chmod((path + ".tmp").c_str(), 0666) == 0, "permissive temporary fixture");
            fog::saveAuthCredentials(path, credentials);
            struct stat info{};
            require(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
                "reused temporary file permissions restricted");
            signIn(api);
            require(chmod(temporary.directory.c_str(), 0300) == 0, "commit failure fixture");
            rejects([&] { api.forgetAuth(); }, "directory commit failure reported after deletion");
            require(chmod(temporary.directory.c_str(), 0700) == 0, "restore test directory permissions");
            require(!api.loggedIn() && !api.authStorageError().empty() && !std::filesystem::exists(path),
                "uncertain durable removal does not claim logout success");
        }
        {
            fog::saveAuthCredentials(path, credentials);
            std::filesystem::rename(path, path + ".bak");
            writeFile(path + ".tmp", "PRIVATE-interrupted-write");
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            fake.api = &api;
            api.configureAuthStorage(path);
            require(api.restoreAuth() && fake.validationRequests == 1 && fake.anonymousDuringCheck,
                "sole backup restored then remotely validated before authorization");
            writeFile(path + ".bak", "PRIVATE-obsolete-credential");
            api.forgetAuth();
            require(!std::filesystem::exists(path) && !std::filesystem::exists(path + ".bak") &&
                !std::filesystem::exists(path + ".tmp"), "forget clears every credential copy");
            require(!api.restoreAuth() && fake.validationRequests == 1, "forgotten backup cannot sign back in");
        }
        {
            writeFile(path + ".bak", "PRIVATE-malformed");
            Fake fake;
            fog::Api api(device, [&fake](auto&&... args) { return fake(args...); });
            api.configureAuthStorage(path);
            require(!api.restoreAuth() && fake.validationRequests == 0 && !std::filesystem::exists(path) &&
                !std::filesystem::exists(path + ".bak"), "malformed restored backup removed without network");
        }
        require(!fog::validAuthToken("") && !fog::validAuthToken(std::string(8193, 'a')) &&
            !fog::validAuthToken("bad\n") && fog::validAuthToken(std::string(8192, 'a')), "token bounds");
        auto oversized = credentials;
        oversized.token.assign(8193, 'a');
        rejects([&] { fog::saveAuthCredentials(path, oversized); }, "invalid token cannot persist");
        std::cout << "Auth storage offline tests passed: " << assertions << " assertions. No network, SMS or orders.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Auth storage test failed: " << error.what() << '\n';
        return 1;
    }
}
