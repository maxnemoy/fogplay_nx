#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
#include "features/catalog/infrastructure/artwork_url.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace fog {
namespace {
using Json = nlohmann::json;
int numericId(const Json& value) {
    if (value.is_number_unsigned()) {
        const auto id = value.get<std::uint64_t>();
        return id > 0 && id <= std::numeric_limits<int>::max() ? static_cast<int>(id) : 0;
    }
    if (value.is_number_integer()) {
        const auto id = value.get<std::int64_t>();
        return id > 0 && id <= std::numeric_limits<int>::max() ? static_cast<int>(id) : 0;
    }
    if (!value.is_string()) return 0;
    const auto& text = value.get_ref<const std::string&>(); int id = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && id > 0 ? id : 0;
}
std::string field(const Json& value,const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || found->is_null() || !found->is_primitive()) return {};
    return found->is_string() ? found->get<std::string>() : found->dump();
}
std::vector<Computer> computerArray(const Json& value) {
    if (!value.is_array()) throw std::runtime_error("Unexpected computer list format");
    std::vector<Computer> result; result.reserve(value.size());
    for (const auto& item : value) result.push_back(mapMtsComputer(item));
    return result;
}
}
EntityId mtsEntityId(int id) {
    if (id < 1) throw std::invalid_argument("Invalid MTS identity");
    return {ProviderId::Mts,std::to_string(id)};
}
int mtsNumericId(const EntityId& id) {
    if (id.provider != ProviderId::Mts) throw std::invalid_argument("MTS repository cannot use another provider identity");
    int value = 0; const auto parsed = std::from_chars(id.value.data(),id.value.data()+id.value.size(),value);
    if (parsed.ec != std::errc{} || parsed.ptr != id.value.data()+id.value.size() || value < 1)
        throw std::invalid_argument("Invalid MTS identity");
    return value;
}
std::optional<Money> mapMtsMoney(const Json& value) {
    if (!value.is_number()) return {};
    const auto amount = value.get<double>();
    if (!std::isfinite(amount) || amount < 0 || amount > 1000000000.0) return {};
    const auto minor = std::round(amount*100.0);
    if (std::abs(amount*100.0-minor) > 0.0001) return {};
    return Money{static_cast<std::int64_t>(minor),"RUB"};
}
Game mapMtsGame(const Json& value) {
    if (!value.is_object() || !value.contains("id") || !value.at("id").is_number_integer() ||
        !numericId(value.at("id")) || !value.contains("name") || !value.at("name").is_string())
        throw std::runtime_error("Unexpected catalog game format");
    Game result{mtsEntityId(numericId(value.at("id"))),value.at("name").get<std::string>(),
                field(value,"description"),field(value,"metacritic_score"),{}};
    const auto images = value.find("images");
    if (images != value.end() && images->is_array()) {
        for (std::size_t index=0; index<std::min<std::size_t>(images->size(),16) && result.artworkUrls.size()<maxArtworkCandidates; ++index) {
            const auto& item = images->at(index);
            if (!item.is_string()) continue;
            const auto url = item.get<std::string>();
            if (validArtworkUrl(url) && std::find(result.artworkUrls.begin(),result.artworkUrls.end(),url)==result.artworkUrls.end())
                result.artworkUrls.push_back(url);
        }
    }
    return result;
}
Computer mapMtsComputer(const Json& value) {
    if (!value.is_object() || !value.contains("id") || !numericId(value.at("id")))
        throw std::runtime_error("Unexpected computer format");
    Computer result; result.id = mtsEntityId(numericId(value.at("id")));
    result.name=field(value,"name"); result.cpu=field(value,"cpu"); result.gpu=field(value,"gpu");
    result.ram=field(value,"ram"); result.price=field(value,"price");
    const auto price=value.find("price"); if (price!=value.end()) result.hourlyPrice=mapMtsMoney(*price);
    const auto offer=value.find("tariff_id"); if (offer!=value.end() && numericId(*offer)) result.offerId=mtsEntityId(numericId(*offer));
    const auto availability=field(value,"availability");
    result.availability=availability=="online" ? Availability::Online : availability=="offline" ? Availability::Offline : Availability::Unknown;
    const auto listed=value.find("listed"),launchable=value.find("launchable");
    result.listed=listed!=value.end() && listed->is_boolean() && listed->get<bool>();
    result.launchable=launchable!=value.end() && launchable->is_boolean() && launchable->get<bool>();
    const auto routes=value.find("ping");
    if (routes!=value.end() && routes->is_object()) for (auto entry=routes->begin();entry!=routes->end();++entry) {
        if (!entry->is_number_integer()) continue;
        if (entry->is_number_unsigned() && entry->get<std::uint64_t>()>3000) continue;
        const auto ms=entry->get<std::int64_t>();
        if (ms>=0 && ms<=3000) result.latencyRoutes.push_back({entry.key(),static_cast<int>(ms)});
    }
    return result;
}
Page<Game> mapMtsGames(const Json& value) {
    if (!value.is_array()) throw std::runtime_error("Unexpected catalog format");
    Page<Game> result; result.items.reserve(value.size());
    for (const auto& item:value) result.items.push_back(mapMtsGame(item));
    return result;
}
Page<Computer> mapMtsComputers(const Json& value) {
    if (!value.is_object() || !value.contains("computers")) throw std::runtime_error("Unexpected computer list format");
    return {computerArray(value.at("computers"))};
}
AutomaticComputers mapMtsRecommendations(const Json& value) {
    if (!value.is_object()) throw std::runtime_error("Unexpected recommendations format");
    AutomaticComputers result;
    for (const auto* key:{"min_price","optimum","max_power"}) if (!value.contains(key))
        throw std::runtime_error("Unexpected recommendation category");
    result.cheapest=computerArray(value.at("min_price")); result.balanced=computerArray(value.at("optimum"));
    result.powerful=computerArray(value.at("max_power"));
    for (const auto* items:{&result.cheapest,&result.balanced,&result.powerful})
        for (const auto& item:*items) if (item.offerId.empty()) throw std::runtime_error("Unexpected recommendation offer");
    return result;
}
std::vector<HistoryEntry> mapMtsHistory(const Json& value) {
    if (!value.is_array() || value.size()>10000) throw std::runtime_error("Unexpected order history format");
    std::vector<HistoryEntry> result; result.reserve(value.size());
    for (const auto& item:value) {
        if (!item.is_object()) throw std::runtime_error("Unexpected order history entry");
        HistoryEntry entry{field(item,"comp_name"),field(item,"created"),field(item,"finished"),field(item,"duration_minutes"),field(item,"sum"),{}};
        const auto total=item.find("sum"); if (total!=item.end()) entry.total=mapMtsMoney(*total);
        result.push_back(std::move(entry));
    }
    return result;
}
}
