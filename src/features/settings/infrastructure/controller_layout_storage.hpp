#pragma once
#include "features/settings/domain/controller_layout.hpp"

namespace fog::controller {
Settings loadSettings(const std::string& path);
void saveSettings(const std::string& path, const Settings& settings);
}
