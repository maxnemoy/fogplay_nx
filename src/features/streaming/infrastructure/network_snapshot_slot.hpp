#pragma once
#include <cstdint>
#include <mutex>
#include <type_traits>
namespace stream::telemetry::network {
enum class Publication : unsigned char { NotPublished, Live, FinalRetained, CaptureFailed, ClosedWithoutFinal };
template<class T> class SnapshotSlot {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T)<=16384);
public:
    struct Read {
        T value{};
        Publication publication=Publication::NotPublished;
        std::uint64_t epoch=0,generation=0,revision=0,capturedAtUs=0,ageUs=0;
        bool hasValue=false,ageKnown=false,stale=true;
    };
    void reset(std::uint64_t epoch,std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);state_={};state_.epoch=epoch;state_.generation=generation;
    }
    bool publish(std::uint64_t epoch,std::uint64_t generation,const T& value,std::uint64_t capturedAtUs,bool final=false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!current(epoch,generation)||closed()||(state_.hasValue&&capturedAtUs<state_.capturedAtUs))return false;
        state_.value=value;state_.capturedAtUs=capturedAtUs;state_.hasValue=true;++state_.revision;
        state_.publication=final?Publication::FinalRetained:Publication::Live;return true;
    }
    bool failed(std::uint64_t epoch,std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!current(epoch,generation)||closed())return false;
        state_.publication=Publication::CaptureFailed;return true;
    }
    void closeWithoutFinal(std::uint64_t epoch,std::uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!current(epoch,generation)||closed())return;
        state_.value={};state_.hasValue=false;state_.publication=Publication::ClosedWithoutFinal;
    }
    Read read(std::uint64_t nowUs,std::uint64_t staleAfterUs) const {
        std::lock_guard<std::mutex> lock(mutex_);auto out=state_;
        out.ageKnown=out.hasValue&&nowUs>=out.capturedAtUs;
        out.ageUs=out.ageKnown?nowUs-out.capturedAtUs:0;
        out.stale=!out.ageKnown||out.ageUs>staleAfterUs||out.publication==Publication::CaptureFailed;
        return out;
    }
private:
    bool current(std::uint64_t epoch,std::uint64_t generation)const{return state_.epoch!=0&&state_.epoch==epoch&&state_.generation==generation;}
    bool closed()const{return state_.publication==Publication::FinalRetained||state_.publication==Publication::ClosedWithoutFinal;}
    mutable std::mutex mutex_;
    Read state_{};
};
}
