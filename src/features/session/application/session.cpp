#include "core/constants/error_text.hpp"
#include "features/session/application/session.hpp"
#include "core/storage/storage.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
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
bool hasHash(const Json& sessions, const std::string& hash) {
    for (const auto& session : sessions)
        if (session.at("hash").get<std::string>() == hash) return true;
    return false;
}

}

SessionGuard::SessionGuard(RentalService& api, std::string journalPath)
    : api_(api), journalPath_(std::move(journalPath)) {
    if (journalPath_.empty() || journalPath_.find('\0') != std::string::npos)
        throw std::invalid_argument(fog::constants::error::Session::InvalidSessionJournalPath);
    loadJournal();
}
void SessionGuard::loadJournal() {
    try { recoverStorage(journalPath_); }
    catch (const StorageError& error) {
        persistent_ = true;
        invalidJournal_ = true;
        journalError_ = std::string(fog::constants::error::Session::SessionBackupRecoveryRequiredPrefix) + error.what();
        return;
    }
    struct stat info{};
    if (stat(journalPath_.c_str(), &info) != 0) {
        if (errno == ENOENT) return;
        invalidJournal_ = true;
        journalError_ = fog::constants::error::Session::CannotReadPendingSessionJournalPreserveItForRecovery;
        return;
    }
    persistent_ = true;
    if (!S_ISREG(info.st_mode) || info.st_size < 1 || info.st_size > 4096) {
        invalidJournal_ = true;
        journalError_ = fog::constants::error::Session::InvalidPendingSessionJournalPreserveItForRecovery;
        return;
    }
    FILE* file = std::fopen(journalPath_.c_str(), "rb");
    if (!file) {
        invalidJournal_ = true;
        journalError_ = fog::constants::error::Session::CannotReadPendingSessionJournalPreserveItForRecovery;
        return;
    }
    char bytes[4097];
    const auto count = std::fread(bytes, 1, sizeof(bytes), file);
    const bool failed = std::ferror(file) != 0;
    const bool closed = std::fclose(file) == 0;
    const auto data = Json::parse(bytes, bytes + count, nullptr, false);
    if (failed || !closed || count > 4096 || !data.is_object() || data.size() != 2 ||
        !data.contains("version") || data.at("version") != 1 || !data.contains("hash") ||
        !data.at("hash").is_string() || !validSessionHash(data.at("hash").get<std::string>())) {
        invalidJournal_ = true;
        journalError_ = fog::constants::error::Session::InvalidOrUnreadablePendingSessionJournalPreserveItForRecovery;
        return;
    }
    pendingHash_ = data.at("hash").get<std::string>();
}
void SessionGuard::writeJournal() {
    const auto data = Json{{"version", 1}, {"hash", pendingHash_}}.dump() + "\n";
    try {
        replaceStorage(journalPath_, data);
        persistent_ = true;
    } catch (...) {
        // An install/commit failure may already have left a marker or backup.
        // Initial write failures without a file retain only in-memory ownership.
        struct stat info{};
        if (lstat(journalPath_.c_str(), &info) == 0 || errno != ENOENT) persistent_ = true;
        if (lstat((journalPath_ + ".bak").c_str(), &info) == 0 || errno != ENOENT) persistent_ = true;
        throw;
    }
}
void SessionGuard::adopt(const std::string& hash, bool observedActive) {
    if (invalidJournal_ || !journalError_.empty()) throw std::runtime_error(fog::constants::error::Session::SessionJournalNeedsRecovery);
    if (!api_.loggedIn()) throw std::logic_error(fog::constants::error::Auth::SignInBeforeAdoptingASession);
    if (!validSessionHash(hash)) throw std::invalid_argument(fog::constants::error::Session::InvalidSessionHash);
    if (!pendingHash_.empty()) throw std::logic_error(fog::constants::error::Session::ResolvePendingSessionBeforeConnectingAnother);
    pendingHash_ = hash;
    observedActive_ = observedActive;
    // An authenticated rental was selected or attributed to the recorded order.
    // Even if the SD write fails, retain authority to stop it; never start media then.
    owned_ = true;
    authGeneration_ = api_.authGeneration();
    try { writeJournal(); }
    catch (const StorageError& error) {
        journalError_ = std::string(fog::constants::error::Session::SessionJournalWriteFailedLiveConnectionIsBlockedPrefix) + error.what();
        throw;
    }
    catch (...) {
        journalError_ = fog::constants::error::Session::SessionJournalWriteFailedLiveConnectionIsBlocked;
        throw;
    }
}
bool SessionGuard::recoverMatchingSession() {
    if (invalidJournal_ || pendingHash_.empty()) return false;
    if (!hasHash(api_.activeSessions(true), pendingHash_)) return false;
    owned_ = true;
    authGeneration_ = api_.authGeneration();
    terminationEvidence_ = false;
    observedActive_ = true;
    return true;
}
void SessionGuard::clearJournal() {
    try { if (persistent_) clearStorage(journalPath_); }
    catch (...) {
        // Preserve a conservative marker if a durable clear cannot be established.
        try { writeJournal(); } catch (...) {}
        throw;
    }
    persistent_ = false;
    pendingHash_.clear();
    journalError_.clear();
    owned_ = false;
    terminationEvidence_ = false;
    observedActive_ = false;
}
FinishResult SessionGuard::finishAttempt() {
    if (!ownsSession() || pendingHash_.empty() || invalidJournal_)
        return {false, false, fog::constants::error::Session::PendingSessionMustMatchTheSignedInAccountBeforeStopping};
    bool acknowledged = false;
    try { acknowledged = api_.finishSession(pendingHash_); }
    catch (const FinishRejected&) {}
    catch (const std::logic_error&) {}
    catch (...) {}
    terminationEvidence_ = terminationEvidence_ || acknowledged;
    try {
        const auto sessions = api_.activeSessions(true);
        if (hasHash(sessions, pendingHash_)) {
            observedActive_ = true;
            return {false, false, fog::constants::error::Session::SessionRemainsActiveStopIsUnconfirmedAndCanBeRetried};
        }
        // An attributed checkout hash can precede activation. A failed POST and
        // absence alone never establish that this not-yet-active rental stopped.
        if (!terminationEvidence_ && !observedActive_)
            return {false, false, fog::constants::error::Session::SessionTerminationIsUnconfirmedPendingRecoveryMarkerRetained};
    } catch (...) {
        return {false, false, fog::constants::error::Session::CannotVerifySessionTerminationPendingRecoveryMarkerRetained};
    }
    try { clearJournal(); }
    catch (const StorageError& error) {
        journalError_ = std::string(fog::constants::error::Session::SessionTerminatedButRecoveryJournalCouldNotBeClearedPrefix) + error.what();
        return {true, false, journalError_};
    }
    catch (...) {
        journalError_ = fog::constants::error::Session::SessionTerminatedButRecoveryJournalCouldNotBeCleared;
        return {true, false, journalError_};
    }
    return {true, true, "Session termination confirmed"};
}
}
