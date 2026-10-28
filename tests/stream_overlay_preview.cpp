#include "app/presentation/ui.hpp"
#include "features/streaming/presentation/stream_overlay.hpp"
#include "features/settings/presentation/rumble_text.hpp"
#include <SDL_image.h>
#include <filesystem>
#include <stdexcept>
#include <string>
// Synthetic preview: no stream, remote computer or account.
int main(int argc, char** argv) {
    if (SDL_Init(0) != 0 || TTF_Init() != 0) return 1;
    const std::string path = argc > 1 ? argv[1] : "work/video047/preview";
    std::filesystem::create_directories(path);
    auto* surface = SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_RGBA32);
    auto* renderer = SDL_CreateSoftwareRenderer(surface);
    auto* font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",23);
    auto* title = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",34);
    if (!surface || !renderer || !font || !title) return 2;
    {
        fog::ui::Ui ui(renderer,font,title);
        for (int mode=0; mode<14; ++mode) {
            SDL_SetRenderDrawColor(renderer,30,53,57,255);
            SDL_RenderClear(renderer);
            ui.text("Синтетический фон трансляции",30,10,1000);
            fog::stream::Overlay overlay;
            overlay.openMenu();
            if (mode==1) { overlay.down(); overlay.select(); overlay.appendText("Привет, Switch! Текст в выбранное поле ПК."); }
            if (mode==2) overlay.select();
            if (mode==3) { overlay.down(); overlay.down(); overlay.down(); overlay.select(); }
            if (mode==4) { overlay.down(); overlay.select(); overlay.touch(60,510); overlay.appendText("Привет, Switch! Проверка русской раскладки."); }
            if (mode==5) {
                overlay.down(); overlay.down(); overlay.select();
                overlay.setVideoStatus("H.264 · приём: 18.4 Мбит/с · цель сервера: 20 Мбит/с");
                overlay.setSaveStatus("Сохранено на SD. Запрошено; подтверждение сервера не гарантируется.");
            }
            if (mode==7 || mode==8) {
                overlay.setProvider(fog::ProviderId::FogHub);
                overlay.down(); overlay.down(); overlay.select();
                overlay.setVideoStatus("FogHub · H.265 · 1280×720 · 30 FPS");
                overlay.setSaveStatus("Сохранено на SD. Применится при следующем подключении.");
                if (mode==8) overlay.setVideoChangesEnabled(false);
            }
            if (mode>=9) {
                overlay.up(); overlay.select();
                overlay.setRumbleSettings({static_cast<fog::rumble::Mode>((mode - 9) % 4)});
                overlay.setRumbleStatus(mode==13 ? fog::rumble::text::SaveFailed : fog::rumble::text::Saved);
            }
            if (mode==6) {
                fog::ui::Chrome chrome;
                chrome.title = "НАСТРОЙКИ"; chrome.section = fog::ui::Section::Settings;
                chrome.account = "Аккаунт подключён";
                ui.frame(chrome);
                ui.settings({{"Изображение","Открыть","Кодек, качество, битрейт и ключевые кадры"},
                    {"Раскладка Xbox","Включено","Кнопки A/B и X/Y расположены как на Xbox",true},
                    {"Соединение","Обычное","Выбор способа подключения"},
                    {"Активные сессии","Открыть","Подключиться к уже работающему ПК или завершить аренду"},
                    {"Выйти из аккаунта","FogPlay NX","Удалить сохранённый вход с SD-карты"},
                    {fog::rumble::text::MenuLabel,fog::rumble::text::label(fog::rumble::Mode::DualBand),
                        fog::rumble::text::Description}},5);
            } else overlay.paint(ui,renderer);
            SDL_RenderPresent(renderer);
            if (IMG_SavePNG(surface,(path+"/overlay-"+std::to_string(mode)+".png").c_str()) != 0) return 3;
        }
    }
    TTF_CloseFont(font); TTF_CloseFont(title);
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface);
    TTF_Quit(); SDL_Quit();
}
