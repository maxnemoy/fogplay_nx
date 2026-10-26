#include "platform/switch/ps_sensors.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
unsigned styles=HidNpadStyleTag_NpadHandheld;
Result getResult=0,startResult=0,stopResult=0;
unsigned starts=0,stops=0,gets=0;
bool hasTouch=true,hasMotion=true;
HidTouchScreenState touch{};
HidSixAxisSensorState motion{};
unsigned hidGetNpadStyleSet(HidNpadIdType){return styles;}
void hidInitializeTouchScreen(){}
Result hidGetSixAxisSensorHandles(HidSixAxisSensorHandle* h,s32 n,HidNpadIdType,HidNpadStyleTag){++gets;for(int i=0;i<n;++i)h[i].value=i;return getResult;}
Result hidStartSixAxisSensor(HidSixAxisSensorHandle){++starts;return startResult;}
Result hidStopSixAxisSensor(HidSixAxisSensorHandle){++stops;return stopResult;}
std::size_t hidGetTouchScreenStates(HidTouchScreenState* s,std::size_t){*s=touch;return hasTouch?1:0;}
std::size_t hidGetSixAxisSensorStates(HidSixAxisSensorHandle,HidSixAxisSensorState* s,std::size_t){*s=motion;return hasMotion?1:0;}
int main(){
    using fogplay::platform::PsSensors;
    PsSensors reader;
    PadState pad{};pad.activeHandheld=true;
    reader.start();
    touch.sampling_number=1;touch.count=3;
    touch.touches[0]={41,1279,719};touch.touches[1]={42,0,0};touch.touches[2]={43,1500,900};
    motion.sampling_number=1;motion.attributes=1;motion.angular_velocity={1,2,3};motion.acceleration={4,5,6};
    auto state=reader.update(pad,100);
    assert(state.touchCount==2 && state.hidTouchCount==3 && state.touches[0].fingerId==41);
    assert(state.touches[0].x==1919 && state.touches[0].y==1079);
    assert(state.motion.valid && state.motion.fresh && std::abs(state.motion.psGyroRadiansPerSecond[1]-18.849556f)<0.001f);
    assert(state.motion.psAccelerationG[1]==-6);
    touch.sampling_number=2;touch.count=2;touch.touches[0]={42,120,60};touch.touches[1]={41,400,200};
    state=reader.update(pad,110);
    assert(state.touches[0].fingerId==41 && state.touches[0].screenX==400 && state.touches[1].fingerId==42);
    touch.sampling_number=3;touch.touches[0]={99,500,300};touch.touches[1]={42,120,60};
    state=reader.update(pad,120);
    assert(state.touches[0].fingerId==99 && state.touches[1].fingerId==42);
    hasTouch=false;hasMotion=false;
    state=reader.update(pad,200);assert(state.touchCount==2 && !state.motion.valid);
    state=reader.update(pad,371);assert(state.touchCount==0 && state.touchStale && state.counters.touchStaleReleases==1);
    styles=HidNpadStyleTag_NpadJoyDual;pad.activeHandheld=false;pad.activeMask=1;
    state=reader.update(pad,400);assert(state.handleCount==2 && starts==3 && stops==1);
    stopResult=77;reader.stop();assert(stops==3 && reader.snapshot().counters.stopFailures==2 && reader.snapshot().touchCount==0);
    reader.start();assert(reader.snapshot().counters.updates==0);
    getResult=88;state=reader.update(pad,500);assert(state.lastGetHandlesResult==88 && state.handleCount==0);
    const unsigned before=gets;reader.update(pad,1000);assert(gets==before);
    getResult=0;state=reader.update(pad,2501);assert(state.handleCount==2);
    hasMotion=true;motion.sampling_number=2;motion.angular_velocity.x=std::numeric_limits<float>::quiet_NaN();
    state=reader.update(pad,2510);assert(!state.motion.valid && state.counters.motionInvalid==2);
    pad.activeMask=0;state=reader.update(pad,2520);assert(state.handleCount==0 && state.npadId==-1);
    reader.stop();reader.stop();
    std::cout<<"PS sensor capture lifecycle, multi-touch, stale release, units, init retry and invalid motion: PASS\n";
}
