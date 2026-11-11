#include "platform/switch/rumble.hpp"
#include "features/streaming/application/rumble_diagnostics_formatter.hpp"
#include <algorithm>

namespace fogplay::platform {
Rumble::~Rumble() { stop(0); }

void Rumble::stop(std::uint64_t nowMs) noexcept {
    playback_.stop(nowMs);
    apply(nowMs, true);
}

void Rumble::setMode(fog::rumble::Mode mode, std::uint64_t nowMs) noexcept {
    if (!fog::rumble::valid({mode})) mode = fog::rumble::Mode::Disabled;
    if (mode_ == mode) return;
    stop(nowMs); // A new profile never replays an effect received before the change.
    mode_ = mode;
    dirty_ = true;
}

void Rumble::bind(const PadState& pad, std::uint64_t nowMs) noexcept {
    int nextId = -1;
    std::uint32_t nextStyle = 0;
    for (const auto id : {HidNpadIdType_Handheld, HidNpadIdType_No1}) {
        if (!padIsNpadActive(&pad, id)) continue;
        const auto styles = hidGetNpadStyleSet(id);
        for (const auto style : {HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadFullKey,
                                HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadJoyLeft,
                                HidNpadStyleTag_NpadJoyRight}) {
            if (styles & style) { nextId = static_cast<int>(id); nextStyle = style; break; }
        }
        if (nextId >= 0) break;
    }
    const bool changed = nextId != npadId_ || nextStyle != style_;
    if (!changed && (count_ || nowMs < nextBindMs_)) return;
    if (changed) stop(nowMs); // Never move an old effect onto a replacement pad.
    count_ = 0;
    npadId_ = nextId;
    style_ = nextStyle;
    nextBindMs_ = nowMs + 2000;
    if (nextId < 0) return;
    const unsigned count = nextStyle == HidNpadStyleTag_NpadJoyLeft ||
                           nextStyle == HidNpadStyleTag_NpadJoyRight ? 1 : 2;
    lastResult_ = hidInitializeVibrationDevices(handles_.data(), static_cast<s32>(count),
        static_cast<HidNpadIdType>(nextId), static_cast<HidNpadStyleTag>(nextStyle));
    if (R_FAILED(lastResult_)) { ++failures_; return; }
    count_ = count;
    dirty_ = true;
    nextSendMs_ = 0;
}

void Rumble::apply(std::uint64_t nowMs, bool force) noexcept {
    if (!count_) return;
    const auto& value = playback_.current();
    const bool changed = value.lowFrequency != appliedLow_ || value.highFrequency != appliedHigh_;
    if (!dirty_ && !changed) return;
    const bool stopping = !value.lowFrequency && !value.highFrequency && (appliedLow_ || appliedHigh_);
    if (!force && !stopping && nowMs < nextSendMs_) return;
    const float low = value.lowFrequency / 65535.0f;
    const float high = value.highFrequency / 65535.0f;
    // Remote low/high identify motor strengths. Each profile translates them
    // into the two frequency bands supported by each physical actuator.
    std::array<HidVibrationValue, 2> values{{{low, 160.f, 0.f, 320.f},
                                           {high, 160.f, 0.f, 320.f}}};
    using Mode = fog::rumble::Mode;
    switch (mode_) {
        case Mode::Balanced:
            if (count_ == 1) values[0].amp_low = std::max(low, high);
            break;
        case Mode::DualBand:
            values = {{{low, 160.f, low, 320.f}, {high, 160.f, high, 320.f}}};
            if (count_ == 1) values[0] = {std::max(low, high), 160.f, std::max(low, high), 320.f};
            break;
        case Mode::Chromium:
            // Match Chromium's strong/weak translation, including its deliberate
            // 0.9/0.1 amplitude scaling. The weak motor is expected to feel softer.
            values = {{{low * .9f, 141.f, 0.f, 182.f}, {0.f, 141.f, high * .1f, 182.f}}};
            if (count_ == 1) values[0] = {low * .9f, 141.f, high * .1f, 182.f};
            break;
        case Mode::Disabled:
            values = {{{0.f, 160.f, 0.f, 320.f}, {0.f, 160.f, 0.f, 320.f}}};
            break;
    }
    lastResult_ = hidSendVibrationValues(handles_.data(), values.data(), static_cast<s32>(count_));
    if (R_FAILED(lastResult_)) {
        dirty_ = true;
        nextSendMs_ = nowMs + 250;
        ++failures_;
        return;
    }
    dirty_ = false;
    nextSendMs_ = 0;
    appliedLow_ = value.lowFrequency;
    appliedHigh_ = value.highFrequency;
    ++applied_;
    if (!appliedLow_ && !appliedHigh_) ++stops_;
}

void Rumble::update(const PadState& pad, const stream::Rumble* feedback,
                    bool enabled, std::uint64_t nowMs) noexcept {
    bind(pad, nowMs);
    if (feedback) ++received_;
    playback_.update(feedback, enabled && count_ > 0 && mode_ != fog::rumble::Mode::Disabled, nowMs);
    apply(nowMs);
}

stream::telemetry::rumble::Snapshot Rumble::captureTelemetry() const noexcept {
    using Mapping=stream::telemetry::rumble::Mapping;
    Mapping mapping=Mapping::Invalid;
    switch(mode_){case fog::rumble::Mode::Balanced:mapping=Mapping::Balanced;break;
        case fog::rumble::Mode::DualBand:mapping=Mapping::DualBand;break;
        case fog::rumble::Mode::Chromium:mapping=Mapping::Chromium;break;
        case fog::rumble::Mode::Disabled:mapping=Mapping::Disabled;break;}
    return {received_,applied_,stops_,failures_,lastResult_,style_,count_,npadId_,mapping};
}
std::string Rumble::diagnostics() const {return stream::telemetry::rumble::rumbleText(captureTelemetry());}
}
