#include "probe_buffers.hpp"
#include <cassert>
#include <iostream>
int main(){
    using namespace moonlight_probe;
    char sps[]={0,0,1,7},frame[]={0,0,1,5};
    LENTRY last{nullptr,frame,4,BUFFER_TYPE_PICDATA};LENTRY first{&last,sps,4,BUFFER_TYPE_SPS};
    DECODE_UNIT unit{};unit.bufferList=&first;unit.fullLength=8;
    std::vector<std::byte> result;assert(copyFrame(unit,result)&&result.size()==8);
    unit.fullLength=7;assert(!copyFrame(unit,result));unit.fullLength=9;assert(!copyFrame(unit,result));
    unit.fullLength=8;last.next=&first;assert(!copyFrame(unit,result));last.next=nullptr;
    last.length=-1;assert(!copyFrame(unit,result));last.length=4;
    last.data=nullptr;assert(!copyFrame(unit,result));unit.fullLength=9*1024*1024;assert(!copyFrame(unit,result));
    AudioQueue audio;audio.push(frame,4);auto packets=audio.take();assert(packets.size()==1 && packets.front().size()==4);
    for(int i=0;i<21;++i)audio.push(frame,4);
    assert(audio.lost()==20 && audio.take().size()==1);
    audio.push(nullptr,0);audio.push(frame,100000);assert(audio.lost()==22 && audio.take().empty());
    std::cout<<"Moonlight probe frame ownership and bounded audio tests passed\n";
}
