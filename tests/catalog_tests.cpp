#include "features/catalog/domain/catalog.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int assertions = 0;
void require(bool condition, const char* label) {
    ++assertions;
    if (!condition) throw std::runtime_error(label);
}
nlohmann::json page(int first, int count = fog::Catalog::PageSize) {
    auto result = nlohmann::json::array();
    for (int id = first; id < first + count; ++id)
        result.push_back({{"id", id}, {"name", "Game " + std::to_string(id)}, {"images", {"public-cover"}}});
    return result;
}
fog::Catalog::Request begin(fog::Catalog& catalog, bool manual = false) {
    const auto request = catalog.begin(manual);
    require(request.has_value(), "request available");
    return request.value();
}
void expectMalformed(fog::Catalog& catalog, const fog::Catalog::Request& request,
                     const nlohmann::json& response) {
    bool rejected = false;
    try { catalog.complete(request, response); } catch (const std::exception&) { rejected = true; }
    require(rejected, "malformed page rejected");
}
}
int main() {
    try {
        using fog::Catalog;
        using Direction = Catalog::Direction;
        Catalog catalog;
        require(!catalog.begin(), "catalog waits until startup auth recovery chooses catalog screen");
        catalog.reset("");
        auto first = begin(catalog);
        require(first.page == 1 && first.query.empty() && Catalog::PageSize == 16, "first request loads four rows");
        require(!catalog.begin() && catalog.busy(), "one request while loading");
        require(catalog.complete(first, page(1)), "initial completion");
        require(catalog.items().size() == 16 && !catalog.ended(), "full first page can continue");
        require(catalog.items().front().at("images").at(0) == "public-cover", "keep cover fields verbatim");
        catalog.focus(7);
        require(catalog.first() == 0 && !catalog.begin(), "no prefetch before third row including last column");
        catalog.move(Direction::Down);
        require(catalog.selected() == 11 && catalog.first() == 4, "third row scrolls one row");
        auto second = begin(catalog);
        require(second.page == 2, "prefetch page two on third row");
        catalog.move(Direction::Down);
        require(catalog.selected() == 15 && catalog.first() == 8, "navigation remains active while fetching");
        require(!catalog.begin() && !catalog.retry(), "no duplicate in-flight request or retry");
        require(catalog.complete(second, page(17)), "append second page");
        require(catalog.items().size() == 32 && catalog.selected() == 15 && catalog.first() == 8,
            "append preserves focus and scroll even after navigation");
        require(!catalog.complete(second, page(17)) && catalog.items().size() == 32, "replayed completion ignored");
        require(!catalog.begin(), "append does not immediately fetch without reaching threshold");
        catalog.focus(23);
        require(!catalog.begin(), "eight remaining rows starts at next row boundary");
        catalog.move(Direction::Down);
        auto third = begin(catalog);
        require(third.page == 3 && catalog.selected() == 27 && catalog.first() == 20, "repeated prefetch threshold");
        require(catalog.fail(third, "offline"), "network failure retained");
        require(catalog.items().size() == 32 && catalog.selected() == 27 && catalog.first() == 20,
            "failure preserves cards focus and scroll");
        for (int frame = 0; frame < 1000; ++frame) require(!catalog.begin(), "failure never retries per frame");
        catalog.focus(0);
        require(catalog.retry(), "explicit retry allowed after moving away");
        auto retry = begin(catalog, true);
        require(retry.page == third.page && retry.query == third.query, "retry same page and query");
        require(catalog.fail(retry, "offline again") && !catalog.begin(), "second failure stays gated");
        require(catalog.retry(), "retry after second failure");
        retry = begin(catalog, true);
        require(catalog.complete(retry, page(33, 3)) && catalog.ended(), "short page terminates");
        require(catalog.items().size() == 35 && !catalog.begin(true), "no requests after end even manual");
        catalog.focus(31);
        catalog.move(Direction::Down);
        require(catalog.selected() == 34 && catalog.first() == 28, "down from right column reaches partial last row");
        catalog.move(Direction::Down);
        require(catalog.selected() == 34, "down stops at last row");
        catalog.move(Direction::Up);
        require(catalog.selected() == 30 && catalog.first() == 28, "up within viewport retains first row");
        catalog.move(Direction::Up);
        require(catalog.selected() == 26 && catalog.first() == 24, "up scrolls earlier row");
        catalog.focus(0);
        require(catalog.move(Direction::Left) && catalog.selected() == 0, "left edge enters sidebar without losing focus");

        catalog.reset("racing",13);
        const auto genreOld=begin(catalog);
        require(genreOld.genreId==13 && genreOld.query=="racing","request captures genre plus query");
        require(catalog.complete(genreOld,page(1)),"filtered initial page");
        catalog.focus(8);
        const auto genreSecond=begin(catalog);
        require(genreSecond.genreId==13 && genreSecond.page==2,"genre remains on prefetch");
        catalog.reset("racing",173);
        const auto genreNew=begin(catalog);
        require(!catalog.complete(genreSecond,page(17)) && catalog.busy() && catalog.items().empty(),"old genre cannot append after filter change");
        require(!catalog.fail(genreSecond,"offline") && catalog.busy(),"stale genre failure ignored");
        auto forged=genreNew;forged.genreId=13;
        require(!catalog.complete(forged,page(1)) && catalog.busy(),"mismatched genre token ignored");
        require(catalog.complete(genreNew,page(20,2)),"new genre completion");
        require(fog::catalogGenres().size()==18 && fog::catalogGenres()[1].id==13,"web genres and exact IDs");
        catalog.reset("war");
        auto old = begin(catalog);
        catalog.reset("spider");
        auto newer = begin(catalog);
        require(!catalog.complete(old, page(1)) && catalog.items().empty() && catalog.busy(), "stale success cannot contaminate new search");
        require(!catalog.fail(old, "cancelled") && catalog.error().empty() && catalog.busy(), "stale error cannot clear current busy state");
        require(catalog.fail(newer, "new query offline"), "current generation fails");
        require(!catalog.fail(old, "old query offline") && catalog.error() == "new query offline", "stale failure cannot replace new query error");
        require(catalog.retry(), "new query retry");
        newer = begin(catalog, true);
        require(newer.page == 1 && newer.query == "spider", "search reset starts page one");
        require(catalog.complete(newer, page(100, 8)) && catalog.ended(), "short search first page terminates");
        require(catalog.items().size() == 8 && catalog.selected() == 0 && catalog.first() == 0, "query reset clears items focus and scroll");
        catalog.reset("spider");
        newer = begin(catalog);
        require(!catalog.complete(old, page(1)) && catalog.busy(), "generation guards queries that later repeat");
        require(catalog.complete(newer, nlohmann::json::array()) && catalog.ended(), "zero matches terminate without repeat requests");
        require(!catalog.begin(), "empty catalog does not loop loading");

        catalog.reset("");
        first = begin(catalog);
        require(catalog.complete(first, page(1)), "reset restores end state");
        catalog.focus(8);
        second = begin(catalog);
        require(catalog.complete(second, page(9)), "overlapping page accepted");
        require(catalog.items().size() == 24 && !catalog.ended(), "duplicates suppressed while new IDs continue");
        for (int i = 0; i < 24; ++i) require(catalog.items().at(i).at("id") == i + 1, "original order remains stable");
        catalog.focus(16);
        third = begin(catalog);
        require(catalog.complete(third, page(9)) && catalog.ended() && catalog.items().size() == 24,
            "fully repeated page ends without infinite prefetch");

        catalog.reset("");
        first = begin(catalog);
        auto duplicatePage = page(1);
        duplicatePage.at(15) = duplicatePage.at(0);
        require(catalog.complete(first, duplicatePage) && catalog.items().size() == 15, "deduplicate within one page");
        catalog.focus(8);
        second = begin(catalog);
        const auto size = catalog.items().size();
        expectMalformed(catalog, second, page(16, 17));
        expectMalformed(catalog, second, nlohmann::json::object());
        auto bad = page(16);
        bad.at(15).at("id") = "invalid";
        expectMalformed(catalog, second, bad);
        require(catalog.items().size() == size && catalog.busy(), "malformed page cannot partially append");
        require(catalog.fail(second, "malformed") && catalog.retry(), "malformed page uses same retry path");
        retry = begin(catalog);
        require(retry.page == second.page && catalog.complete(retry, page(16, 1)), "malformed retry did not advance page or remember rejected IDs");
        require(catalog.items().size() == 16 && catalog.ended(), "recovered short response terminates");
        std::cout << "catalog tests: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "catalog test failed: " << error.what() << '\n';
        return 1;
    }
}
