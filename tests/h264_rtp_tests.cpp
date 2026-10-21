#include "features/streaming/domain/h264_rtp.hpp"
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <vector>
using A = stream::H264RtpAssembler;
using Bytes = std::vector<std::byte>;
static Bytes rtp(unsigned seq, unsigned ts, bool marker, std::initializer_list<unsigned> payload) {
    Bytes p(12);
    p[0] = std::byte{0x80}; p[1] = std::byte(marker ? 0xe0 : 0x60);
    p[2] = std::byte(seq >> 8); p[3] = std::byte(seq);
    p[4] = std::byte(ts >> 24); p[5] = std::byte(ts >> 16);
    p[6] = std::byte(ts >> 8); p[7] = std::byte(ts); p[11] = std::byte{1};
    for (auto value : payload) p.push_back(std::byte(value));
    return p;
}
static void feed(A& a, unsigned seq, unsigned ts, bool marker,
                 std::initializer_list<unsigned> payload, std::uint64_t now = 0) {
    const auto p = rtp(seq, ts, marker, payload); a.receive(p.data(), p.size(), now);
}
static void init(A& a, unsigned seq = 98) {
    feed(a,seq,100,true,{0x61,0x22});
    feed(a,seq+1,200,true,{0x65,0x11});
    A::Frame f; assert(a.poll(f)); assert(f.timestamp == 200); assert(!a.waiting());
}
static void empty(A& a) { A::Frame f; assert(!a.poll(f)); }
static void recover(A& a, unsigned seq, unsigned ts = 1000) {
    feed(a, seq, ts, true, {0x65,0x44});
    A::Frame f; assert(a.poll(f)); assert(f.timestamp == ts); assert(!a.waiting());
}
static void padding(A& a, unsigned seq, unsigned ts, bool marker, std::uint64_t now = 0) {
    auto p = rtp(seq, ts, marker, {}); p[0] = std::byte{0xa0};
    p.insert(p.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4}});
    a.receive(p.data(), p.size(), now);
}
int main() {
    {
        A a; init(a);
        const auto faults = a.stats().faultSerial;
        padding(a,100,200,true); // Repeated old timestamp/marker must not reopen an AU.
        feed(a,101,300,true,{0x61,0x44},1);
        A::Frame f; assert(a.poll(f) && f.firstSequence == 101 && !a.waiting());
        assert(a.stats().faultSerial == faults && a.stats().padding == 1 && a.stats().gaps == 0);
    }
    {
        A a; init(a);
        feed(a,100,300,false,{0x7c,0x81,0x11});
        feed(a,102,300,true,{0x7c,0x41,0x22},1); // Padding may be reordered inside a FU.
        padding(a,101,999,true,2); // Marker and unrelated timestamp are ignored.
        A::Frame f; assert(a.poll(f) && f.data.size() == 7 && f.firstSequence == 100 && f.lastSequence == 102);
        assert(!a.waiting() && a.stats().gaps == 0 && a.stats().malformed == 0 && a.stats().padding == 1);
        padding(a,101,999,true,3); assert(a.stats().duplicate == 1);
    }
    {
        A a; padding(a,65535,888,true); // Padding is no startup AU boundary.
        feed(a,0,100,true,{0x61,0x11}); empty(a);
        feed(a,1,200,true,{0x65,0x22}); A::Frame f; assert(a.poll(f) && !a.waiting());
        assert(a.stats().padding == 1 && a.stats().malformed == 0 && a.stats().gaps == 0);
    }
    {
        A a; init(a); feed(a,100,300,false,{0x7c,0x81,0x11});
        auto foreign = rtp(4000,999,true,{0x65,0x22}); foreign[11] = std::byte{2};
        a.receive(foreign.data(),foreign.size(),1);
        feed(a,101,300,true,{0x7c,0x41,0x22},2);
        A::Frame f; assert(a.poll(f) && f.data.size() == 7 && !a.waiting());
        assert(a.stats().foreignSsrc == 1 && a.stats().faultSerial == 0 && a.stats().malformed == 0);
    }
    {
        A a; init(a); auto p = rtp(100,300,true,{});
        a.receive(p.data(),p.size(),1); assert(a.stats().malformed == 1); // Empty unpadded media is invalid.
        p = rtp(100,300,true,{0}); p[0] = std::byte{0xa0};
        a.receive(p.data(),p.size(),2); assert(a.stats().malformed == 2); // Zero padding count.
        p.back() = std::byte{2}; a.receive(p.data(),p.size(),3); assert(a.stats().malformed == 3);
    }
    {
        A a; init(a);
        feed(a,100,300,false,{0x7c,0x85,0x11}); // Partial IDR must never survive a consumer flush.
        const auto before = a.stats().faultSerial;
        a.requestResync(1);
        assert(a.waiting() && a.stats().faultSerial == before + 1 && a.stats().pressure == 1);
        feed(a,101,300,true,{0x7c,0x45,0x22},2); empty(a);
        feed(a,102,400,true,{0x61,0x33},3); empty(a);
        recover(a,103,500);
        feed(a,104,600,true,{0x61,0x44},4);
        A::Frame f; assert(a.poll(f) && f.timestamp == 600);
    }

    {
        A a; init(a);
        feed(a,100,300,false,{0x7c,0x81,0x11});
        feed(a,101,300,false,{0x7c,0x01,0x22});
        feed(a,102,300,true,{0x7c,0x41,0x33});
        A::Frame f; assert(a.poll(f));
        const Bytes expected{std::byte{0},std::byte{0},std::byte{0},std::byte{1},
                             std::byte{0x61},std::byte{0x11},std::byte{0x22},std::byte{0x33}};
        assert(f.data == expected && f.firstSequence == 100 && f.lastSequence == 102);
    }
    {
        A a; init(a);
        feed(a,100,300,false,{0x7c,0x81,0x11});
        feed(a,102,300,true,{0x7c,0x41,0x33},1);
        empty(a); assert(!a.waiting()); a.tick(20); empty(a);
        a.tick(21); empty(a); assert(a.waiting() && a.stats().missing == 1);
        feed(a,101,300,false,{0x7c,0x01,0x22},22); empty(a); assert(a.stats().late == 1);
        recover(a,103);
    }
    {
        A a; init(a);
        feed(a,100,300,false,{0x7c,0x81,0x11});
        feed(a,101,300,false,{0x7c,0x01,0x22});
        feed(a,103,400,true,{0x61,0x99},1);
        a.tick(21); empty(a); assert(a.waiting()); recover(a,104);
    }
    {
        A a; init(a);
        feed(a,101,300,false,{0x7c,0x01,0x22});
        feed(a,102,300,true,{0x7c,0x41,0x33},1);
        a.tick(21); empty(a); assert(a.waiting()); recover(a,103);
    }
    {
        A a; init(a);
        feed(a,100,300,false,{0x7c,0x81,0x11});
        feed(a,102,300,true,{0x7c,0x41,0x33},1); empty(a);
        feed(a,101,300,false,{0x7c,0x01,0x22},2);
        A::Frame f; assert(a.poll(f) && f.data.size() == 8);
        assert(a.stats().gaps == 0 && a.stats().reordered == 1);
        feed(a,102,300,true,{0x7c,0x41,0x33},3); empty(a); assert(a.stats().duplicate == 1);
    }
    {
        // A complete first NAL never escapes when another NAL of the AU is lost.
        A a; init(a); feed(a,100,300,false,{0x61,0xaa});
        feed(a,101,300,false,{0x7c,0x81,0x11});
        feed(a,103,300,true,{0x7c,0x41,0x33},1); a.tick(21);
        empty(a); assert(a.waiting()); recover(a,104);
    }
    {
        // An entirely missing AU still breaks reference-chain continuity.
        A a; init(a);
        feed(a,103,400,true,{0x61,0x22},1); a.tick(21); empty(a);
        assert(a.stats().missing == 3 && a.waiting());
        feed(a,104,500,true,{0x61,0x33},22); empty(a); recover(a,105);
        feed(a,101,300,true,{0x65,0x55},23); empty(a); assert(!a.waiting());
    }
    {
        A a; init(a,65533);
        feed(a,65535,300,false,{0x7c,0x81,0x11});
        feed(a,1,300,true,{0x7c,0x41,0x33},1);
        feed(a,0,300,false,{0x7c,0x01,0x22},2);
        A::Frame f; assert(a.poll(f) && f.firstSequence == 65535 && f.lastSequence == 65537);
        feed(a,2,400,true,{0x61,0x55},3); assert(a.poll(f));
    }
    {
        A a;
        feed(a,1,0xfffffff0,true,{0x61,0x22});
        feed(a,2,0xfffffff8,true,{0x65,0x22});
        A::Frame f; assert(a.poll(f));
        feed(a,3,5,true,{0x61,0x22}); assert(a.poll(f) && f.timestamp == 5);
        feed(a,4,0xfffffff9,true,{0x65,0x22});
        assert(a.poll(f) && f.timestamp == 0xfffffff9 && !a.waiting());
        assert(a.stats().late == 0);
        feed(a,3,5,true,{0x61,0x22}); empty(a); assert(a.stats().duplicate == 1);
    }
    {
        A a; init(a);
        // Two complete STAP-A NALs.
        feed(a,100,300,true,{0x78,0,2,0x61,0x11,0,2,0x61,0x22});
        A::Frame f; assert(a.poll(f) && f.data.size() == 12);
        // Last length points outside payload: whole AU discarded.
        feed(a,101,400,true,{0x78,0,2,0x61,0x11,0,3,0x61,0x22});
        empty(a); assert(a.waiting()); recover(a,102);
    }
    {
        A a; init(a);
        auto p = rtp(100,300,true,{0x61,0x55});
        p[0] = std::byte{0xb1}; // X/P/one CSRC.
        p.insert(p.begin()+12,{std::byte{0},std::byte{0},std::byte{0},std::byte{9},
            std::byte{0xbe},std::byte{0xde},std::byte{0},std::byte{1},
            std::byte{0x10},std::byte{0},std::byte{0},std::byte{0}});
        p.insert(p.end(),{std::byte{0},std::byte{0},std::byte{0},std::byte{4}});
        a.receive(p.data(),p.size(),1);
        A::Frame f; assert(a.poll(f) && f.data.size() == 6);
        p = rtp(101,400,true,{0x61,0x55}); p[0] = std::byte{0xa0}; p.back() = std::byte{0};
        a.receive(p.data(),p.size(),2); empty(a); assert(a.waiting() && a.stats().malformed == 1);
        auto badExt = rtp(101,400,true,{0x61,0x55}); badExt[0] = std::byte{0x90};
        a.receive(badExt.data(),badExt.size(),3); assert(a.stats().malformed == 2);
    }
    {
        A a; init(a); feed(a,100,300,false,{0x7c,0x81,0x11});
        a.tick(A::AssemblyMs); empty(a); assert(a.waiting() && a.stats().timeout == 1);
        feed(a,101,300,true,{0x7c,0x41,0x33},151); empty(a); recover(a,102);
    }
    {
        // Pending-packet bound (all arrive inside the short reorder deadline).
        A a; init(a);
        for (unsigned seq=101; seq<=357; ++seq) feed(a,seq,300,false,{0x61,0x22},1);
        assert(a.stats().pressure > 0 && a.pendingPackets() <= A::MaxPending &&
               a.pendingBytes() <= A::MaxPendingBytes); empty(a);
        feed(a,358,400,true,{0x61,0x22},2); recover(a,359);
        // Queued frame bound cannot silently lose reference pictures.
        for (unsigned seq=360; seq<380; ++seq) feed(a,seq,seq*3000,true,{0x61,0x22},3);
        assert(a.waiting() && a.stats().pressure > 1);
    }
    {
        // Startup AU may contain the only parameter sets ever transmitted.
        A a;
        feed(a,1,100,false,{0x67,0x42,0,0x1f,0x80});
        feed(a,2,100,false,{0x68,0xc0});
        feed(a,3,100,true,{0x65,0xb8}); empty(a);
        feed(a,4,200,true,{0x65,0xb8});
        A::Frame f; assert(a.poll(f));
        const auto u = fog::detail::inspectVideo(reinterpret_cast<const unsigned char*>(f.data.data()),f.data.size());
        assert(u.nals.size() == 3 && u.nals[0].type == 7 && u.nals[1].type == 8 && u.nals[2].type == 5);
    }
    {
        // FU-start/end misuse and timestamp transition without marker.
        A a; init(a); feed(a,100,300,true,{0x7c,0xc1,0x11}); empty(a); assert(a.waiting());
        recover(a,101); feed(a,102,1100,false,{0x61,0x22});
        feed(a,103,1200,true,{0x61,0x33}); empty(a); assert(a.waiting()); recover(a,104,1300);
    }
    {
        A a; init(a);
        for (unsigned seq=100; seq<200; ++seq) {
            feed(a,seq,seq*3000,true,{0x7c,0xc1,0x11});
        }
        std::string e; unsigned n=0; while(a.pollEvent(e)) { ++n; assert(e.find("seq=")!=std::string::npos); }
        assert(n<=32 && a.stats().eventOverflow>0);
    }
    {
        // Filling an earlier gap starts the remaining gap's wait at its own
        // oldest buffered packet, rather than the already-resolved gap's age.
        A a; init(a);
        feed(a,101,300,false,{0x61,0x22},0);
        feed(a,103,300,true,{0x61,0x44},19);
        feed(a,100,300,false,{0x61,0x11},19);
        a.tick(20); empty(a); assert(!a.waiting() && a.stats().gaps == 0);
        feed(a,102,300,false,{0x61,0x33},25);
        A::Frame f; assert(a.poll(f) && f.data.size() == 24 && a.stats().gaps == 0);
    }
    {
        A a; init(a);
        auto p = rtp(100,300,false,{0x61}); p.resize(65000,std::byte{0x22});
        for (unsigned seq=100; seq<180; ++seq) {
            p[2]=std::byte(seq>>8); p[3]=std::byte(seq);
            p[1]=std::byte(seq==179 ? 0xe0 : 0x60);
            a.receive(p.data(),p.size(),1);
        }
        empty(a); assert(a.waiting() && a.stats().pressure>0 && a.assemblyBytes()<=A::MaxFrameBytes);
    }
    {
        A a; init(a);
        auto p = rtp(101,300,false,{0x61}); p.resize(65000,std::byte{0x22});
        for (unsigned seq=101; seq<140; ++seq) {
            p[2]=std::byte(seq>>8); p[3]=std::byte(seq);
            a.receive(p.data(),p.size(),1);
        }
        assert(a.stats().pressure>0 && a.pendingBytes()<=A::MaxPendingBytes); empty(a);
    }
    {
        A a; init(a); std::uint32_t random=1234567;
        for (unsigned n=0;n<20000;++n) {
            random=random*1664525u+1013904223u;
            Bytes p(random%256);
            for (auto& value:p) { random=random*1664525u+1013904223u; value=std::byte(random>>24); }
            a.receive(p.data(),p.size(),n);
            a.tick(n); A::Frame f; while(a.poll(f)) assert(f.data.size()<=A::MaxFrameBytes);
            assert(a.pendingPackets()<=A::MaxPending && a.pendingBytes()<=A::MaxPendingBytes);
        }
    }
    {
        // Valid decoding order can have backward presentation timestamps.
        A a; init(a);
        feed(a,100,400,true,{0x61,0x11});
        A::Frame f; assert(a.poll(f) && f.timestamp == 400);
        feed(a,101,300,false,{0x7c,0x81,0x11});
        feed(a,102,300,true,{0x7c,0x41,0x22});
        assert(a.poll(f) && f.timestamp == 300 && f.firstSequence == 101 &&
               f.lastSequence == 102 && f.data.size() == 7);
        assert(!a.waiting() && a.stats().gaps == 0 && a.stats().late == 0);
        feed(a,100,400,true,{0x61,0x11}); empty(a); assert(a.stats().duplicate == 1);
    }
    std::cout << "H264 RTP assembler: 28 deterministic scenarios passed\n";
}
