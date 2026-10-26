#pragma once
#include <cstdint>
#include <cstddef>
using Result = std::uint32_t; using s32 = std::int32_t;
#define R_FAILED(x) ((x) != 0)
#define R_SUCCEEDED(x) ((x) == 0)
enum HidNpadIdType { HidNpadIdType_No1, HidNpadIdType_No8=7, HidNpadIdType_Handheld=32 };
enum HidNpadStyleTag { HidNpadStyleTag_NpadFullKey=1,HidNpadStyleTag_NpadHandheld=2,HidNpadStyleTag_NpadJoyDual=4,HidNpadStyleTag_NpadJoyLeft=8,HidNpadStyleTag_NpadJoyRight=16 };
constexpr unsigned HidSixAxisSensorAttribute_IsConnected=1;
struct PadState { bool activeHandheld=false; unsigned activeMask=0; };
inline bool padIsNpadActive(const PadState* p,HidNpadIdType id) { return id==HidNpadIdType_Handheld?p->activeHandheld:(p->activeMask&(1u<<id)); }
struct HidSixAxisSensorHandle { unsigned value=0; };
struct HidVector { float x=0,y=0,z=0; };
struct HidDirectionState { float direction[3][3]{}; };
struct HidSixAxisSensorState { std::uint64_t delta_time=0,sampling_number=0; HidVector acceleration,angular_velocity,angle; HidDirectionState direction; unsigned attributes=0; };
struct HidTouchState { std::uint32_t finger_id=0,x=0,y=0; };
struct HidTouchScreenState { std::uint64_t sampling_number=0; int count=0; HidTouchState touches[16]{}; };
unsigned hidGetNpadStyleSet(HidNpadIdType);
void hidInitializeTouchScreen();
Result hidGetSixAxisSensorHandles(HidSixAxisSensorHandle*,s32,HidNpadIdType,HidNpadStyleTag);
Result hidStartSixAxisSensor(HidSixAxisSensorHandle);
Result hidStopSixAxisSensor(HidSixAxisSensorHandle);
std::size_t hidGetTouchScreenStates(HidTouchScreenState*,std::size_t);
std::size_t hidGetSixAxisSensorStates(HidSixAxisSensorHandle,HidSixAxisSensorState*,std::size_t);
