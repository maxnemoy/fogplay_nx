#pragma once
#include "features/session/application/rental_session_coordinator.hpp"

// UI-only adapter double. Guard/storage/slot behavior is tested with the real
// coordinator in rental_session_coordinator_tests.cpp; this mutable fixture
// state lets isolated auth/input/catalog adapters exercise their own gates.
struct RentalSessionUiFake {
    using Owner = fog::RentalSessionCoordinator;
    Owner::State snapshot;
    fog::RentalStopCoordinator policy;
    unsigned externalJoins = 0;
    const Owner::State& state() const { return snapshot; }
    void pending(bool value) { snapshot.pending = snapshot.durablePending = value; }
    void paired(bool value) { snapshot.pairedPending = value; snapshot.pairedSupported = true; if (value) pending(true); }
    bool requested() const { return policy.requested(); }
    bool paused() const { return policy.paused(); }
    unsigned attempts() const { return policy.attempts(); }
    void request() { policy.request(); }
    void pause() { policy.pause(); }
    bool manualRetry() { return policy.manualRetry(); }
    void shutdown() { policy.shutdown(); }
    bool beginLaunch(Owner::Admission admission) {
        if (admission.busy || admission.exiting || !admission.focused || snapshot.pending) return false;
        policy.newRental(); snapshot.pending = snapshot.launchReserved = true; return true;
    }
    bool cancelLaunch() {
        if (snapshot.durablePending) return false;
        snapshot.pending = snapshot.launchReserved = false; policy.clearRequest(); return true;
    }
    void externalServiceTaskJoined() { ++externalJoins; }
    Owner::Effects completeJoined(std::exception_ptr error, Owner::Admission, Owner::Time) {
        policy.pause(); Owner::Effects effects; effects.flow = Owner::Flow::Paused; effects.error = error; return effects;
    }
    Owner::Effects completeJoined(Owner::WorkerResult, Owner::Admission, Owner::Time) { return {}; }
};
