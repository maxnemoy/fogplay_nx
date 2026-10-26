#pragma once
#include "app/app_diagnostics.hpp"
#include <string>
namespace fogplay {
inline std::string display(const std::string& value, const std::string& fallback = "—") {
    return value.empty() ? fallback : diagnostics::safeText(value);
}
}
