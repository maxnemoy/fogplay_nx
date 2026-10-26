#include "app/application_controller.hpp"

namespace fogplay {
void ApplicationController::loadPlayStation() {
    if (sessionPending_ || pending_.valid()) return;
    const bool alreadyViewing = screen_ == Screen::PlayStation || screen_ == Screen::PlayStationDetails;
    latency_.cancel();
    screen_ = Screen::PlayStation; nav_ = 2;
    if (!alreadyViewing) selection_ = 0;
    if (playStationPending_.valid()) return;
    fog::applyPlayStationCatalogSnapshot(playStationComputers_,selection_,fog::playStationComputerPlaceholders());
    playStationLoadedAt_ = {};
    playStationError_.clear();
    status_.clear();
    auto updates = std::make_shared<fog::PlayStationCatalogProgressChannel>();
    auto repository = services_->publicCatalog();
    playStationUpdates_ = updates;
    playStationRepository_ = repository;
    auto featured = pairedServices_ ? pairedServices_->publicCatalog() : std::shared_ptr<fog::CatalogRepository>{};
    featuredRepository_ = featured;
    playStationPending_ = std::async(std::launch::async,[updates,repository,featured] {
        struct Snapshots {
            std::mutex mutex;
            std::vector<fog::Computer> primary = fog::playStationComputerPlaceholders(), featured;
        };
        auto snapshots = std::make_shared<Snapshots>();
        const auto publish = [updates,snapshots](bool secondary,const std::vector<fog::Computer>& snapshot) {
            std::lock_guard<std::mutex> lock(snapshots->mutex);
            (secondary ? snapshots->featured : snapshots->primary) = snapshot;
            auto merged = snapshots->primary;
            merged.insert(merged.end(),snapshots->featured.begin(),snapshots->featured.end());
            updates->publish(merged);
        };
        std::future<std::vector<fog::Computer>> featuredWork;
        if (featured) featuredWork = std::async(std::launch::async,[featured,updates,publish] {
            return featured->playStationComputers(
                [publish](const std::vector<fog::Computer>& snapshot) { publish(true,snapshot); },
                [updates] { return updates->cancelled(); });
        });
        bool primaryOk = false, featuredOk = false;
        std::exception_ptr failure;
        try {
            const auto result = repository->playStationComputers(
                [publish](const std::vector<fog::Computer>& snapshot) { publish(false,snapshot); },
                [updates] { return updates->cancelled(); });
            publish(false,result); primaryOk = true;
        } catch (...) { failure = std::current_exception(); }
        if (featuredWork.valid()) {
            try { publish(true,featuredWork.get()); featuredOk = true; }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (!primaryOk && !featuredOk && failure) std::rethrow_exception(failure);
        std::lock_guard<std::mutex> lock(snapshots->mutex);
        auto result = snapshots->primary;
        result.insert(result.end(),snapshots->featured.begin(),snapshots->featured.end());
        return result;
    });
}
void ApplicationController::pollPlayStation() {
    if (!playStationPending_.valid()) return;
    const bool viewing = screen_ == Screen::PlayStation || screen_ == Screen::PlayStationDetails;
    if (playStationUpdates_) {
        if (auto snapshot = playStationUpdates_->take()) {
            if (viewing) fog::applyPlayStationCatalogSnapshot(playStationComputers_,selection_,std::move(*snapshot));
            else playStationComputers_ = std::move(*snapshot);
        }
    }
    if (playStationPending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        auto snapshot = playStationPending_.get();
        if (playStationUpdates_ && playStationUpdates_->cancelled()) {
            playStationUpdates_.reset(); playStationRepository_.reset(); featuredRepository_.reset(); return;
        }
        if (viewing) fog::applyPlayStationCatalogSnapshot(playStationComputers_,selection_,std::move(snapshot));
        else playStationComputers_ = std::move(snapshot);
        playStationLoadedAt_ = std::chrono::steady_clock::now();
    } catch (const std::exception& error) {
        if (!playStationUpdates_ || !playStationUpdates_->cancelled()) {
            playStationError_ = safeText(error.what());
            diagnostic("PlayStation refresh failed: " + playStationError_);
        }
    }
    playStationUpdates_.reset();
    playStationRepository_.reset(); featuredRepository_.reset();
}
void ApplicationController::cancelPlayStation() {
    if (playStationUpdates_) playStationUpdates_->cancel();
    if (playStationRepository_) playStationRepository_->cancel();
    if (featuredRepository_) featuredRepository_->cancel();
}
}
