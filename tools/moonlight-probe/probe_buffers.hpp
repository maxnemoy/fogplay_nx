#pragma once
#include <Limelight.h>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace moonlight_probe {
// Copy while the pull-renderer frame handle is owned; never retain core pointers.
inline bool copyFrame(const DECODE_UNIT& unit, std::vector<std::byte>& target) {
    constexpr std::size_t limit=8*1024*1024;
    if(unit.fullLength<=0 || static_cast<std::size_t>(unit.fullLength)>limit) return false;
    target.clear(); target.reserve(unit.fullLength);
    std::size_t entries=0;
    for(auto* entry=unit.bufferList;entry;entry=entry->next) {
        if(++entries>4096 || !entry->data || entry->length<=0 ||
            static_cast<std::size_t>(entry->length)>static_cast<std::size_t>(unit.fullLength)-target.size()) return false;
        const auto begin=target.size(); target.resize(begin+entry->length);
        std::memcpy(target.data()+begin,entry->data,entry->length);
    }
    return target.size()==static_cast<std::size_t>(unit.fullLength);
}
class AudioQueue {
public:
    void push(const char* data,int size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!data || size<=0 || size>4096) {++lost_;return;}
        // Accept only 5/10 ms stereo in audioInit; at most 200 ms wait here.
        if(packets_.size()>=20) {lost_+=packets_.size();packets_.clear();}
        std::vector<std::byte> packet(static_cast<std::size_t>(size));
        std::memcpy(packet.data(),data,size); packets_.push_back(std::move(packet));
    }
    std::deque<std::vector<std::byte>> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::deque<std::vector<std::byte>> result; result.swap(packets_); return result;
    }
    unsigned lost() const {std::lock_guard<std::mutex> lock(mutex_);return lost_;}
private:
    mutable std::mutex mutex_;
    std::deque<std::vector<std::byte>> packets_;
    unsigned lost_=0;
};
}
