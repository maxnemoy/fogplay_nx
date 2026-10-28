#include "features/session/presentation/session_rating_view.hpp"
#include "features/session/presentation/session_rating_text.hpp"

namespace fog::rating {
void draw(ui::Ui& ui, const Presentation& value) {
    const auto area = ui.content();
    constexpr SDL_Color white{234,238,245,255}, muted{163,178,200,255}, accent{107,216,190,255};
    if (value.state() == State::Loading || value.state() == State::LoadFailed) {
        ui.empty(text::Introduction,value.state() == State::Loading ? text::Loading : value.error());
        ui.status(value.state() == State::Loading ? text::Loading : text::LoadFailed,
            value.state() == State::Loading,value.state() == State::LoadFailed);
        return;
    }
    if (value.state() == State::Sent || value.state() == State::None) {
        const auto message = value.state() == State::None ? text::None :
            value.alreadyRated() ? text::AlreadyRated : text::Sent;
        ui.empty(message,value.score() >= 1 && value.score() <= 5 ?
            "Оценка: " + std::to_string(value.score()) + " из 5" : "");
        ui.button(text::Back,{area.x,500,area.w,60},true);
        return;
    }
    const bool enabled = value.editable();
    ui.text(text::Introduction,area.x,128,area.w,white,true);
    if (value.form()) ui.text(value.form()->computerName + (value.form()->sessionDate.empty() ? "" :
        " · " + value.form()->sessionDate),area.x,180,area.w,muted);
    ui.text(value.score() ? "Оценка: " + std::to_string(value.score()) + " из 5" : text::Choice,
        area.x,222,area.w,accent);
    const int starWidth = std::min(140,(area.w-48)/5);
    for (int score = 1; score <= 5; ++score) {
        ui.button(std::string(value.score() >= score ? "★ " : "☆ ") + std::to_string(score),
            {area.x+(score-1)*(starWidth+12),264,starWidth,62},
            enabled && !value.focus() && score == (value.score() ? value.score() : 1),enabled);
    }
    if (value.commentSupported()) {
        ui.panel({area.x,350,area.w,130},enabled && value.focus() == 1);
        ui.text(text::Comment,area.x+18,364,area.w-36,white);
        ui.text(value.comment().empty() ? text::EmptyComment : value.comment(),area.x+18,402,area.w-36,
            value.comment().empty() ? muted : white,false,2);
    }
    const int half = (area.w-18)/2;
    ui.button(text::Send,{area.x,516,half,58},enabled && value.focus() == value.sendRow(),value.canSend());
    ui.button(text::Later,{area.x+half+18,516,area.w-half-18,58},
        enabled && value.focus() == value.laterRow(),enabled);
    ui.text(text::DeferredHint,area.x,596,area.w,muted);
    if (value.state() == State::Submitting) ui.status(text::Sending,true);
    else if (!value.error().empty()) ui.status(value.error(),false,true);
}
}
