#include "core/constants/error_text.hpp"
#include "features/playstation/infrastructure/availability.hpp"
#include "core/network/http.hpp"
#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>

namespace fog {
using Json = nlohmann::json;
namespace {
int computerId(const Json& value) {
    if (value.is_number_integer()) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            return number > 0 && number <= std::numeric_limits<int>::max() ? static_cast<int>(number) : 0;
        }
        const auto number = value.get<std::int64_t>();
        return number > 0 && number <= std::numeric_limits<int>::max() ? static_cast<int>(number) : 0;
    }
    if (!value.is_string()) return 0;
    const auto& text = value.get_ref<const std::string&>();
    int id = 0;
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && id > 0 ? id : 0;
}
}
void updatePlayStationAvailability(std::vector<Computer>& computers, const PlayStationAvailabilityLoader& load,
                                  int pageSize, int maxPages, const PlayStationCatalogProgress& progress,
                                  const PlayStationCancellation& cancelled) {
    if (pageSize < 1 || pageSize > 100 || maxPages < 1) throw std::invalid_argument(fog::constants::error::PlayStation::InvalidCatalogBounds);
    std::set<int> seen;
    bool complete = false, consistent = true;
    for (auto& computer : computers) computer.availability = Availability::Unknown;
    for (int page = 1; page <= maxPages; ++page) {
        checkPlayStationCancellation(cancelled);
        try {
            const auto response = load(page, pageSize);
            checkPlayStationCancellation(cancelled);
            if (!response.is_object() || !response.contains("computers") || !response["computers"].is_array()) break;
            const auto& items = response["computers"];
            if (items.size() > static_cast<std::size_t>(pageSize)) break;
            std::set<int> pageIds;
            for (const auto& item : items) {
                const int id = item.is_object() && item.contains("id") ? computerId(item["id"]) : 0;
                if (!id || !pageIds.insert(id).second) throw std::runtime_error(fog::constants::error::PlayStation::InvalidCatalogIdentities);
            }
            // A shifting or repeated page cannot establish that an absent PC is offline.
            for (const auto id : pageIds)
                if (!seen.insert(id).second) consistent = false;
            for (auto& computer : computers)
                if (seen.count(mtsNumericId(computer.id))) computer.availability = Availability::Online;
            if (progress) progress(computers);
            if (items.size() < static_cast<std::size_t>(pageSize)) {
                complete = consistent;
                break;
            }
            bool allFound = !computers.empty();
            for (const auto& computer : computers)
                if (!seen.count(mtsNumericId(computer.id))) allFound = false;
            if (allFound) break;
        } catch (const std::exception&) { checkPlayStationCancellation(cancelled); break; }
    }
    checkPlayStationCancellation(cancelled);
    for (auto& computer : computers) {
        const int id = mtsNumericId(computer.id);
        computer.availability = seen.count(id) ? Availability::Online : complete ? Availability::Offline : Availability::Unknown;
    }
    if (progress) progress(computers);
}
void updatePlayStationAvailability(std::vector<Computer>& computers, HttpClient& http,
                                  const PlayStationCatalogProgress& progress,
                                  const PlayStationCancellation& cancelled) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(20);
    updatePlayStationAvailability(computers, [&http, deadline](int page, int pageSize) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline-std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error(fog::constants::error::PlayStation::CatalogStatusDeadlineReached);
        const auto response = http.request("GET", "/api/content/computer/",
            {{"app", "android:ru.mts.fogplay"}, {"version", "1.0.159"},
             {"page", std::to_string(page)}, {"pageSize", std::to_string(pageSize)}}, "", "",
            {static_cast<long>(std::min<std::int64_t>(remaining, 5000)),
             static_cast<long>(std::min<std::int64_t>(remaining, 3000))});
        if (response.status != 200) throw std::runtime_error(fog::constants::error::PlayStation::CatalogStatusUnavailable);
        return Json::parse(response.body);
    },100,50,progress,cancelled);
}
void updatePlayStationAvailability(std::vector<Computer>& computers) {
    HttpClient http;
    updatePlayStationAvailability(computers,http);
}
}
