#include "app/application_controller.hpp"

namespace fogplay {

void ApplicationController::loadHistory() {
    if (!api_.loggedIn()) { loginReturn_ = Screen::History; screen_ = Screen::Consent; return; }
    screen_ = Screen::History; selection_ = 0;
    start(Task::History,[this] { return api_.orderHistory(); });
}

void ApplicationController::loadGameDetail() {
    latency_.cancel();
    screen_ = Screen::GameDetail; selection_ = 0;
    start(Task::GameDetail,[this] {
        auto game = api_.gameDetail(selectedGameId_);
        Json choices = Json::object();
        try { choices = api_.computerAuto(selectedGameId_,ownSteam_); } catch (...) {}
        return Json{{"game",std::move(game)},{"auto",std::move(choices)}};
    });
}

void ApplicationController::loadGames() {
    screen_ = Screen::Games;
    latency_.cancel();
    catalog_.reset(search_, genreId_); selection_ = 0;
    status_.clear();
    homeCatalog_.reset(recentGames_.items().size(), search_.empty() && genreId_ == 0);
    if (catalogApi_) catalogApi_->cancel();
    // A superseded worker is joined by pollCatalog; never wait in input or
    // replace a live async future (its destructor would block navigation).
    requestCatalog();
}

void ApplicationController::requestCatalog(bool manual) {
    if (catalogPending_.valid() || exitAfterStop_ || sessionPending_) return;
    const auto request = catalog_.begin(manual);
    if (!request) return;
    try {
        // Public catalog has its own API instance. Auth/session state remains
        // exclusive to the original worker, as required by Api's contract.
        catalogApi_ = std::make_shared<fog::Api>(catalogDeviceId_);
        catalogRequest_ = *request;
        catalogPending_ = std::async(std::launch::async, [api=catalogApi_, request=*request] {
            return api->games(request.query, request.page, fog::Catalog::PageSize, request.genreId);
        });
    } catch (const std::exception& error) {
        catalog_.fail(*request, safeText(error.what()));
        catalogApi_.reset();
        diagnostic("Catalog worker could not start: " + catalog_.error());
    }
}

void ApplicationController::pollCatalog() {
    if (!catalogPending_.valid() || catalogPending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto request = catalogRequest_;
    try {
        const auto page = catalogPending_.get();
        if (catalog_.complete(request, page))
            diagnostic("Catalog page OK, page=" + std::to_string(request.page) +
                " loaded=" + std::to_string(catalog_.items().size()) +
                " end=" + std::to_string(catalog_.ended()));
    } catch (const std::exception& error) {
        if (catalog_.fail(request, safeText(error.what())))
            diagnostic("Catalog page failed: " + catalog_.error());
    }
    catalogApi_.reset();
}

void ApplicationController::retryCatalog() {
    if (!catalogPending_.valid() && catalog_.retry()) requestCatalog(true);
}

void ApplicationController::moveCatalog(u64 keys) {
    if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) &&
        (homeCatalog_.recentFocused(catalog_,recentGames_.items().size()) ||
         (!homeCatalog_.available(catalog_,recentGames_.items().size()) && catalog_.selected() < fog::Catalog::Columns))) {
        genreBarFocused_ = true;
        return;
    }
    using Direction = fog::Catalog::Direction;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
        if (homeCatalog_.move(catalog_,recentGames_.items().size(),Direction::Left) && !sessionPending_) sidebar_ = true;
    }
    if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) homeCatalog_.move(catalog_,recentGames_.items().size(),Direction::Right);
    if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) homeCatalog_.move(catalog_,recentGames_.items().size(),Direction::Up);
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) homeCatalog_.move(catalog_,recentGames_.items().size(),Direction::Down);
    selection_ = catalog_.selected();
}

void ApplicationController::selectCatalogGame(const Json& game) {
    // Detail responses may omit or replace catalog artwork/name metadata.
    selectedRecentGame_ = fog::RecentGame::fromCatalog(game);
    autoComputers_ = Json::object(); computers_ = Json::array();
    detail_ = game;
    selectedGameId_ = detail_.at("id").get<int>(); selectedGameName_ = field(detail_,"name");
    ownSteam_ = false; loadGameDetail();
}

void ApplicationController::loadComputers() {
    latency_.request();
    screen_ = Screen::Computers;
    computers_ = Json::array(); selection_ = 0;
    const int id = selectedGameId_, page = computerPage_;
    start(Task::Computers, [this, id, page] { return api_.computers(id, page, 8, ownSteam_); });
}

void ApplicationController::searchGames() {
    const auto query = keyboard("Search games", search_, 96);
    if (query) { search_ = *query; sidebar_ = false; genreBarFocused_ = false; loadGames(); }
}

std::string ApplicationController::latencyText(const Json& computer) const {
    if (const auto ping = latency_.estimate(computer)) return "≈ " + std::to_string(*ping) + " мс";
    return latency_.busy() ? "Пинг: измеряем…" : "Пинг: нет данных";
}

std::size_t ApplicationController::selectedGenreIndex() const {
    const auto& genres = fog::catalogGenres();
    for (std::size_t i=0; i<genres.size(); ++i) if (genres[i].id == genreId_) return i+1;
    return 0;
}

std::size_t ApplicationController::selectedGenreChip() const {
    const auto index = selectedGenreIndex();
    return index > 0 && index <= 5 ? index-1 : 5;
}

void ApplicationController::chooseGenre(int id) {
    genreId_ = id;
    loadGames();
    genreBarFocused_ = true;
    genreChip_ = selectedGenreChip();
}

void ApplicationController::inputGenres(u64 keys) {
    constexpr std::size_t columns = 2;
    const auto count = fog::catalogGenres().size()+1;
    if (keys & HidNpadButton_B) { screen_ = Screen::Games; selection_ = catalog_.selected(); return; }
    if ((keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) && genreDialogSelection_ % columns) --genreDialogSelection_;
    if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) && genreDialogSelection_ % columns == 0 && genreDialogSelection_+1 < count) ++genreDialogSelection_;
    if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && genreDialogSelection_ >= columns) genreDialogSelection_ -= columns;
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) genreDialogSelection_ = std::min(count-1,genreDialogSelection_+columns);
    if (keys & HidNpadButton_A) chooseGenre(genreDialogSelection_ ? fog::catalogGenres()[genreDialogSelection_-1].id : 0);
}

void ApplicationController::inputGenreBar(u64 keys) {
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
        if (genreChip_) --genreChip_; else sidebar_ = true;
    }
    if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) && genreChip_ < 5) ++genreChip_;
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) genreBarFocused_ = false;
    if (keys & HidNpadButton_A) {
        if (genreChip_ < 5) chooseGenre(fog::catalogGenres()[genreChip_].id);
        else { genreDialogSelection_ = selectedGenreIndex(); screen_ = Screen::Genres; }
    }
}

void ApplicationController::pollRecentSave() noexcept {
    try {
        if (recentSave_.valid()) {
            if (recentSave_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try { diagnostic(recentSave_.get() ? "Recent game saved." : "Recent game save failed; playback continues."); }
            catch (...) { diagnostic("Recent game save failed; playback continues."); }
        }
        if (!recentSavePending_) return;
        recentSave_ = std::async(std::launch::async, [snapshot = recentGames_]() mutable { return snapshot.save(); });
        recentSavePending_ = false;
    } catch (...) {
        recentSavePending_ = false;
        diagnostic("Recent history worker unavailable; playback continues.");
    }
}

void ApplicationController::rememberDecodedGame() noexcept {
    try {
        const auto game = recentLaunch_.decoded();
        if (!game) return;
        if (!recentGames_.update(*game)) return;
        homeCatalog_.launched();
        recentSavePending_ = true;
        pollRecentSave();
    } catch (...) {
        // Optional metadata storage cannot stop or restart paid playback.
    }
}

}
