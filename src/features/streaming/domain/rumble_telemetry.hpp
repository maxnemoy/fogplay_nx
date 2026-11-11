#pragma once
#include <cstdint>
#include <type_traits>
namespace stream::telemetry::rumble {
enum class Mapping : std::uint8_t {Balanced,DualBand,Chromium,Disabled,Invalid=255};
struct Snapshot {
    std::uint64_t received=0,applied=0,stops=0,failures=0;
    std::uint32_t result=0,style=0,handles=0;
    std::int32_t npad=-1;
    Mapping mapping=Mapping::Balanced;
};
static_assert(std::is_trivially_copyable_v<Snapshot> && sizeof(Snapshot)<=64);
}
