#pragma once
#include "features/settings/domain/video_settings.hpp"

namespace fog::video {
Settings loadSettings(const std::string& path);
void saveSettings(const std::string& path, const Settings& settings);
}
