#pragma once
#include "features/session/domain/service_models.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace fog {
using PlayStationCatalogProgress = std::function<void(const Json&)>;
using PlayStationCancellation = std::function<bool()>;
inline void checkPlayStationCancellation(const PlayStationCancellation& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("PlayStation refresh cancelled");
}
// One producer, main-thread consumer. Only the latest complete snapshot is retained.
// The shared channel outlives either endpoint; cancellation discards late deliveries.
class PlayStationCatalogProgressChannel {
public:
    void publish(const Json& snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled()) latest_ = snapshot;
    }
    std::optional<Json> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = std::move(latest_);
        latest_.reset();
        return result;
    }
    void cancel() {
        cancelled_.store(true,std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.reset();
    }
    bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }
private:
    std::mutex mutex_;
    std::optional<Json> latest_;
    std::atomic_bool cancelled_{false};
};
inline void applyPlayStationCatalogSnapshot(Json& computers, std::size_t& selected, Json snapshot) {
    const int selectedId = selected < computers.size() ? computers[selected].value("id",0) : 0;
    computers = std::move(snapshot);
    if (selectedId) {
        for (std::size_t index=0; index<computers.size(); ++index)
            if (computers[index].value("id",0) == selectedId) { selected = index; return; }
    }
    selected = computers.empty() ? 0 : std::min(selected,computers.size()-1);
}
}
