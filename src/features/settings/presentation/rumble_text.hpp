#pragma once
#include "features/settings/domain/rumble_settings.hpp"

namespace fog::rumble::text {
inline constexpr char Title[] = "ВИБРАЦИЯ";
inline constexpr char MenuLabel[] = "Вибрация";
inline constexpr char Description[] = "Режим отдачи для всех сервисов";
inline constexpr char Hints[] = "← → / A — изменить · B — назад";
inline constexpr char Saved[] = "Настройка вибрации сохранена";
inline constexpr char SaveFailed[] = "Не удалось сохранить настройку вибрации";
inline constexpr char LoadFailed[] = "Не удалось прочитать настройку вибрации: включён режим 160 Гц";
inline constexpr char Shared[] = "Общая настройка для МТС Fog Play и Moonlight / FogHub.";
inline const char* label(Mode mode) {
    switch (mode) {
        case Mode::Balanced: return "Равномерная · 160 Гц";
        case Mode::DualBand: return "Две частоты · 160 + 320 Гц";
        case Mode::Chromium: return "Chromium · 141 / 182 Гц";
        case Mode::Disabled: return "Выключена";
    }
    return "Неизвестный режим";
}
inline const char* description(Mode mode) {
    switch (mode) {
        case Mode::Balanced: return "Одинаковая частота с обеих сторон. Силу каждого Joy-Con задаёт игра.";
        case Mode::DualBand: return "Каждый Joy-Con сочетает 160 и 320 Гц. Сила сторон задаётся отдельно.";
        case Mode::Chromium: return "141 Гц слева, 182 Гц справа. Правый канал намеренно слабее, как в Chromium.";
        case Mode::Disabled: return "Отдача отключена для всех игр и сервисов.";
    }
    return "";
}
}
