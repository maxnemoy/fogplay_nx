#pragma once
#include "features/settings/domain/rumble_settings.hpp"
#include <string>

namespace fog::rumble {
Settings loadSettings(const std::string& path);
void saveSettings(const std::string& path, const Settings& settings);
}
