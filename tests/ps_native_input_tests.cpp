#include "features/playstation/infrastructure/native_input.hpp"
#include "features/playstation/domain/touch_trace.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>
using namespace fog::playstation;
using J=nlohmann::json;
PsSensorSnapshot touch(unsigned id,int x,int y) {
    PsSensorSnapshot s;s.enabled=s.touchValid=s.touchFresh=true;s.touchCount=1;
    s.touches[0]={true,id,static_cast<std::uint16_t>(x),static_cast<std::uint16_t>(y),0,0};return s;
}
NativeInput initializedInput() {
    NativeInput input;
    assert(input.initializeContacts([](const J&){return true;}));
    return input;
}
int main() {
    std::vector<J> out;
    bool accept=true;
    auto emit=[&](const J& v){if(accept)out.push_back(v);return accept;};
    NativeInput input=initializedInput();auto sample=touch(12,0,0);
    input.update(sample,100,true,emit);
    assert(out.size()==1 && out[0]["id"]=="gamepad_touch" && out[0]["index"]==0);
    assert(out[0]["data"]==J({{"type",1622},{"finger",0},{"x",0.0},{"y",0.0}}));
    assert(!input.click(100));
    sample.touches[0].screenX=1279;sample.touches[0].screenY=719;
    input.update(sample,140,true,emit);
    assert(out.back()["data"]==J({{"type",1623},{"finger",0},{"x",1.0},{"y",1.0}}));
    sample.touches={};sample.touchCount=0;
    input.update(sample,160,true,emit);
    assert(out.back()["data"]["type"]==1624 && !input.click(160));
    assert(input.counters().down==1 && input.counters().move==1 && input.counters().up==1 && input.counters().taps==0);
    // Tap becomes a bounded button pulse after UP, not during a swipe.
    sample=touch(13,100,100);input.update(sample,200,true,emit);
    sample.touches={};sample.touchCount=0;input.update(sample,250,true,emit);
    assert(input.click(250) && input.click(329) && !input.click(330));
    assert(input.counters().taps==1);
    // Rapid taps must contain an observed button-up between bounded pulses.
    input=initializedInput();sample=touch(1,100,100);input.update(sample,100,true,emit);
    sample.touches={};sample.touchCount=0;input.update(sample,150,true,emit);assert(input.click(150));
    sample=touch(2,100,100);input.update(sample,170,true,emit);
    sample.touches={};sample.touchCount=0;input.update(sample,200,true,emit);assert(input.click(200));
    assert(!input.click(230,true) && !input.click(250,true) && !input.click(260,true));
    assert(input.click(270,false) && !input.click(350,true));
    // Two contacts, stable slot IDs and replacement release before new down.
    input=initializedInput();out.clear();sample=touch(10,100,100);sample.touchCount=2;
    sample.touches[1]={true,20,200,200,0,0};input.update(sample,100,true,emit);
    assert(out.size()==2 && out[1]["data"]["finger"]==1);
    sample.touches[0].fingerId=30;input.update(sample,120,true,emit);
    assert(out[2]["data"]["type"]==1624 && out[3]["data"]["type"]==1622);
    sample.touches={};sample.touchCount=0;input.update(sample,130,true,emit);
    assert(!input.click(130));
    // A blocked DOWN is not treated as accepted, and failed UP is retried.
    input=initializedInput();out.clear();sample=touch(10,1,1);accept=false;input.update(sample,100,true,emit);
    assert(input.counters().down==0 && input.counters().rejected==1);
    accept=true;input.update(sample,110,true,emit);assert(input.counters().down==1);
    sample.touches={};sample.touchCount=0;accept=false;input.update(sample,140,true,emit);
    assert(input.counters().up==0 && !input.click(140));
    accept=true;input.update(sample,150,true,emit);assert(input.counters().up==1);
    // Missing/stale HID and suppression release without generating clicks.
    input=initializedInput();out.clear();sample=touch(1,1,1);input.update(sample,100,true,emit);
    sample.touchValid=false;sample.touchStale=true;input.update(sample,120,true,emit);
    assert(input.counters().up==1 && !input.click(120));
    sample=touch(2,1,1);input.update(sample,150,true,emit);input.update(sample,160,false,emit);
    assert(input.counters().up==2 && !input.click(160));
    const auto count=out.size();input.update(sample,180,false,emit);assert(out.size()==count);
    // Sensor SI conversion, 30 ms pacing, no duplicate samples and neutral on pause.
    input=initializedInput();out.clear();sample={};sample.motion.valid=true;sample.motion.sampleNumber=1;
    sample.motion.psAccelerationG={0,1,0};sample.motion.psGyroRadiansPerSecond={1,2,3};
    input.update(sample,100,true,emit);assert(out.size()==2);
    assert(out[0]["data"]["sensor"]==1 && std::abs(out[0]["data"]["y"].get<float>()-9.80665f)<0.0001f);
    assert(out[1]["data"]==J({{"sensor",2},{"x",1.0},{"y",2.0},{"z",3.0}}));
    sample.motion.sampleNumber=2;input.update(sample,110,true,emit);assert(out.size()==2);
    input.update(sample,140,true,emit);assert(out.size()==4);
    input.update(sample,180,true,emit);assert(out.size()==4);
    input.update(sample,200,false,emit);assert(out.size()==6 && out[4]["data"]["x"]==0);
    assert(out[4]["data"]["sensor"]==2 && out[5]["data"]["sensor"]==1);
    input.update(sample,210,false,emit);assert(out.size()==6);
    sample.motion.sampleNumber=3;input.update(sample,240,true,emit);assert(out.size()==8);
    sample.motion.valid=false;input.update(sample,260,true,emit);assert(out.size()==10);
    sample.motion.valid=true;sample.motion.sampleNumber=4;sample.motion.psGyroRadiansPerSecond[0]=std::numeric_limits<float>::quiet_NaN();
    input.update(sample,300,true,emit);assert(out.size()==10 && input.counters().rejected==1);
    // Out-and-back movement cannot become a tap just because endpoints coincide.
    input=initializedInput();out.clear();sample=touch(1,100,100);input.update(sample,100,true,emit);
    sample.touches[0].screenX=180;input.update(sample,110,true,emit);
    sample.touches[0].screenX=100;input.update(sample,120,true,emit);
    sample.touches={};sample.touchCount=0;input.update(sample,130,true,emit);assert(!input.click(130));
    // Reset is sent without physical touches; suppression does not consume it.
    input=NativeInput{};out.clear();sample={};
    input.update(sample,100,false,emit);assert(out.empty() && !input.contactsInitialized());
    input.update(sample,120,true,emit);
    assert(input.contactsInitialized() && out.size()==2 && !input.click(120));
    for (int i=0;i<2;++i) assert(out[i]==NativeInput::touch(NativeInput::Up,i,0,0));
    input.update(sample,140,true,emit);input.suspend(emit);input.update(sample,160,true,emit);
    assert(out.size()==2 && input.counters().resetUpQueued==2);
    // A rejected second reset blocks held touch and sensors; retry only that slot.
    input=NativeInput{};out.clear();sample=touch(1,100,100);
    sample.motion.valid=true;sample.motion.sampleNumber=1;sample.motion.psAccelerationG={0,1,0};
    unsigned attempts=0;
    auto partial=[&](const J& value){++attempts;if(attempts==2)return false;out.push_back(value);return true;};
    input.update(sample,200,true,partial);
    assert(!input.contactsInitialized() && out.size()==1 && input.counters().resetRejected==1);
    assert(input.counters().down==0 && input.counters().gyro==0);
    input.update(sample,240,true,emit);
    assert(input.contactsInitialized() && out.size()==5);
    assert(out[0]==NativeInput::touch(NativeInput::Up,0,0,0));
    assert(out[1]==NativeInput::touch(NativeInput::Up,1,0,0));
    assert(out[2]["data"]["type"]==NativeInput::Down && out[3]["id"]=="gamepad_sensor");
    // New transport resets both contacts again before replaying held input.
    input=NativeInput{};out.clear();input.update(sample,300,true,emit);
    assert(out.size()==5 && out[0]==NativeInput::touch(NativeInput::Up,0,0,0) && out[1]==NativeInput::touch(NativeInput::Up,1,0,0));
        TouchTrace trace;sample=touch(15,0,0);trace.observe(sample,1,false);
    sample.touches[0].screenX=200;trace.observe(sample,60,false);
    sample.touches={};sample.touchCount=0;trace.observe(sample,100,true);
    assert(trace.down==1 && trace.moves==1 && trace.up==1 && trace.swipes==1 && trace.lastDx==200 && trace.suppressedFrames==1);
    std::cout<<"PS native input: touch/swipe/tap/two fingers/rejection/release/sensors/units/pacing/suppression PASS\n";
}
