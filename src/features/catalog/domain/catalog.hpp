#pragma once
#include "features/catalog/domain/models.hpp"
#include <set>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace fog {
struct CatalogGenre { int id; const char* slug; };
// Public web taxonomy snapshot, 2026-10-20. The backend filters by taxonomy_id.
inline const std::array<CatalogGenre,18>& catalogGenres() {
    static const std::array<CatalogGenre,18> genres{{
        {173,"shooter"},{13,"racing"},{75,"horror"},
        {93,"fantasy"},{4,"action"},{21,"multiplayer"},
        {7,"indie"},{8,"strategy"},{9,"rpg"},{112,"anime"},
        {12,"simulation"},{15,"casual"},{20,"sport"},
        {94,"arcade"},{176,"survival"},{212,"platformer"},
        {309,"fighting"},{331,"mmorpg"}
    }};
    return genres;
}
// Main-thread catalog state. Request tokens also identify the query generation,
// so a cancelled request cannot append to a newer search or clear its error.
class Catalog {
public:
    static constexpr int PageSize = 16;
    static constexpr std::size_t Columns = 4;
    static constexpr std::size_t VisibleItems = 8;
    struct Request {
        std::uint64_t generation;
        int page;
        std::string query;
        int genreId = 0;
    };
    void reset(std::string query, int genreId = 0) {
        if (genreId < 0 || genreId > 1000000) throw std::invalid_argument("Invalid genre ID");
        genreId_ = genreId;
        initialized_ = true;
        ++generation_;
        query_ = std::move(query);
        items_.clear();
        ids_.clear();
        selected_ = first_ = 0;
        nextPage_ = 1;
        busy_ = ended_ = false;
        error_.clear();
    }
    enum class Direction { Left, Right, Up, Down };
    // True means focus reached the left rail; navigation never starts a request.
    bool move(Direction direction) {
        if (items_.empty()) return direction == Direction::Left;
        auto selected = selected_;
        if (direction == Direction::Left) {
            if (selected % Columns == 0) return true;
            --selected;
        } else if (direction == Direction::Right && selected + 1 < items_.size()) ++selected;
        else if (direction == Direction::Up && selected >= Columns) selected -= Columns;
        else if (direction == Direction::Down && (selected / Columns + 1) * Columns < items_.size())
            selected = std::min(selected + Columns, items_.size() - 1);
        focus(selected);
        return false;
    }
    void focus(std::size_t selected) {
        selected_ = items_.empty() ? 0 : std::min(selected, items_.size() - 1);
        const auto row = selected_ / Columns;
        if (row < first_ / Columns) first_ = row * Columns;
        else if (row >= first_ / Columns + VisibleItems / Columns)
            first_ = (row - VisibleItems / Columns + 1) * Columns;
    }
    std::optional<Request> begin(bool manual = false) {
        if (!initialized_ || busy_ || ended_ || !error_.empty()) return std::nullopt;
        // Fetch when the focus enters the last two loaded rows. Initially this
        // is row three of four, and the same threshold applies after appends.
        if (!manual && !items_.empty() && items_.size() - selected_ / Columns * Columns > VisibleItems)
            return std::nullopt;
        busy_ = true;
        return Request{generation_, nextPage_, query_, genreId_};
    }
    bool complete(const Request& request, const Page<Game>& page) {
        if (!current(request)) return false;
        if (page.items.size() > PageSize)
            throw std::runtime_error("Unexpected catalog page format");
        auto ids = ids_;
        auto items = items_;
        for (const auto& item : page.items) {
            if (item.id.empty()) throw std::runtime_error("Unexpected catalog game ID");
            const auto& id = item.id;
            if (ids.insert(id).second) items.push_back(item);
        }
        // A full repeated page is terminal too: do not poll an API that ignores
        // page numbers indefinitely. Overlapping pages with new IDs continue.
        ended_ = page.items.size() < PageSize || items.size() == items_.size() || nextPage_ >= 1000000;
        items_ = std::move(items);
        ids_ = std::move(ids);
        ++nextPage_;
        busy_ = false;
        return true;
    }
    bool fail(const Request& request, std::string error) {
        if (!current(request)) return false;
        busy_ = false;
        error_ = error.empty() ? "Catalog request failed" : std::move(error);
        return true;
    }
    bool retry() {
        if (busy_ || error_.empty()) return false;
        error_.clear();
        return true;
    }
    const std::vector<Game>& items() const { return items_; }
    const std::string& query() const { return query_; }
    int genreId() const { return genreId_; }
    const std::string& error() const { return error_; }
    std::size_t selected() const { return selected_; }
    std::size_t first() const { return first_; }
    bool busy() const { return busy_; }
    bool ended() const { return ended_; }
private:
    bool current(const Request& request) const {
        return busy_ && request.generation == generation_ && request.page == nextPage_ && request.query == query_ && request.genreId == genreId_;
    }
    std::vector<Game> items_;
    std::set<EntityId> ids_;
    std::string query_, error_;
    std::uint64_t generation_ = 0;
    std::size_t selected_ = 0, first_ = 0;
    int nextPage_ = 1, genreId_ = 0;
    bool initialized_ = false, busy_ = false, ended_ = false;
};
}
