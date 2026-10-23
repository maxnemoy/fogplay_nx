#include <rtc/rtcpreceivingsession.hpp>
#include "features/streaming/infrastructure/rtcp_feedback_worker.hpp"
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
using Receiver = rtc::RtcpReceivingSession;
static rtc::message_ptr rtp(unsigned seq, unsigned ssrc = 42) {
    auto message = rtc::make_message(sizeof(rtc::RtpHeader), rtc::Message::Binary);
    auto *header = reinterpret_cast<rtc::RtpHeader *>(message->data());
    header->preparePacket(); header->setSsrc(ssrc); header->setSeqNumber(seq); header->setPayloadType(96);
    return message;
}
static rtc::message_ptr sr(unsigned ssrc = 42, uint64_t ntp = 0x123456789abcdef0) {
    auto message = rtc::make_message(rtc::RtcpSr::Size(0), rtc::Message::Control);
    auto *header = reinterpret_cast<rtc::RtcpSr *>(message->data());
    header->preparePacket(ssrc, 0); header->setNtpTimestamp(ntp); header->setRtpTimestamp(1234);
    return message;
}
static void feed(Receiver &receiver, rtc::message_ptr packet, const rtc::message_callback &send) {
    rtc::message_vector messages{std::move(packet)}; receiver.incoming(messages, send);
}
static const rtc::RtcpRr &rr(const rtc::message_ptr &packet) {
    assert(packet->size() >= rtc::RtcpRr::SizeWithReportBlocks(1));
    const auto *report = reinterpret_cast<const rtc::RtcpRr *>(packet->data());
    assert(report->header.payloadType() == 201); return *report;
}
static unsigned fraction(const rtc::message_ptr &packet) {
    // Read wire byte; the upstream convenience accessor has an unrelated bitmask typo.
    return std::to_integer<unsigned char>((*packet)[12]);
}
static void correctness() {
    Receiver receiver;
    std::vector<rtc::message_ptr> sent;
    const rtc::message_callback send = [&](rtc::message_ptr p) { sent.push_back(std::move(p)); };
    feed(receiver, rtp(100), send); feed(receiver, sr(), send);
    assert(sent.size() == 1);
    assert(rr(sent.back()).getReportBlock(0)->extendedHighestSeqNo() == 100);
    assert(rr(sent.back()).getReportBlock(0)->getPacketsLostCount() == 0);
    assert(fraction(sent.back()) == 0);
    for (auto seq : {101, 103}) feed(receiver, rtp(seq), send);
    feed(receiver, sr(), send);
    assert(rr(sent.back()).getReportBlock(0)->getPacketsLostCount() == 1);
    assert(fraction(sent.back()) == 85); // one lost out of three since prior emission
    feed(receiver, rtp(104), send); feed(receiver, sr(), send);
    assert(fraction(sent.back()) == 0);
    Receiver wrap;
    for (auto seq : {65534, 65535, 0, 1}) feed(wrap, rtp(seq), send);
    feed(wrap, sr(), send);
    assert(rr(sent.back()).getReportBlock(0)->extendedHighestSeqNo() == 65537);
    assert(rr(sent.back()).getReportBlock(0)->getPacketsLostCount() == 0);
    Receiver duplicates;
    feed(duplicates, rtp(100), send); feed(duplicates, rtp(100), send); feed(duplicates, sr(), send);
    assert(rr(sent.back()).getReportBlock(0)->getPacketsLostCount() == 0xffffff); // signed -1
    assert(fraction(sent.back()) == 0);
    std::puts("PASS first packet, sequence rollover, signed cumulative and interval loss");
}
static void coalescing() {
    Receiver receiver; receiver.setDeferredReports(true);
    std::vector<rtc::message_ptr> sent;
    const rtc::message_callback send = [&](rtc::message_ptr p) { sent.push_back(std::move(p)); };
    feed(receiver, rtp(100), send);
    receiver.requestBitrate(30000000, send);
    for (unsigned i = 0; i < 1000; ++i) feed(receiver, sr(), send);
    receiver.requestBitrate(45000000, send);
    assert(sent.empty());
    assert(receiver.flushPendingReports());
    assert(sent.size() == 1);
    const auto bytes = rr(sent.back()).header.lengthInBytes();
    assert(bytes < sent.back()->size());
    auto *remb = reinterpret_cast<rtc::RtcpRemb *>(sent.back()->data() + bytes);
    assert(remb->header.header.payloadType() == 206);
    assert(remb->getBitrate() >= 44900000 && remb->getBitrate() <= 45100000);
    auto stats = receiver.getFeedbackDiagnostics();
    assert(stats.senderReports == 1000 && stats.coalescedRequests == 1001);
    assert(stats.rrPackets == 1 && stats.rembPackets == 1 && stats.compoundPackets == 1);
    feed(receiver, sr(), send);
    assert(!receiver.flushPendingReports());
    std::this_thread::sleep_for(260ms);
    assert(receiver.flushPendingReports());
    assert(sent.size() == 2);
    receiver.discardPendingReports();
    feed(receiver, sr(), send); receiver.requestBitrate(10000000, send);
    assert(!receiver.flushPendingReports(0ms));
    assert(!receiver.getFeedbackDiagnostics().pending);
    std::puts("PASS 1000 SR -> one compound RR/REMB, latest bitrate, pacing, terminal discard");
}
static void failedEmission() {
    Receiver receiver; receiver.setDeferredReports(true);
    std::vector<rtc::message_ptr> sent;
    bool fail = true;
    const rtc::message_callback send = [&](rtc::message_ptr p) {
        if (fail) throw std::runtime_error("synthetic send failure"); sent.push_back(std::move(p));
    };
    feed(receiver, rtp(100), send); feed(receiver, rtp(102), send); feed(receiver, sr(), send);
    try { receiver.flushPendingReports(0ms); assert(false); } catch (const std::runtime_error &) {}
    fail = false;
    feed(receiver, rtp(104), send);
    assert(receiver.flushPendingReports(0ms));
    assert(rr(sent.back()).getReportBlock(0)->getPacketsLostCount() == 2);
    assert(fraction(sent.back()) == 102); // uncommitted failed report retains full 2/5 interval
    assert(receiver.getFeedbackDiagnostics().sendFailures == 1);
    std::puts("PASS failed emission preserves loss interval and retries current state");
}
static void restartDuringFlush() {
    Receiver receiver; receiver.setDeferredReports(true);
    std::mutex mutex; std::condition_variable changed;
    bool entered = false, release = false;
    std::vector<rtc::message_ptr> sent;
    const rtc::message_callback blocked = [&](rtc::message_ptr p) {
        std::unique_lock lock(mutex); entered = true; changed.notify_all();
        changed.wait(lock, [&] { return release; }); sent.push_back(std::move(p));
    };
    feed(receiver, rtp(100), blocked); feed(receiver, rtp(101), blocked); feed(receiver, sr(), blocked);
    auto flush = std::async(std::launch::async, [&] { receiver.flushPendingReports(0ms); });
    { std::unique_lock lock(mutex); assert(changed.wait_for(lock, 2s, [&] { return entered; })); }
    feed(receiver, rtp(40000), blocked); feed(receiver, rtp(40001), blocked); // same-SSRC restart
    { std::lock_guard lock(mutex); release = true; changed.notify_all(); }
    assert(flush.wait_for(2s) == std::future_status::ready); flush.get();
    const rtc::message_callback send = [&](rtc::message_ptr p) { sent.push_back(std::move(p)); };
    feed(receiver, rtp(40003), send); feed(receiver, sr(), send);
    assert(receiver.flushPendingReports(0ms));
    assert(rr(sent.back()).getReportBlock(0)->getPacketsLostCount() == 1);
    assert(fraction(sent.back()) == 85);
    assert(rr(sent.back()).getReportBlock(0)->extendedHighestSeqNo() == 40003);
    std::puts("PASS in-flight old report cannot overwrite same-SSRC restart statistics");
}
static void workerAndUrgent() {
    auto receiver = std::make_shared<Receiver>(); receiver->setDeferredReports(true);
    stream::RtcpFeedbackWorker worker;
    assert(worker.addSession(receiver)); assert(worker.addSession(receiver));
    std::mutex mutex; std::condition_variable changed;
    bool entered = false, release = false;
    unsigned urgent = 0;
    const rtc::message_callback send = [&](rtc::message_ptr p) {
        const auto *header = reinterpret_cast<const rtc::RtcpHeader *>(p->data());
        std::unique_lock lock(mutex);
        if (header->payloadType() == 206 && header->reportCount() == 1) { ++urgent; return; }
        entered = true; changed.notify_all(); changed.wait(lock, [&] { return release; });
    };
    feed(*receiver, rtp(1), send); feed(*receiver, sr(), send); worker.start();
    { std::unique_lock lock(mutex); assert(changed.wait_for(lock, 2s, [&] { return entered; })); }
    auto input = std::async(std::launch::async, [&] {
        feed(*receiver, rtp(2), send); feed(*receiver, sr(), send);
        receiver->requestKeyframe(send); worker.requestStop();
    });
    assert(input.wait_for(100ms) == std::future_status::ready); input.get();
    assert(urgent == 1 && !worker.quiescent());
    assert(!receiver->getFeedbackDiagnostics().pending);
    auto rejected = std::make_shared<Receiver>(); rejected->setDeferredReports(true);
    assert(!worker.addSession(rejected));
    assert(rejected->getFeedbackDiagnostics().stopped);
    { std::lock_guard lock(mutex); release = true; changed.notify_all(); }
    worker.join(); assert(worker.quiescent());
    auto stats = worker.snapshot();
    assert(stats.sessions == 1 && stats.stopped && !stats.inFlight && !stats.running);
    std::puts("PASS blocked regular send leaves reception/PLI/stop responsive; join deferred until cleanup");
}
static void compoundAndMalformed() {
    Receiver receiver; receiver.setDeferredReports(true);
    std::vector<rtc::message_ptr> sent;
    const rtc::message_callback send = [&](rtc::message_ptr p) { sent.push_back(std::move(p)); };
    auto message = rtc::make_message(rtc::RtcpRr::SizeWithReportBlocks(0), rtc::Message::Control);
    reinterpret_cast<rtc::RtcpRr *>(message->data())->preparePacket(77, 0);
    auto sender = sr(); message->insert(message->end(), sender->begin(), sender->end());
    feed(receiver, message, send); assert(receiver.flushPendingReports(0ms));
    assert(receiver.getFeedbackDiagnostics().senderReports == 1);
    for (unsigned size = 1; size < 28; ++size) {
        auto malformed = rtc::make_message(size, rtc::Message::Control);
        std::memset(malformed->data(), 0xff, size); feed(receiver, malformed, send);
    }
    assert(receiver.getFeedbackDiagnostics().malformedControl == 27);
    std::puts("PASS compound RTCP routing and truncated control bounds");
}
static void concurrentFlushAndIdleStop() {
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        stream::RtcpFeedbackWorker idle;
        idle.start(); idle.requestStop();
        assert(idle.quiescent()); idle.join();
    }
    stream::RtcpFeedbackWorker limited;
    auto first = std::make_shared<Receiver>(), second = std::make_shared<Receiver>(), third = std::make_shared<Receiver>();
    first->setDeferredReports(true); second->setDeferredReports(true); third->setDeferredReports(true);
    assert(limited.addSession(first) && limited.addSession(second) && !limited.addSession(third));
    assert(third->getFeedbackDiagnostics().stopped);
    assert(limited.snapshot().rejectedSessions == 1);
    limited.requestStop(); assert(limited.quiescent());

    Receiver receiver; receiver.setDeferredReports(true);
    std::atomic<bool> done{false};
    std::atomic<unsigned> sends{0};
    const rtc::message_callback send = [&](rtc::message_ptr) { ++sends; };
    auto flushing = std::async(std::launch::async, [&] {
        while (!done) receiver.flushPendingReports(0ms);
    });
    for (unsigned i = 50000; i < 150000; ++i) {
        feed(receiver, rtp(i), send);
        if (i % 10 == 0) feed(receiver, sr(), send);
    }
    done = true; flushing.get();
    std::vector<rtc::message_ptr> final;
    const rtc::message_callback capture = [&](rtc::message_ptr p) { final.push_back(std::move(p)); };
    feed(receiver, sr(), capture); assert(receiver.flushPendingReports(0ms));
    assert(rr(final.back()).getReportBlock(0)->extendedHighestSeqNo() == 149999);
    assert(rr(final.back()).getReportBlock(0)->getPacketsLostCount() == 0);
    assert(sends > 0 && receiver.getFeedbackDiagnostics().rtpPackets == 100000);
    std::puts("PASS concurrent reception/flush across wrap; idle-stop quiescence; two-track capacity");
}
int main() {
    correctness(); coalescing(); failedEmission(); restartDuringFlush(); workerAndUrgent(); compoundAndMalformed(); concurrentFlushAndIdleStop();
    std::puts("PASS RTCP feedback regressions");
}
