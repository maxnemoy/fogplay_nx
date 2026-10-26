#include "core/constants/ui_text.hpp"
#include "features/playstation/presentation/catalog_view.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>

namespace uiText = ::fog::constants::ui;

namespace fog::playstation {
namespace {
constexpr SDL_Color White{247,248,255,255}, Muted{179,185,209,255}, Green{107,216,150,255};
std::string name(const Computer& computer) {
    return computer.name.empty() ? "PlayStation 5" : computer.name;
}
std::string price(const Computer& computer) {
    if (!computer.hourlyPrice || computer.hourlyPrice->minorUnits <= 0) return uiText::Server::TariffUnconfirmed;
    const auto amount = static_cast<double>(computer.hourlyPrice->minorUnits)/100.0;
    std::ostringstream value;
    value << std::fixed << std::setprecision(2) << amount;
    auto result = value.str();
    if (result.size() >= 3 && result.substr(result.size()-3) == ".00") result.resize(result.size()-3);
    return result + uiText::Common::RublesPerHourSuffix;
}
Availability availability(const Computer& computer) { return computer.availability; }
std::string status(Availability availability) {
    if (availability == Availability::Online) return uiText::PlayStation::Online;
    if (availability == Availability::Offline) return uiText::PlayStation::Offline;
    return uiText::PlayStation::UnknownStatus;
}
}
void drawCatalog(ui::Ui& ui, const std::vector<Computer>& computers, std::size_t selected,
                 bool focused, bool loading) {
    if (computers.empty()) {
        ui.empty(loading ? uiText::PlayStation::Loading : uiText::PlayStation::NotLoaded,
            ui.controllerHints(uiText::PlayStation::RefreshHint));
        return;
    }
    const auto area = ui.content();
    constexpr std::size_t columns = 4, pageSize = columns*2;
    constexpr int gap = 18, top = 122, height = 218, rowStep = 230;
    const int width = (area.w-gap*static_cast<int>(columns-1))/static_cast<int>(columns);
    const auto first = selected/pageSize*pageSize;
    for (std::size_t i=first; i<computers.size() && i<first+pageSize; ++i) {
        const auto& computer = computers[i];
        const auto offset = i-first;
        const int x = area.x+static_cast<int>(offset%columns)*(width+gap);
        const int y = top+static_cast<int>(offset/columns)*rowStep;
        const auto state = availability(computer);
        ui.panel({x,y,width,height},focused && selected==i);
        ui.text(name(computer),x+16,y+14,width-32,White,false,2);
        ui.text(status(state),x+16,y+82,width-32,state == Availability::Online ? Green : Muted,false,2);
        ui.text(price(computer),x+16,y+146,width-32,White,false,2);
    }
}
void drawConfirmation(ui::Ui& ui, const Computer& computer) {
    ui.dialog({uiText::Server::ConnectPrefix + name(computer) + "?",
        "PlayStation 5 · " + price(computer) + ".\n" + status(availability(computer)) +
        uiText::PlayStation::ConnectionNotice,
        uiText::Common::Connect, uiText::Common::Back});
}
}
