#include "app/presentation/ui.hpp"
#include "features/catalog/domain/catalog.hpp"
#include <SDL_image.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// Offscreen synthetic fixtures only: no network, account, hardware or runtime claims.
int main(int argc,char** argv) {
    try {
        if(SDL_Init(0)!=0 || TTF_Init()!=0) throw std::runtime_error(SDL_GetError());
        const std::string path=argc>1?argv[1]:"work/ui-preview";
        std::filesystem::create_directories(path);
        auto* surface=SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_RGBA32);
        if(!surface) throw std::runtime_error(SDL_GetError());
        auto* renderer=SDL_CreateSoftwareRenderer(surface);
        auto* regular=TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",23);
        auto* large=TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",34);
        if(!renderer || !regular || !large) throw std::runtime_error("Host renderer/fonts unavailable");
        SDL_Texture* publicCover=nullptr;
        {
            bool nullRejected=false;
            try { fog::ui::Ui invalid(nullptr,regular,large); } catch (const std::invalid_argument&) { nullRejected=true; }
            assert(nullRejected);
            fog::ui::Ui ui(renderer,regular,large);
            assert(ui.controllerHints("A B X Y") == "A B X Y");
            ui.setXboxLayout(true);
            assert(ui.controllerHints("A B X Y · L / R · ZR · ABC · Xbox") == "B A Y X · L / R · ZR · ABC · Xbox");
            ui.setXboxLayout(false);
            assert(ui.controllerHints("A B X Y") == "A B X Y");
            fog::ui::Chrome chrome;
            chrome.subtitle="";
            std::vector<std::string> genres;
            for(std::size_t i=0;i<5;++i) genres.emplace_back(fog::catalogGenres()[i].label);
            genres.emplace_back("Все жанры");
            std::vector<std::string> genreLabels{"Все игры"};
            for(const auto& genre:fog::catalogGenres()) genreLabels.emplace_back(genre.label);
            std::vector<fog::ui::GameCard> games;
            const std::string names[]{"Cyberpunk 2077","Red Dead Redemption 2","Horizon Zero Dawn","God of War","Marvel’s Spider-Man 2","Ghost of Tsushima DIRECTOR’S CUT","The Last of Us Part II Remastered","Forza Horizon 5"};
            for(const auto& name:names) games.push_back({name,"Цена не загружена","Поддержка сохранений",nullptr});
            ui.frame(chrome); ui.genres(genres,genres.size(),4); ui.games(games,4,0,"",154,2);
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/games.png").c_str())!=0) throw std::runtime_error(IMG_GetError());

            std::vector<fog::ui::GameCard> recent(games.begin(),games.begin()+5);
            ui.frame(chrome); ui.genres(genres,genres.size(),4); ui.recentGames(recent,4,1); ui.games(games,games.size(),0,"",386,1);
            ui.status("ДЕМО: недавние игры — горизонтальная полоса, ↓ — каталог.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/recent-fifth.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            ui.frame(chrome); ui.genres(genres,genres.size(),4); ui.recentGames(recent,recent.size()); ui.games(games,2,0,"",386,1);
            ui.status("ДЕМО: фокус каталога под недавними играми, ↑ — вернуться к полосе.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/recent-catalog.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            chrome.sidebarExpanded=true;chrome.sidebarFocused=true;
            ui.frame(chrome); ui.genres(genres,genres.size(),4); ui.recentGames(recent,recent.size()); ui.games(games,games.size(),0,"",386,1);
            ui.status("ДЕМО: открытая боковая панель, история игр сохраняет фокус.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/recent-sidebar.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            chrome.sidebarExpanded=false;chrome.sidebarFocused=false;
            const std::string coverPath=argc>2?argv[2]:"work/artwork-cover.webp";
            if(std::filesystem::exists(coverPath)) {
                publicCover=IMG_LoadTexture(renderer,coverPath.c_str());
                if(!publicCover) throw std::runtime_error(IMG_GetError());
                games[0].cover=publicCover;
                recent[0].cover=publicCover;
                ui.frame(chrome); ui.genres(genres,genres.size(),4); ui.recentGames(recent,0); ui.games(games,games.size(),0,"",386,1);
                ui.status("ДЕМО: обложка из открытого каталога; недавние игры — синтетическая история.");
                SDL_RenderPresent(renderer);
                if(IMG_SavePNG(surface,(path+"/recent-covers.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
                chrome.subtitle="";
                ui.frame(chrome); ui.genres(genres,genres.size(),4); ui.games(games,0,0,"",154,2);
                SDL_RenderPresent(renderer);
                if(IMG_SavePNG(surface,(path+"/games-covers.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
                chrome.subtitle="";
            }
            ui.frame(chrome); ui.genres(genres,5,4); ui.games(games,games.size(),0,"",154,2);
            ui.genreDialog(genreLabels,7,0,5);
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/genres.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            ui.frame(chrome); ui.genres(genres,5,5); ui.games(games,games.size(),0,"",154,2);
            ui.genreDialog(genreLabels,18,8,18);
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/genres-last.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            std::vector<fog::ui::ComputerCard> pcs;
            for(int i=0;i<8;++i) pcs.push_back({"DEMO-PC-"+std::to_string(i+1),"GPU: NVIDIA GeForce RTX 4070 Ti","CPU: AMD Ryzen 7 5700X","Демо: 35 ₽/час","Steam","Нужен Steam аккаунт",i==0?"Пинг: 42 мс":i==1?"Измерение…":i==2?"Нет ответа":"Пинг: 87 мс",true});
            chrome.title="КОМПЬЮТЕРЫ"; chrome.section=fog::ui::Section::Computers;
            ui.frame(chrome); ui.computers(pcs,1); ui.status("ДЕМО: значения характеристик и тарифов — тестовая фикстура.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/computers.png").c_str())!=0) throw std::runtime_error(IMG_GetError());

            chrome.title="Red Dead Redemption 2"; chrome.section=fog::ui::Section::Games;
            fog::ui::GameDetail detail{"Red Dead Redemption 2","Америка, 1899 год. Артур Морган и другие сподвижники Датча ван дер Линде скрываются от закона. Большой открытый мир и история банды.","",{"Шутеры","Приключения","Песочницы","Экшен"},true,{{"ЛУЧШАЯ ЦЕНА","Сервер с минимальной стоимостью","Демо: 24 ₽/час",true},{"ОПТИМАЛЬНЫЙ","Баланс цены и характеристик","Демо: 43 ₽/час",true},{"МОЩНЫЙ","Лучшие характеристики","Демо: 58 ₽/час",true}}};
            ui.frame(chrome); const int top=ui.gameDetail(detail,0); ui.computers(pcs,0,0,top); ui.status("ДЕМО: автоподбор и тарифы используют синтетические данные.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/game-detail.png").c_str())!=0) throw std::runtime_error(IMG_GetError());

            chrome.sidebarExpanded=true; chrome.title="ЖУРНАЛ СЕССИЙ"; chrome.section=fog::ui::Section::History;
            std::vector<fog::ui::HistoryCard> history{{"DEMO-01","14.10.2026 07:17","14.10.2026 07:18","0 ч 1 мин","Демо: 0 ₽","Демонстрация"},{"DEMO-02","13.10.2026 18:00","13.10.2026 19:00","1 ч 0 мин","Демо: 35 ₽","Демонстрация"}};
            for (int i=2;i<10;++i) history.push_back({"DEMO-"+std::to_string(i),"start","end","duration","cost","status"});
            for (std::size_t selected=0;selected<history.size();++selected) {
                const auto first=selected/fog::ui::HistoryColumns*fog::ui::HistoryColumns;
                assert(first<=selected && selected<first+fog::ui::HistoryColumns);
                ui.frame(chrome); ui.history(history,selected,first);
                SDL_RenderPresent(renderer);
                if (selected==4 || selected==5)
                    assert(IMG_SavePNG(surface,(path+"/history-selected-"+std::to_string(selected)+".png").c_str())==0);
            }
            ui.frame(chrome); ui.history(history,0); ui.status("ДЕМО: журнал не содержит реальных сессий.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/history.png").c_str())!=0) throw std::runtime_error(IMG_GetError());

            chrome.title="НАСТРОЙКИ"; chrome.section=fog::ui::Section::Settings;
            std::vector<fog::ui::SettingCard> settings{
                {"Изображение","Открыть","Кодек, качество, битрейт и ключевые кадры"},
                {"Xbox-раскладка","Включена","В меню: B — выбор, A — назад. В игре — Xbox.",true},
                {"Активные сессии","Открыть","Подключиться к уже работающему ПК или завершить аренду"},
                {"Выйти из аккаунта","FogPlay NX 0.4.15","Удалить сохранённый вход с SD-карты"}};
            ui.setXboxLayout(true);
            ui.frame(chrome); ui.settings(settings,1); ui.status("ДЕМО: включена раскладка Xbox; подсказки соответствуют кнопкам.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/settings.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            if(IMG_SavePNG(surface,(path+"/settings-xbox.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            settings[1].value="Выключена";
            settings[1].description="В меню: A — выбор, B — назад. В игре — Nintendo.";
            settings[1].checked=false;
            ui.setXboxLayout(false);
            ui.frame(chrome); ui.settings(settings,1); ui.status("ДЕМО: раскладка Nintendo; флажок снят.");
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/settings-nintendo.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            settings[1].value="Включена";
            settings[1].description="В меню: B — выбор, A — назад. В игре — Xbox.";
            settings[1].checked=true;
            ui.setXboxLayout(true);
            ui.frame(chrome); ui.settings(settings,settings.size()); ui.genreDialog(genreLabels,7,0,5);
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/genres-xbox.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            ui.setXboxLayout(false);

            ui.frame(chrome); ui.dialog({"Подтвердите аренду","ДЕМО ВЁРСТКИ. Это окно не создаёт аренду и не списывает деньги.\n\nПроверьте компьютер и почасовой тариф перед продолжением.\n\nДлинные тексты ограничиваются размером окна и не перекрывают кнопки.","A Создать заказ","B Отмена",true,false,nullptr});
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/checkout.png").c_str())!=0) throw std::runtime_error(IMG_GetError());

            ui.frame(chrome);
            ui.dialog({"Не закрывайте приложение",std::string("ДЕМО ВЁРСТКИ. Ожидаем подтверждения завершения аренды. При потере связи проверьте заказ на официальном сайте. ")+std::string(1600,'x'),"Проверить ещё раз","Назад",true,true,nullptr});
            SDL_RenderPresent(renderer);
            if(IMG_SavePNG(surface,(path+"/stop-long.png").c_str())!=0) throw std::runtime_error(IMG_GetError());
            for(int i=0;i<320;++i) ui.text("Cache fixture "+std::to_string(i),0,0,600);
            assert(ui.cachedTextCount()<=192);
            ui.text(std::string("Invalid UTF-8 ")+static_cast<char>(0xff),0,0,200);
            ui.text(std::string(10000,'x'),0,0,100,SDL_Color{255,255,255,255},false,2);
            ui.clearTextCache(); assert(ui.cachedTextCount()==0);
        }
        if(publicCover) SDL_DestroyTexture(publicCover);
        TTF_CloseFont(large); TTF_CloseFont(regular); SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface);
        TTF_Quit(); SDL_Quit();
        std::printf("UI preview: synthetic 1280x720 catalog, recent, genre, server, checkbox, controller hints and dialog variants; text cache bounds and malformed/long text exercised.\n");
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}