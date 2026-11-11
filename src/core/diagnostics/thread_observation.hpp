#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
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
    for (const char* value : {"application", "runlog", "artwork", "steam-artwork", "latency", "settings-save",
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
// Published values deliberately exclude the thread handle/id. These are
// observations of thread entry, never live scheduling queries.
struct ThreadObservationValue {
    std::array<char,48> role{};
    int core=-1, idealCore=-1, priority=-1;
    std::uint64_t affinity=0, observedEpoch=0;
    std::int64_t affinityResult=-1, priorityResult=-1;
    unsigned queryErrors=0;
    bool carried=false, available=false;
};
struct ThreadObservationSnapshot {
    std::array<ThreadObservationValue,128> entries{};
    std::size_t size=0;
    std::uint64_t epoch=0, overflow=0, persistentOverflow=0;
};
static_assert(std::is_trivially_copyable_v<ThreadObservationSnapshot>);
static_assert(sizeof(ThreadObservationSnapshot) <= 16384);
inline ThreadObservationSnapshot captureThreadObservations() {
    auto& state = threadObservations();
    std::lock_guard<std::mutex> lock(state.mutex);
    ThreadObservationSnapshot value;
    value.size=state.size; value.epoch=state.epoch; value.overflow=state.overflow;
    value.persistentOverflow=state.persistentOverflow;
    for (std::size_t i = 0; i < state.size; ++i) {
        const auto& item = state.entries[i];
        value.entries[i]={item.role,item.core,item.idealCore,item.priority,item.affinity,item.observedEpoch,
            item.affinityResult,item.priorityResult,item.queryErrors,item.carried,item.available};
    }
    return value;
}
inline bool operator==(const ThreadObservationValue& a, const ThreadObservationValue& b) noexcept {
    return a.role == b.role && a.core == b.core && a.idealCore == b.idealCore && a.priority == b.priority &&
        a.affinity == b.affinity && a.observedEpoch == b.observedEpoch && a.affinityResult == b.affinityResult &&
        a.priorityResult == b.priorityResult && a.queryErrors == b.queryErrors && a.carried == b.carried && a.available == b.available;
}
inline bool operator==(const ThreadObservationSnapshot& a, const ThreadObservationSnapshot& b) noexcept {
    if (a.size != b.size || a.epoch != b.epoch || a.overflow != b.overflow || a.persistentOverflow != b.persistentOverflow)
        return false;
    return std::equal(a.entries.begin(), a.entries.begin() + std::min(a.size, a.entries.size()), b.entries.begin());
}
inline bool operator!=(const ThreadObservationSnapshot& a, const ThreadObservationSnapshot& b) noexcept { return !(a == b); }
inline std::string threadObservationSummary(std::uint64_t epoch, std::size_t size,
        std::uint64_t overflow, std::uint64_t persistentOverflow) {
    return "Thread scheduling scope=transport-observed-starts inventory=not-live epoch=" + std::to_string(epoch) +
        " count=" + std::to_string(size) + " overflow=" + std::to_string(overflow) +
        " persistentOverflowLifetime=" + std::to_string(persistentOverflow);
}
inline std::string threadObservationRow(const ThreadObservationValue& item, std::size_t slot, std::uint64_t epoch) {
    const auto end = std::find(item.role.begin(), item.role.end(), '\0');
    std::string role(item.role.begin(), end);
    // Matches diagnostic(safeText): role names never inject a deferred separator.
    for (char& c : role) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return "Thread scheduling role=" + role + " slot=" + std::to_string(slot) +
            " epoch=" + std::to_string(epoch) + " observedEpoch=" + std::to_string(item.observedEpoch) +
            " carried=" + std::to_string(item.carried) +
            " available=" + std::to_string(item.available) + " core=" + std::to_string(item.core) +
            " ideal=" + std::to_string(item.idealCore) + " mask=" + std::to_string(item.affinity) +
            " priority=" + std::to_string(item.priority) + " queryErrors=" + std::to_string(item.queryErrors) +
            " affinityResult=" + std::to_string(item.affinityResult) + " priorityResult=" + std::to_string(item.priorityResult);
}
// Pure formatter, independent of registry lock and worker/application lifetime.
inline std::string threadObservationLine(const ThreadObservationSnapshot& value, std::size_t line) {
    if (!line) return threadObservationSummary(value.epoch, value.size, value.overflow, value.persistentOverflow);
    const auto i = line - 1;
    if (i >= value.size || i >= value.entries.size()) return {};
    return threadObservationRow(value.entries[i], i, value.epoch);
}
inline std::vector<std::string> threadObservationLines(const ThreadObservationSnapshot& value) {
    std::vector<std::string> result;
    result.push_back(threadObservationLine(value,0));
    for(std::size_t i=0;i<value.size && i<value.entries.size();++i)
        result.push_back(threadObservationLine(value,i+1));
    return result;
}
inline std::vector<std::string> threadObservationLines() {
    return threadObservationLines(captureThreadObservations());
}
inline std::string threadObservationText() {
    std::string result;
    for (const auto& line : threadObservationLines()) { result += line; result += '\n'; }
    return result;
}
}
