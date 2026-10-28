#pragma once
#include <cstdint>
using Result = std::uint32_t;
using s32 = std::int32_t;
#define R_FAILED(value) ((value) != 0)
enum HidNpadIdType { HidNpadIdType_No1 = 0, HidNpadIdType_Handheld = 32 };
enum HidNpadStyleTag {
    HidNpadStyleTag_NpadFullKey = 1, HidNpadStyleTag_NpadHandheld = 2,
    HidNpadStyleTag_NpadJoyDual = 4, HidNpadStyleTag_NpadJoyLeft = 8,
    HidNpadStyleTag_NpadJoyRight = 16
};
struct PadState { bool handheld = false, playerOne = false; };
inline bool padIsNpadActive(const PadState* pad, HidNpadIdType id) {
    return id == HidNpadIdType_Handheld ? pad->handheld : pad->playerOne;
}
struct HidVibrationDeviceHandle { unsigned value; };
struct HidVibrationValue { float amp_low, freq_low, amp_high, freq_high; };
std::uint32_t hidGetNpadStyleSet(HidNpadIdType);
Result hidInitializeVibrationDevices(HidVibrationDeviceHandle*, s32, HidNpadIdType, HidNpadStyleTag);
Result hidSendVibrationValues(const HidVibrationDeviceHandle*, const HidVibrationValue*, s32);
