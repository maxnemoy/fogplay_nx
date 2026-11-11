#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <type_traits>
namespace stream::telemetry::network {
// One shared numeric journal feeds typed and legacy consumers; a peek is not
// delivery. Retire only after enqueue outcome, with uncertain/drop explicit.
template<class T>class EventJournal {
    static_assert(std::is_trivially_copyable_v<T>);
public:
    static constexpr std::size_t Capacity=32;
    struct Entry{T value{};std::uint64_t sequence=0,missed=0;};
    void push(const T& value){
        std::lock_guard<std::mutex> lock(mutex_);
        if(size_==Capacity){head_=(head_+1)%Capacity;--size_;++lost_;}
        entries_[(head_+size_)%Capacity]={value,++sequence_,lost_};++size_;
    }
    // Optional diagnostic producers must not evict recovery/control evidence.
    // Reserve and append share the lock, including concurrent RTP producers.
    bool tryPushReserved(const T& value,std::size_t reserve){
        std::lock_guard<std::mutex> lock(mutex_);
        if(reserve>=Capacity||size_>=Capacity-reserve)return false;
        entries_[(head_+size_)%Capacity]={value,++sequence_,lost_};++size_;return true;
    }
    bool peek(Entry& out)const{
        std::lock_guard<std::mutex> lock(mutex_);if(!size_)return false;
        out=entries_[head_];out.missed=lost_;return true;
    }
    std::size_t peekBatch(Entry* out,std::size_t capacity)const{
        if(!out||!capacity)return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto count=capacity<size_?capacity:size_;
        for(std::size_t i=0;i<count;++i){out[i]=entries_[(head_+i)%Capacity];out[i].missed=lost_;}
        return count;
    }
    bool retire(std::uint64_t sequence,bool admitted){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!size_||entries_[head_].sequence!=sequence)return false;
        head_=(head_+1)%Capacity;--size_;if(!admitted){++lost_;++uncertain_;}cursor_=sequence;return true;
    }
    void discardPending(){std::lock_guard<std::mutex> lock(mutex_);lost_+=size_;uncertain_+=size_;size_=0;head_=0;cursor_=sequence_;}
    std::uint64_t lost()const{std::lock_guard<std::mutex> lock(mutex_);return lost_;}
    std::uint64_t cursor()const{std::lock_guard<std::mutex> lock(mutex_);return cursor_;}
    std::uint64_t uncertain()const{std::lock_guard<std::mutex> lock(mutex_);return uncertain_;}
    std::size_t size()const{std::lock_guard<std::mutex> lock(mutex_);return size_;}
private:
    mutable std::mutex mutex_;std::array<Entry,Capacity> entries_{};std::size_t head_=0,size_=0;
    std::uint64_t sequence_=0,lost_=0,uncertain_=0,cursor_=0;
};
}
