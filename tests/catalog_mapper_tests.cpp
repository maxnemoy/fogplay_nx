#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
#include <cassert>
#include "features/session/infrastructure/mts_operation.hpp"
#include <iostream>
#include <limits>
using Json=nlohmann::json;
namespace {
template<class Action> bool rejects(Action action) { try { action();return false; } catch(const std::exception&) { return true; } }
}
int main() {
    const auto first=fog::mtsEntityId(7);
    assert(first.provider==fog::ProviderId::Mts && first.value=="7" && fog::mtsNumericId(first)==7);
    assert(rejects([]{fog::mtsNumericId({fog::ProviderId::FogHub,"7"});}));
    assert(rejects([]{fog::mtsNumericId({fog::ProviderId::Mts,"2147483648"});}));
    const std::string url="https://fogplay.mts.ru/image/preview/example.webp";
    const auto game=fog::mapMtsGame(Json{{"id",7},{"name","Example"},{"description","Description"},{"metacritic_score",90},
        {"images",{nullptr,"https://evil.test/token",url,url}}});
    assert(game.id==first && game.name=="Example" && game.description=="Description" && game.rating=="90");
    assert(game.artworkUrls==std::vector<std::string>{url});
    assert(rejects([]{fog::mapMtsGame(Json{{"id","7"},{"name","Wrong game ID type"}});}));
    assert(rejects([]{fog::mapMtsGame(Json{{"id",std::numeric_limits<std::uint64_t>::max()},{"name","Overflow"}});}));
    const auto computer=fog::mapMtsComputer(Json{{"id","7"},{"tariff_id",8},{"price",29.50},{"name","Server"},
        {"availability","online"},{"listed",true},{"launchable",true},
        {"ping",{{"good",17},{"unknown",32767},{"bad",-1},{"string","3"}}}});
    assert(computer.id==first && computer.offerId.value=="8" && computer.hourlyPrice &&
        computer.hourlyPrice->minorUnits==2950 && computer.hourlyPrice->currency=="RUB");
    assert(computer.latencyRoutes.size()==1 && computer.latencyRoutes[0].routeKey=="good" && computer.latencyRoutes[0].serverToNodeMs==17);
    assert(computer.availability==fog::Availability::Online && fog::playStationLaunchable(computer));
    auto offline=computer;offline.availability=fog::Availability::Unknown;
    assert(fog::playStationLaunchable(offline)); // Public-list status is not a new paid-start gate.
    auto changed=computer;changed.offerId.value="9";assert(!fog::sameOffer(computer,changed));
    changed=computer;changed.price="30";assert(!fog::sameOffer(computer,changed));
    assert(fog::mapMtsComputer(Json{{"id",7}}).availability==fog::Availability::Unknown);
    assert(rejects([]{fog::mapMtsComputers(Json{{"computers",{Json{{"id",7}},Json{{"id","bad"}}}}});}));
    assert(rejects([]{fog::mapMtsRecommendations(Json{{"min_price",{Json{{"id",7}}}},{"optimum",Json::array()},{"max_power",Json::array()}});}));
    const auto history=fog::mapMtsHistory(Json::array({Json{{"comp_name","Server"},{"duration_minutes",12},{"sum",24.75}}}));
    assert(history.size()==1 && history[0].computerName=="Server" && history[0].durationMinutes=="12" &&
        history[0].total && history[0].total->minorUnits==2475 && history[0].startedAt.empty());
    assert(rejects([]{fog::mapMtsHistory(Json::array({nullptr}));}));
    bool classified=false;
    try { fog::mtsOperation([]{return fog::mapMtsGame(Json::object());}); }
    catch(const fog::RepositoryError& error) {classified=error.kind()==fog::RepositoryErrorKind::InvalidResponse;}
    assert(classified);
    std::cout<<"MTS catalog mapper: provider IDs, malformed pages, exact monetary units, image filtering, latency and offer semantics PASS\n";
}
