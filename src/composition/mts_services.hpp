#pragma once
#include "app/services.hpp"
namespace fogplay { std::unique_ptr<Services> makeMtsServices(const std::string& deviceId); }
