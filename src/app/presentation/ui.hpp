#pragma once
#include <SDL.h>
#include <SDL_ttf.h>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace fog::ui {
inline constexpr std::size_t HistoryColumns = 4;
constexpr int Width = 1280;
constexpr int Height = 720;
enum class Section { Search, Games, Computers, History, Settings, PlayStation };
struct Chrome {
    Section section = Section::Games;
    bool sidebarExpanded = false;
    bool sidebarFocused = false;
    std::string account = "Войти в аккаунт";
    std::string title = "ВЫБЕРИ ИГРУ";
    std::string subtitle;
    std::string hints = "A Выбрать   B Назад   X Поиск   Y Аккаунт   L / R Страницы";
};
struct GameCard {
    std::string name, price, note;
    SDL_Texture* cover = nullptr; // Borrowed until draw returns.
};
struct ComputerCard {
    std::string name, gpu, cpu, price;
    std::string platform, accountNote, ping; // Empty ping means unmeasured.
    bool available = true;
};
struct AutoChoice { std::string title, description, price; bool available = false; };
struct GameDetail {
    std::string name, description, rating;
    std::vector<std::string> genres;
    bool ownSteamAccount = false;
    std::vector<AutoChoice> choices;
};
struct HistoryCard { std::string name, start, end, duration, amount, note; };
struct SettingCard { std::string title, value, description; std::optional<bool> checked = {}; };
struct Dialog {
    std::string title, body, primary = "Продолжить", secondary = "Назад";
    bool primaryFocused = true;
    bool danger = false;
    SDL_Texture* image = nullptr;
};
// Main-thread only. Renderer/fonts/covers are borrowed; destroy Ui before them.
// Logical coordinates 1280x720. Constructor loads bundled branding once.
// Drawing never presents, mutates models or performs IO.
class Ui {
public:
    Ui(SDL_Renderer* renderer, TTF_Font* regular, TTF_Font* large);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;
    void setXboxLayout(bool enabled);
    // Explicit controller instructions only; content and keyboard letters stay literal.
    std::string controllerHints(const std::string& value) const;
    void frame(const Chrome& chrome);
    SDL_Rect content() const;
    void games(const std::vector<GameCard>& cards, std::size_t selected,
               std::size_t first = 0, const std::string& heading = "Игры",
               int top = 154, std::size_t rows = 2);
    // Genre chips occupy y=82..126. Indices outside labels mean no focus/selection.
    void genres(const std::vector<std::string>& labels, std::size_t focused = static_cast<std::size_t>(-1),
                std::size_t selected = static_cast<std::size_t>(-1));
    // Modal grid: two columns, six rows; first must align to a two-column row.
    void genreDialog(const std::vector<std::string>& labels, std::size_t focused,
                     std::size_t first = 0, std::size_t selected = static_cast<std::size_t>(-1));
    void recentGames(const std::vector<GameCard>& cards, std::size_t selected, std::size_t first = 0);
    void computers(const std::vector<ComputerCard>& cards, std::size_t selected,
                   std::size_t first = 0, int top = 154);
    // Focus 0: Steam toggle; 1..3: auto choices. Empty choices draw header only.
    // Returns y for subsequent content (300 header-only; 542 with auto choices).
    int gameDetail(const GameDetail& detail, int focus = -1);
    void history(const std::vector<HistoryCard>& cards, std::size_t selected,
                 std::size_t first = 0);
    void settings(const std::vector<SettingCard>& cards, std::size_t selected);
    void dialog(const Dialog& dialog);
    void status(const std::string& message, bool busy = false, bool error = false);
    void empty(const std::string& title, const std::string& detail);
    void text(const std::string& value, int x, int y, int width,
              SDL_Color color = {245, 246, 255, 255}, bool large = false, int lines = 1);
    void panel(SDL_Rect rect, bool focused = false, bool danger = false);
    void button(const std::string& label, SDL_Rect rect, bool focused = false, bool enabled = true);
    void image(SDL_Texture* texture, SDL_Rect rect, bool cover = false);
    void clearTextCache();
    std::size_t cachedTextCount() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}