#include "core/constants/app_info.hpp"
#include "features/catalog/presentation/genre_text.hpp"
#include "features/settings/presentation/settings_text.hpp"
#include "core/constants/ui_text.hpp"
#include "app/application_controller.hpp"
#include "features/playstation/presentation/catalog_view.hpp"

namespace uiText = ::fog::constants::ui;

namespace fogplay {
namespace {
bool routineStatus(const std::string& value) {
    // Explicitly known success/instruction copy only: unknown errors stay visible.
    for (const auto* routine : {uiText::Common::Ready, uiText::Profile::SessionHistory, uiText::Settings::LayoutSaved,
            uiText::Profile::SignedOut,
            uiText::Profile::AuthSaved,
            uiText::Stream::ConnectingMenuHint,
            uiText::Stream::RestoredMenuHint})
        if (value == routine) return true;
    return value.empty();
}
}

void ApplicationController::drawStreamOverlay() {
    using Mode = fog::stream::OverlayMode;
    if (overlay_.mode() == Mode::Mouse) {
        const int x = static_cast<int>(pointerX_), y = static_cast<int>(pointerY_);
        SDL_SetRenderDrawColor(rt_.renderer,0,0,0,255);
        SDL_Rect border{x-7,y-7,15,15}; SDL_RenderDrawRect(rt_.renderer,&border);
        SDL_SetRenderDrawColor(rt_.renderer,255,255,255,255);
        SDL_RenderDrawLine(rt_.renderer,x-6,y,x+6,y);
        SDL_RenderDrawLine(rt_.renderer,x,y-6,x,y+6);
    }
    if (overlay_.mode() == Mode::ImageSettings && stream_)
        overlay_.setVideoStatus(stream_->qualityStatus());
    // Mouse instructions fade so they do not obscure remote fields/taskbar.
    if (overlay_.mode() != Mode::Mouse || std::chrono::steady_clock::now()-overlayHintAt_ < std::chrono::seconds(5))
        overlay_.paint(ui_,rt_.renderer);
    if (transportReconnect_.recovering()) {
        const auto attempt = transportReconnect_.connecting() ? transportReconnect_.attempts() :
            std::min(transportReconnect_.attempts()+1,fog::stream::TransportReconnect::MaxAttempts);
        box(0,0,1280,82,{23,32,46,255});
        text(uiText::Stream::ReconnectAttemptPrefix + std::to_string(attempt) + uiText::Stream::ReconnectAttemptSuffix,20,8,accent,1240);
        text(uiText::Stream::ReconnectLimitHint,20,44,white,1240);
    } else if (videoRecovery_.active() || videoLossWaiting_) {
        box(0,0,1280,45,{23,32,46,255});
        text(uiText::Stream::RecoveringVideo,20,8,accent,1240);
    } else if (!overlayNotice_.empty() && std::chrono::steady_clock::now()-overlayNoticeAt_ < std::chrono::seconds(4)) {
        box(0,0,1280,45,{23,32,46,255}); text(overlayNotice_,20,8,accent,1240);
    }
}

void ApplicationController::text(const std::string& value, int x, int y, SDL_Color color, int width, bool title) {
    ui_.text(value,x,y,width,color,title,1);
}

void ApplicationController::box(int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(rt_.renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect{x,y,w,h}; SDL_RenderFillRect(rt_.renderer, &rect);
}

SDL_Texture* ApplicationController::cover(const Json& game) {
    return fog::catalogArtwork(game,artwork_,steamArtwork_);
}

void ApplicationController::draw() {
    fog::stream::LoopTiming::Scope artworkScope(loopTiming_, fog::stream::LoopTiming::Stage::Artwork);
    artwork_.update(rt_.renderer);
    artworkScope.finish();
    bool videoDrawn = false;
    {
        fog::stream::LoopTiming::Scope mediaScope(loopTiming_, fog::stream::LoopTiming::Stage::MediaDraw,
            screen_ == Screen::Streaming && media_);
        if (screen_ == Screen::Streaming && media_) videoDrawn = media_->draw();
    }
    if (videoDrawn) {
        fog::stream::LoopTiming::Scope overlayScope(loopTiming_, fog::stream::LoopTiming::Stage::Overlay);
        if (overlay_.mode() == fog::stream::OverlayMode::Closed && std::chrono::steady_clock::now()-streamStarted_ < std::chrono::seconds(12)) {
            box(0,665,1280,55,{23,32,46,255});
            text(uiText::Stream::OverlayMenuHint,30,680,accent);
        }
        drawStreamOverlay();
        overlayScope.finish();
        const auto presentStarted = std::chrono::steady_clock::now();
        fog::stream::LoopTiming::Scope presentScope(loopTiming_, fog::stream::LoopTiming::Stage::Present);
        SDL_RenderPresent(rt_.renderer);
        media_->presented();
        presentTiming_.record(presentStarted);
        presentScope.finish();
        loopTiming_.presented(media_->presentedFrames(),
            !stopAfterTask_ && !transportReconnect_.recovering() && !videoRecovery_.active() &&
            !videoLossWaiting_ && !media_->waitingForVideoKeyframe() &&
            (overlay_.mode() == fog::stream::OverlayMode::Closed || overlay_.mode() == fog::stream::OverlayMode::Mouse),
            std::chrono::steady_clock::now());
        return;
    }
    loopTiming_.suspendPresentation();
    fog::ui::Chrome chrome;
    chrome.sidebarExpanded = sidebar_; chrome.sidebarFocused = sidebar_;
    chrome.account = api_.loggedIn() ? uiText::Profile::Connected : uiText::Profile::SignInAccount;
    chrome.title = uiText::Catalog::Title;
    chrome.section = fog::ui::Section::Games;
    if (screen_ == Screen::Computers || screen_ == Screen::Details || screen_ == Screen::GameDetail) { chrome.title = selectedGameName_; chrome.section = fog::ui::Section::Computers; }
    if (screen_ == Screen::PlayStation || screen_ == Screen::PlayStationDetails) {
        chrome.title = "PLAYSTATION"; chrome.section = fog::ui::Section::PlayStation;
        chrome.subtitle = playStationPending_.valid() ? uiText::PlayStation::RefreshingServers : uiText::PlayStation::SourceStatus;
        chrome.hints = uiText::PlayStation::CatalogHints;
    }
    if (screen_ == Screen::History) { chrome.title = uiText::Profile::HistoryTitle; chrome.section = fog::ui::Section::History; }
    if (screen_ == Screen::Settings || screen_ == Screen::ImageSettings || screen_ == Screen::ConnectionSettings) {
        chrome.title = screen_ == Screen::ImageSettings ? uiText::Settings::ImageTitle :
            screen_ == Screen::ConnectionSettings ? uiText::Settings::ConnectionTitle : uiText::Settings::Title;
        chrome.section = fog::ui::Section::Settings;
    }
    if (screen_ == Screen::ConnectionSettings) chrome.hints = uiText::Settings::ConnectionHints;
    if (screen_ == Screen::ImageSettings) chrome.hints = uiText::Settings::ImageHints;
    if (sidebar_) chrome.section = std::array<fog::ui::Section,5>{fog::ui::Section::Search,fog::ui::Section::Games,fog::ui::Section::PlayStation,fog::ui::Section::History,fog::ui::Section::Settings}[nav_];
    if (screen_ == Screen::Games) {
        if (!search_.empty()) chrome.title = uiText::Search::TitlePrefix + search_;
        chrome.hints = uiText::Catalog::Hints;
        if (!catalog_.error().empty()) chrome.hints += uiText::Common::RetryHintSuffix;
    }
    if (screen_ == Screen::Genres) { chrome.title = uiText::Catalog::GenresTitle; chrome.hints = uiText::Catalog::GenreHints; }
    if (screen_ == Screen::Computers) chrome.hints = uiText::Server::CatalogHints;
    if (screen_ == Screen::Details) chrome.hints = detailsFromAutomatic_ ? uiText::Server::ConnectHints : uiText::Server::RefreshPingHints;
    if (sessionPending_) chrome.hints = uiText::Server::PendingSessionHints;
    if (api_.loggedIn() && authSaveFailed_) chrome.subtitle.clear();
    ui_.frame(chrome);
    const auto area = ui_.content();
    if (screen_ == Screen::Games) {
        std::vector<std::string> genreLabels;
        for (std::size_t i=0; i<5; ++i) genreLabels.emplace_back(fog::ui::genreLabel(fog::catalogGenres()[i]));
        genreLabels.emplace_back(selectedGenreIndex()>5 ? fog::ui::genreLabel(fog::catalogGenres()[selectedGenreIndex()-1]) : uiText::Catalog::AllGenres);
        ui_.genres(genreLabels,!sidebar_ && genreBarFocused_ ? genreChip_ : SIZE_MAX,selectedGenreChip());
        const bool strip = homeCatalog_.stripVisible(catalog_,recentGames_.items().size());
        const bool recentFocused = homeCatalog_.recentFocused(catalog_,recentGames_.items().size());
        if (strip) {
            std::vector<fog::ui::GameCard> recentCards;
            for (std::size_t index=0; index<recentGames_.items().size(); ++index) {
                const auto& game = recentGames_.items()[index];
                SDL_Texture* image = nullptr;
                if (index >= homeCatalog_.first() && index < homeCatalog_.first()+fog::HomeCatalogNavigation::VisibleRecent) {
                    image = cover(game.catalogItem());
                }
                recentCards.push_back({game.name,"","",image});
            }
            ui_.recentGames(recentCards,!sidebar_ && !genreBarFocused_ && recentFocused ? homeCatalog_.selected() : recentCards.size(),homeCatalog_.first());
        }
        std::vector<fog::ui::GameCard> cards;
        // Only visible covers are requested; the artwork cache is bounded.
        const auto first = catalog_.first();
        const auto visible = strip ? fog::Catalog::Columns : fog::Catalog::VisibleItems;
        const auto last = std::min(catalog_.items().size(), first + visible);
        cards.reserve(last - first);
        for (auto index = first; index < last; ++index) {
            const auto& game = catalog_.items().at(index);
            cards.push_back({field(game,"name"),"","",cover(game)});
        }
        if (!cards.empty()) ui_.games(cards,sidebar_ || genreBarFocused_ || recentFocused ? cards.size() : catalog_.selected()-first,0,"",strip ? 386 : 154,strip ? 1 : 2);
        else if (strip) {
            ui_.text(catalog_.busy() || catalogPending_.valid() || pending_.valid() ? uiText::Catalog::LoadingCatalog :
                     !catalog_.error().empty() ? ui_.controllerHints(uiText::Catalog::LoadFailedRetryHint) : ui_.controllerHints(uiText::Search::NoGamesSearchHint),area.x,390,area.w,muted,false,2);
        } else if (catalog_.busy() || catalogPending_.valid() || pending_.valid()) ui_.empty(uiText::Catalog::LoadingGames, ui_.controllerHints(uiText::Catalog::LoadingExplanation));
        else if (!catalog_.error().empty()) ui_.empty(uiText::Catalog::LoadFailed, ui_.controllerHints(uiText::Search::RetrySearchHint));
        else ui_.empty(uiText::Search::NoGames, ui_.controllerHints(uiText::Search::ChangeQueryHint));
    } else if (screen_ == Screen::PlayStation) {
        fog::playstation::drawCatalog(ui_,playStationComputers_,selection_,!sidebar_,playStationPending_.valid());
    } else if (screen_ == Screen::PlayStationDetails && selection_ < playStationComputers_.size()) {
        fog::playstation::drawConfirmation(ui_,playStationComputers_[selection_]);
    } else if (screen_ == Screen::Genres) {
        std::vector<std::string> labels{uiText::Catalog::AllGames};
        for (const auto& genre : fog::catalogGenres()) labels.emplace_back(fog::ui::genreLabel(genre));
        const auto first = genreDialogSelection_ < 12 ? 0 : (genreDialogSelection_/2-5)*2;
        ui_.genreDialog(labels,genreDialogSelection_,first,selectedGenreIndex());
    } else if (screen_ == Screen::GameDetail) {
        fog::ui::GameDetail detail;
        detail.name = selectedGameName_; detail.description = field(detail_,"description","");
        detail.rating = field(detail_,"metacritic_score",""); detail.ownSteamAccount = ownSteam_;
        const std::array<const char*,3> keys{"min_price","optimum","max_power"};
        const std::array<const char*,3> names{uiText::Server::BestPrice,uiText::Server::Balanced,uiText::Server::Powerful};
        for (std::size_t i=0; i<keys.size(); ++i) {
            const auto found = autoComputers_.find(keys[i]);
            const bool available = found != autoComputers_.end() && found->is_array() && !found->empty();
            detail.choices.push_back({names[i],"",available ? field(found->front(),"price")+uiText::Common::RublesPerHourSuffix : uiText::Server::None,available});
        }
        ui_.gameDetail(detail,selection_ < 4 ? int(selection_) : -1);
        ui_.button(uiText::Server::All,{area.x,535,area.w,56},selection_ == 4);
    } else if (screen_ == Screen::Computers) {
        std::vector<fog::ui::ComputerCard> cards;
        for (const auto& pc : computers_) cards.push_back({field(pc,"name"),field(pc,"gpu"),field(pc,"cpu"),field(pc,"price")+uiText::Common::RublesPerHourSuffix,"Steam",ownSteam_ ? uiText::Profile::OwnAccount : uiText::Profile::ServerAccount,latencyText(pc),true});
        ui_.computers(cards,selection_);
        if (computers_.empty() && !pending_.valid()) ui_.empty(uiText::Server::NoSuitableServers, ui_.controllerHints(uiText::Server::RefreshPagesHints));
    } else if (screen_ == Screen::Details && selection_ < computers_.size()) {
        const auto& pc = computers_[selection_];
        ui_.dialog({uiText::Server::ConnectPrefix + field(pc,"name") + "?", field(pc,"gpu") + " • " + field(pc,"cpu") + "\n" + field(pc,"price") + uiText::Server::HourlyPriceSentenceSuffix + (ownSteam_ ? uiText::Profile::OwnSteamSentence : uiText::Profile::ServerAccountSentence) + (detailsFromAutomatic_ ? "" : "\n" + latencyText(pc) + uiText::Server::PingEstimateSuffix) + uiText::Server::PaidRentalNotice, uiText::Common::Connect, uiText::Common::Back});
    } else if (screen_ == Screen::History) {
        std::vector<fog::ui::HistoryCard> cards;
        for (const auto& item : history_) cards.push_back({field(item,"comp_name"),field(item,"created"),field(item,"finished"),field(item,"duration_minutes")+uiText::Common::MinutesSuffix,field(item,"sum")+uiText::Common::RublesSuffix,""});
        ui_.history(cards,selection_,selection_/fog::ui::HistoryColumns*fog::ui::HistoryColumns);
        if (cards.empty() && !pending_.valid()) ui_.empty(uiText::Profile::NoSessions, uiText::Profile::HistoryExplanation);
    } else if (screen_ == Screen::Settings) {
        ui_.settings({{uiText::Settings::Image,uiText::Common::Open,uiText::Settings::ImageDescription},{uiText::Settings::XboxLayout,controllerSettings_.xbox ? uiText::Common::Enabled : uiText::Common::Disabled,controllerSettings_.xbox ? uiText::Settings::XboxLayoutDescription : uiText::Settings::NintendoLayoutDescription,controllerSettings_.xbox},{uiText::Settings::Connection,fog::ui::connectionModeLabel(connectionSettings_),uiText::Settings::ConnectionDescription},{uiText::Profile::ActiveSessions,uiText::Common::Open,uiText::Profile::ActiveSessionsDescription},{uiText::Profile::SignOut,fog::constants::common::Title,uiText::Profile::SignOutDescription}},selection_);
    } else if (screen_ == Screen::ImageSettings) {
        drawImageSettings();
    } else if (screen_ == Screen::ConnectionSettings) {
        drawConnectionSettings();
    } else if (screen_ == Screen::Sessions) {
        for (std::size_t i = selection_/5*5; i < sessions_.size() && i < selection_/5*5+5; ++i)
            ui_.button(field(sessions_[i],"computer_name"),{area.x,170+int(i%5)*75,area.w,65},i==selection_);
        if (sessions_.empty() && !pending_.valid()) ui_.empty(uiText::Profile::NoActiveSessions, uiText::Profile::StartRentalHint);
    } else if (screen_ == Screen::ConnectConsent) {
        ui_.dialog({uiText::Server::AdoptRentalTitle,uiText::Server::AdoptRentalNotice,uiText::Common::Connect,uiText::Common::Back});
    } else if (screen_ == Screen::Checkout) {
        // Worker owns the coordinator while a request is running. Never read its mutable result here.
        ui_.dialog({uiText::Server::ConnectingTitle, checkoutStatus_ + ui_.controllerHints(uiText::Server::ManualCheckHint),ui_.controllerHints(uiText::Server::CheckButtonHint),ui_.controllerHints(uiText::Server::FinishButtonHint)});
    } else if (screen_ == Screen::ManualResolution) {
        ui_.dialog({uiText::Server::ManualResolutionTitle,uiText::Server::ManualResolutionNotice,uiText::Server::ManualResolutionConfirmed,uiText::Common::Back,true,true});
    } else if (screen_ == Screen::StopConfirm) {
        ui_.dialog({uiText::Server::FinishGameSessionTitle,uiText::Server::FinishRentalNotice,uiText::Server::FinishRental,uiText::Common::Continue,true,true});
    } else if (screen_ == Screen::Stopping || screen_ == Screen::Recovery) {
        ui_.dialog({uiText::Server::TerminationUnconfirmed,ui_.controllerHints(uiText::Server::TerminationRecoveryHint),uiText::Common::Check,"",true,true});
    } else if (screen_ == Screen::Streaming) {
        if (transportReconnect_.recovering())
            ui_.empty(uiText::Stream::Reconnecting,uiText::Stream::ReconnectNotice);
        else ui_.empty(uiText::Stream::ConnectingComputer,uiText::Stream::FirstFrameHint);
    } else if (screen_ == Screen::Consent) {
        ui_.dialog({uiText::Profile::MtsSignInTitle,ui_.controllerHints(uiText::Profile::MtsSignInConsent),uiText::Profile::SignIn,uiText::Common::Back});
    } else if (screen_ == Screen::Auth) {
        ui_.dialog({uiText::Profile::SignInTitle,authStep_ == fog::AuthStep::Captcha ? ui_.controllerHints(uiText::Profile::CaptchaHint) : ui_.controllerHints(uiText::Profile::SmsHint),uiText::Profile::EnterCode,uiText::Common::Back,true,false,captcha_});
    }
    if (api_.loggedIn() && authSaveFailed_ && screen_ != Screen::Games && screen_ != Screen::Genres)
        ui_.text(uiText::Profile::AuthNotSaved, area.x, 77, area.w-80);
    if (screen_ == Screen::Checkout) ui_.status(checkoutStatus_, pending_.valid());
    else if (screen_ == Screen::Games) {
        if (api_.loggedIn() && authSaveFailed_) ui_.status(uiText::Profile::AuthNotSaved,false,true);
        else if (pending_.valid()) ui_.status(uiText::Common::Loading, true);
        else if (!catalog_.error().empty()) ui_.status(uiText::Catalog::LoadFailedPrefix + catalog_.error(), false, true);
        else if (catalog_.busy() || catalogPending_.valid()) ui_.status(catalog_.items().empty() ? uiText::Catalog::LoadingStatus : uiText::Catalog::LoadMoreStatus, true);

    } else if ((screen_ == Screen::PlayStation || screen_ == Screen::PlayStationDetails) && !playStationError_.empty())
        ui_.status(playStationError_,false,true);
    else if (screen_ != Screen::Genres && (pending_.valid() || !routineStatus(status_)))
        ui_.status(pending_.valid() ? uiText::Common::Loading : status_, pending_.valid());
    if (screen_ == Screen::Streaming) drawStreamOverlay();
    SDL_RenderPresent(rt_.renderer);
    fogplay::diagnostics::firstUiFramePresented();
}

}
