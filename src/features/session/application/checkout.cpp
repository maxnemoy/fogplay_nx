#include "features/session/application/checkout.hpp"
#include "core/storage/storage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#ifdef __SWITCH__
extern "C" {
#include <switch/result.h>
#include <switch/runtime/devices/fs_dev.h>
}
#endif

namespace fog {
namespace {
std::string text(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
std::int64_t id(const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number_integer()) return 0;
    if (found->is_number_unsigned() && found->get<std::uint64_t>() >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) return 0;
    const auto result = found->get<std::int64_t>();
    return result > 0 ? result : 0;
}
bool hasHash(const Json& sessions, const std::string& hash) {
    for (const auto& session : sessions) if (text(session, "hash") == hash) return true;
    return false;
}
bool paymentLink(const std::string& url) {
    return url.size() > 8 && url.size() <= 4096 && url.rfind("https://", 0) == 0 &&
        std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c != 127; });
}

}
CheckoutGuard::CheckoutGuard(RentalService& api, SessionGuard& sessions, std::string journalPath)
    : api_(api), sessions_(sessions), journalPath_(std::move(journalPath)) {
    if (journalPath_.empty() || journalPath_.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid checkout journal path");
    load();
}
CheckoutResult CheckoutGuard::recovery(const std::string& message) {
    result_.state = CheckoutState::RecoveryRequired;
    result_.message = message;
    return result_;
}
CheckoutResult CheckoutGuard::waiting(const std::string& message) {
    result_.state = result_.paymentUrl.empty() ? CheckoutState::Pending : CheckoutState::PaymentRequired;
    result_.message = polls_ >= 30 ?
        "Automatic checks paused; order remains unresolved. Check again or request stop" : message;
    return result_;
}
void CheckoutGuard::load() {
    try { recoverStorage(journalPath_); }
    catch (const StorageError& error) {
        pending_ = true;
        invalid_ = true;
        recovery(std::string("Checkout backup recovery required; ") + error.what());
        return;
    }
    struct stat info{};
    if (stat(journalPath_.c_str(), &info) != 0 && errno == ENOENT) return;
    pending_ = true;
    invalid_ = true;
    recovery("Checkout recovery marker retained; verify the official account before another rental");
    if (!S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 131072) return;
    FILE* file = std::fopen(journalPath_.c_str(), "rb");
    if (!file) return;
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    const auto count = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool failed = std::ferror(file) != 0;
    const bool closed = std::fclose(file) == 0;
    const auto data = Json::parse(bytes, nullptr, false);
    try {
        if (failed || !closed || count != bytes.size() || !data.is_object() || data.at("version") != 1) return;
        account_ = data.at("account").get<std::string>();
        if (account_.size() != 64 || !std::all_of(account_.begin(), account_.end(),
            [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return;
        selection_.computerId = data.at("computerId").get<int>();
        selection_.tariffId = data.at("tariffId").get<int>();
        result_.orderId = data.at("orderId").get<std::int64_t>();
        result_.hash = data.at("hash").get<std::string>();
        result_.stopRequested = data.at("stopRequested").get<bool>();
        before_ = data.at("before").get<std::vector<std::string>>();
        if (selection_.computerId < 1 || selection_.tariffId < 1 || result_.orderId < 0 ||
            (!result_.hash.empty() && !validSessionHash(result_.hash)) || before_.size() > 64) return;
        for (const auto& hash : before_) if (!validSessionHash(hash)) return;
        invalid_ = false;
        result_.message = result_.orderId ? "Sign in to the original account and check pending order" :
            "Create outcome unknown; verify the official account manually. Creation will not be repeated";
    } catch (...) {}
}
void CheckoutGuard::persist() {
    const auto bytes = Json{{"version", 1}, {"account", account_},
        {"computerId", selection_.computerId}, {"tariffId", selection_.tariffId},
        {"orderId", result_.orderId}, {"hash", result_.hash}, {"before", before_},
        {"stopRequested", result_.stopRequested}}.dump() + "\n";
    replaceStorage(journalPath_, bytes);
}
void CheckoutGuard::clear() {
    try { clearStorage(journalPath_); }
    catch (...) { try { persist(); } catch (...) {} throw; }
    pending_ = false;
    invalid_ = false;
    result_ = {};
    account_.clear();
    before_.clear();
    polls_ = 0;
}
bool CheckoutGuard::sameAccount() const {
    return api_.loggedIn() && !account_.empty() && account_ == api_.accountIdentity();
}
CheckoutResult CheckoutGuard::create(const CreateOrderRequest& order) {
    if (pending_ || !sessions_.pendingHash().empty() || !sessions_.journalError().empty())
        throw std::logic_error("Resolve pending checkout or rental before creating another");
    if (!api_.loggedIn() || api_.accountIdentity().empty()) throw std::logic_error("Sign in first");
    if (order.computerId < 1 || order.tariffId < 1 || order.gameId < 0)
        throw std::invalid_argument("Invalid checkout selection");
    const auto active = api_.activeSessions(true); // Before-create snapshot, never infer a new hash.
    if (active.size() > 64) throw std::runtime_error("Too many sessions for safe checkout recovery");
    selection_ = order;
    account_ = api_.accountIdentity();
    before_.clear();
    for (const auto& session : active) before_.push_back(text(session, "hash"));
    result_ = {};
    result_.state = CheckoutState::Pending;
    result_.message = "Creating selected rental once";
    pending_ = true;
    try { persist(); }
    catch (const StorageError& error) {
        return recovery(std::string("Cannot save create intent; no create request sent. ") + error.what());
    }
    catch (...) { return recovery("Cannot save create intent; no create request sent. Recovery required"); }
    try {
        const auto response = api_.createOrder(order);
        result_.orderId = id(response, "orderId");
        if (result_.orderId) persist(); // Before interpreting payment state or making more requests.
        if (!text(response, "error").empty()) {
            if (result_.orderId) return recovery("Create returned an error with an order ID; check the recorded order");
            // No cancellation/idempotency contract establishes that an error cannot
            // follow partial creation. Preserve even an explicit no-ID rejection.
            return recovery("Server reported a create error without order ID; verify official account before another rental");
        }
        if (!result_.orderId) return recovery("Create outcome unknown; check official account. Do not create again");
        const auto link = text(response, "paymentUrl");
        if (paymentLink(link)) {
            result_.paymentUrl = link;
            result_.paymentAmount = text(response, "paymentAmount");
            result_.state = CheckoutState::PaymentRequired;
            result_.message = "Payment required on official page; order remains pending";
        } else result_.message = "Order recorded; checking payment and session readiness";
        return result_;
    } catch (const StorageError& error) {
        return recovery(std::string("Create outcome uncertain; marker retained. ") + error.what());
    } catch (...) { return recovery("Create outcome uncertain; marker retained. Never repeat creation automatically"); }
}
CheckoutResult CheckoutGuard::poll(bool manualRetry) {
    if (!pending_) return result_;
    if (invalid_) return recovery("Invalid checkout marker; preserve it and verify official account manually");
    if (!sameAccount()) return recovery("Sign in to the original account before checking or stopping the order");
    if (!result_.orderId) return recovery("Create outcome unknown; official account check required before another rental");
    if (!manualRetry && polls_ >= 30) return waiting("");
    if (manualRetry) polls_ = 0;
    ++polls_;
    result_.pollCount = polls_;
    result_.serverStatus = -1;
    result_.serverHashPresent = false;
    try {
        const auto status = api_.orderStatus(result_.orderId);
        const auto field = status.find("status");
        if (field != status.end() && field->is_number_integer() &&
            *field >= (std::numeric_limits<int>::min)() && *field <= (std::numeric_limits<int>::max)())
            result_.serverStatus = field->get<int>();
        const auto hash = text(status, "hash");
        result_.serverHashPresent = std::any_of(hash.begin(), hash.end(),
            [](unsigned char c) { return c > 32 && c != 127; });
        if (id(status, "id") != result_.orderId) {
            result_.readyObservations = 0;
            return recovery("Order status identifier mismatch; marker retained");
        }
        const auto cancelled = status.find("found_cancel");
        if (cancelled != status.end() && !cancelled->is_boolean()) {
            result_.readyObservations = 0;
            return recovery("Order cancellation status is unknown; marker retained");
        }
        if (cancelled != status.end() && cancelled->get<bool>()) {
            result_.readyObservations = 0;
            return recovery("Order cancellation reported; verify rental termination on the official account");
        }
        if (result_.serverStatus != 1 && result_.serverStatus != 2) {
            result_.readyObservations = 0;
            if (result_.serverStatus == 3) return recovery("Order not paid; this does not prove rental termination. Check official account");
            if (result_.serverStatus != 0) return recovery("Order status unavailable or unknown; marker retained");
            result_.state = result_.paymentUrl.empty() ? CheckoutState::Pending : CheckoutState::PaymentRequired;
            result_.message = result_.stopRequested ? "Stop requested; waiting for session hash. Order is not cancelled" :
                "Waiting for payment or rental readiness";
            return result_;
        }
        if (!validSessionHash(hash)) {
            result_.readyObservations = 0;
            result_.state = result_.paymentUrl.empty() ? CheckoutState::Pending : CheckoutState::PaymentRequired;
            result_.message = result_.stopRequested ? "Stop requested; waiting for session hash. Order is not cancelled" :
                "Waiting for a valid rental session hash";
            return result_;
        }
        if (std::find(before_.begin(), before_.end(), hash) != before_.end()) {
            result_.readyObservations = 0;
            return recovery("Order points to a pre-existing rental; manual account check required");
        }
        if (!result_.hash.empty() && result_.hash != hash) {
            result_.readyObservations = 0;
            return recovery("Order session changed; marker retained");
        }
        result_.hash = hash;
        // Persist the first attributed hash before any connection or cleanup action.
        // Observations are deliberately not persisted: restart requires fresh checks.
        try { persist(); }
        catch (...) { if (!result_.stopRequested) throw; }
        if (!result_.stopRequested && sessions_.pendingHash() == hash &&
            (!sessions_.persistent() || !sessions_.journalError().empty()))
            return recovery("Session journal is not durable; connection blocked. Stop the recorded rental");
        if (result_.readyObservations < 3) ++result_.readyObservations;
        if (!result_.stopRequested && result_.readyObservations < 3) {
            result_.state = CheckoutState::Pending;
            result_.message = "Rental session identified; confirming readiness";
            return result_;
        }
        bool observedActive = false;
        if (!result_.stopRequested) {
            observedActive = hasHash(api_.activeSessions(true), hash);
            if (!observedActive)
                return waiting("Recorded rental is not active yet; checking again before connecting");
        }
        if (sessions_.pendingHash().empty()) {
            try { sessions_.adopt(hash, observedActive); }
            catch (...) {
                if (result_.stopRequested && sessions_.ownsSession() && sessions_.pendingHash() == hash)
                    return finishReady();
                throw;
            }
        }
        else if (sessions_.pendingHash() != hash) return recovery("Another rental is pending recovery; resolve it first");
        else if (!sessions_.ownsSession() && !sessions_.recoverMatchingSession())
            return recovery("Cannot establish ownership of recorded rental");
        if (!result_.stopRequested && (!sessions_.persistent() || !sessions_.journalError().empty()))
            return recovery("Session journal is not durable; connection blocked. Stop the recorded rental");
        result_.state = CheckoutState::Ready;
        result_.message = "Rental ready; session recovery journal saved";
        return result_.stopRequested ? finishReady() : result_;
    } catch (const StorageError& error) {
        result_.readyObservations = 0;
        return recovery(std::string("Cannot save recorded order; recovery marker retained. ") + error.what());
    } catch (...) {
        result_.readyObservations = 0;
        return waiting("Cannot verify recorded order temporarily; checking again. Recovery marker retained");
    }
}
CheckoutResult CheckoutGuard::finishReady() {
    const auto finished = sessions_.finishAttempt();
    if (!finished.terminated || !finished.journalCleared) return recovery(finished.message);
    try { clear(); }
    catch (const StorageError& error) {
        return recovery(std::string("Rental stopped, but checkout marker retained. ") + error.what());
    }
    catch (...) { return recovery("Rental stopped, but checkout recovery marker could not be cleared"); }
    result_.message = "Rental termination confirmed";
    return result_;
}
CheckoutResult CheckoutGuard::finishKnown() {
    // The checkout hash was attributed to this account/order before stabilization.
    // Cleanup must not depend on payment status or additional readiness observations.
    try {
        if (sessions_.pendingHash().empty()) {
            try { sessions_.adopt(result_.hash); }
            catch (...) {
                if (!sessions_.ownsSession() || sessions_.pendingHash() != result_.hash) throw;
            }
        } else if (sessions_.pendingHash() != result_.hash) {
            return recovery("Another rental is pending recovery; resolve it first");
        } else if (!sessions_.ownsSession() && !sessions_.recoverMatchingSession()) {
            return recovery("Cannot establish ownership of recorded rental");
        }
        return finishReady();
    } catch (const StorageError& error) {
        return recovery(std::string("Cannot stop recorded rental; recovery marker retained. ") + error.what());
    } catch (...) { return recovery("Cannot stop recorded rental; recovery marker retained"); }
}
CheckoutResult CheckoutGuard::stop() {
    if (!pending_) return result_;
    if (invalid_) return recovery("Invalid checkout marker; verify official account manually");
    result_.stopRequested = true;
    try { persist(); }
    catch (const StorageError& error) {
        if (sameAccount() && !result_.hash.empty()) return finishKnown();
        if (sameAccount() && result_.orderId > 0) return poll(true);
        return recovery(std::string("Cannot save stop intent; order remains unresolved. ") + error.what());
    }
    catch (...) {
        // Already-owned rentals remain stoppable when the SD becomes unwritable.
        if (sameAccount() && !result_.hash.empty()) return finishKnown();
        if (sameAccount() && result_.orderId > 0) return poll(true);
        return recovery("Cannot save stop intent; order remains unresolved");
    }
    if (sameAccount() && !result_.hash.empty()) return finishKnown();
    return poll(true);
}
CheckoutResult CheckoutGuard::acknowledgeManualResolution() {
    if (!sessions_.pendingHash().empty() || !sessions_.journalError().empty())
        throw std::logic_error("Resolve session recovery before acknowledging checkout");
    if (!pending_) return result_;
    if (!invalid_ && !sameAccount()) throw std::logic_error("Sign in to the original account first");
    clear();
    result_.message = "Local marker cleared after your account check; no server cancellation was sent";
    return result_;
}
}
