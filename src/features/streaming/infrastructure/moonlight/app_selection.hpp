#pragma once
#include <optional>
#include <string>
#include <vector>

namespace moonlight {
struct Application { int id; std::string name; };
inline std::string normalizedApplicationName(const std::string& name) {
    std::string result;
    bool space = false;
    for (unsigned char value : name) {
        if (value == ' ' || value == '\t' || value == '\r' || value == '\n') { space = !result.empty(); continue; }
        if (space) result += ' ';
        space = false;
        result += static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    }
    return result;
}
inline int applicationPriority(const std::string& name) {
    const auto value = normalizedApplicationName(name);
    if (value == "desktop") return 100;
    if (value == "playnite для геймпада" || value == "playnite (для геймпада)" ||
        value == "playnite - для геймпада" || value == "playnite fullscreen" ||
        value == "playnite (fullscreen)" || value == "playnite - fullscreen" ||
        value == "playnite gamepad" || value == "playnite (gamepad)") return 80;
    if (value == "playnite") return 70;
    if (value == "steam" || value == "steam big picture" || value == "steam (big picture)") return 60;
    return 0;
}
// Desktop is the generic default. FH hosts without Desktop prefer the controller
// launcher, then Playnite, then Steam. Equal-priority distinct IDs are ambiguous;
// unknown names are accepted only when there is exactly one application.
inline std::optional<int> selectApplication(const std::vector<Application>& apps) {
    int count = 0, unique = 0, selected = 0, priority = 0;
    bool ambiguous = false;
    for (const auto& app : apps) {
        if (app.id <= 0 || normalizedApplicationName(app.name).empty()) continue;
        ++count; unique = app.id;
        const auto next = applicationPriority(app.name);
        if (next > priority) { priority = next; selected = app.id; ambiguous = false; }
        else if (next != 0 && next == priority && selected != app.id) ambiguous = true;
    }
    if (priority != 0) return ambiguous ? std::nullopt : std::optional<int>{selected};
    return count == 1 ? std::optional<int>{unique} : std::nullopt;
}
}
