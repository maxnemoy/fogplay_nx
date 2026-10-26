#pragma once
#include "core/constants/error_text.hpp"
#include "features/catalog/domain/models.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace fog {
using PlayStationCatalogProgress = CatalogProgress;
using PlayStationCancellation = CatalogCancellation;
inline void checkPlayStationCancellation(const PlayStationCancellation& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error(fog::constants::error::PlayStation::PlayStationRefreshCancelled);
}
// One producer, main-thread consumer. Only the latest complete snapshot is retained.
// The shared channel outlives either endpoint; cancellation discards late deliveries.
class PlayStationCatalogProgressChannel {
public:
    void publish(const std::vector<Computer>& snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled()) latest_ = snapshot;
    }
    std::optional<std::vector<Computer>> take() {
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
    std::optional<std::vector<Computer>> latest_;
    std::atomic_bool cancelled_{false};
};
inline void applyPlayStationCatalogSnapshot(std::vector<Computer>& computers, std::size_t& selected, std::vector<Computer> snapshot) {
    const EntityId selectedId = selected < computers.size() ? computers[selected].id : EntityId{};
    computers = std::move(snapshot);
    if (!selectedId.empty()) {
        for (std::size_t index=0; index<computers.size(); ++index)
            if (computers[index].id == selectedId) { selected = index; return; }
    }
    selected = computers.empty() ? 0 : std::min(selected,computers.size()-1);
}
}
