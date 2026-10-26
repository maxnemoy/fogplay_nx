#include "features/session/infrastructure/rental_journal_storage.hpp"
#include "core/constants/error_text.hpp"
#include "core/storage/storage.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <sys/stat.h>
#include <utility>

namespace fog {
namespace {
using Json = nlohmann::json;
namespace SessionError = constants::error::Session;
EntityId mts(std::string value) { return {ProviderId::Mts, std::move(value)}; }
bool validPath(const std::string& path) { return !path.empty() && path.find('\0') == std::string::npos; }
std::int64_t integer(const EntityId& id, std::int64_t maximum, bool optional = false) {
    if (id.provider != ProviderId::Mts || id.value.empty()) {
        if (optional && id.provider == ProviderId::Mts && id.empty()) return 0;
        throw JournalError("Invalid MTS journal identifier");
    }
    std::int64_t result = 0;
    for (const auto c : id.value) {
        if (c < '0' || c > '9' || result > (maximum - (c - '0')) / 10) throw JournalError("Invalid MTS journal identifier");
        result = result * 10 + (c - '0');
    }
    if (result < 1) throw JournalError("Invalid MTS journal identifier");
    return result;
}
void writeStorage(const std::string& path, const std::string& bytes) {
    try { replaceStorage(path, bytes); }
    catch (const StorageError& error) { throw JournalError(error.what()); }
}
void removeStorage(const std::string& path) {
    try { clearStorage(path); }
    catch (const StorageError& error) { throw JournalError(error.what()); }
}
class MtsSessionJournal final : public SessionJournal {
public:
    explicit MtsSessionJournal(std::string path) : path_(std::move(path)), backupPath_(path_ + ".bak") {
        if (!validPath(path_)) throw std::invalid_argument(SessionError::InvalidSessionJournalPath);
    }
    SessionJournalRecord load() override {
        SessionJournalRecord result;
        try { recoverStorage(path_); }
        catch (const StorageError& error) {
            result.persistent = result.invalid = true;
            result.error = std::string(SessionError::SessionBackupRecoveryRequiredPrefix) + error.what();
            return result;
        }
        struct stat info{};
        if (stat(path_.c_str(), &info) != 0) {
            if (errno == ENOENT) return result;
            result.invalid = true;
            result.error = SessionError::CannotReadPendingSessionJournalPreserveItForRecovery;
            return result;
        }
        result.persistent = true;
        result.invalid = true;
        result.error = SessionError::InvalidPendingSessionJournalPreserveItForRecovery;
        if (!S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 4096) return result;
        FILE* file = std::fopen(path_.c_str(), "rb");
        if (!file) { result.error = SessionError::CannotReadPendingSessionJournalPreserveItForRecovery; return result; }
        char bytes[4097];
        const auto count = std::fread(bytes, 1, sizeof(bytes), file);
        const bool failed = std::ferror(file) != 0;
        const bool closed = std::fclose(file) == 0;
        const auto data = Json::parse(bytes, bytes + count, nullptr, false);
        if (failed || !closed || count > 4096 || !data.is_object() || data.size() != 2 ||
            !data.contains("version") || data.at("version") != 1 || !data.contains("hash") ||
            !data.at("hash").is_string() || !validSessionHash(data.at("hash").get<std::string>())) {
            result.error = SessionError::InvalidOrUnreadablePendingSessionJournalPreserveItForRecovery;
            return result;
        }
        result.rental = mts(data.at("hash").get<std::string>());
        result.invalid = false;
        result.error.clear();
        return result;
    }
    void write(const RentalId& rental) override {
        if (rental.provider != ProviderId::Mts || !validRentalId(rental)) throw JournalError("Invalid MTS journal rental");
        writeStorage(path_, Json{{"version", 1}, {"hash", rental.value}}.dump() + "\n");
    }
    void clear() override { removeStorage(path_); }
    bool hasMarker() const noexcept override {
        struct stat info{};
        if (lstat(path_.c_str(), &info) == 0 || errno != ENOENT) return true;
        return lstat(backupPath_.c_str(), &info) == 0 || errno != ENOENT;
    }
private:
    std::string path_, backupPath_;
};
class MtsCheckoutJournal final : public CheckoutJournal {
public:
    explicit MtsCheckoutJournal(std::string path) : path_(std::move(path)) {
        if (!validPath(path_)) throw std::invalid_argument(SessionError::InvalidCheckoutJournalPath);
    }
    CheckoutJournalLoad load() override {
        CheckoutJournalLoad result;
        try { recoverStorage(path_); }
        catch (const StorageError& error) {
            result.pending = result.invalid = true;
            result.error = std::string(SessionError::CheckoutBackupRecoveryRequiredPrefix) + error.what();
            return result;
        }
        struct stat info{};
        if (stat(path_.c_str(), &info) != 0 && errno == ENOENT) return result;
        result.pending = result.invalid = true;
        result.error = SessionError::CheckoutRecoveryMarkerRetainedVerifyTheOfficialAccountBeforeAnotherRental;
        if (!S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 131072) return result;
        FILE* file = std::fopen(path_.c_str(), "rb");
        if (!file) return result;
        std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
        const auto count = std::fread(bytes.data(), 1, bytes.size(), file);
        const bool failed = std::ferror(file) != 0;
        const bool closed = std::fclose(file) == 0;
        const auto data = Json::parse(bytes, nullptr, false);
        try {
            if (failed || !closed || count != bytes.size() || !data.is_object() || data.at("version") != 1) return result;
            const auto account = data.at("account").get<std::string>();
            if (account.size() != 64 || !std::all_of(account.begin(), account.end(),
                [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return result;
            const auto computer = data.at("computerId").get<int>();
            const auto offer = data.at("tariffId").get<int>();
            const auto creation = data.at("orderId").get<std::int64_t>();
            const auto hash = data.at("hash").get<std::string>();
            const auto before = data.at("before").get<std::vector<std::string>>();
            const auto stop = data.at("stopRequested").get<bool>();
            if (computer < 1 || offer < 1 || creation < 0 || (!hash.empty() && !validSessionHash(hash)) || before.size() > 64) return result;
            for (const auto& prior : before) if (!validSessionHash(prior)) return result;
            result.record.account = mts(account);
            result.record.selection.computerId = mts(std::to_string(computer));
            result.record.selection.offerId = mts(std::to_string(offer));
            result.record.creation = mts(creation ? std::to_string(creation) : std::string{});
            result.record.rental = mts(hash);
            result.record.stopRequested = stop;
            for (const auto& prior : before) result.record.before.push_back(mts(prior));
            result.invalid = false;
            result.error = creation ? "Sign in to the original account and check pending order" :
                SessionError::CreateOutcomeUnknownVerifyTheOfficialAccountManuallyCreationWillNotBeRepeated;
        } catch (...) {}
        return result;
    }
    void write(const CheckoutJournalRecord& record) override {
        if (record.account.provider != ProviderId::Mts || record.rental.provider != ProviderId::Mts)
            throw JournalError("Invalid MTS journal provider");
        std::vector<std::string> before;
        for (const auto& rental : record.before) {
            if (rental.provider != ProviderId::Mts || !validRentalId(rental)) throw JournalError("Invalid MTS journal rental");
            before.push_back(rental.value);
        }
        writeStorage(path_, Json{{"version", 1}, {"account", record.account.value},
            {"computerId", integer(record.selection.computerId, (std::numeric_limits<int>::max)())},
            {"tariffId", integer(record.selection.offerId, (std::numeric_limits<int>::max)())},
            {"orderId", integer(record.creation, (std::numeric_limits<std::int64_t>::max)(), true)},
            {"hash", record.rental.value}, {"before", before}, {"stopRequested", record.stopRequested}}.dump() + "\n");
    }
    void clear() override { removeStorage(path_); }
private:
    std::string path_;
};
}
std::unique_ptr<SessionJournal> makeMtsSessionJournal(std::string path) {
    return std::make_unique<MtsSessionJournal>(std::move(path));
}
std::unique_ptr<CheckoutJournal> makeMtsCheckoutJournal(std::string path) {
    return std::make_unique<MtsCheckoutJournal>(std::move(path));
}
}
