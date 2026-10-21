#include "features/streaming/presentation/stream_overlay.hpp"
#include "app/presentation/ui.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fog::stream {
namespace {
constexpr SDL_Color White{247, 248, 255, 255}, Muted{179, 185, 209, 255};
constexpr SDL_Rect MenuBox{374, 82, 532, 563};
constexpr SDL_Rect KeyboardBox{28, 22, 1224, 680};
constexpr SDL_Rect ConfirmBox{294, 215, 692, 290};
constexpr int KeyX = 52, KeyY = 244, KeyStep = 98, RowStep = 65;
SDL_Rect menuButton(int index) { return {408, 174 + index * 76, 464, 58}; }
SDL_Rect imageButton(int index) { return {174, 142 + index * 62, 932, 53}; }
SDL_Rect confirmButton(int index) { return {328 + index * 320, 412, 296, 58}; }
SDL_Rect keyRect(int index) {
    if (index < 48) return {KeyX + (index % 12) * KeyStep, KeyY + (index / 12) * RowStep, 90, 55};
    if (index < 54) return {KeyX + (index - 48) * KeyStep * 2, KeyY + RowStep * 4, 188, 55};
    const std::array<int, 5> starts{0, 3, 5, 7, 10}, spans{3, 2, 2, 3, 2};
    const auto action = static_cast<std::size_t>(index - 54);
    return {KeyX + starts[action] * KeyStep, KeyY + RowStep * 5, spans[action] * KeyStep - 8, 55};
}
int keyRow(int index) { return index < 48 ? index / 12 : index < 54 ? 4 : 5; }
bool contains(SDL_Rect rect, int x, int y) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}
std::vector<std::string> characters(const std::string& value) {
    std::vector<std::string> result;
    for (std::size_t pos = 0; pos < value.size();) {
        auto end = pos + 1;
        while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) ++end;
        result.push_back(value.substr(pos, end - pos));
        pos = end;
    }
    return result;
}
const std::vector<std::string>& alphabet(bool russian, bool uppercase) {
    static const auto enLower = characters("1234567890-=qwertyuiop[]asdfghjkl;'\\zxcvbnm,./`?");
    static const auto enUpper = characters("!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"|ZXCVBNM<>/~?");
    static const auto ruLower = characters("1234567890-=йцукенгшщзхъфывапролджэ\\ячсмитьбюё.,");
    static const auto ruUpper = characters("!@#$%^&*()_+ЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭ|ЯЧСМИТЬБЮЁ,?");
    return russian ? (uppercase ? ruUpper : ruLower) : (uppercase ? enUpper : enLower);
}
bool validText(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i]);
        const std::size_t count = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (count == 0 || i + count > value.size() || c < 32 || c == 127) return false;
        for (std::size_t j = 1; j < count; ++j)
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        const auto second = count > 1 ? static_cast<unsigned char>(value[i + 1]) : 0;
        if (count == 3 && ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0))) return false;
        if (count == 4 && ((c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90))) return false;
        i += count;
    }
    return true;
}
std::string visibleText(const std::string& value) {
    const auto chars = characters(value);
    if (chars.size() <= 64) return value;
    std::string result = "…";
    for (auto i = chars.size() - 64; i < chars.size(); ++i) result += chars[i];
    return result;
}
void shade(SDL_Renderer* renderer) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 5, 7, 17, 175);
    const SDL_Rect screen{0, 0, ui::Width, ui::Height};
    SDL_RenderFillRect(renderer, &screen);
}
}

void Overlay::openMenu() { text_.clear(); mode_ = OverlayMode::Menu; selection_ = 0; }
void Overlay::close() { text_.clear(); mode_ = OverlayMode::Closed; selection_ = 0; }
void Overlay::up() { move(0, -1); }
void Overlay::down() { move(0, 1); }
OverlayAction Overlay::left() {
    if (mode_ == OverlayMode::ImageSettings) return adjustVideo(-1);
    move(-1, 0); return {};
}
OverlayAction Overlay::right() {
    if (mode_ == OverlayMode::ImageSettings) return adjustVideo(1);
    move(1, 0); return {};
}
OverlayAction Overlay::adjustVideo(int direction) {
    if (mode_ != OverlayMode::ImageSettings || selection_ < 1 || selection_ > 3) return {};
    const std::array<video::Field, 3> fields{video::Field::Quality, video::Field::Bitrate, video::Field::Keyframes};
    return video::adjust(videoSettings_, fields[static_cast<std::size_t>(selection_ - 1)], direction) ?
        OverlayAction{OverlayActionType::SettingsChanged, {}} : OverlayAction{};
}
void Overlay::move(int horizontal, int vertical) {
    if (mode_ == OverlayMode::Menu) {
        const int step = vertical != 0 ? vertical : horizontal;
        selection_ = (selection_ + step + 5) % 5;
    } else if (mode_ == OverlayMode::ImageSettings) {
        selection_ = 1 + (selection_ - 1 + vertical + 5) % 5;
    } else if (mode_ == OverlayMode::StopConfirm) {
        selection_ = 1 - selection_;
    } else if (mode_ == OverlayMode::Keyboard) {
        const int row = keyRow(selection_);
        if (horizontal != 0) {
            const int first = row < 4 ? row * 12 : row == 4 ? 48 : 54;
            const int count = row < 4 ? 12 : row == 4 ? 6 : 5;
            selection_ = first + (selection_ - first + horizontal + count) % count;
        } else {
            const int targetRow = (row + vertical + 6) % 6;
            const auto selected = keyRect(selection_);
            const int center = selected.x + selected.w / 2;
            int bestDistance = ui::Width;
            for (int i = 0; i < KeyboardKeyCount; ++i) {
                if (keyRow(i) != targetRow) continue;
                const auto candidate = keyRect(i);
                const int distance = std::abs(center - candidate.x - candidate.w / 2);
                if (distance < bestDistance) { bestDistance = distance; selection_ = i; }
            }
        }
    }
}
bool Overlay::appendText(const std::string& value) {
    if (mode_ != OverlayMode::Keyboard || value.size() > MaxTextBytes - text_.size() || !validText(value)) return false;
    text_ += value;
    return true;
}
void Overlay::eraseLast() {
    if (text_.empty()) return;
    auto pos = text_.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text_[pos]) & 0xc0) == 0x80) --pos;
    text_.resize(pos);
}
std::string Overlay::keyLabel(int index) const {
    if (index < 48) return alphabet(russian_, uppercase_)[static_cast<std::size_t>(index)];
    const std::array<std::string, 11> labels{
        russian_ ? "RU → EN" : "EN → RU", uppercase_ ? "ABC → abc" : "abc → ABC", "Пробел", "Удалить", "Очистить", "Отмена",
        "Отправить текст", "Enter ПК", "Tab ПК", "Backspace ПК", "Escape ПК"};
    return labels[static_cast<std::size_t>(index - 48)];
}
OverlayAction Overlay::select() {
    if (mode_ == OverlayMode::Menu) {
        switch (selection_) {
            case 0: mode_ = OverlayMode::Mouse; break;
            case 1: mode_ = OverlayMode::Keyboard; text_.clear(); break;
            case 2: mode_ = OverlayMode::ImageSettings; selection_ = 1; return {};
            case 3: mode_ = OverlayMode::StopConfirm; selection_ = 1; return {};
            case 4: close(); return {};
        }
        selection_ = 0;
    } else if (mode_ == OverlayMode::StopConfirm) {
        if (selection_ == 0) { close(); return {OverlayActionType::StopSession, {}}; }
        mode_ = OverlayMode::Menu; selection_ = 3;
    } else if (mode_ == OverlayMode::ImageSettings) {
        if (selection_ == 4) return {OverlayActionType::RequestKeyframe, {}};
        if (selection_ == 5) return back();
        return adjustVideo(1);
    } else if (mode_ == OverlayMode::Keyboard) {
        if (selection_ < 48) appendText(keyLabel(selection_));
        else switch (selection_) {
            case 48: russian_ = !russian_; break;
            case 49: uppercase_ = !uppercase_; break;
            case 50: appendText(" "); break;
            case 51: eraseLast(); break;
            case 52: text_.clear(); break;
            case 53: return back();
            case 54: {
                if (text_.empty()) return {};
                auto sent = text_;
                close();
                return {OverlayActionType::SendText, std::move(sent)};
            }
            case 55: return {OverlayActionType::Enter, {}};
            case 56: return {OverlayActionType::Tab, {}};
            case 57: return {OverlayActionType::Backspace, {}};
            case 58: return {OverlayActionType::Escape, {}};
        }
    }
    return {};
}
OverlayAction Overlay::back() {
    if (mode_ == OverlayMode::Menu) close();
    else if (mode_ != OverlayMode::Closed) {
        const int item = mode_ == OverlayMode::Keyboard ? 1 : mode_ == OverlayMode::StopConfirm ? 3 : mode_ == OverlayMode::ImageSettings ? 2 : 0;
        text_.clear(); mode_ = OverlayMode::Menu; selection_ = item;
    }
    return {};
}
OverlayAction Overlay::touch(int x, int y) {
    if (mode_ == OverlayMode::Menu) {
        for (int i = 0; i < 5; ++i) if (contains(menuButton(i), x, y)) { selection_ = i; return select(); }
    } else if (mode_ == OverlayMode::ImageSettings) {
        for (int i = 1; i < 6; ++i) if (contains(imageButton(i), x, y)) { selection_ = i; return select(); }
    } else if (mode_ == OverlayMode::Keyboard) {
        for (int i = 0; i < KeyboardKeyCount; ++i) if (contains(keyRect(i), x, y)) { selection_ = i; return select(); }
    } else if (mode_ == OverlayMode::StopConfirm) {
        for (int i = 0; i < 2; ++i) if (contains(confirmButton(i), x, y)) { selection_ = i; return select(); }
    }
    return {};
}
void Overlay::paint(ui::Ui& ui, SDL_Renderer* renderer) const {
    if (!renderer || mode_ == OverlayMode::Closed) return;
    if (mode_ == OverlayMode::Mouse) {
        ui.panel({26, 598, 1228, 96});
        ui.text(ui.controllerHints("Мышь: левый стик — курсор · A / ZR — левая кнопка · X — правая"), 48, 610, 1184, White);
        ui.text(ui.controllerHints("L / R — прокрутка · касание — курсор и перетаскивание · B — меню"), 48, 648, 1184, Muted);
        return;
    }
    shade(renderer);
    if (mode_ == OverlayMode::Menu) {
        ui.panel(MenuBox);
        ui.text("Управление сессией", 408, 108, 464, White, true);
        const std::array<std::string, 5> labels{"Мышь", "Клавиатура", "Изображение", "Завершить сессию", "Закрыть меню"};
        for (int i = 0; i < 5; ++i) ui.button(labels[static_cast<std::size_t>(i)], menuButton(i), selection_ == i);
        ui.text(ui.controllerHints("A — выбрать · B — закрыть"), 408, 593, 464, Muted);
    } else if (mode_ == OverlayMode::ImageSettings) {
        ui.panel({140, 15, 1000, 690});
        ui.text("Изображение · трансляция продолжается", 174, 42, 932, White, true);
        ui.text(ui.controllerHints("Кодек меняется до подключения. ← / → или A — изменить · B — меню"), 174, 96, 932, Muted);
        const std::array<std::string, 6> labels{
            "Кодек: " + video::codecLabel(videoSettings_) + " (до подключения)",
            "Качество сервера: " + video::qualityLabel(videoSettings_),
            "Запрошенный предел: " + std::to_string(videoSettings_.bitrateMbps) + " Мбит/с",
            "Ключевые кадры: " + video::keyframeLabel(videoSettings_),
            "Запросить ключевой кадр сейчас", "Назад"};
        for (int i = 0; i < 6; ++i) ui.button(labels[static_cast<std::size_t>(i)], imageButton(i), selection_ == i, i != 0);
        ui.text("Битрейт может отличаться. Запрос кадров не задаёт частоту видео.", 174, 523, 932, Muted);
        ui.text(videoStatus_, 174, 559, 932, White, false, 2);
        ui.text(saveStatus_, 174, 630, 932, Muted, false, 2);
    } else if (mode_ == OverlayMode::StopConfirm) {
        ui.panel(ConfirmBox, false, true);
        ui.text("Завершить сессию?", 328, 240, 624, White, true);
        ui.text("Аренда завершится после подтверждения сервера.\nДождитесь результата.", 328, 300, 624, Muted, false, 3);
        ui.button("Завершить", confirmButton(0), selection_ == 0);
        ui.button("Продолжить сессию", confirmButton(1), selection_ == 1);
    } else if (mode_ == OverlayMode::Keyboard) {
        ui.panel(KeyboardBox);
        ui.text("Клавиатура", 52, 36, 600, White, true);
        ui.text(ui.controllerHints("Текст отправляется только кнопкой «Отправить текст». B — отмена"), 52, 84, 1176, Muted);
        ui.panel({52, 130, 1176, 100});
        ui.text(text_.empty() ? "Введите текст…" : visibleText(text_), 68, 143, 1144, text_.empty() ? Muted : White, false, 2);
        ui.text(std::to_string(text_.size()) + " / 512 байт · " + (russian_ ? "RU" : "EN") + (uppercase_ ? " ABC" : " abc"), 836, 39, 392, Muted);
        for (int i = 0; i < KeyboardKeyCount; ++i) ui.button(keyLabel(i), keyRect(i), selection_ == i, i != 54 || !text_.empty());
        ui.text("Удалить / Очистить — местный текст. Кнопки «ПК» сразу отправляют клавишу.", 52, 650, 1176, Muted);
    }
}
}



