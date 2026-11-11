#pragma once
#include <array>
#include <cstdint>
#include <type_traits>
namespace stream::telemetry::network {
inline constexpr std::array<const char*,74> DiscoveryKeys={"unknown","id","index","large_motor","small_motor","network_features","flags","parameters","fec_group_limit","fresh_metrics","packet_recovery","repair_deadlines","metric_stale_ms","metric_window_ms","recovery_max_hold_ms","recovery_nack_retries","fecMaskType","fecfactor","useUnequalProtection","ratio","mode","enabled","level","nested","items","cpu_usage","gpu_usage","ping","encode","encodefull","grab","serverfps","serverqueuefps","packetize","sendsamples_video","sendsamples_audio","bitrate","fps","codec","quality","profile","automatic","target_bitrate","targetMbps","resolutions","width","height","preset","ts","accessToken","Cookie","password","authorization","url","sdp","candidate","ip","address","host","session","hash","email","phone","pin","text","clipboard","body","session_hash","credential","input","account_number","admin","network","flags_extra"};
inline constexpr std::array<const char*,12> DiscoveryIds={"<missing>","<invalid>","unknown","stats","ping","pong","close","error","gamepad_notification","network_features_state","params","resolutions"};
enum class DiscoveryTokenKind:unsigned char {ObjectBegin,ObjectEnd,ArrayBegin,ArrayEnd,Comma,Ellipsis,Key,Redacted,Null,Bool,Integer,Number,String,Binary,Invalid,ObjectDepth,ArrayDepth,BooleanValue,SignedValue,UnsignedValue,FloatValue,FecMaskValue};
struct DiscoveryToken {DiscoveryTokenKind kind=DiscoveryTokenKind::Invalid;unsigned char code=0;union {std::int64_t integer=0;double number;};};
struct DiscoveryShape {std::array<DiscoveryToken,64> tokens{};unsigned count=0;bool truncated=false;};
struct ControlDiscoveryDescription {
    unsigned id=0; // fixed allowlisted id only
    unsigned unknownNames=0,classification=255;std::uint64_t bytes=0;
    bool binary=false,oversized=false;
    DiscoveryShape schema{},sample{};
};
struct ControlDiscoveryEvent {
    static constexpr std::size_t BatchCapacity=4;
    std::uint64_t sequence=0,count=0,pending=0,firstMs=0,lastMs=0,peekedAtMs=0,schemas=0,discarded=0;
    bool overflow=false;ControlDiscoveryDescription description{};
};
static_assert(std::is_trivially_copyable_v<ControlDiscoveryEvent> && sizeof(ControlDiscoveryEvent)<=4096);
}
