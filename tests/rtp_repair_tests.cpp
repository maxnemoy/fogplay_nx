#include "features/streaming/domain/rtp_repair.hpp"
#include "features/streaming/domain/h264_rtp.hpp"
#include <cassert>
#include <iostream>
using Bytes = std::vector<std::byte>;
static Bytes packet(unsigned seq, unsigned pt = 96, unsigned source = 1) {
    return {std::byte{0x80}, std::byte(128 | pt), std::byte(seq >> 8), std::byte(seq),
        std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{0},std::byte{0},std::byte{0},std::byte(source),std::byte{0x61},std::byte{0x22}};
}
using A = stream::H264RtpAssembler;
static std::uint64_t textMetric(const std::string& text, const std::string& name) {
    const auto start = text.find(name + "=");
    assert(start != std::string::npos);
    return std::stoull(text.substr(start + name.size() + 1));
}
static std::uint64_t metric(const A& a, const std::string& name) { return textMetric(a.diagnostics(), name); }
static std::uint64_t gapMetric(const A& a, const std::string& name) { return textMetric(a.gapDiagnostics(), name); }
static Bytes mediaPacket(unsigned seq, unsigned ts, bool marker, std::initializer_list<unsigned> body) {
    auto p = packet(seq); p[1] = std::byte((marker ? 128 : 0) | 96);
    p[4] = std::byte(ts >> 24); p[5] = std::byte(ts >> 16); p[6] = std::byte(ts >> 8); p[7] = std::byte(ts);
    p.resize(12); for (auto value : body) p.push_back(std::byte(value)); return p;
}
static void feed(A& a, unsigned seq, unsigned ts, bool marker, std::initializer_list<unsigned> body, std::uint64_t now) {
    const auto p = mediaPacket(seq, ts, marker, body); a.receive(p.data(), p.size(), now);
}
static void initialize(A& a) {
    a.enableNack(true); feed(a,98,100,true,{0x61,0x11},0); feed(a,99,200,true,{0x65,0x22},1);
    A::Frame f; assert(a.poll(f) && !a.waiting());
}
static void fragmentGap(A& a) {
    initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2); feed(a,102,300,true,{0x7c,0x41,0x33},3);
    assert(a.takeNacks(23) == std::vector<std::uint16_t>{101});
}
static void adaptiveRepairTests() {
    {
        A a; fragmentGap(a);
        feed(a,101,300,false,{0x7c,0x01,0x22},163); // First NACK -> arrival = 140 ms, beyond the old 120-ms gap budget.
        A::Frame f; assert(a.poll(f) && f.timestamp == 300 && f.firstSequence == 100 && f.lastSequence == 102);
        assert(a.stats().resyncs == 0 && a.stats().gaps == 0 && metric(a,"repairedPackets") == 1);
        assert(metric(a,"nackArrivalSamples") == 1 && metric(a,"nackFirstArrivalLastMs") == 140);
        assert(metric(a,"repairWindowMs") == A::MaxRepairMs && metric(a,"nackLateSamples") == 0);
    }
    {
        A a; fragmentGap(a); a.takeNacks(113); // Retry makes the responsible send ambiguous.
        feed(a,101,300,false,{0x7c,0x01,0x22},184);
        assert(a.stats().gaps == 1 && a.stats().resyncs == 1 && a.waiting()); A::Frame f; assert(!a.poll(f));
        assert(metric(a,"nackArrivalSamples") == 1 && metric(a,"nackLateSamples") == 1 && metric(a,"nackRetrySamples") == 1);
        assert(metric(a,"nackFirstArrivalLastMs") == 161 && metric(a,"repairWindowMs") == A::MaxRepairMs);
        feed(a,101,300,false,{0x7c,0x01,0x22},185); assert(metric(a,"nackArrivalSamples") == 1);
        // The late sample trains a future gap; it never revives the damaged AU.
        feed(a,103,400,true,{0x65,0x44},186); assert(a.poll(f));
        feed(a,104,500,false,{0x7c,0x81,0x11},187); feed(a,106,500,true,{0x7c,0x41,0x33},188);
        assert(a.takeNacks(208) == std::vector<std::uint16_t>{105});
        feed(a,105,500,false,{0x7c,0x01,0x22},400); assert(a.poll(f) && f.timestamp == 500);
        assert(a.stats().resyncs == 1 && a.stats().gaps == 1 && metric(a,"nackArrivalSamples") == 2);
    }
    {
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        feed(a,102,300,false,{0x7c,0x01,0x22},3); feed(a,104,300,true,{0x7c,0x41,0x44},4);
        assert(a.takeNacks(23) == (std::vector<std::uint16_t>{101,103}));
        feed(a,101,300,false,{0x7c,0x01,0x11},30);
        assert(metric(a,"repairWindowMs") == 170 && metric(a,"gapRepairWindowMs") == 180);
        a.tick(183); assert(a.stats().gaps == 0); a.tick(184); assert(a.stats().gaps == 1 && a.waiting());
    }
    {
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        feed(a,104,300,true,{0x7c,0x41,0x44},3); assert(a.takeNacks(23).size() == 3);
        for (unsigned seq = 101; seq <= 103; ++seq) feed(a,seq,300,false,{0x7c,0x01,0x22},seq - 71);
        assert(metric(a,"nackArrivalSamples") == 3 && metric(a,"repairWindowMs") == 170);
        A::Frame f; assert(a.poll(f));
        feed(a,105,400,false,{0x7c,0x81,0x11},1050); feed(a,107,400,true,{0x7c,0x41,0x33},1051);
        assert(a.takeNacks(1071).size() == 1); feed(a,106,400,false,{0x7c,0x01,0x22},1080);
        assert(a.poll(f) && metric(a,"repairWindowMs") == 160);
    }
    {
        A a; fragmentGap(a); a.tick(183); // Preserve the first gap's outstanding request for late learning.
        feed(a,103,400,true,{0x65,0x44},184); A::Frame f; assert(a.poll(f));
        feed(a,104,500,false,{0x7c,0x81,0x11},185); feed(a,106,500,true,{0x7c,0x41,0x33},186);
        assert(a.takeNacks(206) == std::vector<std::uint16_t>{105});
        feed(a,101,300,false,{0x7c,0x01,0x22},230);
        assert(metric(a,"repairWindowMs") == A::MaxRepairMs && metric(a,"gapRepairWindowMs") == A::RepairMs);
        a.tick(365); assert(a.stats().gaps == 1); a.tick(366); assert(a.stats().gaps == 2 && a.waiting());
    }
    {
        A a; fragmentGap(a);
        for (unsigned now = 24; now < 400; ++now) { a.takeNacks(now); a.tick(now); }
        assert(metric(a,"nackPackets") <= 3 && a.stats().gaps == 1 && a.waiting());
        assert(a.requestedSequence(101,200) && !a.requestedSequence(101,2024));
        a.tick(2056); assert(metric(a,"nackHistory") == 0);
        feed(a,101,300,false,{0x7c,0x01,0x22},2057); assert(metric(a,"nackArrivalSamples") == 0);
    }
    {
        A a; a.enableNack(true); feed(a,65534,100,true,{0x61,0x11},0); feed(a,0,200,true,{0x65,0x22},1);
        assert(a.takeNacks(21) == std::vector<std::uint16_t>{65535}); a.tick(181);
        assert(a.requestedSequence(65535,182) && !a.requestedSequence(65534,182));
        feed(a,65535,150,true,{0x61,0x33},182);
        assert(metric(a,"nackLateSamples") == 1 && metric(a,"nackFirstArrivalLastMs") == 161 && a.stats().gaps == 1);
    }
    {
        A a; fragmentGap(a); auto p = mediaPacket(101,300,false,{0x7c,0x01,0x22});
        p[0] = std::byte{0}; a.receive(p.data(),p.size(),50); assert(metric(a,"nackArrivalSamples") == 0);
        p = mediaPacket(101,300,false,{0x7c,0x01,0x22}); p[11] = std::byte{2}; a.receive(p.data(),p.size(),60);
        assert(a.stats().foreignSsrc == 1 && metric(a,"nackArrivalSamples") == 0);
        p = mediaPacket(101,300,false,{0}); a.receive(p.data(),p.size(),70);
        assert(metric(a,"nackArrivalSamples") == 0 && metric(a,"repairedPackets") == 0);
    }
    {
        A a; initialize(a);
        for (unsigned seq = 101; seq <= 112; ++seq) feed(a,seq,seq * 10,true,{0x61,0x11},seq - 100);
        assert(a.takeNacks(21) == std::vector<std::uint16_t>{100});
        feed(a,100,1000,true,{0x61,0x22},161);
        unsigned frames = 0; std::uint32_t previous = 0; A::Frame f;
        for (unsigned cycle = 0; cycle < 10; ++cycle) {
            unsigned batch = 0;
            while (a.poll(f)) { assert(f.timestamp > previous); previous = f.timestamp; ++frames; ++batch; }
            assert(batch <= 2 && a.takeNacks(162 + cycle).empty()); a.tick(162 + cycle);
        }
        assert(frames == 13 && a.pendingPackets() == 0 && a.stats().gaps == 0 && a.stats().resyncs == 0 && a.stats().pressure == 0);
        assert(metric(a,"catchupYields") >= 5);
    }
    {
        A a; initialize(a); auto p = mediaPacket(100,300,true,{0x61,0x33}); a.receive(p.data(),p.size(),2,0);
        A::Frame f; assert(!a.poll(f) && a.pendingPackets() == 1 && a.takeNacks(100).empty());
        a.tick(100,0); assert(!a.poll(f) && a.stats().gaps == 0);
        a.tick(101,1); assert(a.poll(f) && f.timestamp == 300 && a.pendingPackets() == 0 && a.stats().pressure == 0);
    }
    {
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        for (unsigned seq = 102; seq <= 240; ++seq) feed(a,seq,300,seq == 240,{0x7c,unsigned(seq == 240 ? 0x41 : 0x01),0x33},3);
        assert(a.takeNacks(23) == std::vector<std::uint16_t>{101});
        feed(a,101,300,false,{0x7c,0x01,0x22},163); assert(a.pendingPackets() > 0);
        A::Frame f; assert(!a.poll(f) && a.takeNacks(164).empty()); a.tick(164); assert(a.poll(f));
        assert(f.firstSequence == 100 && f.lastSequence == 240 && a.stats().resyncs == 0 && a.stats().timeout == 0);
    }
    {
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        feed(a,102,300,false,{0x7c,0x01,0x33},3); a.takeNacks(23);
        feed(a,101,300,false,{0x7c,0x01,0x22},163);
        // Resolving a gap and starting another inside the same AU cannot reset its global age.
        feed(a,104,300,false,{0x7c,0x01,0x44},164); assert(a.takeNacks(184).size() == 1);
        feed(a,103,300,false,{0x7c,0x01,0x33},300); feed(a,106,300,false,{0x7c,0x01,0x66},301);
        a.tick(301); assert(a.stats().timeout == 0);
        a.tick(302); assert(a.stats().timeout == 1 && a.waiting());
    }
    {
        A a; a.enableNack(true); auto p = packet(1); a.receive(p.data(),p.size(),0);
        for (unsigned i = 0; i < 4; ++i) {
            p = packet(258 + i * 300); a.receive(p.data(),p.size(),i * 200 + 1);
            assert(a.takeNacks(i * 200 + 21).size() == 256); a.tick(i * 200 + 181);
            assert(metric(a,"nackHistory") <= 512);
        }
        assert(metric(a,"nackHistoryEvicted") >= 512); a.tick(3000); assert(metric(a,"nackHistory") == 0);
    }
    {
        // No-FID RTX can learn late response timing using the bounded requested-sequence predicate.
        A a; fragmentGap(a); a.tick(183);
        stream::RtpRepairConfig cfg; cfg.media[96] = true; cfg.rtx[97] = 96;
        stream::RtpRepair repair(cfg); auto primary = packet(99); assert(repair.normalize(primary,[](auto){ return false; }));
        auto late = mediaPacket(400,300,false,{0x7c,0x01,0x22}); late[1] = std::byte{97}; late[11] = std::byte{2};
        late.insert(late.begin() + 12,{std::byte{0},std::byte{101}});
        assert(!a.missingSequence(101) && a.requestedSequence(101,184));
        assert(repair.normalize(late,[&](auto seq){ return a.missingSequence(seq) || a.requestedSequence(seq,184); }));
        a.receive(late.data(),late.size(),184);
        assert(metric(a,"nackLateSamples") == 1 && metric(a,"nackFirstArrivalLastMs") == 161 && a.waiting());
        A::Frame f; assert(!a.poll(f));
    }
}
static void padding(A& a, unsigned seq, std::uint64_t now, std::size_t budget = 2) {
    auto p = packet(seq); p.resize(13); p[0] = std::byte{0xa0}; p[12] = std::byte{1};
    a.receive(p.data(), p.size(), now, budget);
}
static void frozenDeadlineTests() {
    {
        // Production repro: resolving the first hole at171 must not retroactively
        // change the remaining hole's original t0+180 deadline to t0+170.
        A a; a.enableNack(true); padding(a,99,0); padding(a,102,0);
        assert(a.takeNacks(170) == (std::vector<std::uint16_t>{100,101}));
        feed(a,100,100,true,{0x61,0x11},171);
        assert(metric(a,"repairWindowMs") == 170 && metric(a,"gapRepairWindowMs") == 180);
        feed(a,101,200,true,{0x65,0x22},172);
        A::Frame f; assert(a.poll(f) && f.timestamp == 200);
        assert(a.stats().gaps == 0 && a.stats().late == 0 && metric(a,"repairedPackets") == 2);
        // Independently new evidence uses the reduced estimate.
        padding(a,104,173); a.tick(342); assert(a.stats().gaps == 0);
        a.tick(343); assert(a.stats().gaps == 1 && gapMetric(a,"lastBudgetMs") == 170);
    }
    {
        A a; a.enableNack(true); padding(a,99,0); padding(a,102,0); padding(a,104,1);
        assert(a.takeNacks(20) == (std::vector<std::uint16_t>{100,101,103}));
        feed(a,100,100,true,{0x61,0x11},161);
        assert(metric(a,"repairWindowMs") == 220 && metric(a,"gapRepairWindowMs") == 180);
        feed(a,101,200,true,{0x65,0x22},179); A::Frame f; assert(a.poll(f));
        assert(metric(a,"gapRepairWindowMs") == 180);
        a.tick(180); assert(a.stats().gaps == 0); a.tick(181);
        assert(a.stats().gaps == 1 && gapMetric(a,"lastDeadlineMs") == 181);
        // Independently new evidence uses the increased estimate.
        padding(a,106,182); a.tick(401); assert(a.stats().gaps == 1);
        a.tick(402); assert(a.stats().gaps == 2 && gapMetric(a,"lastBudgetMs") == 220);
    }
    for (const auto newerMs : {30u,40u}) {
        // Oldest arrival can have a higher sequence; ties must preserve acceptance
        // order when an adaptive update and the next packet share a millisecond.
        A a; a.enableNack(true); padding(a,99,0); padding(a,102,0); a.takeNacks(20);
        padding(a,105,30); feed(a,100,100,true,{0x61,0x11},30);
        padding(a,104,newerMs); feed(a,101,200,true,{0x65,0x22},newerMs + 1);
        A::Frame f; assert(a.poll(f));
        assert(metric(a,"gapRepairWindowMs") == 180);
        a.tick(209); assert(a.stats().gaps == 0); a.tick(210);
        assert(a.stats().gaps == 1 && gapMetric(a,"lastOldestEvidenceMs") == 30);
        assert(gapMetric(a,"lastDetectedMs") == newerMs + 1 && gapMetric(a,"lastDeadlineMs") == 210);
    }
    {
        // A zero frame budget can hide the next hole until the consumer resumes.
        A a; a.enableNack(true); padding(a,99,0); padding(a,103,0); a.takeNacks(20);
        auto p = mediaPacket(100,100,true,{0x61,0x11}); a.receive(p.data(),p.size(),30,0);
        p = mediaPacket(101,200,true,{0x65,0x22}); a.receive(p.data(),p.size(),31,0);
        a.tick(179,0); assert(a.stats().gaps == 0 && a.pendingPackets() == 3);
        a.tick(179,1); assert(metric(a,"gapRepairWindowMs") == 180);
        a.tick(180); assert(a.stats().gaps == 1 && gapMetric(a,"lastDeadlineMs") == 180);
    }
    {
        // The 128-packet catch-up yield must not reset or reduce a newly exposed
        // hole's bound when its evidence has already spent179ms in the buffer.
        A a; a.enableNack(true); padding(a,0,0); padding(a,131,0);
        for (unsigned seq = 2; seq <= 129; ++seq) padding(a,seq,0);
        assert(a.takeNacks(20) == (std::vector<std::uint16_t>{1,130}));
        feed(a,1,100,true,{0x61,0x11},30);
        assert(a.pendingPackets() == 2 && metric(a,"catchupYields") >= 1);
        a.tick(179); assert(a.pendingPackets() == 1 && metric(a,"gapRepairWindowMs") == 180);
        a.tick(180); assert(a.stats().gaps == 1 && gapMetric(a,"lastDetectedMs") == 179);
    }
    {
        A a; a.enableNack(true); padding(a,65534,0); padding(a,1,0); a.takeNacks(170);
        feed(a,65535,100,true,{0x61,0x11},171); feed(a,0,200,true,{0x65,0x22},172);
        assert(a.stats().gaps == 0 && a.stats().late == 0 && a.pendingPackets() == 0);
    }
}
static void gapDiagnosticTests() {
    {
        A a; fragmentGap(a); assert(a.takeNacks(113).size() == 1);
        a.tick(190);
        assert(gapMetric(a,"expiredGaps") == 1 && gapMetric(a,"expiredPackets") == 1);
        assert(gapMetric(a,"expiredNoNackGaps") == 0 && gapMetric(a,"expiredUnrequestedPackets") == 0);
        assert(gapMetric(a,"expiredNackAttempts") == 2 && gapMetric(a,"lastNackAttempts") == 2);
        assert(gapMetric(a,"lastFirst") == 101 && gapMetric(a,"lastLast") == 101);
        assert(gapMetric(a,"lastDetectedMs") == 3 && gapMetric(a,"lastOldestEvidenceMs") == 3);
        assert(gapMetric(a,"lastBudgetMs") == 180 && gapMetric(a,"lastDeadlineMs") == 183);
        assert(gapMetric(a,"lastExpiredMs") == 190 && gapMetric(a,"maxExpiryOvershootMs") == 7);
        assert(gapMetric(a,"lastFirstNackMs") == 23 && gapMetric(a,"lastLastNackMs") == 113);
        assert(gapMetric(a,"lastPendingFirst") == 102 && gapMetric(a,"lastPendingLast") == 102 && gapMetric(a,"lastPendingDepth") == 1);
        feed(a,101,300,false,{0x7c,0x01,0x22},191);
        assert(gapMetric(a,"matchedLatePackets") == 1 && gapMetric(a,"lateByMs") == 8);
        assert(gapMetric(a,"lateSeq") == 101 && gapMetric(a,"lateDeadlineMs") == 183 && gapMetric(a,"lateNackAttempts") == 2);
        feed(a,101,300,false,{0x7c,0x01,0x22},192); assert(gapMetric(a,"matchedLatePackets") == 1);
        // Retained summaries survive event ring overflow and history expiry.
        for (unsigned i = 0; i < 40; ++i) a.requestResync(200 + i);
        a.tick(3000); assert(a.stats().eventOverflow > 0);
        assert(metric(a,"expiredGaps") == 1 && metric(a,"lastGapDeadlineMs") == 183 && gapMetric(a,"lateByMs") == 8);
    }
    {
        A a; padding(a,9,0); padding(a,12,1); a.tick(21);
        assert(gapMetric(a,"expiredNoNackGaps") == 1 && gapMetric(a,"expiredUnrequestedPackets") == 2);
        assert(gapMetric(a,"expiredPackets") == 2 && gapMetric(a,"lastBudgetMs") == 20);
        assert(gapMetric(a,"lastRequestedPackets") == 0 && gapMetric(a,"lastFirstNackMs") == 0 && gapMetric(a,"lastLastNackMs") == 0);
    }
    {
        // NACK enabled but the pump never requested; this is distinct from a late
        // response to an actually generated NACK attempt.
        A a; a.enableNack(true); padding(a,9,0); padding(a,11,1); a.tick(181);
        feed(a,10,100,true,{0x61,0x11},182);
        assert(gapMetric(a,"expiredNoNackGaps") == 1 && gapMetric(a,"matchedLatePackets") == 0);
    }
    {
        // Exact deadline arrival is late even if no previous tick expired it.
        A a; a.enableNack(true); padding(a,9,0); padding(a,11,1); a.takeNacks(21);
        feed(a,10,100,true,{0x61,0x11},181);
        assert(a.stats().gaps == 1 && gapMetric(a,"matchedLatePackets") == 1 && gapMetric(a,"lateByMs") == 0);
    }
    for (const bool resync : {false,true}) {
        A a; fragmentGap(a);
        if (resync) a.requestResync(30);
        else padding(a,5000,30); // Pressure retires requests without a gap expiry.
        feed(a,101,300,false,{0x7c,0x01,0x22},40);
        assert(metric(a,"nackLateSamples") == 1 && gapMetric(a,"expiredGaps") == 0 && gapMetric(a,"matchedLatePackets") == 0);
        padding(a,resync ? 104 : 5002,41); a.tick(221);
        assert(gapMetric(a,"expiredGaps") == 1 && gapMetric(a,"lastOldestEvidenceMs") == 41);
    }
    {
        // An AU timeout retires requests while its gap remains active. A later
        // NACK scan can create another record: count one packet and both attempts.
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        feed(a,102,300,true,{0x7c,0x41,0x33},150);
        assert(a.takeNacks(170) == std::vector<std::uint16_t>{101});
        a.tick(302); assert(a.stats().timeout == 1 && a.stats().gaps == 0);
        assert(a.takeNacks(303) == std::vector<std::uint16_t>{101});
        a.tick(330);
        assert(gapMetric(a,"lastRequestedPackets") == 1 && gapMetric(a,"expiredUnrequestedPackets") == 0);
        assert(gapMetric(a,"lastFirstNackMs") == 170 && gapMetric(a,"lastLastNackMs") == 303 && gapMetric(a,"lastNackAttempts") == 2);
        feed(a,101,300,false,{0x7c,0x01,0x22},331);
        assert(gapMetric(a,"matchedLatePackets") == 1 && gapMetric(a,"lateFirstNackMs") == 170 && gapMetric(a,"lateNackAttempts") == 2);
    }
    {
        // Multiple independent AU damage events may retire successive records
        // for one still-missing sequence. Preserve generated diagnostic evidence.
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        feed(a,102,300,true,{0x7c,0x41,0x33},150); a.takeNacks(170); a.tick(302);
        a.takeNacks(303); auto malformed = packet(101); malformed[0] = std::byte{0};
        a.receive(malformed.data(),malformed.size(),304); a.tick(330);
        assert(gapMetric(a,"lastRequestedPackets") == 1 && gapMetric(a,"lastNackAttempts") == 2);
        assert(gapMetric(a,"lastFirstNackMs") == 170 && gapMetric(a,"lastLastNackMs") == 303);
        feed(a,101,300,false,{0x7c,0x01,0x22},331);
        assert(gapMetric(a,"matchedLatePackets") == 1 && gapMetric(a,"lateFirstNackMs") == 170 && gapMetric(a,"lateNackAttempts") == 2);
        // The arrival estimator continues using the baseline last request record.
        assert(metric(a,"nackFirstArrivalLastMs") == 28 && metric(a,"nackRetrySamples") == 0);
    }
    {
        // A delayed pump beyond the correlation TTL must snapshot the original
        // generated request before pruning the history retired by an AU timeout.
        A a; initialize(a); feed(a,100,300,false,{0x7c,0x81,0x11},2);
        feed(a,102,300,true,{0x7c,0x41,0x33},150); a.takeNacks(170); a.tick(302);
        assert(a.takeNacks(3000).empty()); // Actual pump order scans before tick.
        a.tick(3000);
        assert(gapMetric(a,"expiredNoNackGaps") == 0 && gapMetric(a,"expiredUnrequestedPackets") == 0);
        assert(gapMetric(a,"lastFirstNackMs") == 170 && gapMetric(a,"expiredNackAttempts") == 1);
        assert(gapMetric(a,"lastOvershootMs") == 2670);
    }
    {
        // The expiry range can be larger than a single capped NACK scan.
        A a; a.enableNack(true); padding(a,0,0); padding(a,300,1);
        assert(a.takeNacks(21).size() == 256); a.tick(181);
        assert(gapMetric(a,"expiredPackets") == 299 && gapMetric(a,"lastRequestedPackets") == 256);
        assert(gapMetric(a,"expiredUnrequestedPackets") == 43 && gapMetric(a,"expiredNoNackGaps") == 0);
    }
}
int main() {
    stream::RtpRepairConfig config; config.media[96] = true; config.rtx[97] = 96;
    config.mediaSsrc = 1; config.rtxSsrc = 2;
    stream::RtpRepair repair(config);
    auto rtx = packet(77,97,2); rtx.insert(rtx.begin()+12,{std::byte{0xff},std::byte{0xff}});
    assert(repair.normalize(rtx,[](auto){return false;}));
    assert(rtx == packet(65535));
    Bytes primarySr(28); primarySr[0]=std::byte{0x80};primarySr[1]=std::byte{200};primarySr[3]=std::byte{6};primarySr[7]=std::byte{1};
    assert(repair.acceptControl(primarySr));auto rtxSr=primarySr;rtxSr[7]=std::byte{2};assert(!repair.acceptControl(rtxSr));
    rtxSr.insert(rtxSr.end(),primarySr.begin(),primarySr.end());assert(!repair.acceptControl(rtxSr));
    auto primaryRr=primarySr;primaryRr.resize(8);primaryRr[1]=std::byte{201};primaryRr[3]=std::byte{1};
    assert(repair.acceptControl(primaryRr));primaryRr[7]=std::byte{2};assert(!repair.acceptControl(primaryRr));
    primarySr[3]=std::byte{255};assert(!repair.acceptControl(primarySr));
    auto nack = repair.nack({65535,0,2,17});
    assert(nack.size()==20 && nack[0]==std::byte{0x81} && nack[1]==std::byte{205} && nack[3]==std::byte{4});
    assert(nack[11]==std::byte{1} && nack[12]==std::byte{0xff} && nack[13]==std::byte{0xff});
    assert(nack[14]==std::byte{0} && nack[15]==std::byte{5} && nack[17]==std::byte{17});
    auto foreign=packet(1,97,3); foreign.insert(foreign.begin()+12,{std::byte{0},std::byte{1}});
    assert(!repair.normalize(foreign,[](auto){return true;}));
    for (auto broken : {Bytes{}, Bytes(11), packet(1,99)}) assert(!repair.normalize(broken,[](auto){return true;}));
    auto shortRtx=packet(1,97,2); shortRtx.resize(14); assert(!repair.normalize(shortRtx,[](auto){return true;}));
    auto extension=packet(5,97,2); extension[0]=std::byte{0xb1};
    extension.insert(extension.begin()+12,{std::byte{0},std::byte{0},std::byte{0},std::byte{9}, // CSRC
        std::byte{0xbe},std::byte{0xde},std::byte{0},std::byte{1},std::byte{1},std::byte{2},std::byte{3},std::byte{4},
        std::byte{0},std::byte{10}});
    extension.insert(extension.end(),{std::byte{0},std::byte{2}});
    assert(repair.normalize(extension,[](auto){return true;}));
    assert(extension.size()==26 && extension[0]==std::byte{0x91} && extension[3]==std::byte{10});
    config.mediaSsrc=0;config.rtxSsrc=0;stream::RtpRepair learned(config);
    auto unsolicited=packet(1,97,2); unsolicited.insert(unsolicited.begin()+12,{std::byte{0},std::byte{5}});
    assert(!learned.normalize(unsolicited,[](auto){return true;}));
    auto original=packet(4); assert(learned.normalize(original,[](auto){return false;}));
    assert(!learned.normalize(unsolicited,[](auto){return false;}));
    assert(learned.normalize(unsolicited,[](auto seq){return seq==5;}));
    A a; a.enableNack(true);
    auto p=packet(65534);a.receive(p.data(),p.size(),0);p=packet(0);a.receive(p.data(),p.size(),1);
    assert(a.takeNacks(20).empty()); assert(a.takeNacks(21)==std::vector<std::uint16_t>{65535});
    assert(a.takeNacks(22).empty()); assert(a.takeNacks(51).empty());assert(a.takeNacks(81).empty());assert(a.takeNacks(111).size()==1);
    p=packet(65535);a.receive(p.data(),p.size(),115);a.tick(122);
    assert(a.stats().gaps==0 && a.stats().faultSerial==0 && a.diagnostics().find("repairedPackets=1")!=std::string::npos);
    p=packet(2);a.receive(p.data(),p.size(),125);a.tick(304);assert(a.stats().gaps==0);a.tick(305);assert(a.stats().gaps==1 && a.waiting());
    // Recover an interior FU fragment without dropping or resetting the reference chain.
    A fragmented; fragmented.enableNack(true);
    auto feed=[&](unsigned seq,unsigned ts,bool marker,std::initializer_list<unsigned> body,std::uint64_t now) {
        auto value=packet(seq);value[1]=std::byte((marker?128:0)|96);value[7]=std::byte(ts);
        value.resize(12);for(auto b:body)value.push_back(std::byte(b));fragmented.receive(value.data(),value.size(),now);
    };
    feed(98,1,true,{0x61,0x11},0);feed(99,2,true,{0x65,0x22},1);A::Frame complete;assert(fragmented.poll(complete));
    feed(100,3,false,{0x7c,0x81,0x11},2);feed(102,3,true,{0x7c,0x41,0x33},3);
    assert(fragmented.takeNacks(23)==std::vector<std::uint16_t>{101});
    feed(101,3,false,{0x7c,0x01,0x22},70);assert(fragmented.poll(complete));
    assert(complete.data==(Bytes{std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{0x61},std::byte{0x11},std::byte{0x22},std::byte{0x33}}));
    assert(fragmented.stats().faultSerial==0 && fragmented.stats().resyncs==0);
    A deadline; deadline.enableNack(true); p=packet(1);deadline.receive(p.data(),p.size(),0);
    p=packet(3);deadline.receive(p.data(),p.size(),1);assert(deadline.takeNacks(21).size()==1);
    p=packet(2);deadline.receive(p.data(),p.size(),181);
    assert(deadline.stats().gaps==1 && deadline.stats().late>=1 && deadline.stats().faultSerial==1);
    A unsupported;p=packet(1);unsupported.receive(p.data(),p.size(),0);p=packet(3);unsupported.receive(p.data(),p.size(),1);
    assert(unsupported.takeNacks(21).empty());unsupported.tick(21);assert(unsupported.stats().gaps==1);
    adaptiveRepairTests(); frozenDeadlineTests(); gapDiagnosticTests();
    std::cout<<"RTP repair: RTX/FID/OSN, adaptive finite NACK deadlines, correlated/late timing, bounded history, paced retries, bounded burst catch-up and no-support fallback passed\n";
}
