#include "features/streaming/domain/h264_rtp.hpp"
#include <cassert>
#include <iostream>
#include <vector>
using A = stream::H264RtpAssembler;
using B = std::vector<std::byte>;
static B n(unsigned type, std::initializer_list<unsigned> bytes = {0xa0}) {
    B out{std::byte(type << 1), std::byte{1}};
    for (auto b : bytes) out.push_back(std::byte(b));
    return out;
}
static B sps(unsigned version = 0) {
    // vps_id=0, max_sub_layers_minus1=0, nesting=1, profile_tier_level,
    // sps_seq_parameter_set_id=0. Escape every emulation sequence.
    std::vector<unsigned> rbsp{1}; rbsp.resize(13, 0); rbsp[2] = version; rbsp.push_back(0xc0);
    B out{std::byte{66},std::byte{1}}; unsigned zeros=0;
    for (auto v : rbsp) {
        if (zeros == 2 && v <= 3) { out.push_back(std::byte{3}); zeros=0; }
        out.push_back(std::byte(v)); zeros = v == 0 ? zeros+1 : 0;
    }
    return out;
}
static B packet(unsigned sequence, unsigned timestamp, bool marker, const B& payload) {
    B p(12); p[0]=std::byte{128}; p[1]=std::byte(marker?224:96);
    p[2]=std::byte(sequence>>8); p[3]=std::byte(sequence);
    p[4]=std::byte(timestamp>>24); p[5]=std::byte(timestamp>>16);
    p[6]=std::byte(timestamp>>8); p[7]=std::byte(timestamp); p[11]=std::byte{1};
    p.insert(p.end(),payload.begin(),payload.end()); return p;
}
static void feed(A& a,unsigned seq,unsigned ts,bool marker,const B& payload,unsigned now=0) {
    const auto p=packet(seq,ts,marker,payload); a.receive(p.data(),p.size(),now);
}
static void empty(A& a) { A::Frame f; assert(!a.poll(f)); }
static void config(A& a,unsigned seq=1,unsigned ts=100) {
    feed(a,seq,ts,false,n(32,{8})); feed(a,seq+1,ts,false,sps());
    feed(a,seq+2,ts,false,n(34,{0xc0})); feed(a,seq+3,ts,true,n(19)); empty(a);
}
static void recover(A& a,unsigned seq=5,unsigned ts=200) {
    feed(a,seq,ts,true,n(19)); A::Frame f; assert(a.poll(f));
    const auto unit=fog::detail::inspectVideo(reinterpret_cast<const unsigned char*>(f.data.data()), f.data.size(),stream::VideoCodec::H265);
    assert(unit.complete && unit.nals.size()==4 && unit.nals[0].type==32 && unit.nals[3].type==19 && !a.waiting());
}
static B fu(unsigned flags,unsigned type=1,unsigned temporal=1) { return {std::byte{98},std::byte(temporal),std::byte(type|flags),std::byte{0x55}}; }
static B ap(const B& a,const B& b) {
    B out{std::byte{96},std::byte{1},std::byte(a.size()>>8),std::byte(a.size())};
    out.insert(out.end(),a.begin(),a.end()); out.push_back(std::byte(b.size()>>8)); out.push_back(std::byte(b.size()));
    out.insert(out.end(),b.begin(),b.end()); return out;
}
int main() {
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      feed(a,6,300,false,fu(128));
      auto pad=packet(7,999,true,{}); pad[0]=std::byte{160}; pad.push_back(std::byte{1});
      a.receive(pad.data(),pad.size(),1);
      feed(a,8,300,true,fu(64),2); A::Frame f;
      assert(a.poll(f) && f.data.size()==8 && f.firstSequence==6 && f.lastSequence==8);
      assert(!a.waiting() && a.stats().padding==1 && a.stats().malformed==0 && a.stats().gaps==0); }
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      feed(a,6,300,false,fu(128)); feed(a,8,300,true,fu(64),1); feed(a,7,300,false,fu(0),2);
      A::Frame f; assert(a.poll(f) && f.data.size()==9 && a.stats().gaps==0);
      feed(a,8,300,true,fu(64),3); empty(a); assert(a.stats().duplicate==1);
      feed(a,9,400,true,ap(n(1),n(1))); assert(a.poll(f) && f.data.size()==14); }
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      feed(a,6,300,false,fu(128)); feed(a,8,300,true,fu(64),1); a.tick(21); empty(a);
      assert(a.waiting() && a.stats().missing==1); feed(a,7,300,false,fu(0),22); assert(a.stats().late==1);
      // CRA/BLA and RASL cannot end quarantine; only IDR can.
      feed(a,9,400,true,n(21),23); feed(a,10,500,true,n(8),24); feed(a,11,600,true,n(16),25); empty(a);
      recover(a,12,700); }
    { A a(stream::VideoCodec::H265); config(a,65531); recover(a,65535);
      feed(a,0,300,false,fu(128)); feed(a,2,300,true,fu(64),1); feed(a,1,300,false,fu(0),2);
      A::Frame f; assert(a.poll(f) && f.firstSequence==65536 && f.lastSequence==65538); }
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      feed(a,6,300,false,fu(128)); feed(a,7,300,true,fu(64,1,2)); empty(a); assert(a.waiting());
      recover(a,8,400); feed(a,9,500,true,fu(192)); empty(a); assert(a.waiting()); recover(a,10,600); }
    for (const auto& bad : {B{std::byte{38}}, B{std::byte{166},std::byte{1},std::byte{160}}, B{std::byte{38},std::byte{0},std::byte{160}},
                           B{std::byte{39},std::byte{1},std::byte{160}}, n(50), ap(n(48),n(1)),
                           B{std::byte{96},std::byte{1},std::byte{0},std::byte{9},std::byte{2}}, fu(128,48)}) {
      A a(stream::VideoCodec::H265); config(a); recover(a); feed(a,6,300,true,bad); empty(a);
      assert(a.waiting()); recover(a,7,400);
    }
    { A a(stream::VideoCodec::H265); feed(a,1,100,true,n(1)); feed(a,2,200,true,n(19)); empty(a); assert(a.waiting()); }
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      feed(a,6,300,false,fu(128)); a.tick(A::AssemblyMs); empty(a); assert(a.waiting() && a.stats().timeout==1);
      feed(a,7,300,true,fu(64),151); recover(a,8,400); }
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      feed(a,6,300,false,n(1)); feed(a,7,400,true,n(1)); empty(a); assert(a.waiting()); recover(a,8,500); }
    { A a(stream::VideoCodec::H265); config(a); recover(a);
      auto bad=ap(n(1),n(1)); bad.pop_back(); feed(a,6,300,true,bad); empty(a); assert(a.waiting()); recover(a,7,400); }
    { fog::detail::VideoParameters p(stream::VideoCodec::H265);
      const auto observe=[&](B b) { b.insert(b.begin(),{std::byte{0},std::byte{0},std::byte{0},std::byte{1}});
          p.observe(fog::detail::inspectVideo(reinterpret_cast<const unsigned char*>(b.data()),b.size(),stream::VideoCodec::H265)); };
      B idr=n(19); idr.insert(idr.begin(),{std::byte{0},std::byte{0},std::byte{0},std::byte{1}});
      const auto unit=fog::detail::inspectVideo(reinterpret_cast<const unsigned char*>(idr.data()),idr.size(),stream::VideoCodec::H265);
      std::vector<unsigned char> prefix; observe(n(32,{8})); observe(sps()); observe(n(34,{0xc0})); assert(p.recoveryPrefix(unit,prefix));
      observe(sps(1)); assert(!p.recoveryPrefix(unit,prefix)); observe(n(34,{0xc0})); assert(p.recoveryPrefix(unit,prefix));
      observe(n(32,{9})); assert(!p.recoveryPrefix(unit,prefix)); observe(sps()); observe(n(34,{0xc0})); assert(p.recoveryPrefix(unit,prefix));
      observe(n(34,{0xa0})); assert(!p.recoveryPrefix(unit,prefix)); }
    { A a(stream::VideoCodec::H265); unsigned random=1234567;
      for (unsigned i=0;i<20000;++i) {
        random=random*1664525u+1013904223u; B p(random%256);
        for (auto& b:p) { random=random*1664525u+1013904223u; b=std::byte(random>>24); }
        a.receive(p.data(),p.size(),i); a.tick(i); A::Frame f;
        while(a.poll(f)) assert(f.data.size()<=A::MaxFrameBytes);
        assert(a.pendingPackets()<=A::MaxPending && a.pendingBytes()<=A::MaxPendingBytes);
      } }
    std::cout << "HEVC RTP tests passed: AP/FU, strict headers, no stale parameter chains, IDR-only recovery, loss/reorder/wrap/timeout, 20000 fuzz packets\n";
}
