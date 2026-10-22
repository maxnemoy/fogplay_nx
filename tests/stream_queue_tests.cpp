#include "features/streaming/infrastructure/stream_queue.hpp"
#include "features/streaming/infrastructure/control_queue.hpp"
#include <cassert>
#include <iostream>
using namespace stream;
static Packet packet(Kind kind, std::size_t size = 8) { return {kind, std::vector<std::byte>(size)}; }
int main() {
    {
        ControlQueue q;
        for (unsigned i=0; i<10000; ++i)
            assert(q.push(std::to_string(i), ControlQueue::Mode::Gamepad, {}, i));
        assert(q.size() == 1 && q.front().payload == "9999" && q.bytes() == 4);
        assert(!q.stalled(14999) && q.stalled(15000)); // Updating latest state cannot extend the deadline.
        q.pop(100); assert(q.empty() && !q.stalled(999999));
        assert(q.push("ready", ControlQueue::Mode::Ordered, {}, 20000));
        assert(!q.stalled(34999) && q.stalled(35000));
    }
    {
        ControlQueue q;
        assert(q.push("position1", ControlQueue::Mode::MouseMove, {}, 1));
        assert(q.push("down", ControlQueue::Mode::Ordered, "mouse:0", 1));
        assert(q.push("position2", ControlQueue::Mode::MouseMove, {}, 1));
        assert(q.push("position3", ControlQueue::Mode::MouseMove, {}, 1));
        assert(q.push("up", ControlQueue::Mode::Release, "mouse:0", 1));
        for (const auto* expected : {"position1", "down", "position3", "up"}) {
            assert(q.front().payload == expected); q.pop(100);
        }
        assert(q.empty());
        for (std::size_t n=0; n<ControlQueue::MaxMessages-ControlQueue::ReleaseReserve-1; ++n)
            assert(q.push("command", ControlQueue::Mode::Ordered, {}, 1));
        assert(!q.push("command", ControlQueue::Mode::Ordered, {}, 1));
        assert(q.push("keyup", ControlQueue::Mode::Release, "key:Enter", 1));
        for (unsigned n=0;n<1000;++n) assert(q.push("keyup", ControlQueue::Mode::Release, "key:Enter", 1));
        assert(q.push("mouseup", ControlQueue::Mode::Release, "mouse:0", 1));
        assert(q.push("neutral", ControlQueue::Mode::Gamepad, {}, 1));
        assert(q.size() == ControlQueue::MaxMessages-ControlQueue::ReleaseReserve-1+3);
        while(q.size()>3) q.pop(100);
        assert(q.front().payload == "keyup"); q.pop(100); assert(q.front().payload == "mouseup"); q.pop(100);
        assert(q.front().payload == "neutral"); q.pop(100);
        assert(q.bytes() == 0 && q.rejected() == 1);
    }
    {
        ControlQueue q;
        const std::string large(64*1024, 'x');
        for(unsigned n=0;n<2;++n) assert(q.push(large, ControlQueue::Mode::Ordered, {}, 1));
        assert(!q.push(large, ControlQueue::Mode::Ordered, {}, 1));
        assert(q.push("up", ControlQueue::Mode::Release, "key:Tab", 1));
        assert(q.push(large, ControlQueue::Mode::Gamepad, {}, 1));
        assert(q.bytes() <= ControlQueue::MaxBytes && q.size() <= ControlQueue::MaxMessages);
    }
    {
        ControlQueue q;
        assert(q.push("initial", ControlQueue::Mode::Gamepad, {}, 0));
        assert(!q.push(std::string(ControlQueue::GamepadBytes+1, 'x'), ControlQueue::Mode::Gamepad, {}, 1));
        assert(q.size() == 1 && q.front().payload == "initial" && q.bytes() == 7 && q.stalled(15000));
        q.clear();
        assert(q.push("command", ControlQueue::Mode::Ordered, {}, 0));
        for (unsigned now=1000; now<=30000; now+=1000) {
            assert(q.push("command", ControlQueue::Mode::Ordered, {}, now));
            q.pop(now); // Never empty, but a successful send every second.
            assert(q.size() == 1 && !q.stalled(now+1000));
        }
        assert(!q.stalled(44999) && q.stalled(45000));
    }
    {
        ControlQueue q;
        for (std::size_t n=0; n<ControlQueue::MaxMessages-ControlQueue::ReleaseReserve-1; ++n)
            assert(q.push("command", ControlQueue::Mode::Ordered, {}, 0));
        for (unsigned n=0; n<ControlQueue::ReleaseReserve; ++n)
            assert(q.push("up", ControlQueue::Mode::Release, std::to_string(n), 0));
        assert(q.push("neutral", ControlQueue::Mode::Gamepad, {}, 0));
        assert(q.size() == ControlQueue::MaxMessages);
        assert(q.push("new neutral", ControlQueue::Mode::Gamepad, {}, 1));
        assert(q.size() == ControlQueue::MaxMessages);
        while(q.size()>1) q.pop(100);
        assert(q.front().payload == "new neutral");
    }
    {
        MediaQueue q;
        for (unsigned i = 0; i < 400; ++i) assert(q.push(packet(Kind::Audio), 1));
        assert(q.audioCount == MediaQueue::MaxAudio && q.videoCount == 0 && q.audioDropped == 388);
        assert(q.audioBytes == 96);
        assert(q.push(packet(Kind::Video), 1));
        for (unsigned i = 0; i < 100; ++i) assert(q.push(packet(Kind::Audio), 2));
        assert(q.videoCount == 1 && q.videoBytes == 8 && q.audioCount == 12);
        q.expireAudio(123);
        assert(q.audioCount == 0 && q.videoCount == 1 && q.audioBytes == 0);
        assert(q.age(Kind::Video, 123) == 122);
        assert(q.videoAgeHighMs == 122 && q.audioAgeHighMs == 121);
        Packet out; assert(q.pop(out) && out.kind == Kind::Video);
        assert(q.videoCount == 0 && q.videoBytes == 0 && !q.pop(out));
    }
    {
        MediaQueue q;
        for (unsigned i = 0; i < MediaQueue::MaxVideo; ++i) assert(q.push(packet(Kind::Video), 100));
        assert(!q.push(packet(Kind::Video), 100));
        assert(q.videoCount == MediaQueue::MaxVideo && q.videoHigh == MediaQueue::MaxVideo);
        assert(q.pressureCount == 1 && q.lastPressure.reason == MediaQueue::PressureReason::Count);
        assert(q.lastPressure.videoCount == 12 && q.lastPressure.videoBytes == 96 && q.lastPressure.incomingBytes == 8);
        assert(q.lastPressure.atMs == 100 && q.lastPressure.videoAgeMs == 0);
        assert(q.push(packet(Kind::Audio), 100));
        q.clearVideo();
        assert(q.videoCount == 0 && q.videoBytes == 0 && q.audioCount == 1);
        assert(q.push(packet(Kind::Video, MediaQueue::MaxVideoBytes), 101));
        assert(!q.push(packet(Kind::Video, 1), 101));
        assert(q.pressureCount == 2 && q.lastPressure.reason == MediaQueue::PressureReason::Bytes);
        assert(q.lastPressure.videoCount == 1 && q.lastPressure.videoBytes == MediaQueue::MaxVideoBytes);
        assert(q.lastPressure.audioCount == 1 && q.lastPressure.audioAgeMs == 1);
        assert(q.pressureText().find("pressureReason=bytes") != std::string::npos);
        q.clearVideo();
        assert(q.push(packet(Kind::Audio, MediaQueue::MaxAudioBytes + 1), 101));
        assert(q.audioRejected == 1 && q.audioCount == 1);
        assert(q.videoCount == 0 && q.videoBytes == 0);
        assert(q.bytesHigh == MediaQueue::MaxVideoBytes + 8);
    }
    {
        MediaQueue q;
        auto v = packet(Kind::Video); v.videoGeneration = 42;
        assert(q.push(std::move(v), 1000));
        assert(q.age(Kind::Video, 900) == 0); // Defensive nonnegative monotonic age.
        assert(q.age(Kind::Video, 1250) == MediaQueue::MaxVideoAgeMs);
        assert(q.age(Kind::Video, 1251) > MediaQueue::MaxVideoAgeMs);
        q.recordVideoPressure(MediaQueue::PressureReason::Age, 1251);
        assert(q.lastPressure.videoAgeMs == 251 && q.lastPressure.videoCount == 1);
        q.clearVideo();
        assert(q.lastPressure.videoCount == 1 && q.lastPressure.videoBytes == 8); // Captured before safe clear.
        assert(q.pressureText().find("pressureReason=age") != std::string::npos);
        v = packet(Kind::Video); v.videoGeneration = 42;
        assert(q.push(std::move(v), 1000));
        Packet out; assert(q.pop(out) && out.videoGeneration == 42 && out.enqueueMs == 1000);
        for (unsigned i=0; i<200; ++i) {
            assert(q.push(packet(Kind::Audio, 24000), 2000));
            assert(q.audioCount <= 12 && q.audioBytes <= MediaQueue::MaxAudioBytes);
        }
        assert(q.audioDropped > 0);
    }
    {
        MediaQueue q;
        for (unsigned i=0;i<6;++i) {
            auto v=packet(Kind::Video); v.timestamp=i;
            auto a=packet(Kind::Audio); a.timestamp=i+100;
            assert(q.push(std::move(v),100)); assert(q.push(std::move(a),100));
        }
        Packet out;
        for (unsigned i=0;i<6;++i) {
            assert(q.popAudio(out) && out.kind==Kind::Audio && out.timestamp==i+100);
        }
        assert(!q.popAudio(out) && q.audioCount==0 && q.audioBytes==0 && q.audioPriorityPopped==6);
        for (unsigned i=0;i<6;++i) assert(q.pop(out) && out.kind==Kind::Video && out.timestamp==i);
        assert(!q.pop(out) && q.videoBytes==0);
        for (unsigned i=0;i<13;++i) assert(q.push(packet(Kind::Audio),100));
        assert(q.audioCapacityDropped==1 && q.audioAgeDropped==0 && q.audioDropped==1);
        q.expireAudio(221);
        assert(q.audioAgeDropped==12 && q.audioCapacityDropped==1 && q.audioDropped==13);
        assert(!q.popAudio(out));
    }
    std::cout << "Media queue independent bounds, age, audio shedding, bytes, generation PASS\n";
}
