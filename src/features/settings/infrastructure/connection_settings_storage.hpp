#pragma once
#include "features/settings/domain/connection_settings.hpp"

namespace fog::connection {
Settings loadSettings(const std::string& path);
void saveSettings(const std::string& path, const Settings& settings);
}
