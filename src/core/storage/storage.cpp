// newlib requires POSIX visibility for descriptor truncation.
#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "core/storage/storage.hpp"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __SWITCH__
extern "C" {
#include <switch/result.h>
#include <switch/runtime/devices/fs_dev.h>
}
#endif

namespace fog {
namespace {
bool exists(const std::string& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) == 0) return true;
    if (errno == ENOENT) return false;
    throw StorageError("inspect", errno);
}
void regular(const std::string& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) throw StorageError("inspect", errno);
    if (!S_ISREG(info.st_mode)) throw StorageError("file-type", EINVAL);
}
std::string parentPath(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash == 0 ? 1 : slash);
}
void syncParent(const std::string& path) {
#ifdef __SWITCH__
    FsFileSystem* device = nullptr;
    char translated[FS_MAX_PATH];
    if (fsdevTranslatePath(path.c_str(), &device, translated) < 0 || !device)
        throw StorageError("commit-device", errno);
    if (R_FAILED(fsFsCommit(device))) throw StorageError("commit", EIO);
#else
    const int fd = open(parentPath(path).c_str(), O_RDONLY);
    if (fd < 0) throw StorageError("open-directory", errno);
    int error = 0;
    if (fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (error) throw StorageError("commit", error);
#endif
}
bool removeFile(const std::string& path, const char* operation) {
    if (unlink(path.c_str()) == 0) return true;
    if (errno == ENOENT) return false;
    throw StorageError(operation, errno);
}
void moveFile(const std::string& from, const std::string& to, const char* operation) {
    if (std::rename(from.c_str(), to.c_str()) != 0) throw StorageError(operation, errno);
}
void writeTemporary(const std::string& path, const std::string& bytes) {
    int flags = O_WRONLY | O_CREAT;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    flags |= O_NONBLOCK;
#endif
    const int fd = open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0) throw StorageError("open-temporary", errno);
    struct stat info{};
    int error = 0;
    const char* operation = "inspect-temporary";
    if (fstat(fd, &info) != 0) error = errno;
    else if (!S_ISREG(info.st_mode)) error = EINVAL;
#ifndef __SWITCH__
    else if (info.st_nlink != 1) error = EINVAL;
    if (!error && fchmod(fd, S_IRUSR | S_IWUSR) != 0) { error = errno; operation = "permissions"; }
#endif
    if (!error && ftruncate(fd, 0) != 0) { error = errno; operation = "truncate-temporary"; }
    std::size_t offset = 0;
    while (!error && offset < bytes.size()) {
        const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; operation = "write-temporary"; break; }
        offset += static_cast<std::size_t>(count);
    }
    if (!error && fsync(fd) != 0) { error = errno; operation = "flush-temporary"; }
    if (close(fd) != 0 && !error) { error = errno; operation = "close-temporary"; }
    if (error) throw StorageError(operation, error);
}
}
StorageError::StorageError(const char* operation, int error)
    : std::runtime_error(std::string("Storage ") + operation + " failed (errno " + std::to_string(error) + ")") {}
void recoverStorage(const std::string& path) {
    if (exists(path) || !exists(path + ".bak")) return;
    regular(path + ".bak");
    moveFile(path + ".bak", path, "recover-backup");
    syncParent(path);
}
void replaceStorage(const std::string& path, const std::string& bytes) {
    recoverStorage(path);
    const auto backup = path + ".bak";
    const auto temporary = path + ".tmp";
    const bool primary = exists(path);
    if (primary) regular(path);
    if (exists(backup)) {
        // A prior swap might have failed its final commit. Establish primary
        // durability before discarding its fallback, even on a later retry.
        regular(backup);
        syncParent(path);
        removeFile(backup, "remove-backup");
        syncParent(path);
    }
    writeTemporary(temporary, bytes);
    if (primary) {
        moveFile(path, backup, "backup-primary");
        syncParent(path);
    }
    // Never remove the backup on failure. Startup recovers it if primary is absent.
    moveFile(temporary, path, "install-primary");
    syncParent(path);
    if (primary) {
        removeFile(backup, "remove-backup");
        syncParent(path);
    }
}
void clearStorage(const std::string& path) {
    // Do not recover here: explicit forget must delete even malformed backups.
    // If either auxiliary deletion fails, the primary remains untouched.
    const bool backup = removeFile(path + ".bak", "clear-backup");
    const bool temporary = removeFile(path + ".tmp", "clear-temporary");
    if (backup || temporary) syncParent(path);
    const bool primary = removeFile(path, "clear-primary");
    // A prior clear can unlink successfully then fail its commit. Retrying an
    // already-absent record must still commit before reporting durable removal.
    // A never-created store under a missing directory has no removal to commit.
    if (primary || exists(parentPath(path))) syncParent(path);
}
}
