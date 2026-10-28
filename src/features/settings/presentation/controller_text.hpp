#pragma once
#include "features/settings/domain/controller_layout.hpp"

namespace fog::controller::text {
inline constexpr char Menu[] = "Контроллер";
inline constexpr char Title[] = "КОНТРОЛЛЕР";
inline constexpr char Description[] = "Тип контроллера в игре и раскладка кнопок";
inline constexpr char ProfilePrefix[] = "Профиль: ";
inline constexpr char LayoutPrefix[] = "Раскладка кнопок: ";
inline constexpr char XboxDescription[] = "Кнопки, стики и вибрация Xbox.";
inline constexpr char DualSenseDescription[] = "Кнопки PlayStation, тачпад с экрана Switch, гироскоп и вибрация.";
inline constexpr char ApplyNotice[] = "Профиль применяется при следующем подключении к любой сессии.";
inline constexpr char HostNotice[] = "Сервис может определить DualSense как контроллер PS4.";
inline constexpr char LayoutNotice[] = "Раскладка меняет кнопки меню; в игре DualSense кнопки соответствуют их положению.";
inline constexpr char Saved[] = "Настройки контроллера сохранены на SD.";
inline constexpr char SaveFailed[] = "Не удалось сохранить. Предыдущие настройки сохранены.";
inline constexpr char LoadFailed[] = "Настройки не прочитаны. Используются профиль и раскладка Xbox.";
inline const char* profileLabel(::stream::ControllerProfile profile) {
    return profile == ::stream::ControllerProfile::DualSense ? "DualSense" : "Xbox";
}
}
