#pragma once
#include <stdexcept>
#include <string>

namespace fog {
// Fixed operation names and numeric errno only; never includes paths or payloads.
class StorageError : public std::runtime_error {
public:
    StorageError(const char* operation, int error);
};
// The primary is authoritative when present; an interrupted swap's backup is
// recovered only when it is missing. Temporary writes are never promoted.
void recoverStorage(const std::string& path);
// Backup-assisted replacement for filesystems whose rename cannot overwrite.
// This is crash-recoverable, not an atomic multi-file transaction.
void replaceStorage(const std::string& path, const std::string& bytes);
// Delete and commit auxiliary files before deleting the primary, preventing
// successful clears from resurrecting an obsolete backup on restart.
void clearStorage(const std::string& path);
}
