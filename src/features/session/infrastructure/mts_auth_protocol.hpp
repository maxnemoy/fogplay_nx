#pragma once
#include "features/auth/domain/auth_repository.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
namespace fog {
using Json = nlohmann::json;
std::string normalizePhone(const std::string& input);
AuthStep authStep(const Json& response);
std::vector<unsigned char> decodeCaptcha(const std::string& input);
}
