#include "features/playstation/infrastructure/catalog.hpp"
#include "features/playstation/infrastructure/availability.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int assertions = 0;
void require(bool value, const char* label) {
    ++assertions;
    if (!value) throw std::runtime_error(label);
}
std::string card(int id = 11573, std::string tariff = "51733", std::string price = "22",
                 std::string name = "3A7X47") {
    return "<div class='card card-outside computer-" + std::to_string(id) + "'>"
        "<div class='card__header'><p class='card__title'><span>Сервер: </span>" + name + "</p></div>"
        "<ul><li class='card__system__item--cpu'><span>CPU:</span>"
        "<span class='card__system__value'>AMD &amp; &#x52;yzen &#039;9&#039;</span></li>"
        "<li class='card__system__item--gpu'><span class='card__system__value'>RTX &lt;3070&gt;</span></li>"
        "<li class='card__system__item--ram'><span class='card__system__value'>16&nbsp;Gb</span></li></ul>"
        "<div class='card__price'>" + price + " ₽/час</div>"
        "<div class='card card-inside'><div><a class='button js-play' href='/platform/comp/" + std::to_string(id) +
        "/?tariff_id=" + tariff + "&amp;steamacc=0&amp;isPromo=1'>Играть</a></div></div></div>";
}
void tests() {
    require(fog::isPlayStationComputerId(11573) && fog::isPlayStationComputerId(12921) &&
        fog::isPlayStationComputerId(11575) && fog::isPlayStationComputerId(13025) &&
        fog::isPlayStationComputerId(7705) && fog::isPlayStationComputerId(5632) && fog::isPlayStationComputerId(5510), "exact whitelist IDs");
    require(!fog::isPlayStationComputerId(11574) && !fog::isPlayStationComputerId(0), "reject unknown IDs");
    const auto parsed = fog::parsePlayStationComputers("<!-- <div class='computer-11573'> -->" + card());
    require(parsed.size() == 3, "all three computers retained");
    require(parsed[0]["id"] == 11573 && parsed[1]["id"] == 12921 && parsed[2]["id"] == 11575, "stable mapping");
    require(parsed[0]["name"] == "3A7X47" && parsed[1]["name"] == "3A7X49" && parsed[2]["name"] == "3A7X48", "stable names");
    require(parsed[0]["listed"] == true && parsed[0]["launchable"] == true, "valid listing launchable");
    require(parsed[0]["tariff_id"] == 51733 && parsed[0]["price"] == 22, "dynamic price and tariff");
    require(parsed[0]["cpu"] == "AMD & Ryzen '9'" && parsed[0]["gpu"] == "RTX <3070>" && parsed[0]["ram"] == "16 Gb", "entities and nested fields");
    require(parsed[0]["availability"] == "unknown", "listing never proves free status");
    require(parsed[1]["listed"] == false && parsed[1]["launchable"] == false && parsed[1]["price"].is_null() &&
        parsed[1]["tariff_id"] == 0 && parsed[1]["availability"] == "unknown", "absent listing fail closed");
    for (const auto& tariff : {"", "0", "-1", "1x", "9999999999999999999999"}) {
        const auto invalid = fog::parsePlayStationComputers(card(11573, tariff));
        require(invalid[0]["listed"] == true && invalid[0]["launchable"] == false, "invalid tariff rejected");
    }
    for (const auto& price : {"", "0", "-2", "NaN", "22junk", "2.2.2", "22.", "22,", "1000001"})
        require(fog::parsePlayStationComputers(card(11573, "51733", price))[0]["launchable"] == false, "invalid price rejected");
    require(fog::parsePlayStationComputers(card(11573, "90000", "29,50"))[0]["price"] == 29.5, "updated decimal tariff");
    require(fog::parsePlayStationComputers(card() + card())[0]["launchable"] == false, "duplicate exact PC rejected");
    auto mismatch = card();
    const auto href = mismatch.find("/platform/comp/11573/");
    mismatch.replace(href, 21, "/platform/comp/12921/");
    require(fog::parsePlayStationComputers(mismatch)[0]["launchable"] == false, "other PC tariff never reused");
    auto badName = card();
    badName.replace(badName.find("3A7X47"), 6, "3A7X49");
    require(fog::parsePlayStationComputers(badName)[0]["launchable"] == false, "name identity checked");
    require(fog::parsePlayStationComputers("<html>maintenance</html>")[0]["launchable"] == false, "layout failure fail closed");
    require(fog::parsePlayStationComputers(card(99999))[0]["listed"] == false, "unrelated PCs ignored");
    const auto fourth = card(13025, "48377", "22", "F4BX8");
    const auto karahun = fog::parsePlayStationComputers(fourth, "Karahun");
    require(karahun.size() == 1 && karahun[0]["id"] == 13025 && karahun[0]["name"] == "F4BX8",
        "fourth computer source identity");
    require(karahun[0]["listed"] == true && karahun[0]["launchable"] == true &&
        karahun[0]["tariff_id"] == 48377, "fourth source dynamic offer");
    require(fog::parsePlayStationComputers(card(13025, "60001", "31", "F4BX8"), "Karahun")[0]["tariff_id"] == 60001,
        "fourth tariff never hardcoded");
    require(fog::parsePlayStationComputers(fourth + fourth, "Karahun")[0]["launchable"] == false,
        "fourth duplicate rejected");
    require(fog::parsePlayStationComputers(card(13025), "Karahun")[0]["launchable"] == false,
        "fourth name mismatch rejected");
    require(fog::parsePlayStationComputers(card(), "Karahun")[0]["listed"] == false,
        "original PC never accepted through fourth source");
    require(fog::parsePlayStationComputers(fourth)[0]["listed"] == false,
        "fourth PC never accepted through original source");
    require(fog::parsePlayStationComputers(fourth, "unknown").empty(), "unknown source rejected");
    const auto missingFourth = fog::parsePlayStationComputers("<html>maintenance</html>", "Karahun");
    require(missingFourth[0]["listed"] == false && missingFourth[0]["launchable"] == false &&
        missingFourth[0]["tariff_id"] == 0 && missingFourth[0]["price"].is_null() &&
        missingFourth[0]["availability"] == "unknown", "fourth missing listing fail closed");

    const auto crazy = fog::parsePlayStationComputers(card(7705, "60000", "35", "61EFX15") +
        card(5632, "60001", "36", "61EFX4") + card(5510, "60002", "37", "61EFX3"), "CrazyGames");
    require(crazy.size() == 3 && crazy[0]["id"] == 7705 && crazy[1]["id"] == 5632 &&
        crazy[2]["id"] == 5510, "CrazyGames stable identities");
    require(crazy[0]["launchable"] == true && crazy[1]["launchable"] == true && crazy[2]["launchable"] == true,
        "CrazyGames fresh offers accepted");
    require(crazy[0]["tariff_id"] == 60000 && crazy[1]["price"] == 36,
        "CrazyGames tariffs never taken from static merchant discovery");
    require(fog::parsePlayStationComputers(card(7705, "60000", "35", "wrong"), "CrazyGames")[0]["launchable"] == false,
        "CrazyGames wrong name fails closed");
    const auto offlineCrazy = fog::parsePlayStationComputers("<html></html>", "CrazyGames");
    require(offlineCrazy.size() == 3 && offlineCrazy[2]["launchable"] == false,
        "absent CrazyGames PCs kept without stale tariff");

    std::vector<std::string> requested;
    const auto combined = fog::fetchPlayStationComputers([&](std::string_view slug) {
        requested.emplace_back(slug);
        // Foreign-source duplicates must not disable or replace the correct source.
        return slug == "Karahun" ? fourth + card() + card() : card() + fourth + fourth;
    });
    require(requested == std::vector<std::string>{"MamkinZabiyaka", "Karahun", "CrazyGames"}, "unique known sources loaded once");
    require(combined.size() == 7 && combined[0]["id"] == 11573 && combined[1]["id"] == 12921 &&
        combined[2]["id"] == 11575 && combined[3]["id"] == 13025, "combined stable mapping without placeholders duplicated");
    require(combined[0]["launchable"] == true && combined[3]["launchable"] == true,
        "each source validates only its own computers");
    for (const auto failingSource : {"MamkinZabiyaka", "Karahun", "CrazyGames"}) {
        const auto partial = fog::fetchPlayStationComputers([&](std::string_view slug) {
            if (slug == failingSource) throw std::runtime_error("synthetic source failure");
            return slug == "Karahun" ? fourth : card();
        });
        const auto failedIndex = std::string_view(failingSource) == "CrazyGames" ? 4 :
            std::string_view(failingSource) == "Karahun" ? 3 : 0;
        const auto successfulIndex = failedIndex == 0 ? 3 : 0;
        require(partial.size() == 7 && partial[successfulIndex]["launchable"] == true,
            "independent source failure preserves valid offers");
        require(partial[failedIndex]["listed"] == false && partial[failedIndex]["launchable"] == false &&
            partial[failedIndex]["availability"] == "unknown" && partial[failedIndex]["tariff_id"] == 0 &&
            partial[failedIndex]["price"].is_null(), "failed source creates safe placeholders");
    }
    const auto malformed = fog::fetchPlayStationComputers([&](std::string_view slug) {
        return slug == "Karahun" ? std::string(8 * 1024 * 1024 + 1, 'x') : card();
    });
    require(malformed[0]["launchable"] == true && malformed[3]["launchable"] == false,
        "oversized source failure isolated");
    int promoCalls = 0;
    const auto pageTwoOffers = fog::loadPlayStationPromoPages("CrazyGames", [&](int page) {
        ++promoCalls;
        return page == 1 ? card(42) + "<a class='pagination__next' href='?slug=CrazyGames&amp;page=2'>Next</a>" :
            card(7705,"60000","35","61EFX15") + card(5632,"60001","36","61EFX4") + card(5510,"60002","37","61EFX3");
    });
    require(promoCalls == 2 && fog::parsePlayStationComputers(pageTwoOffers,"CrazyGames")[2]["launchable"] == true,
        "tariffs loaded from second promo page");
    promoCalls = 0;
    const auto partialPromo = fog::loadPlayStationPromoPages("CrazyGames", [&](int page) -> std::string {
        ++promoCalls;
        if (page == 2) throw std::runtime_error("page two outage");
        return card(7705,"60000","35","61EFX15") + "<a class='pagination__next' href='?page=2'>Next</a>";
    });
    const auto partialOffers = fog::parsePlayStationComputers(partialPromo,"CrazyGames");
    require(promoCalls == 2 && partialOffers[0]["launchable"] == true && partialOffers[1]["launchable"] == false,
        "failed later page retains only already verified tariffs");
    promoCalls = 0;
    fog::loadPlayStationPromoPages("CrazyGames", [&](int) { ++promoCalls;
        return card(42) + "<a class='pagination__next' href='?page=1'>loop</a>";
    });
    require(promoCalls == 1, "invalid promo next-page loop rejected");
    promoCalls = 0;
    fog::loadPlayStationPromoPages("CrazyGames", [&](int page) { ++promoCalls;
        return "<a class='pagination__next' href='?page=" + std::to_string(page+1) + "'>Next</a>";
    });
    require(promoCalls == 20, "promo pagination bounded");

    auto status = parsed;
    int calls = 0;
    fog::updatePlayStationAvailability(status, [&](int page, int pageSize) {
        ++calls; require(pageSize == 2, "status page size forwarded");
        return page == 1 ? fog::Json{{"computers", {{{"id", "123"}}, {{"id", 456}}}}} :
            fog::Json{{"computers", {{{"id", "11573"}}}}};
    }, 2);
    require(calls == 2 && status[0]["availability"] == "online" && status[1]["availability"] == "offline",
        "late-page online and absent after complete pagination");
    require(status[0]["launchable"] == parsed[0]["launchable"], "presence does not replace offer validation");
    for (int failure = 0; failure < 5; ++failure) {
        fog::updatePlayStationAvailability(status, [failure](int page, int) -> fog::Json {
            if (page == 1) return {{"computers", {{{"id", "11573"}}, {{"id", "42"}}}}};
            if (failure == 0) throw std::runtime_error("network outage");
            if (failure == 1) return {{"error", "maintenance"}};
            if (failure == 2) return {{"computers", {{{"id", "42"}}}}};
            if (failure == 3) return {{"computers", {{{"id", "bad"}}}}};
            return {{"computers", {{{"id", 1}}, {{"id", 2}}, {{"id", 3}}}}};
        }, 2);
        require(status[0]["availability"] == "online" && status[1]["availability"] == "unknown",
            "failure, malformed or moving pagination cannot mark absent PC offline");
    }
    fog::updatePlayStationAvailability(status, [](int, int) {
        return fog::Json{{"computers", {{{"id", 1}}, {{"id", 2}}}}};
    }, 2, 1);
    require(status[0]["availability"] == "unknown", "page limit is not end of catalogue");
    fog::updatePlayStationAvailability(status, [](int, int) { return fog::Json{{"computers", fog::Json::array()}}; });
    require(status[0]["availability"] == "offline", "valid empty listing offline");
    fog::updatePlayStationAvailability(status, [](int, int) -> fog::Json { throw std::runtime_error("outage"); });
    require(status[0]["availability"] == "unknown", "old offline state cleared on failed refresh");
    calls = 0;
    fog::updatePlayStationAvailability(status, [&](int, int) { ++calls;
        return fog::Json{{"computers", {{{"id", 11573}}, {{"id", 12921}}, {{"id", 11575}}}}};
    }, 3);
    require(calls == 1 && status[2]["availability"] == "online", "stop early once all targets found");

    bool allFailed = false;
    try {
        fog::fetchPlayStationComputers([](std::string_view) -> std::string {
            throw std::runtime_error("synthetic total outage");
        });
    } catch (const std::runtime_error&) { allFailed = true; }
    require(allFailed, "all source failures reported");
}
}
int main(int argc, char** argv) {
    try {
        tests();
        if (argc == 2 || argc == 3) {
            std::ifstream stream(argv[1]);
            if (!stream) throw std::runtime_error("Cannot read supplied public fixture");
            const std::string html((std::istreambuf_iterator<char>(stream)), {});
            std::cout << fog::parsePlayStationComputers(html, argc == 3 ? argv[2] : "MamkinZabiyaka").dump(2) << '\n';
        }
        std::cout << "PlayStation catalog tests PASS (" << assertions << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PlayStation catalog tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
