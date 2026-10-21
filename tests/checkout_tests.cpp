#include "features/session/infrastructure/api.hpp"
#include "features/session/application/checkout.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action, const char* message) {
    bool rejected = false;
    try { action(); } catch (...) { rejected = true; }
    require(rejected, message);
}
std::string parameter(const fog::Query& query, const std::string& key) {
    for (const auto& item : query) if (item.first == key) return item.second;
    return {};
}
struct Fixture {
    std::string directory;
    std::string checkoutPath, sessionPath;
    fog::Json active = fog::Json::array();
    fog::Json created = {{"orderId", 42}, {"status", 2}};
    fog::Json status = {{"id", 42}, {"status", 2}, {"hash", "fixture-session"}};
    bool createTimeout = false, statusTimeout = false, activeTimeout = false, finishTimeout = false, rejectedFinish = false, finishActive = false;
    int creates = 0, checks = 0, finishes = 0;
    fog::Api api;
    Fixture() : api("11111111111141118111111111111111", {},
        [this](const std::string& method, const std::string& path, const fog::Query& query,
            const std::string& body, const std::string& token, fog::RequestOptions options) {
            if (path == "/api/login/mts-phone/") return fog::HttpResponse{200, R"({"form":"otp"})"};
            if (path == "/api/login/mts-otp/") return fog::HttpResponse{200, R"({"success":true,"token":"PRIVATE-token"})"};
            require(token == "PRIVATE-token", "authenticated request");
            if (path == "/api/order/create/") {
                ++creates;
                require(method == "POST" && body.empty(), "empty body create POST");
                require(options.timeoutMs == 30000 && options.connectTimeoutMs == 15000, "creation has independent 30 second budget");
                require(parameter(query, "computerId") == "3587" && parameter(query, "tariffId") == "41199", "selected tariff and computer");
                require(parameter(query, "gameId") == "175" && parameter(query, "steamacc") == "1", "game and Steam choice");
                require(parameter(query, "is_tv") == "1" && parameter(query, "gamepad_type") == "physical", "TV launcher with physical gamepad");
                require(parameter(query, "price").empty() && parameter(query, "auto").empty(), "no invented price or automatic repeat");
                std::ifstream intent(checkoutPath);
                const std::string marker((std::istreambuf_iterator<char>(intent)), {});
                require(marker.find("orderId") != std::string::npos && marker.find("PRIVATE") == std::string::npos &&
                    marker.find("79991234567") == std::string::npos, "durable intent before POST without token or phone");
                if (createTimeout) throw std::runtime_error("PRIVATE transport detail");
                return fog::HttpResponse{200, created.dump()};
            }
            if (path == "/api/order/status/") {
                ++checks;
                require(method == "GET" && parameter(query, "id") == "42", "recorded order status GET");
                require(options.timeoutMs == 4000, "bounded status request");
                if (statusTimeout) throw std::runtime_error("PRIVATE response");
                return fog::HttpResponse{200, status.dump()};
            }
            if (path == "/api/order/get-active-sessions/") {
                if (activeTimeout) throw std::runtime_error("PRIVATE active timeout");
                return fog::HttpResponse{200, active.dump()};
            }
            if (path == "/api/order/finish-session/") {
                ++finishes;
                require(method == "POST" && parameter(query, "hash") == "fixture-session", "only recorded hash stopped");
                if (finishTimeout) throw std::runtime_error("PRIVATE finish timeout");
                if (rejectedFinish) return fog::HttpResponse{403, "PRIVATE"};
                if (!finishActive) active = fog::Json::array();
                return fog::HttpResponse{200, R"({"ok":true})"};
            }
            if (path == "/api/content/games/") {
                require(parameter(query, "game_id") == "175", "game detail query");
                return fog::HttpResponse{200, R"([{"id":175,"name":"Fixture game"}])"};
            }
            if (path == "/api/content/computer-auto/") {
                require(parameter(query, "steamacc") == "1" && parameter(query, "no_cache") == "1", "server computer recommendations");
                return fog::HttpResponse{200, R"({"min_price":[{"id":3587,"tariff_id":41199}],"optimum":[],"max_power":[]})"};
            }
            if (path == "/api/order/history/") return fog::HttpResponse{200, "[]"};
            throw std::runtime_error("Unexpected fixture request");
        }) {
        char pattern[] = "/tmp/fog-checkout-XXXXXX";
        const auto path = mkdtemp(pattern);
        if (!path) throw std::runtime_error("Cannot create test directory");
        directory = path;
        checkoutPath = directory + "/checkout.json";
        sessionPath = directory + "/session.json";
        login();
    }
    ~Fixture() {
        std::remove(checkoutPath.c_str()); std::remove((checkoutPath + ".tmp").c_str());
        std::remove((checkoutPath + ".bak").c_str()); std::remove((sessionPath + ".bak").c_str());
        std::remove(sessionPath.c_str()); std::remove((sessionPath + ".tmp").c_str());
        rmdir(directory.c_str());
    }
    void login(const std::string& phone = "89991234567") { api.submitPhone(phone); api.submitOtp("1111"); }
    void makeActive() { active = fog::Json::array({{{"hash", "fixture-session"}, {"computer_name", "Fixture PC"}}}); }
    fog::CreateOrderRequest selection() { return {3587, 41199, 175, true}; }
    fog::CheckoutResult settle(fog::CheckoutGuard& checkout) {
        require(checkout.poll().state == fog::CheckoutState::Pending, "first readiness observation remains pending");
        require(checkout.poll().state == fog::CheckoutState::Pending, "second readiness observation remains pending");
        return checkout.poll();
    }
};
}
int main() {
    try {
        using fog::CheckoutState;
        {
            Fixture f;
            require(f.api.accountIdentity() == "aaa3a8d51bbd45911d65744e547c0b7094464669438951c04b8a86c56e1ce8db", "SHA256 normalized phone identity");
            require(f.api.gameDetail(175).at("id") == 175, "game detail response");
            require(f.api.computerAuto(175, true).at("min_price").size() == 1, "computer auto response");
            require(f.api.orderHistory().empty(), "history response");
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            require(checkout.create(f.selection()).state == CheckoutState::Pending, "create recorded");
            rejects([&] { checkout.create(f.selection()); }, "duplicate creation blocked");
            f.makeActive();
            require(f.settle(checkout).state == CheckoutState::Ready && sessions.ownsSession() && sessions.persistent(), "paid exact active hash adopted durably");
            f.api.cancel();
            const int checks = f.checks;
            require(checkout.stop().state == CheckoutState::Idle && !checkout.unresolved(), "shutdown stops rental and clears both journals");
            require(f.finishes == 1 && f.checks == checks, "known owned stop does not need order status");
        }
        {
            Fixture f; f.createTimeout = true;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            require(checkout.create(f.selection()).state == CheckoutState::RecoveryRequired, "timeout ambiguous");
            require(checkout.result().message.find("PRIVATE") == std::string::npos, "private transport detail suppressed");
            fog::CheckoutGuard recovered(f.api, sessions, f.checkoutPath);
            rejects([&] { recovered.create(f.selection()); }, "restart ambiguous outcome blocks POST");
            recovered.stop(); recovered.poll(true);
            require(f.creates == 1 && f.checks == 0 && f.finishes == 0, "no retry or arbitrary stop without order ID");
            require(recovered.acknowledgeManualResolution().state == CheckoutState::Idle, "explicit manual account resolution");
        }
        {
            Fixture f; f.created = {{"orderId", 42}, {"paymentUrl", "https://payment.example.test/pay"}, {"paymentAmount", "199"}};
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            require(checkout.create(f.selection()).state == CheckoutState::PaymentRequired, "typed external payment action");
            f.status = {{"id", 42}, {"status", 1}};
            require(checkout.stop().state == CheckoutState::PaymentRequired && checkout.result().stopRequested, "unknown hash stop remains pending");
            fog::CheckoutGuard recovered(f.api, sessions, f.checkoutPath);
            require(recovered.result().stopRequested, "stop intent survives restart");
            f.status = {{"id", 42}, {"status", 2}, {"hash", "fixture-session"}}; f.makeActive();
            require(recovered.poll(true).state == CheckoutState::Idle && f.finishes == 1, "late paid hash stopped after restart");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.login("89991234568");
            require(checkout.poll(true).state == CheckoutState::RecoveryRequired && f.checks == 0, "other account cannot recover order");
            rejects([&] { checkout.acknowledgeManualResolution(); }, "other account cannot clear marker");
            f.login(); f.makeActive();
            require(f.settle(checkout).state == CheckoutState::Ready, "same account reauth recovers");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.status = {{"id", 43}, {"status", 2}, {"hash", "fixture-session"}};
            require(checkout.poll().state == CheckoutState::RecoveryRequired && sessions.pendingHash().empty(), "mismatched ID cannot adopt");
            f.status = {{"id", 42}, {"status", 3}};
            require(checkout.stop().state == CheckoutState::RecoveryRequired && checkout.unresolved(), "not paid is not cancellation");
        }
        {
            Fixture f; f.makeActive();
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection());
            require(checkout.poll().state == CheckoutState::RecoveryRequired && !sessions.ownsSession(), "pre-existing hash never adopted as new rental");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.status = {{"id", 42}, {"status", 0}};
            for (int i = 0; i < 30; ++i) checkout.poll();
            require(checkout.poll().state == CheckoutState::Pending && f.checks == 30 &&
                !fog::checkoutNeedsAutomaticCheck(checkout.result()) && f.finishes == 0, "automatic polling bounded without stopping rental");
            checkout.poll(true);
            require(f.checks == 31 && f.creates == 1, "manual check only retries GET");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); checkout.poll(); f.rejectedFinish = true;
            require(checkout.stop().state == CheckoutState::RecoveryRequired && checkout.unresolved() && sessions.persistent(), "rejected stop retains both journals");
            f.rejectedFinish = false; f.finishActive = true;
            require(checkout.stop().state == CheckoutState::RecoveryRequired, "finish ok with still active cannot clear");
            f.finishActive = false;
            require(checkout.stop().state == CheckoutState::Idle, "retry finish reconciles termination");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.directory + "/missing/checkout.json");
            require(checkout.create(f.selection()).state == CheckoutState::RecoveryRequired && f.creates == 0, "journal failure prevents POST");
        }
        {
            Fixture f;
            std::ofstream(f.checkoutPath) << "PRIVATE corrupted marker";
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            rejects([&] { checkout.create(f.selection()); }, "invalid existing marker blocks checkout");
            require(checkout.stop().state == CheckoutState::RecoveryRequired && f.creates == 0, "invalid marker retained on stop");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); checkout.poll();
            require(symlink("/missing/fog-test-target", (f.checkoutPath + ".tmp").c_str()) == 0, "simulate journal write failure");
            require(checkout.stop().state == CheckoutState::Idle && f.finishes == 1, "owned rental stoppable with failed stop-intent write");
        }
        {
            Fixture f; f.created = {{"error", "PRIVATE server rejection"}};
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            require(checkout.create(f.selection()).state == CheckoutState::RecoveryRequired && checkout.unresolved(), "explicit rejection without order ID retains uncertain intent");
            require(checkout.result().message.find("PRIVATE") == std::string::npos, "private server error suppressed");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.directory + "/missing/session.json");
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive();
            require(f.settle(checkout).state == CheckoutState::RecoveryRequired, "failed adopt journal blocks first connection");
            require(sessions.ownsSession() && !sessions.persistent() && !sessions.journalError().empty(), "failed adopt retains stop authority without durability");
            require(checkout.poll().state == CheckoutState::RecoveryRequired, "later poll cannot bypass failed journal write");
            require(checkout.poll(true).state != CheckoutState::Ready && f.finishes == 0, "manual status retry cannot bypass durability or silently stop");
            require(checkout.stop().state == CheckoutState::Idle && f.finishes == 1, "undurable owned rental remains explicitly stoppable");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.directory + "/missing/session.json");
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive();
            require(symlink("/missing/fog-test-target", (f.checkoutPath + ".tmp").c_str()) == 0, "simulate pending-order journal write failure");
            require(checkout.stop().state == CheckoutState::Idle && f.finishes == 1, "stop exact order despite failed checkout and session journal writes");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            const auto created = checkout.create(f.selection());
            require(created.serverStatus == -1 && !created.serverHashPresent && created.pollCount == 0 &&
                created.readyObservations == 0, "diagnostics start without server evidence");
            f.status = {{"id", 42}, {"status", 0}};
            require(checkout.poll().state == CheckoutState::Pending && checkout.result().readyObservations == 0,
                "NEW does not establish readiness");
            f.status = {{"id", 42}, {"status", 1}, {"hash", "fixture-session"}}; f.makeActive();
            require(checkout.poll().state == CheckoutState::Pending && checkout.result().readyObservations == 1,
                "HOLD hash starts stabilization");
            require(checkout.result().serverStatus == 1 && checkout.result().serverHashPresent &&
                checkout.result().pollCount == 2 && sessions.pendingHash().empty(), "redacted readiness diagnostics");
            std::ifstream marker(f.checkoutPath);
            const auto saved = fog::Json::parse(marker);
            require(saved.at("hash") == "fixture-session", "first exact hash is persisted before Ready");
            require(checkout.poll().state == CheckoutState::Pending && checkout.result().readyObservations == 2,
                "second HOLD observation remains pending");
            require(checkout.poll().state == CheckoutState::Ready && checkout.result().readyObservations == 3 &&
                checkout.result().pollCount == 4 && sessions.persistent(), "three HOLD observations connect durably");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); f.status["status"] = 1;
            require(checkout.poll().state == CheckoutState::Pending, "HOLD first observation");
            f.status["status"] = 2;
            require(checkout.poll().state == CheckoutState::Pending && checkout.result().readyObservations == 2,
                "HOLD to PAID preserves same-hash count");
            require(checkout.poll().state == CheckoutState::Ready, "mixed HOLD/PAID same hash becomes ready");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive();
            checkout.poll(); checkout.poll();
            f.status["hash"] = "   ";
            require(checkout.poll().state == CheckoutState::Pending && checkout.result().readyObservations == 0 &&
                !checkout.result().serverHashPresent, "blank hash resets consecutive readiness");
            f.status["hash"] = "fixture-session";
            checkout.poll(); checkout.poll();
            f.status["status"] = 0;
            require(checkout.poll().state == CheckoutState::Pending && checkout.result().readyObservations == 0,
                "NEW resets consecutive readiness even with hash");
            f.status["status"] = 1;
            require(f.settle(checkout).state == CheckoutState::Ready, "readiness restarts after blank and NEW");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); checkout.poll(); checkout.poll();
            f.status["found_cancel"] = true;
            require(checkout.poll().state == CheckoutState::RecoveryRequired && checkout.result().readyObservations == 0 &&
                !sessions.ownsSession() && checkout.unresolved(), "found_cancel blocks Ready and retains recovery");
            f.status["found_cancel"] = false;
            require(f.settle(checkout).state == CheckoutState::Ready, "uncancelled observations require fresh stabilization");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); checkout.poll(); checkout.poll();
            f.status["hash"] = "different-session";
            require(checkout.poll().state == CheckoutState::RecoveryRequired && checkout.result().readyObservations == 0 &&
                checkout.result().hash == "fixture-session" && !sessions.ownsSession(), "changed hash cannot replace attribution or connect");
            f.statusTimeout = true;
            const int checks = f.checks;
            require(checkout.stop().state == CheckoutState::Idle && f.finishes == 1 && f.checks == checks,
                "stop uses original attributed hash despite changed response");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); f.status["status"] = 1;
            checkout.poll();
            f.statusTimeout = true;
            const int checks = f.checks;
            require(checkout.stop().state == CheckoutState::Idle && f.finishes == 1 && f.checks == checks,
                "stop during HOLD stabilization acts immediately without more status checks");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); checkout.poll(); checkout.poll();
            fog::CheckoutGuard recovered(f.api, sessions, f.checkoutPath);
            require(recovered.result().readyObservations == 0 && recovered.result().hash == "fixture-session",
                "restart retains attribution but resets observations");
            f.status["hash"] = "different-session";
            require(recovered.poll().state == CheckoutState::RecoveryRequired && !sessions.ownsSession(),
                "persisted hash rejects changed session after restart");
            f.status["hash"] = "fixture-session";
            require(f.settle(recovered).state == CheckoutState::Ready, "restart requires three fresh same-hash observations");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive(); checkout.poll(); checkout.poll();
            f.statusTimeout = true;
            require(checkout.poll().state == CheckoutState::Pending && fog::checkoutNeedsAutomaticCheck(checkout.result()) &&
                checkout.result().readyObservations == 0 && checkout.result().serverStatus == -1 && !checkout.result().serverHashPresent, "failed status check resets evidence and diagnostics");
            f.statusTimeout = false;
            f.status["status"] = 3;
            require(checkout.poll().state == CheckoutState::RecoveryRequired && checkout.result().readyObservations == 0,
                "NOT_PAID cannot connect");
            f.status["status"] = 2;
            require(f.settle(checkout).state == CheckoutState::Ready, "fresh observations after failed and NOT_PAID checks");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            require(checkout.create(f.selection()).orderId == 42 && f.creates == 1,
                "create ID replaces initial intent under non-overwriting rename");
            require(std::rename(f.checkoutPath.c_str(), (f.checkoutPath + ".bak").c_str()) == 0,
                "interrupted checkout swap fixture");
            fog::CheckoutGuard restored(f.api, sessions, f.checkoutPath);
            require(restored.unresolved() && restored.result().orderId == 42,
                "checkout restart restores order ID from sole backup");
            rejects([&] { restored.create(f.selection()); }, "restored backup prevents repeated billable create");
            require(f.creates == 1, "recovery never sends duplicate create");
            f.makeActive(); restored.poll();
            require(restored.result().hash == "fixture-session", "hash replaces restored checkout record");
            require(std::rename(f.checkoutPath.c_str(), (f.checkoutPath + ".bak").c_str()) == 0,
                "interrupted hash persistence fixture");
            fog::CheckoutGuard hashRestored(f.api, sessions, f.checkoutPath);
            require(hashRestored.result().hash == "fixture-session" && hashRestored.result().readyObservations == 0,
                "restart retains backed-up hash and requires fresh readiness");
            require(hashRestored.stop().state == CheckoutState::Idle && f.finishes == 1,
                "restored checkout marker retains targeted stop authority");
            fog::CheckoutGuard cleared(f.api, sessions, f.checkoutPath);
            require(!cleared.unresolved(), "stopped rental is not resurrected by backup");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection());
            require(chmod(f.directory.c_str(), 0300) == 0, "checkout clear commit failure fixture");
            rejects([&] { checkout.acknowledgeManualResolution(); }, "failed clear commit reported");
            require(chmod(f.directory.c_str(), 0700) == 0, "restore checkout directory permissions");
            fog::CheckoutGuard recovered(f.api, sessions, f.checkoutPath);
            require(checkout.unresolved() && recovered.unresolved() && recovered.result().orderId == 42,
                "clear commit failure retains pending order in memory and on disk");
            rejects([&] { recovered.create(f.selection()); }, "failed clear cannot authorize repeated create");
            require(f.creates == 1, "uncertain clear never repeats creation");
            recovered.acknowledgeManualResolution();
            fog::CheckoutGuard cleared(f.api, sessions, f.checkoutPath);
            require(!cleared.unresolved(), "manual clear retry succeeds after commit recovers");
        }
        {
            Fixture f;
            std::ofstream(f.checkoutPath + ".bak") << "PRIVATE-malformed";
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            require(checkout.unresolved() && checkout.poll().state == CheckoutState::RecoveryRequired,
                "malformed sole checkout backup blocks rental creation");
            rejects([&] { checkout.create(f.selection()); }, "malformed restored backup cannot be overwritten");
            require(f.creates == 0, "malformed backup sends no create request");
        }
        {
            Fixture f;
            f.status = {{"id",42},{"status",0}};
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection());
            f.statusTimeout = true;
            const auto interrupted = checkout.stop();
            require(interrupted.state == CheckoutState::Pending && interrupted.hash.empty(),
                "stop status failure before session hash is known");
            require(fog::checkoutNeedsAutomaticCheck(interrupted),
                "scheduler continues recorded stop after transient status failure");
            f.statusTimeout = false;
            f.status = {{"id",42},{"status",2},{"hash","fixture-session"}};
            checkout.stop(); // Scheduled retry uses stop(), not bounded normal readiness polling.
            require(!checkout.unresolved() && f.finishes == 1 && f.creates == 1,
                "scheduled stop learns late hash, finishes once and never recreates rental");
            require(!fog::checkoutNeedsAutomaticCheck(checkout.result()), "confirmed stop ends scheduler");
            fog::CheckoutResult unknown;
            unknown.state = CheckoutState::RecoveryRequired; unknown.stopRequested = true;
            require(!fog::checkoutNeedsAutomaticCheck(unknown),
                "lost creation response without order or hash requires manual resolution");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection());
            f.finishTimeout = true;
            const auto stopped = checkout.stop();
            require(stopped.state == CheckoutState::RecoveryRequired && checkout.unresolved() &&
                sessions.persistent() && !sessions.pendingHash().empty(),
                "stop timeout before activation retains both recovery journals");
            f.makeActive();
            require(checkout.stop().state == CheckoutState::RecoveryRequired && checkout.unresolved(),
                "late activation remains tracked after ambiguous stop");
            f.active = fog::Json::array();
            require(checkout.stop().state == CheckoutState::Idle,
                "observed active rental reconciles later absence after ambiguous stop");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection());
            f.statusTimeout = true;
            require(checkout.poll().state == CheckoutState::Pending && fog::checkoutNeedsAutomaticCheck(checkout.result()),
                "status failure before hash retries automatically");
            f.statusTimeout = false;
            checkout.poll(); checkout.poll();
            require(checkout.poll().state == CheckoutState::Pending && f.finishes == 0 && !sessions.ownsSession(),
                "active-list propagation delay does not stop or connect rental");
            f.activeTimeout = true;
            require(checkout.poll().state == CheckoutState::Pending && f.finishes == 0,
                "active-list timeout after hash does not stop rental");
            f.activeTimeout = false; f.makeActive();
            require(f.settle(checkout).state == CheckoutState::Ready && f.finishes == 0,
                "fresh readiness checks recover from transient list failure");
        }
        {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection());
            for (int i = 0; i < 31; ++i) checkout.poll();
            require(checkout.result().state == CheckoutState::Pending && !checkout.result().hash.empty() &&
                !fog::checkoutNeedsAutomaticCheck(checkout.result()) && f.checks == 30 && f.finishes == 0 &&
                sessions.pendingHash().empty() && checkout.unresolved(),
                "unobserved active rental pauses at retry limit without connecting or stopping");
            f.makeActive();
            require(checkout.poll(true).state == CheckoutState::Ready && f.creates == 1,
                "manual check resumes same attributed rental after retry limit");
        }
        for (const auto& invalid : {fog::Json(1), fog::Json("1"), fog::Json(nullptr)}) {
            Fixture f;
            fog::SessionGuard sessions(f.api, f.sessionPath);
            fog::CheckoutGuard checkout(f.api, sessions, f.checkoutPath);
            checkout.create(f.selection()); f.makeActive();
            f.status["found_cancel"] = invalid;
            require(checkout.poll().state == CheckoutState::RecoveryRequired && !sessions.ownsSession(),
                "unexpected cancellation type cannot connect rental");
        }
        std::cout << "Checkout offline tests passed: " << assertions << " assertions. No live orders or payments.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Checkout test failed: " << error.what() << '\n';
        return 1;
    }
}
