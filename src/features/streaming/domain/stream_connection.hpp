#pragma once
#include "core/domain/identity.hpp"

namespace stream {
struct StreamConnection {
    fog::EntityId rental;
    // Only resume the already running app on the authenticated rental host.
    bool resumeOnly = false;
};
}
