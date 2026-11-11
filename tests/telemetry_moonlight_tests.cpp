// Reuse only the existing local SDK fixture; compile the real production adapter.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main moonlightFixtureMain
#include "../tools/moonlight/tests/session_test.cpp"
#undef main
#pragma GCC diagnostic pop
#include "features/streaming/application/stream_diagnostics_formatter.hpp"
int main(){
 using namespace stream::telemetry;
 char name[]="/tmp/fog-telemetry-fh-XXXXXX";const auto created=mkdtemp(name);assert(created);
 {
  moonlight::MoonlightSession session(created);pair(session);auto client=session.create();assert(client);
  auto before=client->captureTelemetry(Capture::All);assert(before.provider==Provider::FogHub);
  assert(before.transport.availability==Availability::NotStarted&&!available(before.fh.core)&&!available(before.fhControl.command));
  expectedVideoFormats=VIDEO_FORMAT_H264|VIDEO_FORMAT_H265;expectedFps=30;
  assert(client->start({rental()}));eventually([&]{return client->ready();});
  stream::Gamepad pad;pad.sampledAtUs=LiGetMicroseconds()-1000;assert(client->sendGamepad(pad));
  const auto live=client->captureTelemetry(Capture::All);
  assert(live.streamEpoch&&live.transportEpoch==1&&available(live.transport)&&available(live.fh.core)&&available(live.fhControl.command));
  assert(live.fh.receive[0].calls==2&&live.fh.receive[0].totalUs==200&&live.fh.receive[2].totalUs==40);
  assert(live.fh.bufferActualBytes==131070&&live.fhControl.accepted==1&&live.fhControl.attempt==1);
  assert(live.input.scope==Scope::ClientRolling5s&&live.inputLatency.samples==1&&live.inputLatency.p50Us>=1000);
  assert(live.captureCompletedAtSteadyUs>=live.capturedAtSteadyUs&&live.videoGeneration>0);
  const auto expected="rx_cycle(n,avg_us,max_us)=2,100,150 rx_wait(n,avg_us,max_us)=2,80,120 rx_work(n,avg_us,max_us)=2,20,30 rx_loop_gap(n,avg_us,max_us)=1,8,8 rx_alloc(n,avg_us,max_us)=1,3,3 rx_decrypt(n,avg_us,max_us)=2,5,7 rx_invalid_times=4 | rcvbuf(requested,selected,actual)=98303,65535,131070 set_failures=2 last_set_error=12 query_ok=1 query_error=0";
  assert(transportText(live).find(expected)!=std::string::npos);
  client->close();const auto final=client->captureTelemetry(Capture::All);
  assert(final.transportEpoch==live.transportEpoch&&final.fh.core.availability==Availability::FinalRetained);
  assert(final.fh.receive[0].totalUs==200);
  assert(client->start({rental()}));eventually([&]{return client->ready();});
  const auto next=client->captureTelemetry(Capture::All);assert(next.transportEpoch==live.transportEpoch+1&&next.streamEpoch==live.streamEpoch);
  assert(next.inputLatency.samples==1); // Existing client rolling window survives reconnect.
  assert(next.fhControl.accepted==0);client->close();
  connectionResult=-77;assert(client->start({rental()}));eventually([&]{return !client->error().empty();});client->close();
  const auto failed=client->captureTelemetry(Capture::All);
  assert(failed.transportEpoch==next.transportEpoch+1&&!available(failed.fh.core));
  assert(transportText(failed).find("rx_cycle")==std::string::npos);connectionResult=0;
 }
 std::filesystem::remove_all(created);
 std::cout<<"Actual FH typed live/final/reconnect/early-failure capture and legacy golden fields passed\n";
}
