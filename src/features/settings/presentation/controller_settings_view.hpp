#pragma once
#include "app/presentation/ui.hpp"
#include "features/settings/presentation/controller_text.hpp"

namespace fog::ui {
inline void controllerSettings(Ui& ui, const controller::Settings& settings, std::size_t selection,
                               const std::string& status) {
    namespace text = controller::text;
    constexpr SDL_Color muted{163, 178, 200, 255};
    constexpr SDL_Color accent{107, 216, 190, 255};
    const auto area = ui.content();
    const bool dualSense = settings.profile == ::stream::ControllerProfile::DualSense;
    ui.button(std::string(text::ProfilePrefix) + text::profileLabel(settings.profile),
        {area.x, 154, area.w, 57}, selection == 0);
    ui.text(dualSense ? text::DualSenseDescription : text::XboxDescription,
        area.x + 14, 225, area.w - 28, muted, false, 2);
    ui.button(std::string(text::LayoutPrefix) + (settings.xbox ? "Xbox" : "Nintendo"),
        {area.x, 300, area.w, 57}, selection == 1);
    ui.text(dualSense ? text::LayoutNotice : settings.xbox ?
        constants::ui::Settings::XboxLayoutDescription : constants::ui::Settings::NintendoLayoutDescription,
        area.x + 14, 371, area.w - 28, muted, false, 2);
    ui.text(text::ApplyNotice, area.x + 14, 465, area.w - 28, muted, false, 2);
    if (dualSense) ui.text(text::HostNotice, area.x + 14, 530, area.w - 28, muted, false, 2);
    if (!status.empty()) ui.text(status, area.x, 600, area.w, accent);
}
}
