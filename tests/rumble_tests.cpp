#include "platform/switch/rumble.hpp"
#include "features/streaming/infrastructure/rumble_mailbox.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
std::uint32_t style = HidNpadStyleTag_NpadHandheld;
Result initResult = 0, sendResult = 0;
unsigned initializes = 0;
struct Sent { unsigned handle; std::vector<HidVibrationValue> values; };
std::vector<Sent> sent;
void quiet() {
    assert(!sent.empty());
    for (const auto& value : sent.back().values) assert(value.amp_low == 0 && value.amp_high == 0);
}
void playback() {
    stream::RumblePlayback player;
    stream::Rumble value{0, 65535, 32768, 100, 100};
    player.update(&value, true, 150);
    assert(player.current().lowFrequency == 65535);
    player.update(nullptr, true, 199);
    assert(player.current().highFrequency == 32768);
    player.update(nullptr, true, 200);
    assert(player.current().lowFrequency == 0); // Receipt-based expiry, not poll-based.
    value = {0, 40000, 10000, 0, 300};
    player.update(&value, true, 301);
    player.update(nullptr, true, 100000);
    assert(player.current().lowFrequency == 40000); // Moonlight holds until replacement.
    player.update(nullptr, false, 100001);
    value.receivedAtMs = 100000;
    player.update(&value, true, 100002);
    assert(player.current().lowFrequency == 0); // No replay after menu/recovery.
    value.receivedAtMs = 100003;
    player.update(&value, true, 100004);
    assert(player.current().lowFrequency == 40000);
    value = {0, 0, 0, 0, 100005};
    player.update(&value, true, 100005);
    assert(player.current().lowFrequency == 0);
    value = {0, 65535, 65535, 0, 100006};
    player.update(&value, true, 100257);
    assert(player.current().lowFrequency == 0); // Late callback cannot start an old effect.
    value.receivedAtMs = 100300;
    player.update(&value, true, 100299);
    assert(player.current().lowFrequency == 0); // Invalid future receipt.
    value.controller = 1;
    player.update(&value, true, 100301);
    assert(player.current().lowFrequency == 0);
    stream::RumbleMailbox mailbox;
    for (unsigned i = 1; i <= 10000; ++i) mailbox.put({0, static_cast<std::uint16_t>(i), 0, 0, i});
    mailbox.put({1, 65000, 65000, 0, 10001});
    assert(mailbox.take(value) && value.lowFrequency == 10000 && !mailbox.take(value));
    mailbox.put({0, 65000, 65000, 0, 10002});
    mailbox.put({0, 0, 0, 0, 10003});
    assert(mailbox.take(value) && value.lowFrequency == 0 && value.highFrequency == 0);
    mailbox.put({0, 65000, 65000, 0, 10004});
    mailbox.clear();
    assert(!mailbox.take(value));
}
void hardware() {
    PadState pad{true, false};
    fogplay::platform::Rumble output;
    output.update(pad, nullptr, false, 1);
    assert(initializes == 1 && sent.back().values.size() == 2);
    quiet();
    stream::Rumble value{0, 65535, 32768, 100, 10};
    output.update(pad, &value, true, 11);
    assert(sent.back().values[0].amp_low == 1.f);
    assert(sent.back().values[0].freq_low == 160.f && sent.back().values[1].freq_low == 160.f);
    assert(std::abs(sent.back().values[1].amp_low - .5f) < .001f);
    assert(sent.back().values[0].amp_high == 0.f && sent.back().values[1].amp_high == 0.f);
    const auto count = sent.size();
    output.update(pad, nullptr, true, 109);
    assert(sent.size() == count); // No redundant HID traffic.
    output.update(pad, nullptr, true, 110);
    quiet();
    value = {0, 65535, 65535, 0, 120};
    output.update(pad, &value, true, 121);
    output.update(pad, nullptr, false, 130);
    quiet();
    output.update(pad, &value, true, 131);
    quiet();
    value.receivedAtMs = 140;
    output.update(pad, &value, true, 141);
    const auto oldHandle = sent.back().handle;
    pad = {false, true}; style = HidNpadStyleTag_NpadFullKey;
    const auto beforeSwitch = sent.size();
    output.update(pad, nullptr, true, 150);
    assert(sent[beforeSwitch].handle == oldHandle && sent[beforeSwitch].values[0].amp_low == 0);
    quiet(); // Replacement controller never inherits an effect.
    assert(sent.back().values.size() == 2);
    style = HidNpadStyleTag_NpadJoyRight;
    output.update(pad, nullptr, true, 160);
    assert(sent.back().values.size() == 1);
    value.receivedAtMs = 170;
    output.update(pad, &value, true, 171);
    assert(sent.back().values[0].amp_low == 1 && sent.back().values[0].amp_high == 0);
    pad = {};
    output.update(pad, nullptr, true, 180);
    quiet();
    pad.playerOne = true; initResult = 42;
    output.update(pad, nullptr, true, 200);
    const auto initCount = initializes;
    output.update(pad, nullptr, true, 1000);
    assert(initializes == initCount); // Failed hardware probe has bounded retries.
    initResult = 0;
    output.update(pad, nullptr, true, 2200);
    assert(initializes == initCount + 1);
    value.receivedAtMs = 2210; sendResult = 43;
    output.update(pad, &value, true, 2211);
    const auto failedCount = sent.size();
    output.update(pad, nullptr, true, 2220);
    assert(sent.size() == failedCount);
    sendResult = 0;
    output.stop(2221); // A stop bypasses a failed nonzero send's retry delay.
    quiet();
    value.receivedAtMs = 2230;
    output.update(pad, &value, true, 2231);
} // Destructor must zero the device.
void pairedBalance() {
    for (const auto controllerStyle : {HidNpadStyleTag_NpadHandheld,
                                      HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadFullKey}) {
        style = controllerStyle;
        PadState pad{style == HidNpadStyleTag_NpadHandheld, style != HidNpadStyleTag_NpadHandheld};
        fogplay::platform::Rumble output;
        output.update(pad, nullptr, false, 1);
        std::uint64_t now = 10;
        for (const auto strength : {std::uint16_t(1), std::uint16_t(6168),
                                    std::uint16_t(32768), std::uint16_t(65535)}) {
            stream::Rumble equal{0, strength, strength, 0, now};
            output.update(pad, &equal, true, now + 1);
            assert(sent.back().values.size() == 2);
            const auto& left = sent.back().values[0];
            const auto& right = sent.back().values[1];
            assert(left.amp_low == right.amp_low && left.amp_high == right.amp_high);
            assert(left.freq_low == right.freq_low && left.freq_high == right.freq_high);
            assert(left.amp_low == strength / 65535.f && left.amp_high == 0);
            now += 10;
        }
        stream::Rumble rightOnly{0, 0, 32768, 0, now};
        output.update(pad, &rightOnly, true, now + 1);
        assert(sent.back().values[0].amp_low == 0 && sent.back().values[0].amp_high == 0);
        assert(sent.back().values[1].amp_low > 0 && sent.back().values[1].amp_high == 0);
        now += 10;
        stream::Rumble leftOnly{0, 32768, 0, 0, now};
        output.update(pad, &leftOnly, true, now + 1);
        assert(sent.back().values[0].amp_low > 0 && sent.back().values[0].amp_high == 0);
        assert(sent.back().values[1].amp_low == 0 && sent.back().values[1].amp_high == 0);
        output.stop(now + 2);
        quiet();
    }
}
void profiles() {
    using Mode = fog::rumble::Mode;
    for (const auto controllerStyle : {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadJoyDual,
            HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight}) {
        style = controllerStyle;
        const bool single = style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight;
        PadState pad{style == HidNpadStyleTag_NpadHandheld, style != HidNpadStyleTag_NpadHandheld};
        fogplay::platform::Rumble output;
        output.update(pad, nullptr, false, 1);
        std::uint64_t now = 10;
        for (const auto mode : {Mode::Balanced, Mode::DualBand, Mode::Chromium}) {
            output.setMode(mode, now++);
            // Isolated source channels must remain isolated on a pair, and
            // neither source may disappear when only one Joy-Con is available.
            for (const auto motors : {std::pair<std::uint16_t, std::uint16_t>{65535, 0},
                                      {0, 65535}, {65535, 32768}, {65535, 65535}}) {
                stream::Rumble effect{0, motors.first, motors.second, 0, now};
                output.update(pad, &effect, true, now + 1);
                const auto& values = sent.back().values;
                assert(values.size() == (single ? 1u : 2u));
                if (!single && motors.first == 0) assert(values[0].amp_low == 0 && values[0].amp_high == 0);
                if (!single && motors.second == 0) assert(values[1].amp_low == 0 && values[1].amp_high == 0);
                for (std::size_t i = 0; i < values.size(); ++i) {
                    const auto& value = values[i];
                    if (mode == Mode::Chromium) {
                        assert(value.freq_low == 141 && value.freq_high == 182);
                        const float strong = single || i == 0 ? motors.first / 65535.f * .9f : 0.f;
                        const float weak = single || i == 1 ? motors.second / 65535.f * .1f : 0.f;
                        assert(std::abs(value.amp_low - strong) < .00001f);
                        assert(std::abs(value.amp_high - weak) < .00001f);
                    } else {
                        assert(value.freq_low == 160 && value.freq_high == 320);
                        const auto source = single ? std::max(motors.first, motors.second) :
                            i == 0 ? motors.first : motors.second;
                        assert(value.amp_low == source / 65535.f);
                        assert(value.amp_high == (mode == Mode::DualBand ? value.amp_low : 0));
                    }
                }
                now += 10;
            }
            output.stop(now++);
            quiet();
        }
    }
}
void profileChanges() {
    using Mode = fog::rumble::Mode;
    style = HidNpadStyleTag_NpadJoyDual;
    PadState pad{false, true};
    fogplay::platform::Rumble output;
    output.update(pad, nullptr, false, 1);
    stream::Rumble effect{0, 65535, 65535, 0, 10};
    output.update(pad, &effect, true, 11);
    output.setMode(Mode::Disabled, 12);
    quiet(); // Off stops a persistent effect immediately, without waiting for update.
    effect.receivedAtMs = 20;
    output.update(pad, &effect, true, 21);
    quiet(); // Incoming effects while disabled cannot reach hardware.
    output.setMode(Mode::DualBand, 30);
    output.update(pad, &effect, true, 31);
    quiet(); // Enabling does not replay an effect received while disabled.
    effect.receivedAtMs = 40;
    output.update(pad, &effect, true, 41);
    assert(sent.back().values[0].amp_high == 1);
    output.setMode(Mode::Chromium, 42);
    quiet();
    output.update(pad, &effect, true, 43);
    quiet(); // Changing enabled profiles also clears the old persistent effect.
    effect.receivedAtMs = 50;
    output.update(pad, &effect, true, 51);
    assert(sent.back().values[0].amp_low == .9f && sent.back().values[1].amp_high == .1f);
    const auto before = sent.size();
    output.setMode(Mode::Chromium, 52);
    output.update(pad, nullptr, true, 53);
    assert(sent.size() == before); // Reapplying the same setting preserves active effects.
    sendResult = 43;
    output.setMode(Mode::Disabled, 54);
    quiet(); // A failed stop remains dirty and is retried with zero, never new rumble.
    sendResult = 0;
    output.update(pad, &effect, true, 55);
    quiet();
    assert(sent.size() == before + 2);
    output.setMode(static_cast<Mode>(99), 56);
    assert(output.diagnostics().find("mapping=disabled") != std::string::npos);
}
}
std::uint32_t hidGetNpadStyleSet(HidNpadIdType) { return style; }
Result hidInitializeVibrationDevices(HidVibrationDeviceHandle* handles, s32 count,
                                     HidNpadIdType id, HidNpadStyleTag) {
    ++initializes;
    for (s32 i = 0; i < count; ++i) handles[i].value = static_cast<unsigned>(id) * 2 + i;
    return initResult;
}
Result hidSendVibrationValues(const HidVibrationDeviceHandle* handles,
                             const HidVibrationValue* values, s32 count) {
    assert(count == 1 || count == 2);
    for (s32 i = 0; i < count; ++i) {
        assert(values[i].amp_low >= 0 && values[i].amp_low <= 1);
        assert(values[i].amp_high >= 0 && values[i].amp_high <= 1);
    }
    sent.push_back({handles[0].value, {values, values + count}});
    return sendResult;
}
int main() {
    playback();
    hardware();
    pairedBalance();
    profiles();
    profileChanges();
    quiet();
    std::cout << "Rumble: timing, routing, three profiles, isolated motors, off, profile changes, errors and teardown PASS\n";
}
