// Emulate Horizon: rename refuses a destination that already exists.
#include <cerrno>
#include <sys/stat.h>
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    struct stat info{};
    if (lstat(to, &info) == 0) { errno = EEXIST; return -1; }
    return __real_rename(from, to);
}
