#include "features/auth/infrastructure/foghub_cookie_storage.hpp"
#include "core/domain/repository_error.hpp"

#include <nlohmann/json.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace fog::foghub;
using Json = nlohmann::json;

struct Temporary {
    std::filesystem::path directory;
    Temporary() {
        char pattern[] = "/tmp/foghub-cookie-storage-tests-XXXXXX";
        const auto created = mkdtemp(pattern);
        assert(created);
        directory = created;
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::string path() const { return (directory / "secret-cookies.json").string(); }
};

const SavedCookies fixture{"login:Exact Login@example.org", {
    "#HttpOnly_foghub.ru\tFALSE\t/\tTRUE\t0\tsession\tsecret-cookie"
}};

std::string contents(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    assert(file.good());
}

Json document(const SavedCookies& saved = fixture) {
    return {{"version", 1}, {"provider", "foghub"}, {"account", saved.account}, {"cookies", saved.cookies}};
}

template<class Function>
void fails(Function function) {
    bool failed = false;
    try { function(); }
    catch (const fog::RepositoryError& error) {
        assert(error.kind() == fog::RepositoryErrorKind::StorageFailure);
        const std::string message = error.what();
        assert(message == "FogHub saved sign-in storage failed");
        assert(message.find("secret") == std::string::npos);
        assert(message.find("Login") == std::string::npos);
        assert(message.find("/tmp/") == std::string::npos);
        failed = true;
    }
    assert(failed);
}

void roundtrip() {
    Temporary temporary;
    auto store = makeCookieStore(temporary.path());
    assert(!store->load());
    store->save(fixture);
    const auto loaded = makeCookieStore(temporary.path())->load();
    assert(loaded && loaded->account == fixture.account && loaded->cookies == fixture.cookies);
    const auto data = Json::parse(contents(temporary.path()));
    assert(data == document());
    assert(data.size() == 4 && !data.contains("password") && !data.contains("pin"));
    struct stat info{};
    assert(stat(temporary.path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    assert(chmod(temporary.path().c_str(), 0666) == 0);
    store->save({"login:OtherLogin", {}});
    assert(stat(temporary.path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    assert(store->load()->account == "login:OtherLogin" && store->load()->cookies.empty());
    store->clear();
    store->clear();
    assert(!store->load());
}

void invalidSchema() {
    Temporary temporary;
    auto store = makeCookieStore(temporary.path());
    std::vector<Json> invalid;
    for (const Json& version : {Json(0), Json(2), Json(1.0), Json("1"), Json(true), Json(nullptr)}) {
        auto data = document(); data["version"] = version; invalid.push_back(data);
    }
    for (const Json& provider : {Json("mts"), Json("FogHub"), Json(1), Json(nullptr)}) {
        auto data = document(); data["provider"] = provider; invalid.push_back(data);
    }
    for (const auto& key : {"version", "provider", "account", "cookies"}) {
        auto data = document(); data.erase(key); invalid.push_back(data);
    }
    auto extra = document(); extra["password"] = "secret-password"; invalid.push_back(extra);
    auto account = document(); account["account"] = 42; invalid.push_back(account);
    auto cookies = document(); cookies["cookies"] = "secret-cookie"; invalid.push_back(cookies);
    cookies["cookies"] = Json::array({false}); invalid.push_back(cookies);
    invalid.push_back(Json::array());
    invalid.push_back(nullptr);
    for (const auto& data : invalid) {
        writeFile(temporary.path(), data.dump());
        fails([&] { store->load(); });
    }
    for (const auto& bytes : {std::string{}, std::string("{"), std::string("{secret"),
            document().dump().substr(0, document().dump().size() - 1), document().dump() + "secret"}) {
        writeFile(temporary.path(), bytes);
        fails([&] { store->load(); });
    }
    auto bounded = document().dump();
    bounded.resize(128 * 1024, ' ');
    writeFile(temporary.path(), bounded);
    assert(store->load()->account == fixture.account);
    bounded += ' ';
    writeFile(temporary.path(), bounded);
    fails([&] { store->load(); });
    writeFile(temporary.path(), std::string(32000, '[') + "0" + std::string(32000, ']'));
    fails([&] { store->load(); });
}

void validationBounds() {
    Temporary temporary;
    auto store = makeCookieStore(temporary.path());
    store->save(fixture);
    const auto durable = contents(temporary.path());
    std::vector<SavedCookies> invalid;
    for (const auto& account : {std::string{}, std::string("login:"), std::string("42"),
            std::string("Login:exact"), "login:" + std::string(251, 'x'),
            std::string("login:secret\n"), std::string("login:secret\t"), std::string("login:secret\177"),
            std::string("login:secret\0name", 17)}) invalid.push_back({account, fixture.cookies});
    for (const auto& cookie : {std::string{}, std::string(4097, 'x'), std::string("secret\nALL"),
            std::string("secret\rALL"), std::string("secret\0ALL", 10)}) invalid.push_back({fixture.account, {cookie}});
    invalid.push_back({fixture.account, std::vector<std::string>(257, "cookie")});
    invalid.push_back({fixture.account, std::vector<std::string>(33, std::string(4096, 'x'))});
    // JSON escaping can exceed the file cap even when the raw strings fit.
    invalid.push_back({fixture.account, std::vector<std::string>(20, std::string(4096, '"'))});
    for (const auto& saved : invalid) {
        fails([&] { store->save(saved); });
        assert(contents(temporary.path()) == durable);
        writeFile(temporary.path(), document(saved).dump());
        fails([&] { store->load(); });
        writeFile(temporary.path(), durable);
    }
    SavedCookies maximum{"login:" + std::string(250, 'x'), std::vector<std::string>(256, "cookie")};
    maximum.cookies.front() = std::string(4096, 'x');
    store->save(maximum);
    const auto loaded = store->load();
    assert(loaded && loaded->account == maximum.account && loaded->cookies == maximum.cookies);
    fails([] { makeCookieStore(""); });
    fails([] { makeCookieStore(std::string("secret\0suffix", 13)); });
}

void recoveryAndClear() {
    Temporary temporary;
    auto store = makeCookieStore(temporary.path());
    // Partial temporary writes alone must never become credentials.
    writeFile(temporary.path() + ".tmp", document().dump());
    assert(!store->load());
    writeFile(temporary.path() + ".bak", document().dump());
    auto loaded = store->load();
    assert(loaded && loaded->account == fixture.account);
    assert(!std::filesystem::exists(temporary.path() + ".bak"));
    const SavedCookies latest{"login:Latest", {"cookie"}};
    store->save(latest);
    writeFile(temporary.path() + ".bak", document().dump());
    assert(store->load()->account == latest.account);
    // A corrupt primary remains authoritative; stale backup must not log in.
    writeFile(temporary.path(), "{");
    fails([&] { store->load(); });
    store->clear();
    assert(!std::filesystem::exists(temporary.path()));
    assert(!std::filesystem::exists(temporary.path() + ".tmp"));
    assert(!std::filesystem::exists(temporary.path() + ".bak"));
    assert(!store->load());
    // Explicit forget clears a backup without restoring it first.
    writeFile(temporary.path() + ".bak", document().dump());
    writeFile(temporary.path() + ".tmp", "partial");
    store->clear();
    assert(!store->load());
    assert(!std::filesystem::exists(temporary.path() + ".bak"));
    assert(!std::filesystem::exists(temporary.path() + ".tmp"));
}

void fileSafety() {
    Temporary temporary;
    const auto path = temporary.path();
    auto store = makeCookieStore(path);
    const auto target = (temporary.directory / "secret-target").string();
    writeFile(target, document().dump());
    assert(symlink(target.c_str(), path.c_str()) == 0);
    fails([&] { store->load(); });
    fails([&] { store->save(fixture); });
    assert(contents(target) == document().dump());
    store->clear();
    assert(std::filesystem::exists(target));
    assert(mkfifo(path.c_str(), 0600) == 0);
    fails([&] { store->load(); });
    fails([&] { store->save(fixture); });
    store->clear();
    assert(std::filesystem::create_directory(path));
    fails([&] { store->load(); });
    fails([&] { store->save(fixture); });
    fails([&] { store->clear(); });
    std::filesystem::remove(path);
    // Recovery must reject an auxiliary symlink or non-regular backup.
    assert(symlink(target.c_str(), (path + ".bak").c_str()) == 0);
    fails([&] { store->load(); });
    store->clear();
    assert(mkfifo((path + ".bak").c_str(), 0600) == 0);
    fails([&] { store->load(); });
    store->clear();
    assert(std::filesystem::create_directory(path + ".bak"));
    fails([&] { store->load(); });
    fails([&] { store->clear(); });
    std::filesystem::remove(path + ".bak");

    store->save(fixture);
    const auto durable = contents(path);
    // Unsafe temporary files fail before altering the primary or target.
    assert(symlink(target.c_str(), (path + ".tmp").c_str()) == 0);
    fails([&] { store->save({"login:Other", {"new-cookie"}}); });
    assert(contents(path) == durable && contents(target) == document().dump());
    std::filesystem::remove(path + ".tmp");
    assert(mkfifo((path + ".tmp").c_str(), 0600) == 0);
    fails([&] { store->save(fixture); });
    assert(contents(path) == durable);
    std::filesystem::remove(path + ".tmp");
    assert(link(target.c_str(), (path + ".tmp").c_str()) == 0);
    fails([&] { store->save(fixture); });
    assert(contents(path) == durable && contents(target) == document().dump());
    std::filesystem::remove(path + ".tmp");
    // Auxiliary removal failure leaves the primary intact.
    assert(std::filesystem::create_directory(path + ".tmp"));
    fails([&] { store->clear(); });
    assert(contents(path) == durable);
    std::filesystem::remove(path + ".tmp");
    assert(store->load()->account == fixture.account);
    store->clear();

    auto missingParent = makeCookieStore((temporary.directory / "missing" / "cookies.json").string());
    assert(!missingParent->load());
    fails([&] { missingParent->save(fixture); });
    missingParent->clear();
}
}

int main() {
    roundtrip();
    invalidSchema();
    validationBounds();
    recoveryAndClear();
    fileSafety();
    std::cout << "FogHub cookie storage tests passed\n";
}
