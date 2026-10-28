#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
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
        return true;
    }
    std::optional<unsigned> preset() const {
        const auto target = std::find(current_.begin(), current_.end(), Size{1280, 720});
        if (target == current_.end()) return std::nullopt;
        return static_cast<unsigned>(target - current_.begin());
    }
    std::optional<unsigned> pending() const {
        if (attempts_ >= MaxRequests) return std::nullopt;
        for (std::size_t i = 0; i < attempts_; ++i)
            if (attempted_[i] == current_) return std::nullopt;
        return preset();
    }
    void submitted(bool accepted) {
        if (!pending()) return;
        attempted_[attempts_++] = current_;
        if (accepted) ++accepted_; else ++rejected_;
    }
    unsigned attempts() const { return attempts_; }
    unsigned accepted() const { return accepted_; }
    unsigned rejected() const { return rejected_; }
private:
    Table current_;
    std::array<Table, MaxRequests> attempted_;
    unsigned attempts_ = 0, accepted_ = 0, rejected_ = 0;
};
}
