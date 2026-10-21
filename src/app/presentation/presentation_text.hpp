#pragma once
#include "features/session/infrastructure/api.hpp"
#include "app/app_diagnostics.hpp"

namespace fogplay {
inline std::string field(const fog::Json& object, const char* key, const std::string& fallback = "—") {
    if (!object.is_object() || !object.contains(key) || object[key].is_null()) return fallback;
    const auto& value = object[key];
    if (value.is_string()) return diagnostics::safeText(value.get<std::string>());
    if (value.is_primitive()) return value.dump();
    return fallback;
}

}
