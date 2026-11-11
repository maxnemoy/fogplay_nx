// Reuse the local-only STOMP/RTC fixture without changing its existing tests.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main mtsFixtureMain
#include "stream_integration.cpp"
#undef main
#pragma GCC diagnostic pop
#include "features/streaming/application/stream_diagnostics_formatter.hpp"
int main(){
 using namespace stream::telemetry;
 {
  Fixture fixture;stream::Client client;
  const auto before=client.captureTelemetry(Capture::All);
  assert(before.provider==Provider::Mts&&before.transport.availability==Availability::NotStarted);
  const std::string endpoint="ws://127.0.0.1:"+std::to_string(fixture.server->port())+"/ws/";
  assert(client.startForTesting("fixture","abcde",endpoint));
  auto pump=[&]{fixture.pump();client.pump();fixture.sendMedia();std::this_thread::sleep_for(std::chrono::milliseconds(2));};
  const auto began=Clock::now();while(Clock::now()-began<std::chrono::seconds(12)&&!available(client.captureTelemetry(Capture::Input).input))pump();
  assert(fixture.enabled&&client.error().empty());
  const auto live=client.captureTelemetry(Capture::All);
  assert(live.streamEpoch&&live.transportEpoch==1&&available(live.transport)&&available(live.control)&&available(live.input));
  assert(live.mts.stages[0].calls>0&&live.mts.iceWorkerPresent);
  assert(live.input.scope==Scope::TransportRolling5s&&live.captureCompletedAtSteadyUs>=live.capturedAtSteadyUs);
  assert(client.transportTimings().find("scope=transport-lifetime nested=1 iceThread=worker")==0);
  client.blockIceForTesting(true);
  const auto blockAt=Clock::now();for(int i=0;i<50;++i)pump();
  const auto captureAt=Clock::now();const auto blocked=client.captureTelemetry(Capture::All);
  assert(Clock::now()-captureAt<std::chrono::milliseconds(100));assert(available(blocked.transport));
  client.blockIceForTesting(false);assert(Clock::now()>=blockAt);
  client.close();const auto final=client.captureTelemetry(Capture::All);
  assert(final.transportEpoch==live.transportEpoch&&final.transport.availability==Availability::FinalRetained);
  assert(final.control.availability==Availability::FinalRetained&&!available(final.input));
  assert(inputText(final)=="provider=MTS unavailable=closed");
  assert(client.startForTesting("fixture","abcdf",endpoint));
  const auto next=client.captureTelemetry(Capture::All);assert(next.transportEpoch==live.transportEpoch+1&&next.streamEpoch==live.streamEpoch);
  assert(next.mts.stages[0].calls==0);client.close();
 }
 stream::Client::cleanupRuntime();
 std::cout<<"Actual MTS typed live/final/reconnect and blocked ICE capture passed\n";
}
