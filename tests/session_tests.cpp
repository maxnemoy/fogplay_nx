#include "features/session/infrastructure/api.hpp"
#include "features/session/infrastructure/mts_rental_repository.hpp"
#include "features/session/infrastructure/rental_journal_storage.hpp"
#include "features/session/application/session.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int assertions = 0;
fog::EntityId mts(std::string value) { return {fog::ProviderId::Mts, std::move(value)}; }
void require(bool condition, const char* label) {
    ++assertions;
    if (!condition) throw std::runtime_error(label);
}
template<class Action> void rejects(Action action, const char* label) {
    bool rejected = false;
    try { action(); } catch (const std::exception& error) {
        rejected = true;
        require(std::string(error.what()).find("PRIVATE") == std::string::npos, "no private error data");
    }
    require(rejected, label);
}
struct Request {
    std::string method, path, body, token;
    fog::Query query;
    fog::RequestOptions options;
};
struct Fake {
    std::vector<Request> requests;
    fog::HttpResponse active{200, "[]"};
    fog::HttpResponse finish{200, R"({"ok":true})"};
    bool finishTimeout = false;
    bool activeTimeout = false;
    std::string token = "PRIVATE_TOKEN_NOT_FOR_DISK";
    fog::HttpResponse call(const std::string& method, const std::string& path,
        const fog::Query& query, const std::string& body, const std::string& auth,
        fog::RequestOptions options) {
        requests.push_back({method, path, body, auth, query, options});
        if (path == "/api/login/mts-phone/")
            return {200, fog::Json{{"success", true}, {"token", token}}.dump()};
        if (path == "/api/order/get-active-sessions/") {
            if (activeTimeout) throw std::runtime_error("fixture timeout PRIVATE");
            return active;
        }
        if (path == "/api/order/finish-session/") {
            if (finishTimeout) throw std::runtime_error("fixture timeout PRIVATE");
            return finish;
        }
        throw std::runtime_error("Unexpected test endpoint");
    }
    void sessions(std::initializer_list<std::string> hashes) {
        auto data = fog::Json::array();
        for (const auto& hash : hashes) data.push_back({{"hash", hash}, {"computer_name", "Fixture"}});
        active = {200, data.dump()};
    }
    std::size_t finishCount() const {
        std::size_t count = 0;
        for (const auto& request : requests) if (request.path == "/api/order/finish-session/") ++count;
        return count;
    }
};
std::string readFile(const std::string& path) {
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
void writeFile(const std::string& path, const std::string& contents) {
    std::ofstream stream(path);
    stream << contents;
    if (!stream) throw std::runtime_error("Fixture file write failed");
}
}

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("fogplay-session-tests-" + std::to_string(getpid()));
    fs::create_directories(root);
    try {
        Fake fake;
        fog::Api api("11111111111141118111111111111111", {},
            [&fake](auto&&... args) { return fake.call(args...); });
        fog::MtsRentalRepository rentals(api);
        rejects([&] { api.activeSessions(); }, "sessions require authentication");
        try { rentals.activeSessions(); require(false, "repository requires authentication"); }
        catch (const fog::RepositoryError& error) {
            require(error.kind() == fog::RepositoryErrorKind::NotAuthenticated, "typed unauthenticated repository result");
        }
        rejects([&] { api.finishSession("owned"); }, "finish requires authentication");
        api.submitPhone("89991234567");
        fake.sessions({"hash:with+/punctuation=", "unrelated"});
        require(api.activeSessions().size() == 2, "active session parse");
        const auto list = fake.requests.back();
        require(list.method == "GET" && list.query.empty() && list.body.empty(), "active endpoint shape");
        require(list.token == fake.token && list.options.timeoutMs == 45000, "normal request auth and timeout");
        for (const auto& invalid : std::vector<std::string>{"", "a\nb", std::string(1025, 'x'), std::string("a\0b", 3)})
            rejects([&] { api.finishSession(invalid); }, "reject invalid hash");
        for (const char* malformed : {"{}", R"([{"hash":"","computer_name":"x"}])",
            R"([{"hash":5,"computer_name":"x"}])", R"([{"hash":"owned","computer_name":1}])"}) {
            fake.active.body = malformed;
            rejects([&] { api.activeSessions(); }, "invalid active session schema");
            try { rentals.activeSessions(); require(false, "repository rejects malformed active list"); }
            catch (const fog::RepositoryError& error) {
                require(error.kind() == fog::RepositoryErrorKind::InvalidResponse, "invalid active list cannot become absence");
            }
        }
        fake.active.body = R"([{"hash":"owned","computer_name":null},{"hash":"other"}])";
        require(api.activeSessions().at(0).at("computer_name") == "" &&
            api.activeSessions().at(1).at("computer_name") == "", "nullable and missing display names do not hide sessions");
        fake.active.body = R"([{"hash":"owned","computer_id":314}])";
        require(rentals.activeSessions().front().computerId == mts("314"), "active rental maps optional computer identity");
        fake.active.body = R"([{"hash":"owned","computer_id":"314"}])";
        require(rentals.activeSessions().front().computerId == mts("314"), "active rental maps decimal-string computer identity");
        fake.active.body = R"([{"hash":"owned","computer_id":null}])";
        require(!rentals.activeSessions().front().computerId, "absent computer metadata never invents identity");
        fake.sessions({});
        fake.finish.body = R"({"ok":"true","token":"PRIVATE"})";
        rejects([&] { api.finishSession("owned"); }, "finish requires boolean ok");
        fake.finish.body = R"({"ok":false})";
        require(!api.finishSession("owned"), "explicit finish rejection");
        fake.finish = {200, R"({"ok":true,"url":"ignored"})"};

        const auto path = (root / "pending.json").string();
        fog::SessionGuard guard(rentals, fog::makeMtsSessionJournal(path));
        require(guard.pendingRental().value.empty() && !guard.persistent(), "no initial journal");
        const std::string hash = "hash:with+/punctuation=";
        guard.adopt(mts(hash));
        require(guard.ownsSession() && guard.persistent() && fs::exists(path), "adopt durable before connect");
        require(fog::Json::parse(readFile(path)).at("hash") == hash, "opaque hash survives journal");
        require(readFile(path).find("PRIVATE") == std::string::npos, "token never journaled");
        rejects([&] { guard.adopt(mts("unrelated")); }, "cannot overwrite pending session");
        fake.sessions({hash, "unrelated"});
        api.cancel();
        auto result = guard.finishAttempt();
        require(!result.terminated && fs::exists(path), "ack alone never confirms active termination");
        const auto finishRequest = fake.requests[fake.requests.size() - 2];
        require(finishRequest.method == "POST" && finishRequest.body.empty() && finishRequest.token == fake.token,
            "finish empty body authenticated POST");
        require(finishRequest.query.size() == 1 && finishRequest.query[0].first == "hash" &&
            finishRequest.query[0].second == hash, "finish targets only owned opaque hash");
        require(finishRequest.options.timeoutMs == 4000 && finishRequest.options.connectTimeoutMs <= 4000,
            "finish bounded independently of normal cancellation");
        require(fake.requests.back().options.timeoutMs == 4000, "fresh verification bounded shutdown transport");
        fake.sessions({"unrelated"});
        result = guard.finishAttempt();
        require(result.terminated && result.journalCleared && !fs::exists(path) && guard.pendingRental().value.empty(),
            "ack plus fresh absence clears durable marker");

        // An attributed but not-yet-active rental must survive timeout + absence.
        guard.adopt(mts("still-preparing"));
        fake.finishTimeout = true;
        fake.sessions({});
        result = guard.finishAttempt();
        require(!result.terminated && guard.persistent() && fs::exists(path),
            "timeout and absence before activation never clear marker");
        fake.finishTimeout = false;
        require(guard.finishAttempt().terminated, "acknowledged stop plus absence resolves preparation");

        // A prior active observation makes subsequent fresh absence meaningful.
        guard.adopt(mts("timed-out"));
        fake.finishTimeout = true;
        fake.activeTimeout = true;
        result = guard.finishAttempt();
        require(!result.terminated && guard.persistent() && fs::exists(path), "ambiguous finish plus verification failure retained");
        require(result.message.find("PRIVATE") == std::string::npos, "transport error remains sanitized");
        fake.activeTimeout = false;
        fake.sessions({"timed-out"});
        require(!guard.finishAttempt().terminated, "timeout and active hash remains unresolved");
        fake.sessions({});
        result = guard.finishAttempt();
        require(result.terminated && result.journalCleared, "timeout reconciled by fresh absence");
        fake.finishTimeout = false;

        guard.adopt(mts("unidentified-entry"));
        fake.active.body = R"([{"hash":null,"computer_name":"Fixture"}])";
        require(!guard.finishAttempt().terminated && guard.persistent(),
            "malformed identity cannot prove session absence even after acknowledged finish");
        fake.sessions({});
        require(guard.finishAttempt().terminated, "valid fresh absence resolves acknowledged finish");

        guard.adopt(mts("rejected"));
        fake.finish = {200, R"({"ok":false})"};
        result = guard.finishAttempt();
        require(!result.terminated && fs::exists(path), "explicit rejection and absence not arbitrary termination proof");
        fake.finish = {401, "PRIVATE"};
        require(!guard.finishAttempt().terminated, "HTTP auth rejection and absence not termination proof");
        fake.finish = {200, R"({"ok":true})"};
        fake.activeTimeout = true;
        require(!guard.finishAttempt().terminated && fs::exists(path), "successful finish failed verification retains marker");
        fake.activeTimeout = false;
        require(guard.finishAttempt().terminated, "retry confirms prior acknowledged finish");

        guard.adopt(mts("recovery-hash"));
        fog::SessionGuard recovered(rentals, fog::makeMtsSessionJournal(path));
        require(recovered.pendingRental().value == "recovery-hash" && recovered.persistent() && !recovered.ownsSession(),
            "restart loads marker but no account ownership");
        auto before = fake.finishCount();
        require(!recovered.finishAttempt().terminated && fake.finishCount() == before, "restart cannot finish without match");
        fake.sessions({"another-account-session"});
        require(!recovered.recoverMatchingSession() && fs::exists(path), "wrong account never clears saved marker");
        require(!recovered.finishAttempt().terminated && fake.finishCount() == before, "wrong account never calls finish");
        fake.sessions({"recovery-hash", "another-account-session"});
        require(recovered.recoverMatchingSession() && recovered.ownsSession(), "exact saved hash restores ownership");
        fake.sessions({"another-account-session"});
        require(recovered.finishAttempt().terminated, "recovery stops saved hash only");
        require(fake.requests[fake.requests.size()-2].query[0].second == "recovery-hash", "recovery targeted hash");

        fog::SessionGuard accountSwitch(rentals, fog::makeMtsSessionJournal((root / "switch.json").string()));
        accountSwitch.adopt(mts("old-account"));
        before = fake.finishCount();
        fake.token = "PRIVATE_DIFFERENT_ACCOUNT";
        api.submitPhone("89991234567");
        fake.sessions({});
        require(!accountSwitch.ownsSession() && !accountSwitch.finishAttempt().terminated && fake.finishCount() == before,
            "same-run account switch invalidates ownership");
        require(!accountSwitch.recoverMatchingSession() && fs::exists(root / "switch.json"), "absent switched-account hash preserved");

        const auto malformedPath = (root / "malformed.json").string();
        writeFile(malformedPath, "PRIVATE_MALFORMED");
        fog::SessionGuard malformed(rentals, fog::makeMtsSessionJournal(malformedPath));
        require(!malformed.journalError().empty() && malformed.persistent(), "malformed journal blocks connection visibly");
        rejects([&] { malformed.adopt(mts("new")); }, "malformed journal cannot be overwritten");
        require(!malformed.finishAttempt().terminated && readFile(malformedPath) == "PRIVATE_MALFORMED", "malformed journal not silently removed");
        require(malformed.journalError().find("PRIVATE") == std::string::npos, "journal errors sanitized");
        const auto directoryPath = (root / "directory.json").string();
        fs::create_directory(directoryPath);
        fog::SessionGuard unreadable(rentals, fog::makeMtsSessionJournal(directoryPath));
        require(!unreadable.journalError().empty() && unreadable.persistent(), "nonregular journal preserved");
        rejects([&] { unreadable.adopt(mts("new")); }, "unreadable journal cannot be overwritten");

        fog::SessionGuard failedWrite(rentals, fog::makeMtsSessionJournal((root / "missing" / "pending.json").string()));
        rejects([&] { failedWrite.adopt(mts("cannot-connect")); }, "failed write blocks live connect");
        require(failedWrite.ownsSession() && !failedWrite.journalError().empty(), "failed durable write blocks media but retains stop authority");
        before = fake.finishCount();
        result = failedWrite.finishAttempt();
        require(result.terminated && result.journalCleared && fake.finishCount() == before+1,
            "selected rental can be stopped even when journal creation failed");

        const auto clearPath = (root / "clear.json").string();
        fog::SessionGuard failedClear(rentals, fog::makeMtsSessionJournal(clearPath));
        failedClear.adopt(mts("clear-failure"));
        fs::remove(clearPath);
        fs::create_directory(clearPath);
        result = failedClear.finishAttempt();
        require(result.terminated && !result.journalCleared && !failedClear.pendingRental().value.empty(),
            "journal clear failure reported separately from verified termination");
        require(fs::is_directory(clearPath), "unexpected journal directory not removed");

        const auto commitPath = (root / "commit-failure.json").string();
        fog::SessionGuard failedCommit(rentals, fog::makeMtsSessionJournal(commitPath));
        failedCommit.adopt(mts("commit-failure"));
        fake.sessions({});
        require(chmod(root.c_str(), 0300) == 0, "session clear commit failure fixture");
        const auto commitResult = failedCommit.finishAttempt();
        require(chmod(root.c_str(), 0700) == 0, "restore session directory permissions");
        require(commitResult.terminated && !commitResult.journalCleared && failedCommit.ownsSession() &&
            failedCommit.pendingRental().value == "commit-failure", "clear commit failure retains in-memory stop ownership");
        require(fs::exists(commitPath) && readFile(commitPath).find("commit-failure") != std::string::npos,
            "clear commit failure restores conservative disk marker");
        require(failedCommit.finishAttempt().journalCleared && !fs::exists(commitPath),
            "retry clears rental marker only after commit succeeds");
        const auto backupPath = (root / "backup.json").string();
        writeFile(backupPath + ".bak", fog::Json{{"version", 1}, {"hash", "backup-session"}}.dump());
        writeFile(backupPath + ".tmp", "PRIVATE-interrupted-write");
        fog::SessionGuard backupRecovered(rentals, fog::makeMtsSessionJournal(backupPath));
        require(backupRecovered.pendingRental().value == "backup-session" && backupRecovered.persistent() &&
            !backupRecovered.ownsSession(), "sole backup restores session marker without inferring account ownership");
        fake.sessions({"backup-session"});
        require(backupRecovered.recoverMatchingSession(), "backup session requires fresh exact account match");
        fake.sessions({});
        require(backupRecovered.finishAttempt().journalCleared, "restored backup can be stopped and cleared");
        fog::SessionGuard backupCleared(rentals, fog::makeMtsSessionJournal(backupPath));
        require(backupCleared.pendingRental().value.empty() && !fs::exists(backupPath + ".bak") &&
            !fs::exists(backupPath + ".tmp"), "clear removes auxiliaries without resurrecting rental");
        writeFile(backupPath + ".bak", "PRIVATE-malformed");
        fog::SessionGuard badBackup(rentals, fog::makeMtsSessionJournal(backupPath));
        require(!badBackup.journalError().empty() && badBackup.persistent(), "malformed restored backup blocks connection");
        rejects([&] { badBackup.adopt(mts("other")); }, "malformed backup retained for recovery");
        require(readFile(backupPath) == "PRIVATE-malformed", "malformed backup is preserved unchanged");
        fs::remove_all(root);
        std::cout << "Session offline tests passed: " << assertions << " assertions. No network, SMS or paid requests.\n";
        return 0;
    } catch (const std::exception& error) {
        fs::remove_all(root);
        std::cerr << "Session test failed: " << error.what() << '\n';
        return 1;
    }
}
