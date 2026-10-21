#include "app/application_controller.hpp"

namespace fogplay {

void ApplicationController::moveGrid(u64 keys, std::size_t count, std::size_t columns) {
    if (!count) { selection_ = 0; return; }
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) { if (selection_ % columns) --selection_; else if (!sessionPending_) sidebar_ = true; }
    if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) && selection_ + 1 < count) ++selection_;
    if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && selection_ >= columns) selection_ -= columns;
    if ((keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) && selection_ + columns < count) selection_ += columns;
}

void ApplicationController::input(u64 keys) {
    if (screen_ == Screen::Streaming) return;
    if (screen_ == Screen::StopConfirm) {
        if (keys & HidNpadButton_A) beginFinish("stop-confirm");
        else if (keys & HidNpadButton_B) screen_ = Screen::Streaming;
        return;
    }
    if (screen_ == Screen::ManualResolution) {
        if (keys & HidNpadButton_A) start(Task::ManualResolution,[this] { checkout_.acknowledgeManualResolution(); return Json::object(); });
        else if (keys & HidNpadButton_B) screen_ = Screen::Checkout;
        return;
    }
    if (screen_ == Screen::Checkout) {
        if (keys & HidNpadButton_Y) screen_ = Screen::Consent;
        else if (keys & HidNpadButton_B) queueCheckoutStop();
        else if ((keys & HidNpadButton_A) && api_.loggedIn()) pollCheckout(true);
        else if ((keys & HidNpadButton_X) && !guard_.persistent() && guard_.pendingHash().empty()) screen_ = Screen::ManualResolution;
        return;
    }
    if (screen_ == Screen::Stopping || screen_ == Screen::Recovery) {
        if (keys & HidNpadButton_Y) screen_ = Screen::Consent;
        else if ((keys & HidNpadButton_A) && api_.loggedIn()) {
            if (guard_.ownsSession()) beginFinish(); else recoverSession();
        }
        return;
    }
    if (screen_ == Screen::Consent) {
        if (keys & HidNpadButton_A) beginLogin();
        else if (keys & HidNpadButton_B) {
            screen_ = sessionPending_ ? Screen::Recovery : loginReturn_;
            if (screen_ == Screen::Games && catalog_.items().empty()) loadGames();
        }
        return;
    }
    if (screen_ == Screen::Auth) {
        if (keys & HidNpadButton_A) enterCode();
        else if (keys & HidNpadButton_B) {
            screen_ = sessionPending_ ? Screen::Recovery : loginReturn_;
            if (screen_ == Screen::Games && catalog_.items().empty()) loadGames();
        }
        return;
    }
    if (screen_ == Screen::ConnectConsent) {
        if (keys & HidNpadButton_A) connectSession();
        else if (keys & HidNpadButton_B) screen_ = Screen::Sessions;
        return;
    }
    if (screen_ == Screen::ImageSettings) { inputImageSettings(keys); return; }
    if (screen_ == Screen::Genres) { inputGenres(keys); return; }
    if (sidebar_) {
        if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) nav_ = (nav_ + 3) % 4;
        if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) nav_ = (nav_ + 1) % 4;
        if (keys & (HidNpadButton_B | HidNpadButton_Right)) sidebar_ = false;
        if (keys & HidNpadButton_Y) { sidebar_ = false; loginReturn_ = screen_; if (api_.loggedIn()) loadSessions(); else screen_ = Screen::Consent; }
        if (keys & HidNpadButton_A) {
            sidebar_ = false; selection_ = 0;
            if (nav_ == 0) searchGames();
            else if (nav_ == 1) { search_.clear(); genreId_ = 0; genreBarFocused_ = false; loadGames(); }
            else if (nav_ == 2) loadHistory();
            else screen_ = Screen::Settings;
        }
        return;
    }
    if (keys & HidNpadButton_Y) { loginReturn_ = screen_; if (api_.loggedIn()) loadSessions(); else screen_ = Screen::Consent; return; }
    if (keys & HidNpadButton_B) {
        if (screen_ == Screen::Details) {
            screen_ = detailsFromAutomatic_ ? Screen::GameDetail : Screen::Computers;
            if (detailsFromAutomatic_) selection_ = automaticSelection_;
            if (detailsFromAutomatic_) latency_.cancel(); else latency_.request();
        }
        else if (screen_ == Screen::Computers) { latency_.cancel(); screen_ = Screen::GameDetail; selection_ = 4; }
        else if (screen_ == Screen::GameDetail) { latency_.cancel(); screen_ = Screen::Games; selection_ = catalog_.selected(); }
        else sidebar_ = true;
        return;
    }
    if (screen_ == Screen::Details) { if (keys & HidNpadButton_A) createSession(); else if ((keys & HidNpadButton_X) && !detailsFromAutomatic_) latency_.request(); return; }
    if (screen_ == Screen::GameDetail) {
        if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && selection_) --selection_;
        if ((keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) && selection_ < 4) ++selection_;
        if ((keys & HidNpadButton_Left) && selection_ > 1 && selection_ < 4) --selection_;
        if ((keys & HidNpadButton_Right) && selection_ > 0 && selection_ < 3) ++selection_;
        if (keys & HidNpadButton_A) {
            if (!selection_) { ownSteam_ = !ownSteam_; loadGameDetail(); }
            else if (selection_ == 4) { computerPage_ = 1; loadComputers(); }
            else {
                const auto key = std::array<const char*,3>{"min_price","optimum","max_power"}[selection_-1];
                const auto found = autoComputers_.find(key);
                if (found != autoComputers_.end() && found->is_array() && !found->empty()) {
                    auto candidate = found->front();
                    candidate["name"] = "Сервер №" + field(candidate,"id");
                    detailsFromAutomatic_ = true; automaticSelection_ = selection_;
                    computers_ = Json::array({candidate}); selection_ = 0; screen_ = Screen::Details; latency_.cancel();
                } else status_ = "Нет предложения в этой категории. Выберите все серверы.";
            }
        }
        return;
    }
    if (screen_ == Screen::Settings) {
        moveGrid(keys, 4, 2);
        if (keys & HidNpadButton_A) {
            if (!selection_) { screen_ = Screen::ImageSettings; selection_ = 0; }
            else if (selection_ == 1) {
                auto next = controllerSettings_; next.xbox = !next.xbox;
                try {
                    fog::controller::saveSettings(std::string(dataDir) + "/controller-settings.json",next);
                    controllerSettings_ = next; ui_.setXboxLayout(next.xbox);
                    status_ = "Раскладка сохранена на SD.";
                } catch (...) { status_ = "Не удалось сохранить раскладку. Настройка не изменена."; }
            }
            else if (selection_ == 2) loadSessions();
            else { api_.forgetAuth(); authSaveFailed_ = false; status_ = "Вы вышли из аккаунта. Сохранённый токен удалён."; }
        }
        return;
    }
    if (screen_ == Screen::History) { moveGrid(keys,history_.size(),fog::ui::HistoryColumns); if (keys & HidNpadButton_X) loadHistory(); return; }
    if (screen_ == Screen::Sessions) {
        moveGrid(keys,sessions_.size(),1);
        if (keys & HidNpadButton_X) loadSessions();
        else if (!sessions_.empty() && (keys & HidNpadButton_A)) screen_ = Screen::ConnectConsent;
        return;
    }
    const auto count = screen_ == Screen::Games ? catalog_.items().size() : computers_.size();
    if (screen_ == Screen::Games && genreBarFocused_) {
        if (keys & HidNpadButton_X) searchGames();
        else if (keys & HidNpadButton_R) retryCatalog();
        else inputGenreBar(keys);
        return;
    }
    if (screen_ == Screen::Games) moveCatalog(keys); else moveGrid(keys,count);
    if (genreBarFocused_ && screen_ == Screen::Games) return;
    if (screen_ == Screen::Games) {
        if (keys & HidNpadButton_X) searchGames();
        else if (keys & HidNpadButton_R) retryCatalog();
        else if (keys & HidNpadButton_A) {
            if (homeCatalog_.recentFocused(catalog_,recentGames_.items().size())) {
                if (homeCatalog_.selected() < recentGames_.items().size())
                    selectCatalogGame(recentGames_.items()[homeCatalog_.selected()].catalogItem());
            } else if (count) selectCatalogGame(catalog_.items().at(selection_));
        }
    } else {
        if (keys & HidNpadButton_X) loadComputers();
        else if (keys & HidNpadButton_R) { if (!computers_.empty()) { ++computerPage_; loadComputers(); } }
        else if (keys & HidNpadButton_L) { if (computerPage_ > 1) { --computerPage_; loadComputers(); } }
        else if (count && (keys & HidNpadButton_A)) { detailsFromAutomatic_ = false; screen_ = Screen::Details; latency_.request(); }
    }
}

}
