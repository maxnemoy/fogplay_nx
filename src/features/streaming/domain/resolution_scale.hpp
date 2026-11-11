#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace stream {
// Resolution presets are positions in the host's advertised table. Preserve
// every position, including native resolution at zero; never invent an index.
class ResolutionScale {
public:
    using Size = std::array<int, 2>;
    using Table = std::vector<Size>;
    enum class PendingReason { NoTarget, DuplicateLast, Limit, Ready };
    static constexpr std::size_t MaxEntries = 64, MaxRequests = 3;
    static bool valid(const Table& table) {
        if (table.empty() || table.size() > MaxEntries) return false;
        for (std::size_t i = 0; i < table.size(); ++i) {
            const auto& size = table[i];
            if (i == 0 && size == Size{0, 0}) continue;
            if (size[0] <= 0 || size[0] > 16384 || size[1] <= 0 || size[1] > 16384) return false;
        }
        return true;
    }
    bool observe(Table table) {
        if (!valid(table) || table == current_) return false;
        current_ = std::move(table);
        ++revision_;
        return true;
    }
    std::optional<unsigned> preset() const {
        const auto target = std::find(current_.begin(), current_.end(), Size{1280, 720});
        if (target == current_.end()) return std::nullopt;
        return static_cast<unsigned>(target - current_.begin());
    }
    std::optional<unsigned> pending() const {
        if (attempts_ >= MaxRequests) return std::nullopt;
        // An intervening submitted table can change scaling (including native
        // preset zero). A returning table must then get its own bounded request.
        if (attempts_ && lastSubmitted_ == current_) return std::nullopt;
        return preset();
    }
    void submitted(bool accepted) {
        const auto value = pending();
        if (!value) return;
        lastSubmitted_ = current_;
        lastSubmittedPreset_ = value;
        ++attempts_;
        if (accepted) ++accepted_; else ++rejected_;
    }
    std::uint64_t tableRevision() const { return revision_; }
    std::size_t tableEntries() const { return current_.size(); }
    Size nativeSize() const { return current_.empty() ? Size{0, 0} : current_.front(); }
    PendingReason pendingState() const {
        if (pending()) return PendingReason::Ready;
        if (attempts_ >= MaxRequests) return PendingReason::Limit;
        if (!preset()) return PendingReason::NoTarget;
        return PendingReason::DuplicateLast;
    }
    const char* pendingReason() const {
        switch (pendingState()) {
        case PendingReason::NoTarget: return "no-target";
        case PendingReason::DuplicateLast: return "duplicate-last";
        case PendingReason::Limit: return "limit";
        case PendingReason::Ready: return "ready";
        }
        return "no-target";
    }
    std::optional<unsigned> lastSubmittedPreset() const { return lastSubmittedPreset_; }
    unsigned attempts() const { return attempts_; }
    unsigned accepted() const { return accepted_; }
    unsigned rejected() const { return rejected_; }
private:
    Table current_;
    Table lastSubmitted_;
    std::optional<unsigned> lastSubmittedPreset_;
    std::uint64_t revision_ = 0;
    unsigned attempts_ = 0, accepted_ = 0, rejected_ = 0;
};
}
