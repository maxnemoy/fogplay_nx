#pragma once
#include "features/playstation/domain/sensors.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace fog::playstation {
struct TouchTrace {
    struct Contact { bool active=false; std::uint32_t id=0; int startX=0,startY=0,x=0,y=0; std::uint64_t began=0; };
    std::array<Contact,2> contacts{};
    std::uint64_t down=0,up=0,moves=0,swipes=0,frames=0,suppressedFrames=0;
    int lastDx=0,lastDy=0,lastX=0,lastY=0;
    std::uint64_t lastDurationMs=0;
    std::uint32_t lastFinger=0;
    void observe(const PsSensorSnapshot& sample, std::uint64_t now, bool suppressed) {
        ++frames; if (suppressed) ++suppressedFrames;
        for (auto& old : contacts) {
            if (!old.active) continue;
            const auto found=std::find_if(sample.touches.begin(),sample.touches.end(),[&old](const PsTouchContact& point) {
                return point.active && point.fingerId==old.id;
            });
            if (found==sample.touches.end()) {
                ++up; lastFinger=old.id; lastDx=old.x-old.startX; lastDy=old.y-old.startY;
                lastDurationMs=now>=old.began ? now-old.began : 0;
                if (std::abs(lastDx)>=60 || std::abs(lastDy)>=60) ++swipes;
                old.active=false;
            }
        }
        for (const auto& point : sample.touches) {
            if (!point.active) continue;
            auto found=std::find_if(contacts.begin(),contacts.end(),[&point](const Contact& old) {return old.active && old.id==point.fingerId;});
            if (found==contacts.end()) {
                found=std::find_if(contacts.begin(),contacts.end(),[](const Contact& old){return !old.active;});
                if (found==contacts.end()) continue;
                *found={true,point.fingerId,point.screenX,point.screenY,point.screenX,point.screenY,now}; ++down;
            } else if (found->x!=point.screenX || found->y!=point.screenY) ++moves;
            found->x=point.screenX; found->y=point.screenY;
            lastFinger=point.fingerId; lastX=point.x; lastY=point.y;
        }
    }
};
}
