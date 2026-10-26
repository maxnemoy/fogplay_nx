#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fog::foghub {

struct SavedCookies {
    // Verified login identity, including its "login:" prefix; never a backend ID.
    std::string account;
    std::vector<std::string> cookies;
};

class CookieStore {
public:
    virtual ~CookieStore() = default;
    virtual std::optional<SavedCookies> load() = 0;
    virtual void save(const SavedCookies& saved) = 0;
    virtual void clear() = 0;
};

// File diagnostics contain neither paths nor cookie/account contents.
// Cookie domain and Netscape syntax validation belongs to WebTransport.
std::unique_ptr<CookieStore> makeCookieStore(const std::string& path);

}
