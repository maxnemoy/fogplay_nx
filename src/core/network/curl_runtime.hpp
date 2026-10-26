#pragma once

#include "core/constants/error_text.hpp"

#include <curl/curl.h>
#include <stdexcept>

namespace fog::detail {
// Initialize libcurl once before any easy handles. The reference intentionally
// lives until process exit, so static destruction cannot race worker cleanup.
inline void ensureCurlRuntime() {
    static const auto initialized = [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error(constants::error::Connection::CannotInitializeHTTPSTransport);
        return true;
    }();
    (void)initialized;
}
}
