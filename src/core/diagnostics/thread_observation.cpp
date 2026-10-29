#include "core/diagnostics/thread_observation.hpp"

// Optional hook used by pinned native dependencies. Their host-only binaries
// leave the weak reference unresolved; the application supplies this observer.
extern "C" void fogObserveThread(const char* role) noexcept {
    fog::diagnostics::observeThread(role);
}
