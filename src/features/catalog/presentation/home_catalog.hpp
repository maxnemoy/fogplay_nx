#pragma once
#include "features/catalog/domain/catalog.hpp"
#include <algorithm>
#include <cstddef>

namespace fog {
// Optional unfiltered home strip. Catalog continues owning its original grid,
// focus/scroll and request thresholds; search always uses that grid directly.
class HomeCatalogNavigation {
public:
    static constexpr std::size_t VisibleRecent = 4;
    void reset(std::size_t count, bool unfiltered) {
        recent_ = unfiltered && count > 0; selected_ = first_ = 0;
    }
    bool available(const Catalog& catalog, std::size_t count) const { return catalog.query().empty() && catalog.genreId() == 0 && count > 0; }
    bool recentFocused(const Catalog& catalog, std::size_t count) const { return available(catalog,count) && recent_; }
    bool stripVisible(const Catalog& catalog, std::size_t count) const {
        return available(catalog,count) && (recent_ || catalog.selected() < Catalog::Columns);
    }
    // True enters the existing sidebar; callers retain this focus for return.
    bool move(Catalog& catalog, std::size_t count, Catalog::Direction direction) {
        if (recentFocused(catalog,count)) {
            selected_ = std::min(selected_, count-1);
            if (direction == Catalog::Direction::Left) {
                if (!selected_) return true;
                --selected_;
            } else if (direction == Catalog::Direction::Right && selected_+1<count) ++selected_;
            else if (direction == Catalog::Direction::Down && !catalog.items().empty()) {
                recent_ = false;
                catalog.focus(std::min(selected_,Catalog::Columns-1));
            }
            if (selected_ < first_) first_ = selected_;
            else if (selected_ >= first_+VisibleRecent) first_ = selected_-VisibleRecent+1;
            return false;
        }
        if (direction == Catalog::Direction::Up && available(catalog,count) && catalog.selected()<Catalog::Columns) {
            recent_ = true;
            selected_ = std::min(selected_,count-1);
            return false;
        }
        return catalog.move(direction);
    }
    std::size_t selected() const { return selected_; }
    std::size_t first() const { return first_; }
    void launched() { selected_ = first_ = 0; }
private:
    bool recent_ = false;
    std::size_t selected_ = 0, first_ = 0;
};
}
