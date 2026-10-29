#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fog::diagnostics {
struct ThreadObservation {
    std::thread::id id;
    std::array<char, 48> role{};
    int core = -1, idealCore = -1, priority = -1;
    std::uint64_t affinity = 0;
    std::uint64_t observedEpoch = 0;
    std::int64_t affinityResult = -1, priorityResult = -1;
    unsigned queryErrors = 0;
    bool carried = false;
    bool available = false;
};
struct ThreadObservations {
    std::mutex mutex;
    std::array<ThreadObservation, 128> entries{};
    std::size_t size = 0;
    std::size_t next = 0;
    std::uint64_t epoch = 0, overflow = 0;
    // Independent from transient catalog work. SDK workers may start before
    // the first transport and do not necessarily call their entry hook again.
    std::array<ThreadObservation, 64> persistent{};
    std::size_t persistentSize = 0, persistentNext = 0;
    std::uint64_t persistentOverflow = 0;
};
inline ThreadObservations& threadObservations() {
    static ThreadObservations state;
    return state;
}
inline bool persistentThreadRole(const char* role) {
    for (const char* value : {"application", "runlog", "artwork", "steam-artwork", "latency",
            "RTC worker", "RTC poll", "RTC server", "juice poll", "juice mux", "juice agent",
            "SCTP timer", "SCTP iterator", "rtc-control", "rtc-signaling"})
        if (std::strcmp(role, value) == 0) return true;
    return false;
}
// Begins a transport's bounded start-observation history, not a live inventory.
// Carried rows retain their original epoch and can describe a thread that has
// exited; they are startup evidence, never a fresh scheduling query.
inline void beginThreadObservationEpoch() noexcept {
    try {
        auto& state = threadObservations();
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.epoch;
        state.size = state.persistentSize;
        state.next = state.size;
        state.overflow = 0;
        for (std::size_t i = 0; i < state.size; ++i) {
            state.entries[i] = state.persistent[i];
            state.entries[i].carried = true;
        }
    } catch (...) { /* Optional observation must not prevent transport startup. */ }
}
// Call once at entry to a worker. Copy the role so SDK temporary names are safe.
// Captures scheduling state without changing affinity or priority and without
// logging recursively from the RunLog writer.
inline void observeThread(const char* role) noexcept {
    try {
        ThreadObservation item;
        item.id = std::this_thread::get_id();
        if (role) std::strncpy(item.role.data(), role, item.role.size() - 1);
#ifdef __SWITCH__
        item.core = svcGetCurrentProcessorNumber();
        s32 ideal = -1, priority = -1;
        u64 mask = 0;
        const auto affinityResult = svcGetThreadCoreMask(&ideal, &mask, CUR_THREAD_HANDLE);
        const auto priorityResult = svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        item.idealCore = ideal; item.affinity = mask; item.priority = priority;
        item.affinityResult = affinityResult; item.priorityResult = priorityResult;
        item.queryErrors = (R_FAILED(affinityResult) ? 1u : 0u) | (R_FAILED(priorityResult) ? 2u : 0u);
        item.available = R_SUCCEEDED(affinityResult) && R_SUCCEEDED(priorityResult);
#endif
        auto& state = threadObservations();
        std::lock_guard<std::mutex> lock(state.mutex);
        item.observedEpoch = state.epoch;
        if (persistentThreadRole(item.role.data())) {
            std::size_t slot = state.persistentSize;
            for (std::size_t i = 0; i < state.persistentSize; ++i)
                if (state.persistent[i].id == item.id && state.persistent[i].role == item.role) { slot = i; break; }
            if (slot == state.persistentSize) {
                if (state.persistentSize < state.persistent.size()) ++state.persistentSize;
                else { slot = state.persistentNext; ++state.persistentOverflow; }
                state.persistentNext = (slot + 1) % state.persistent.size();
            }
            state.persistent[slot] = item;
        }
        for (std::size_t i = 0; i < state.size; ++i)
            if (state.entries[i].id == item.id && state.entries[i].role == item.role) {
                state.entries[i] = item; return;
            }
        if (state.size == state.entries.size()) {
            state.entries[state.next] = item;
            state.next = (state.next + 1) % state.entries.size();
            ++state.overflow;
        } else {
            state.entries[state.size++] = item;
            state.next = state.size % state.entries.size();
        }
    } catch (...) { /* Optional diagnostics must not affect worker startup. */ }
}
inline std::vector<std::string> threadObservationLines() {
    auto& state = threadObservations();
    std::lock_guard<std::mutex> lock(state.mutex);
    std::vector<std::string> result;
    result.push_back("Thread scheduling scope=transport-observed-starts inventory=not-live epoch=" + std::to_string(state.epoch) +
        " count=" + std::to_string(state.size) + " overflow=" + std::to_string(state.overflow) +
        " persistentOverflowLifetime=" + std::to_string(state.persistentOverflow));
    for (std::size_t i = 0; i < state.size; ++i) {
        const auto& item = state.entries[i];
        result.push_back("Thread scheduling role=" + std::string(item.role.data()) + " slot=" + std::to_string(i) +
            " epoch=" + std::to_string(state.epoch) + " observedEpoch=" + std::to_string(item.observedEpoch) +
            " carried=" + std::to_string(item.carried) +
            " available=" + std::to_string(item.available) + " core=" + std::to_string(item.core) +
            " ideal=" + std::to_string(item.idealCore) + " mask=" + std::to_string(item.affinity) +
            " priority=" + std::to_string(item.priority) + " queryErrors=" + std::to_string(item.queryErrors) +
            " affinityResult=" + std::to_string(item.affinityResult) + " priorityResult=" + std::to_string(item.priorityResult));
    }
    return result;
}
inline std::string threadObservationText() {
    std::string result;
    for (const auto& line : threadObservationLines()) { result += line; result += '\n'; }
    return result;
}
}
