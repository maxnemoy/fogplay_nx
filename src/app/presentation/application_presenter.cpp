#include "app/application_controller.hpp"

namespace fogplay {

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
        text("Связь: попытка " + std::to_string(attempt) + "/3 за 2 мин. Та же аренда; оплата продолжается.",20,8,accent,1240);
        text("Минус + Плюс — меню и завершение аренды. Лимит восстановления — 90 секунд.",20,44,white,1240);
    } else if (videoRecovery_.active() || videoLossWaiting_) {
        box(0,0,1280,45,{23,32,46,255});
        text("Восстанавливаем видео… Минус + Плюс — меню сессии",20,8,accent,1240);
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
            text("Минус + Плюс — меню: мышь, клавиатура, изображение, завершение.",30,680,accent);
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
    chrome.account = api_.loggedIn() ? "Аккаунт подключён" : "Войти в аккаунт";
    chrome.title = "ВЫБЕРИ ИГРУ";
    chrome.section = fog::ui::Section::Games;
    if (screen_ == Screen::Computers || screen_ == Screen::Details || screen_ == Screen::GameDetail) { chrome.title = selectedGameName_; chrome.section = fog::ui::Section::Computers; }
    if (screen_ == Screen::History) { chrome.title = "ИСТОРИЯ СЕССИЙ"; chrome.section = fog::ui::Section::History; }
    if (screen_ == Screen::Settings || screen_ == Screen::ImageSettings || screen_ == Screen::ConnectionSettings) {
        chrome.title = screen_ == Screen::ImageSettings ? "ИЗОБРАЖЕНИЕ" :
            screen_ == Screen::ConnectionSettings ? "ПОДКЛЮЧЕНИЕ" : "НАСТРОЙКИ";
        chrome.section = fog::ui::Section::Settings;
    }
    if (screen_ == Screen::ConnectionSettings) chrome.hints = "← / → или A Изменить режим   B Настройки";
    if (screen_ == Screen::ImageSettings) chrome.hints = "↑ / ↓ Параметр   ← / → или A Изменить   B Настройки";
    if (sidebar_) chrome.section = std::array<fog::ui::Section,4>{fog::ui::Section::Search,fog::ui::Section::Games,fog::ui::Section::History,fog::ui::Section::Settings}[nav_];
    if (screen_ == Screen::Games) {
        if (!search_.empty()) chrome.title = "ПОИСК: " + search_;
        chrome.hints = "A Выбрать   B Меню   X Поиск   Y Аккаунт   ↑ Жанры / прокрутка";
        if (!catalog_.error().empty()) chrome.hints += "   R Повторить";
    }
    if (screen_ == Screen::Genres) { chrome.title = "ЖАНРЫ"; chrome.hints = "↑ / ↓ / ← / → Жанр   A Применить   B Назад"; }
    if (screen_ == Screen::Computers) chrome.hints = "A Подключиться   B Назад   X Обновить   L / R Страницы   ≈ оценка через МТС";
    if (screen_ == Screen::Details) chrome.hints = detailsFromAutomatic_ ? "A Подключиться   B Назад" : "A Подключиться   B Назад   X Обновить пинг";
    if (sessionPending_) chrome.hints = "A Проверить / выбрать   B Завершить аренду   Y Войти   Минус + Плюс: меню в игре";
    if (api_.loggedIn() && authSaveFailed_) chrome.subtitle.clear();
    ui_.frame(chrome);
    const auto area = ui_.content();
    if (screen_ == Screen::Games) {
        std::vector<std::string> genreLabels;
        for (std::size_t i=0; i<5; ++i) genreLabels.emplace_back(fog::catalogGenres()[i].label);
        genreLabels.emplace_back(selectedGenreIndex()>5 ? fog::catalogGenres()[selectedGenreIndex()-1].label : "Все жанры");
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
            ui_.text(catalog_.busy() || catalogPending_.valid() || pending_.valid() ? "Загружаем каталог…" :
                     !catalog_.error().empty() ? ui_.controllerHints("Не удалось загрузить каталог. R — повторить • X — поиск") : ui_.controllerHints("Игры не найдены. X — поиск"),area.x,390,area.w,muted,false,2);
        } else if (catalog_.busy() || catalogPending_.valid() || pending_.valid()) ui_.empty("Загружаем игры", ui_.controllerHints("Каталог появится после ответа сервера. X — изменить поиск."));
        else if (!catalog_.error().empty()) ui_.empty("Не удалось загрузить игры", ui_.controllerHints("R — повторить • X — изменить поиск"));
        else ui_.empty("Игры не найдены", ui_.controllerHints("X — изменить поиск"));
    } else if (screen_ == Screen::Genres) {
        std::vector<std::string> labels{"Все игры"};
        for (const auto& genre : fog::catalogGenres()) labels.emplace_back(genre.label);
        const auto first = genreDialogSelection_ < 12 ? 0 : (genreDialogSelection_/2-5)*2;
        ui_.genreDialog(labels,genreDialogSelection_,first,selectedGenreIndex());
    } else if (screen_ == Screen::GameDetail) {
        fog::ui::GameDetail detail;
        detail.name = selectedGameName_; detail.description = field(detail_,"description","");
        detail.rating = field(detail_,"metacritic_score",""); detail.ownSteamAccount = ownSteam_;
        const std::array<const char*,3> keys{"min_price","optimum","max_power"};
        const std::array<const char*,3> names{"Лучшая цена","Оптимальный","Мощный"};
        for (std::size_t i=0; i<keys.size(); ++i) {
            const auto found = autoComputers_.find(keys[i]);
            const bool available = found != autoComputers_.end() && found->is_array() && !found->empty();
            detail.choices.push_back({names[i],"",available ? field(found->front(),"price")+" руб./час" : "Нет серверов",available});
        }
        ui_.gameDetail(detail,selection_ < 4 ? int(selection_) : -1);
        ui_.button("Все серверы",{area.x,535,area.w,56},selection_ == 4);
    } else if (screen_ == Screen::Computers) {
        std::vector<fog::ui::ComputerCard> cards;
        for (const auto& pc : computers_) cards.push_back({field(pc,"name"),field(pc,"gpu"),field(pc,"cpu"),field(pc,"price")+" руб./час","Steam",ownSteam_ ? "Свой аккаунт" : "Аккаунт сервера",latencyText(pc),true});
        ui_.computers(cards,selection_);
        if (computers_.empty() && !pending_.valid()) ui_.empty("Нет подходящих серверов", ui_.controllerHints("X — обновить • L / R — страницы"));
    } else if (screen_ == Screen::Details && selection_ < computers_.size()) {
        const auto& pc = computers_[selection_];
        ui_.dialog({"Подключиться к " + field(pc,"name") + "?", field(pc,"gpu") + " • " + field(pc,"cpu") + "\n" + field(pc,"price") + " руб./час. " + (ownSteam_ ? "Вход в свой Steam." : "Аккаунт сервера.") + (detailsFromAutomatic_ ? "" : "\n" + latencyText(pc) + " — оценка через узлы МТС.") + "\nНачнётся платная аренда. При выходе приложение запросит её завершение.", "Подключиться", "Назад"});
    } else if (screen_ == Screen::History) {
        std::vector<fog::ui::HistoryCard> cards;
        for (const auto& item : history_) cards.push_back({field(item,"comp_name"),field(item,"created"),field(item,"finished"),field(item,"duration_minutes")+" мин.",field(item,"sum")+" руб.",""});
        ui_.history(cards,selection_,selection_/fog::ui::HistoryColumns*fog::ui::HistoryColumns);
        if (cards.empty() && !pending_.valid()) ui_.empty("Сессий пока нет", "Здесь появится история ваших игровых сессий.");
    } else if (screen_ == Screen::Settings) {
        ui_.settings({{"Изображение","Открыть","Кодек, качество, битрейт и ключевые кадры"},{"Xbox-раскладка",controllerSettings_.xbox ? "Включена" : "Выключена",controllerSettings_.xbox ? "В меню: B — выбор, A — назад. В игре — Xbox." : "В меню: A — выбор, B — назад. В игре — Nintendo.",controllerSettings_.xbox},{"Подключение",fog::connection::label(connectionSettings_),"Выбор пути: обычный, авто или через TURN"},{"Активные сессии","Открыть","Подключиться к уже работающему ПК или завершить аренду"},{"Выйти из аккаунта","FogPlay NX 0.4.28","Удалить сохранённый вход с SD-карты"}},selection_);
    } else if (screen_ == Screen::ImageSettings) {
        drawImageSettings();
    } else if (screen_ == Screen::ConnectionSettings) {
        drawConnectionSettings();
    } else if (screen_ == Screen::Sessions) {
        for (std::size_t i = selection_/5*5; i < sessions_.size() && i < selection_/5*5+5; ++i)
            ui_.button(field(sessions_[i],"computer_name"),{area.x,170+int(i%5)*75,area.w,65},i==selection_);
        if (sessions_.empty() && !pending_.valid()) ui_.empty("Активных сессий нет", "Выберите игру и сервер в каталоге, чтобы начать новую аренду.");
    } else if (screen_ == Screen::ConnectConsent) {
        ui_.dialog({"Подключиться к действующей аренде?","Приложение будет завершать эту сессию при выходе, уходе в HOME или потере видео. Минус + Плюс — меню завершения.","Подключиться","Назад"});
    } else if (screen_ == Screen::Checkout) {
        // Worker owns the coordinator while a request is running. Never read its mutable result here.
        ui_.dialog({"Подключение к серверу", checkoutStatus_ + ui_.controllerHints("\nЕсли связь потеряна, проверьте аренду на fogplay.mts.ru с телефона. X — подтвердить ручную проверку."),ui_.controllerHints("Проверить (A)"),ui_.controllerHints("Завершить (B)")});
    } else if (screen_ == Screen::ManualResolution) {
        ui_.dialog({"Вы проверили заказ на сайте?","Подтверждайте только если в официальном аккаунте нет работающей аренды и ожидающего оплаты заказа. Это снимает локальную блокировку; команду отмены на сервер не отправляет.","Я проверил, аренды нет","Назад",true,true});
    } else if (screen_ == Screen::StopConfirm) {
        ui_.dialog({"Завершить игровую сессию?","Аренда завершится после подтверждения сервера. Дождитесь результата.","Завершить аренду","Продолжить",true,true});
    } else if (screen_ == Screen::Stopping || screen_ == Screen::Recovery) {
        ui_.dialog({"Завершение аренды не подтверждено",ui_.controllerHints("A — проверить и завершить. Y — войти в прежний аккаунт. При отсутствии связи завершите сессию на fogplay.mts.ru с телефона: списания могут продолжаться."),"Проверить","",true,true});
    } else if (screen_ == Screen::Streaming) {
        if (transportReconnect_.recovering())
            ui_.empty("Восстанавливаем связь…","Повторное подключение к той же аренде. Оплата продолжается. Минус + Плюс — меню завершения.");
        else ui_.empty("Подключаемся к ПК…","Ожидаем первый видеокадр. Минус + Плюс — меню трансляции.");
    } else if (screen_ == Screen::Consent) {
        ui_.dialog({"Вход в МТС Fog Play",ui_.controllerHints("A — согласиться с условиями сервиса fogplay.mts.ru и запросить SMS. Подписка на рассылку не включается. Токен сохранится на SD для следующего запуска. SMS-код не сохраняется."),"Войти","Назад"});
    } else if (screen_ == Screen::Auth) {
        ui_.dialog({"Вход в аккаунт",authStep_ == fog::AuthStep::Captcha ? ui_.controllerHints("Запомните текст картинки и нажмите A для ввода.") : ui_.controllerHints("Нажмите A и введите код из SMS."),"Ввести код","Назад",true,false,captcha_});
    }
    if (api_.loggedIn() && authSaveFailed_ && screen_ != Screen::Games && screen_ != Screen::Genres)
        ui_.text("Вход не сохранён на SD. После закрытия понадобится войти снова.", area.x, 77, area.w-80);
    if (screen_ == Screen::Checkout) ui_.status(checkoutStatus_, pending_.valid());
    else if (screen_ == Screen::Games) {
        if (api_.loggedIn() && authSaveFailed_) ui_.status("Вход не сохранён на SD. После закрытия понадобится войти снова.",false,true);
        else if (pending_.valid()) ui_.status("Загрузка…", true);
        else if (!catalog_.error().empty()) ui_.status("Не удалось загрузить игры. R — повторить: " + catalog_.error(), false, true);
        else if (catalog_.busy() || catalogPending_.valid()) ui_.status(catalog_.items().empty() ? "Загрузка игр…" : "Подгружаем игры…", true);

    } else if (screen_ != Screen::Genres) ui_.status(pending_.valid() ? "Загрузка…" : status_, pending_.valid());
    if (screen_ == Screen::Streaming) drawStreamOverlay();
    SDL_RenderPresent(rt_.renderer);
}

}
