#pragma once
#include "features/playstation/domain/sensors.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include "features/streaming/domain/remote_input.hpp"

namespace fog::playstation {
class NativeInput {
public:
    using Emit=std::function<bool(const fog::remote::Input&)>;
    struct Counters { std::uint64_t down=0,move=0,up=0,accel=0,gyro=0,rejected=0,taps=0,suppressed=0,resetUpQueued=0,resetRejected=0; };
    static constexpr int Down=0x656, Move=0x657, Up=0x658;
    static constexpr float Gravity=9.80665f;
    static fog::remote::Input touch(int type,int finger,float x,float y) {
        const auto phase=type==Down ? fog::remote::TouchPhase::Down : type==Move ? fog::remote::TouchPhase::Move : type==Up ? fog::remote::TouchPhase::Up : static_cast<fog::remote::TouchPhase>(-1);
        return fog::remote::Touch{phase,finger,x,y};
    }
    static fog::remote::Input sensor(int kind,const std::array<float,3>& xyz) {
        return fog::remote::Sensor{kind==1 ? fog::remote::MotionSensor::Accelerometer : kind==2 ? fog::remote::MotionSensor::Gyroscope : static_cast<fog::remote::MotionSensor>(-1),xyz};
    }
    bool initializeContacts(const Emit& emit) {
        // The host defaults untouched DS4 slots to active (0,0).
        while (resetContacts_<contacts_.size()) {
            if (!send(touch(Up,static_cast<int>(resetContacts_),0,0),emit)) {
                ++counters_.resetRejected;
                return false;
            }
            ++resetContacts_;++counters_.resetUpQueued;
        }
        return true;
    }
    bool contactsInitialized() const { return resetContacts_==contacts_.size(); }
    void update(const PsSensorSnapshot& sample,std::uint64_t now,bool allowed,const Emit& emit) {
        if (!allowed) { ++counters_.suppressed; suspend(emit); return; }
        if (!initializeContacts(emit)) return;
        for (std::size_t i=0;i<contacts_.size();++i) {
            auto& old=contacts_[i]; const auto& point=sample.touches[i];
            const bool present=sample.touchValid && point.active;
            if (old.active && (!present || point.fingerId!=old.id)) {
                const bool tap=sample.touchValid && !sample.touchStale && !present && old.tap &&
                    now>=old.began && now-old.began<=250 && old.maxTravel<=12;
                if (!release(i,emit)) continue;
                if (tap) { if (pendingClicks_<4) ++pendingClicks_; else ++counters_.rejected; ++counters_.taps; }
            }
            if (!present) continue;
            const float x=std::clamp(point.screenX/1279.0f,0.0f,1.0f);
            const float y=std::clamp(point.screenY/719.0f,0.0f,1.0f);
            if (!old.active) {
                if (send(touch(Down,static_cast<int>(i),x,y),emit)) {
                    old={true,sample.touchCount==1,point.fingerId,point.screenX,point.screenY,0,x,y,x,y,now,now};
                    ++counters_.down;
                }
                continue;
            }
            old.observedX=x;old.observedY=y;
            if (sample.touchCount>1) old.tap=false;
            old.maxTravel=std::max(old.maxTravel,std::max(std::abs(static_cast<int>(point.screenX)-old.startX),std::abs(static_cast<int>(point.screenY)-old.startY)));
            if ((x!=old.x || y!=old.y) && now-old.lastMove>=30 && send(touch(Move,static_cast<int>(i),x,y),emit)) {
                old.x=x;old.y=y;old.lastMove=now;++counters_.move;
            }
        }
        if (!sample.motion.valid) { neutralSensors(emit); return; }
        if (now-lastMotionMs_<30 || (motionObserved_ && sample.motion.sampleNumber==lastMotionSample_ && sample.motion.sourceIndex==lastMotionSource_)) return;
        const auto& m=sample.motion;
        std::array<float,3> acceleration{};
        for (std::size_t i=0;i<3;++i) acceleration[i]=m.psAccelerationG[i]*Gravity;
        bool valid=true;
        for (std::size_t i=0;i<3;++i) valid=valid && std::isfinite(acceleration[i]) && std::isfinite(m.psGyroRadiansPerSecond[i]);
        if (!valid) { ++counters_.rejected; neutralSensors(emit); return; }
        const bool accel=send(sensor(1,acceleration),emit);
        const bool gyro=send(sensor(2,m.psGyroRadiansPerSecond),emit);
        if (accel) { accelActive_=true; ++counters_.accel; }
        if (gyro) { gyroActive_=true; ++counters_.gyro; }
        if (accel && gyro) { lastMotionSample_=m.sampleNumber;lastMotionSource_=m.sourceIndex;motionObserved_=true; }
        lastMotionMs_=now;
    }
    void suspend(const Emit& emit) {
        clickUntil_=clickGapUntil_=0;pendingClicks_=0;
        for (std::size_t i=0;i<contacts_.size();++i) { contacts_[i].tap=false; release(i,emit); }
        neutralSensors(emit);
        motionObserved_=false;
    }
    bool click(std::uint64_t now,bool submittedPressed=false) {
        if (clickUntil_!=0) {
            if (now<clickUntil_) return true;
            clickUntil_=0;clickGapUntil_=now+30;
            return false;
        }
        if (pendingClicks_!=0 && now>=clickGapUntil_ && !submittedPressed) {
            --pendingClicks_;clickUntil_=now+80;return true;
        }
        return false;
    }
    const Counters& counters() const {return counters_;}
private:
    struct Contact { bool active=false,tap=false;std::uint32_t id=0;int startX=0,startY=0,maxTravel=0;float x=0,y=0,observedX=0,observedY=0;std::uint64_t began=0,lastMove=0; };
    bool send(const fog::remote::Input& input,const Emit& emit) { if (emit(input)) return true; ++counters_.rejected;return false; }
    bool release(std::size_t finger,const Emit& emit) {
        auto& contact=contacts_[finger];
        if (!contact.active) return true;
        if (!send(touch(Up,static_cast<int>(finger),contact.observedX,contact.observedY),emit)) return false;
        contact={};++counters_.up;return true;
    }
    void neutralSensors(const Emit& emit) {
        if (gyroActive_ && send(sensor(2,{0,0,0}),emit)) {gyroActive_=false;++counters_.gyro;}
        if (accelActive_ && send(sensor(1,{0,Gravity,0}),emit)) {accelActive_=false;++counters_.accel;}
    }
    std::array<Contact,2> contacts_{};
    bool gyroActive_=false,accelActive_=false,motionObserved_=false;
    unsigned pendingClicks_=0;
    std::size_t resetContacts_=0;
    std::uint64_t clickUntil_=0,clickGapUntil_=0,lastMotionMs_=0,lastMotionSample_=0;
    int lastMotionSource_=-1;
    Counters counters_;
};
}
