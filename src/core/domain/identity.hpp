#pragma once
#include <cstdint>
#include <string>
#include <utility>

namespace fog {
enum class ProviderId { Mts, FogHub };
struct EntityId {
    ProviderId provider = ProviderId::Mts;
    std::string value;
    bool empty() const noexcept { return value.empty(); }
    friend bool operator==(const EntityId& a, const EntityId& b) {
        return a.provider == b.provider && a.value == b.value;
    }
    friend bool operator!=(const EntityId& a, const EntityId& b) { return !(a == b); }
    friend bool operator<(const EntityId& a, const EntityId& b) {
        return a.provider != b.provider ? a.provider < b.provider : a.value < b.value;
    }
};
using AccountId = EntityId;
struct Money {
    std::int64_t minorUnits = 0;
    std::string currency = "RUB";
};
}
