#include "core/constants/storage_paths.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

void ApplicationController::moveGrid(u64 keys, std::size_t count, std::size_t columns) {
    if (!count) { selection_ = 0; return; }
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) { if (selection_ % columns) --selection_; else if (!sessionPending_ || recoveryPaused_) sidebar_ = true; }
    if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) && selection_ + 1 < count) ++selection_;
    if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && selection_ >= columns) selection_ -= columns;
    if ((keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) && selection_ + columns < count) selection_ += columns;
}

void ApplicationController::input(u64 keys) {
    if (screen_ == Screen::Streaming) return;
    if (screen_ == Screen::SessionRating) { inputSessionRating(keys); return; }
    if (screen_ == Screen::StopConfirm) {
        if (keys & HidNpadButton_A) beginFinish("stop-confirm");
        else if (keys & HidNpadButton_B) screen_ = Screen::Streaming;
        return;
    }
    if (screen_ == Screen::ManualResolution) {
        if (keys & HidNpadButton_A) start(Task::ManualResolution,[this] { checkout_.acknowledgeManualResolution(); return std::monostate{}; });
        else if (keys & HidNpadButton_B) { screen_ = recoveryPaused_ ? Screen::Recovery : Screen::Checkout; }
        return;
    }
    if (screen_ == Screen::Profile) {
        if (keys & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_StickLUp | HidNpadButton_StickLDown)) selection_ = selection_ ? 0 : 1;
        loginProvider_ = selection_ ? fog::ProviderId::FogHub : fog::ProviderId::Mts;
        if (keys & HidNpadButton_B) { screen_ = sessionPending_ ? Screen::Recovery : Screen::Games; return; }
        if (!providerServices(loginProvider_)) return;
        if (keys & HidNpadButton_A) refreshProfile();
        else if ((keys & HidNpadButton_X) && !sessionPending_) {
            start(Task::ProviderForget,[this] { loginAuth().forget(); return std::monostate{}; });
        } else if ((keys & HidNpadButton_R) && loginProvider_ == fog::ProviderId::Mts) loadSessions();
        else if ((keys & HidNpadButton_R) && pairedPending_) recoverSession();
        else if ((keys & HidNpadButton_R) && loginProvider_ == fog::ProviderId::FogHub && !sessionPending_)
            openSessionRating();
        return;
    }
    if (screen_ == Screen::Checkout) {
        if (keys & HidNpadButton_Y) openProfile();
        else if (keys & HidNpadButton_B) queueCheckoutStop();
        else if ((keys & HidNpadButton_A) && (pairedPending_ ? pairedServices_->auth().authenticated() : auth_.authenticated())) pollCheckout(true);
        else if ((keys & HidNpadButton_X) && !pairedPending_ && checkout_.unresolved()) screen_ = Screen::ManualResolution;
        return;
    }
    if (screen_ == Screen::Stopping || screen_ == Screen::Recovery) {
        if (keys & HidNpadButton_Y) openProfile();
        else if ((keys & HidNpadButton_X) && !pairedPending_ && checkout_.unresolved()) screen_ = Screen::ManualResolution;
        else if ((keys & HidNpadButton_B) && recoveryPaused_) {
            loadGames(); sidebar_ = true; status_ = uiText::Server::RecoveryStillPending;
        }
        else if ((keys & HidNpadButton_A) && (pairedPending_ ? pairedServices_->auth().authenticated() : auth_.authenticated())) {
            retrySessionRecovery();
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
    if (screen_ == Screen::ConnectionSettings) { inputConnectionSettings(keys); return; }
    if (screen_ == Screen::RumbleSettings) { inputRumbleSettings(keys); return; }
    if (screen_ == Screen::ControllerSettings) { inputControllerSettings(keys); return; }
    if (screen_ == Screen::Genres) { inputGenres(keys); return; }
    if (sidebar_) {
        if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) nav_ = (nav_ + 4) % 5;
        if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) nav_ = (nav_ + 1) % 5;
        if (keys & (HidNpadButton_B | HidNpadButton_Right | HidNpadButton_StickLRight)) sidebar_ = false;
        if (keys & HidNpadButton_Y) { openProfile(); }
        if (keys & HidNpadButton_A) {
            sidebar_ = false; selection_ = 0;
            if (nav_ == 0) searchGames();
            else if (nav_ == 1) { search_.clear(); genreId_ = 0; genreBarFocused_ = false; loadGames(); }
            else if (nav_ == 2) loadPlayStation();
            else if (nav_ == 3) loadHistory();
            else screen_ = Screen::Settings;
        }
        return;
    }
    if (keys & HidNpadButton_Y) { openProfile(); return; }
    if (keys & HidNpadButton_B) {
        if (screen_ == Screen::PlayStationDetails) screen_ = Screen::PlayStation;
        else if (screen_ == Screen::Details) {
            screen_ = detailsFromAutomatic_ ? Screen::GameDetail : Screen::Computers;
            if (detailsFromAutomatic_) selection_ = automaticSelection_;
            if (detailsFromAutomatic_) latency_.cancel(); else latency_.request();
        }
        else if (screen_ == Screen::Computers) { latency_.cancel(); screen_ = Screen::GameDetail; selection_ = 4; }
        else if (screen_ == Screen::GameDetail) { latency_.cancel(); screen_ = Screen::Games; selection_ = catalog_.selected(); }
        else sidebar_ = true;
        return;
    }
    if (screen_ == Screen::PlayStationDetails) {
        if (keys & HidNpadButton_A) createSession();
        else if (keys & HidNpadButton_X) loadPlayStation();
        return;
    }
    if (screen_ == Screen::PlayStation) {
        const auto count = playStationComputers_.size();
        constexpr std::size_t columns = 4;
        if (!count) selection_ = 0;
        else {
            selection_ = std::min(selection_,count-1);
            if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) {
                if (selection_ % columns) --selection_;
                else if (!sessionPending_ || recoveryPaused_) sidebar_ = true;
            }
            if ((keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) && selection_ % columns < columns-1 && selection_+1 < count) ++selection_;
            if ((keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) && selection_ >= columns) selection_ -= columns;
            if ((keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) && (selection_/columns+1)*columns < count)
                selection_ = std::min(selection_+columns,count-1);
        }
        if (keys & HidNpadButton_X) loadPlayStation();
        else if ((keys & HidNpadButton_A) && selection_ < playStationComputers_.size()) {
            const auto& pc = playStationComputers_[selection_];
            if (pc.listed && pc.launchable) screen_ = Screen::PlayStationDetails;
            else status_ = uiText::Server::TariffUnconfirmedRefreshHint;
        }
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
                const auto& found = autoComputers_.at(selection_-1);
                if (!found.empty()) {
                    auto candidate = found.front();
                    candidate.name = uiText::Server::NumberPrefix + candidate.id.value;
                    detailsFromAutomatic_ = true; automaticSelection_ = selection_;
                    computers_ = {candidate}; selection_ = 0; screen_ = Screen::Details; latency_.cancel();
                } else status_ = uiText::Server::NoCategoryOffer;
            }
        }
        return;
    }
    if (screen_ == Screen::Settings) {
        moveGrid(keys, 6, 2);
        if (keys & HidNpadButton_A) {
            if (!selection_) { screen_ = Screen::ImageSettings; selection_ = 0; }
            else if (selection_ == 1) { screen_ = Screen::ControllerSettings; selection_ = 0; }
            else if (selection_ == 2) { screen_ = Screen::ConnectionSettings; selection_ = 0; }
            else if (selection_ == 3) loadSessions();
            else if (selection_ == 4) { openProfile(); }
            else { screen_ = Screen::RumbleSettings; selection_ = 0; }
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
