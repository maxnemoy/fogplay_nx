#pragma once

#include "core/network/http.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fog::foghub {

struct WebResponse {
    long status = 0;
    std::string body;
    std::string location;
    std::string contentType;
};

enum class TransportFailure { InvalidRequest, CookieRejected, Cancelled, Network, ResponseTooLarge, Internal, Unsupported };

class TransportError final : public std::runtime_error {
public:
    explicit TransportError(TransportFailure kind);
    TransportFailure kind() const noexcept { return kind_; }
private:
    TransportFailure kind_;
};

// Each transport owns its cookie engine. Calls are serialized; cancel may be
// called from another thread and permanently cancels this transport instance.
class WebTransport {
public:
    virtual ~WebTransport() = default;
    virtual WebResponse get(const std::string& relativePath) = 0;
    virtual WebResponse submitLogin(const Query& fields) = 0;
    // Mutating routes have dedicated operations; get() must never accept close.
    virtual WebResponse submitPairing(const std::string&, const Query&) {
        throw TransportError(TransportFailure::Unsupported);
    }
    virtual WebResponse submitPairing(const std::string& computer, const Query& fields, bool freshPair) {
        if (freshPair) throw TransportError(TransportFailure::Unsupported);
        return submitPairing(computer,fields);
    }
    virtual WebResponse closeRental(const std::string&) {
        throw TransportError(TransportFailure::Unsupported);
    }
    virtual WebResponse submitSessionRating(const std::string&,const Query&) {
        throw TransportError(TransportFailure::Unsupported);
    }
    virtual std::vector<std::string> cookies() = 0;
    virtual void restoreCookies(const std::vector<std::string>& cookies) = 0;
    virtual void clearCookies() = 0;
    virtual void cancel() noexcept = 0;
};

std::unique_ptr<WebTransport> makeWebTransport();

namespace detail {
// Shared by the transport and its offline validation tests, not a URL fetch API.
std::string canonicalReadPath(const std::string& path);
std::string normalizeCookie(const std::string& cookie);
}

}
