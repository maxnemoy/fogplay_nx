#include "features/streaming/domain/streaming_client.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include "features/streaming/infrastructure/moonlight/moonlight_session.hpp"
#include <type_traits>

static_assert(std::is_convertible_v<stream::StreamingClient*,stream::CompressedMediaSource*>);
static_assert(std::is_convertible_v<stream::StreamingClient*,stream::StreamInputSink*>);
static_assert(std::is_convertible_v<stream::StreamingClient*,stream::StreamLifecycle*>);
static_assert(std::is_base_of_v<stream::StreamInputSink,stream::Client>);
static_assert(std::is_base_of_v<stream::StreamLifecycle,stream::Client>);
static_assert(!std::is_abstract_v<stream::Client>);
static_assert(!std::is_abstract_v<moonlight::MoonlightSession>);
// Existing source-level aggregate member calls/pointers remain expressible.
constexpr bool (stream::StreamingClient::*startMember)(const stream::StreamConnection&)=&stream::StreamingClient::start;
constexpr bool (stream::StreamingClient::*inputMember)(const stream::Gamepad&)=&stream::StreamingClient::sendGamepad;
constexpr bool (stream::StreamingClient::*videoMember)(stream::Packet&,std::uint64_t)=&stream::StreamingClient::pollVideo;
static_assert(startMember && inputMember && videoMember);