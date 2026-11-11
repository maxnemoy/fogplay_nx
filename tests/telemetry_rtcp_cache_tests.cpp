#include "features/streaming/infrastructure/rtcp_feedback_worker.hpp"
#include <cassert>
#include <future>
#include <iostream>
using namespace std::chrono_literals;
using stream::telemetry::network::Publication;
static std::uint64_t nowUs(){return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
template<class F> void eventually(F f){const auto end=std::chrono::steady_clock::now()+2s;while(!f()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);assert(f());}
int main(){
 {
  stream::RtcpFeedbackWorker worker;const auto early=worker.numericSnapshot(nowUs());
  assert(!early.hasValue&&early.publication==Publication::NotPublished&&early.stale&&!early.workerStarted);
  auto receiver=std::make_shared<rtc::RtcpReceivingSession>();receiver->holdQuery=true;receiver->holdFlush=true;receiver->value.rtpPackets=11;
  assert(worker.addSession(receiver));worker.start();assert(receiver->waitFor([&]{return receiver->queryEntered;}));
  const auto producer=receiver->queryThread;assert(producer!=std::this_thread::get_id());
  const auto before=receiver->queries.load();
  auto ui=std::async(std::launch::async,[&]{for(int i=0;i<100;++i){assert(!worker.numericSnapshot(nowUs()).hasValue);assert(!worker.snapshot().feedbackAvailable);}});
  assert(ui.wait_for(100ms)==std::future_status::ready);ui.get();assert(receiver->queries==before);
  {std::lock_guard lock(receiver->mutex);receiver->holdQuery=false;receiver->changed.notify_all();}
  assert(receiver->waitFor([&]{return receiver->flushEntered;}));
  const auto live=worker.numericSnapshot(nowUs());assert(live.hasValue&&live.value.feedback.rtpPackets==11&&live.workerActive);
  const auto queries=receiver->queries.load();
  const auto old=worker.numericSnapshot(live.capturedAtUs+stream::RtcpFeedbackWorker::SnapshotStaleAfterUs+1);
  assert(old.stale&&old.ageKnown&&old.publication==Publication::Live);
  assert(receiver->queries==queries); // A stale read never refreshes/query SDK on caller.
  worker.requestStop();assert(!worker.quiescent()); // Existing lifecycle safety probe, independent of telemetry cache.
  receiver->release();worker.join();assert(worker.quiescent());
  const auto final=worker.numericSnapshot(nowUs());assert(final.publication==Publication::FinalRetained&&final.hasValue&&!final.workerActive);
  assert(final.value.stopped&&!final.value.running&&final.stopRequested&&final.workerStarted&&!final.workerRunning&&final.value.feedback.discardedReports==1);
  const auto after=receiver->queries.load();assert(worker.snapshot().feedbackFinal);assert(receiver->queries==after);
 }
 {
  stream::RtcpFeedbackWorker never;auto receiver=std::make_shared<rtc::RtcpReceivingSession>();assert(never.addSession(receiver));
  never.requestStop();never.join();const auto closed=never.numericSnapshot(nowUs());
  assert(!closed.hasValue&&closed.publication==Publication::ClosedWithoutFinal);assert(receiver->queries==0);
 }
 {
  stream::RtcpFeedbackWorker worker;auto a=std::make_shared<rtc::RtcpReceivingSession>(),b=std::make_shared<rtc::RtcpReceivingSession>();
  a->value.rtpPackets=3;a->value.sendMaxUs=7;a->value.pendingAgeMaxUs=18;
  b->value.rtpPackets=5;b->value.sendMaxUs=19;b->value.pendingAgeMaxUs=9;
  assert(worker.addSession(a)&&worker.addSession(b));worker.start();
  eventually([&]{return worker.numericSnapshot(nowUs()).hasValue;});
  const auto s=worker.numericSnapshot(nowUs());assert(s.value.feedback.rtpPackets==8&&s.value.feedback.sendMaxUs==19&&s.value.feedback.pendingAgeMaxUs==18);
  {std::lock_guard lock(a->mutex);a->throwQuery=true;}
  eventually([&]{return worker.numericSnapshot(nowUs()).publication==Publication::CaptureFailed;});
  const auto failed=worker.numericSnapshot(nowUs());assert(failed.hasValue&&failed.stale&&failed.value.feedback.rtpPackets==8);
  {std::lock_guard lock(a->mutex);a->throwQuery=false;}
  worker.requestStop();worker.join();assert(worker.numericSnapshot(nowUs()).publication==Publication::FinalRetained);
 }
 std::cout<<"Actual RTCP worker cache-only SDK-blocked reads, stale/final/early/failure and aggregation tests passed\n";
}
