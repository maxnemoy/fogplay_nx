#include "features/session/presentation/session_rating_view.hpp"
#include "features/session/presentation/session_rating_text.hpp"
#include <SDL_image.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
static unsigned checks = 0;
static void check(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
static fog::SessionRatingForm form(const char* id="owned-finished") {
    fog::SessionRatingForm value; value.rental={fog::ProviderId::FogHub,id};
    value.preparation={fog::ProviderId::FogHub,"synthetic-handle"};
    value.computerName="Игровой компьютер · RTX 4080"; value.sessionDate="27.10.2026 15:00";
    value.commentSupported=true; value.commentMaxLength=2000; return value;
}
static void model() {
    using namespace fog::rating;
    Presentation value; value.loading(form().rental);
    check(!value.request() && value.activate()==Action::None,"loading cannot mutate");
    check(value.loaded(form()) && value.score()==0 && !value.canSend(),"form never auto-selects website default");
    value.down(); value.down(); check(value.activate()==Action::None,"Send disabled before explicit score");
    value.up(); value.up(); value.left(); check(value.score()==1,"first left explicitly selects one");
    for (int i=0;i<10;++i) value.right();
    check(value.score()==5,"score upper bound");
    for (int i=0;i<10;++i) value.left();
    check(value.score()==1,"score lower bound");
    std::string cyrillic; for (int i=0;i<200;++i) cyrillic+="Я";
    check(value.comment(cyrillic) && !value.comment(cyrillic+"Я"),"200 Cyrillic characters accepted without byte truncation");
    std::string emoji; for (int i=0;i<100;++i) emoji+="😀";
    check(value.comment(emoji) && !value.comment(emoji+"😀"),"emoji consume two UTF16 editor units");
    check(!value.comment(std::string("\xc0\xaf",2)) && !value.comment(std::string("\xed\xa0\x80",3)),"invalid UTF8/surrogate rejected");
    check(value.comment("Повторить без потери текста"),"draft comment accepted");
    value.submitting(); check(!value.request() && !value.comment("lost"),"in-flight draft locked");
    value.submitFailed("safe fixed error"); check(value.comment()=="Повторить без потери текста" && value.canSend(),"retry preserves draft");
    check(!value.sent({{fog::ProviderId::FogHub,"other"},5}),"wrong-session confirmation rejected");
    value.loading(form("other").rental); check(value.loaded(form("other")) && value.score()==0 && value.comment().empty(),"new session clears preceding draft");
    auto unsupported=form("other"); unsupported.commentSupported=false;
    value.loading(unsupported.rental); value.loaded(unsupported);
    check(!value.comment("ignored") && value.sendRow()==1,"comment omitted when website does not support it");
    value.loading(std::nullopt); value.loaded(std::nullopt);
    check(value.state()==State::None && !value.request(),"no pending review cannot submit");
}
int main(int argc,char** argv) {
    try { model(); } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    if (SDL_Init(0) || TTF_Init()) return 2;
    const std::filesystem::path path=argc>1?argv[1]:"work/session-rating/preview";
    std::filesystem::create_directories(path);
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,1280,720,32,SDL_PIXELFORMAT_RGBA32);
    auto* renderer=surface?SDL_CreateSoftwareRenderer(surface):nullptr;
    auto* font=TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",23);
    auto* title=TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",34);
    if (!surface || !renderer || !font || !title) return 3;
    int result=0;
    {
        fog::ui::Ui ui(renderer,font,title);
        for (int scene=0;scene<7;++scene) {
            fog::rating::Presentation value; value.loading(form().rental);
            if (scene!=0) value.loaded(form());
            if (scene>=2) { value.right();value.right();value.right(); value.comment("Соединение стало стабильнее, играть удобно. Спасибо!"); value.down(); }
            if (scene==3) value.submitting();
            if (scene==4) value.submitFailed(fog::rating::text::SubmitFailed);
            if (scene==5) value.sent({form().rental,3});
            if (scene==6) { value.loading(form().rental); value.loadFailed(fog::rating::text::LoadFailed); }
            fog::ui::Chrome chrome; chrome.title=fog::rating::text::Title;
            chrome.hints=fog::rating::text::ChoiceHint; chrome.section=fog::ui::Section::PlayStation;
            ui.frame(chrome); fog::rating::draw(ui,value); SDL_RenderPresent(renderer);
            if (IMG_SavePNG(surface,(path/((std::to_string(scene))+".png")).c_str())) result=4;
        }
    }
    TTF_CloseFont(font);TTF_CloseFont(title);SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);TTF_Quit();SDL_Quit();
    if (!result) std::cout<<"Rating presentation: "<<checks<<" assertions, 7 offscreen SDL previews passed\n";
    return result;
}
