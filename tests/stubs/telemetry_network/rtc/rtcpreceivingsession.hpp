#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace rtc {
class RtcpReceivingSession {
public:
    struct FeedbackDiagnostics {
        std::uint64_t rtpPackets=0,rtcpPackets=0,senderReports=0,malformedControl=0;
        std::uint64_t regularRequests=0,coalescedRequests=0,discardedReports=0;
        std::uint64_t sendAttempts=0,sentPackets=0,sendFailures=0,rrPackets=0,rembPackets=0,compoundPackets=0;
        std::uint64_t pendingAgeMaxUs=0,sendTotalUs=0,sendMaxUs=0;
        bool pending=false,stopped=false,inFlight=false;
    };
    std::atomic<unsigned> queries{0},flushes{0};
    bool holdQuery=false,queryEntered=false,holdFlush=false,flushEntered=false,throwQuery=false;
    mutable std::mutex mutex;mutable std::condition_variable changed;
    std::thread::id queryThread;
    FeedbackDiagnostics value{};
    FeedbackDiagnostics getFeedbackDiagnostics() {
        std::unique_lock lock(mutex);++queries;queryThread=std::this_thread::get_id();queryEntered=true;changed.notify_all();
        changed.wait(lock,[&]{return !holdQuery;});if(throwQuery)throw std::runtime_error("SECRET");return value;
    }
    bool flushPendingReports() {
        std::unique_lock lock(mutex);++flushes;flushEntered=true;value.inFlight=true;changed.notify_all();
        changed.wait(lock,[&]{return !holdFlush;});
        if(!value.stopped){++value.sendAttempts;++value.sentPackets;value.rrPackets+=2;value.sendTotalUs+=12;value.sendMaxUs=12;}
        value.inFlight=false;return true;
    }
    void discardPendingReports() {std::lock_guard lock(mutex);value.stopped=true;value.pending=false;++value.discardedReports;}
    template<class F> bool waitFor(F predicate){std::unique_lock lock(mutex);return changed.wait_for(lock,std::chrono::seconds(2),predicate);}
    void release(){std::lock_guard lock(mutex);holdQuery=holdFlush=false;changed.notify_all();}
};
}
