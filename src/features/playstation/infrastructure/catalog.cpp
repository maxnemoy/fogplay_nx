#include "core/constants/error_text.hpp"
#include "features/playstation/infrastructure/catalog.hpp"
#include "features/playstation/infrastructure/availability.hpp"
#include "core/network/http.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fog {
namespace {
struct Tag {
    std::size_t start = 0, end = 0;
    std::string_view name, attributes;
    bool closing = false;
};
bool space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool nextTag(std::string_view html, std::size_t& cursor, Tag& tag) {
    while (true) {
        const auto start = html.find('<', cursor);
        if (start == html.npos) return false;
        if (html.substr(start, 4) == "<!--") {
            const auto end = html.find("-->", start + 4);
            cursor = end == html.npos ? html.size() : end + 3;
            continue;
        }
        std::size_t end = start + 1;
        char quote = 0;
        for (; end < html.size(); ++end) {
            const char c = html[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>') break;
        }
        if (end == html.size()) { cursor = end; return false; }
        cursor = end + 1;
        auto nameStart = start + 1;
        const bool closing = html[nameStart] == '/';
        if (closing) ++nameStart;
        auto nameEnd = nameStart;
        while (nameEnd < end && !space(html[nameEnd]) && html[nameEnd] != '/') ++nameEnd;
        if (nameStart == nameEnd) continue;
        tag = {start, cursor, html.substr(nameStart, nameEnd - nameStart),
               html.substr(nameEnd, end - nameEnd), closing};
        return true;
    }
}
std::string_view attribute(const Tag& tag, std::string_view key) {
    auto text = tag.attributes;
    std::size_t p = 0;
    while (p < text.size()) {
        while (p < text.size() && (space(text[p]) || text[p] == '/')) ++p;
        const auto begin = p;
        while (p < text.size() && !space(text[p]) && text[p] != '=') ++p;
        const auto name = text.substr(begin, p - begin);
        while (p < text.size() && space(text[p])) ++p;
        if (p == text.size() || text[p] != '=') continue;
        ++p;
        while (p < text.size() && space(text[p])) ++p;
        if (p == text.size()) break;
        const char quote = (text[p] == '\'' || text[p] == '"') ? text[p++] : 0;
        const auto valueBegin = p;
        while (p < text.size() && (quote ? text[p] != quote : !space(text[p]))) ++p;
        const auto value = text.substr(valueBegin, p - valueBegin);
        if (quote && p < text.size()) ++p;
        if (name == key) return value;
    }
    return {};
}
bool classHas(const Tag& tag, std::string_view wanted) {
    auto classes = attribute(tag, "class");
    while (!classes.empty()) {
        while (!classes.empty() && space(classes.front())) classes.remove_prefix(1);
        const auto end = classes.find_first_of(" \t\r\n");
        const auto token = classes.substr(0, end);
        if (token == wanted) return true;
        if (end == classes.npos) break;
        classes.remove_prefix(end);
    }
    return false;
}
void appendCodepoint(std::string& out, unsigned code) {
    if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return;
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) {
        out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 63));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 63));
        out += static_cast<char>(0x80 | (code & 63));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 63));
        out += static_cast<char>(0x80 | ((code >> 6) & 63)); out += static_cast<char>(0x80 | (code & 63));
    }
}
std::string decode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') { out += text[i]; continue; }
        const auto end = text.find(';', i + 1);
        if (end == text.npos || end - i > 16) { out += '&'; continue; }
        auto entity = text.substr(i + 1, end - i - 1);
        unsigned code = 0;
        if (entity == "amp") code = '&'; else if (entity == "lt") code = '<';
        else if (entity == "gt") code = '>'; else if (entity == "quot") code = '"';
        else if (entity == "apos") code = '\''; else if (entity == "nbsp") code = ' ';
        else if (!entity.empty() && entity.front() == '#') {
            entity.remove_prefix(1);
            int base = 10;
            if (!entity.empty() && (entity.front() == 'x' || entity.front() == 'X')) {
                base = 16; entity.remove_prefix(1);
            }
            const auto result = std::from_chars(entity.data(), entity.data() + entity.size(), code, base);
            if (entity.empty() || result.ec != std::errc{} || result.ptr != entity.data() + entity.size()) code = 0;
        }
        if (code) { appendCodepoint(out, code == 160 ? ' ' : code); i = end; } else out += '&';
    }
    return out;
}
std::string plain(std::string_view html) {
    std::string raw;
    bool inside = false;
    for (const char c : html) {
        if (c == '<') inside = true;
        else if (c == '>') { inside = false; raw += ' '; }
        else if (!inside) raw += c;
    }
    const auto decoded = decode(raw);
    std::string result;
    bool separator = false;
    for (char c : decoded) {
        if (space(c)) { separator = !result.empty(); continue; }
        if (separator) result += ' ';
        separator = false; result += c;
    }
    return result;
}
std::string_view inner(std::string_view html, const Tag& open) {
    std::size_t cursor = open.end;
    int depth = 1;
    Tag tag;
    while (nextTag(html, cursor, tag)) {
        if (tag.name != open.name) continue;
        depth += tag.closing ? -1 : 1;
        if (depth == 0) return html.substr(open.end, tag.start - open.end);
    }
    return {};
}
std::string classText(std::string_view html, std::string_view name) {
    std::size_t cursor = 0; Tag tag;
    while (nextTag(html, cursor, tag))
        if (!tag.closing && classHas(tag, name)) return plain(inner(html, tag));
    return {};
}
std::string hardware(std::string_view html, std::string_view name) {
    std::size_t cursor = 0; Tag tag;
    while (nextTag(html, cursor, tag))
        if (!tag.closing && classHas(tag, name))
            return classText(inner(html, tag), "card__system__value");
    return {};
}
int positiveInt(std::string_view value) {
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed > 0 ? parsed : 0;
}
int tariff(std::string_view card, int id) {
    std::size_t cursor = 0; Tag tag;
    const auto prefix = "/platform/comp/" + std::to_string(id) + "/?";
    int found = 0;
    while (nextTag(card, cursor, tag)) {
        if (tag.closing || tag.name != "a" || !classHas(tag, "js-play")) continue;
        const auto href = decode(attribute(tag, "href"));
        if (href.compare(0, prefix.size(), prefix) != 0) continue;
        std::string_view query(href.data() + prefix.size(), href.size() - prefix.size());
        while (!query.empty()) {
            const auto end = query.find('&');
            const auto item = query.substr(0, end);
            if (item.substr(0, 10) == "tariff_id=") {
                const int current = positiveInt(item.substr(10));
                if (current == 0 || (found != 0 && current != found)) return 0;
                found = current;
            }
            if (end == query.npos) break;
            query.remove_prefix(end + 1);
        }
    }
    return found;
}
bool hasNextPage(std::string_view html, int page) {
    std::size_t cursor = 0; Tag tag;
    while (nextTag(html, cursor, tag)) {
        if (tag.closing || tag.name != "a" || !classHas(tag,"pagination__next")) continue;
        const auto href = decode(attribute(tag,"href"));
        const auto begin = href.find('?');
        if (begin == href.npos) continue;
        std::string_view query(href.data()+begin+1, href.size()-begin-1);
        while (!query.empty()) {
            const auto end = query.find('&');
            const auto item = query.substr(0,end);
            if (item.substr(0,5) == "page=" && positiveInt(item.substr(5)) == page+1) return true;
            if (end == query.npos) break;
            query.remove_prefix(end+1);
        }
    }
    return false;
}
Json price(std::string_view card) {
    auto text = classText(card, "card__price");
    const auto suffix = text.find("₽");
    if (suffix == text.npos || text.substr(suffix) != "₽/час") return nullptr;
    text.resize(suffix);
    while (!text.empty() && space(text.back())) text.pop_back();
    if (text.empty() || text.back() < '0' || text.back() > '9') return nullptr;
    bool decimal = false; double value = 0; double factor = 1;
    for (const char c : text) {
        if ((c == '.' || c == ',') && !decimal) { decimal = true; continue; }
        if (c < '0' || c > '9') return nullptr;
        if (decimal) { factor *= 0.1; value += (c - '0') * factor; }
        else value = value * 10 + (c - '0');
    }
    if (!std::isfinite(value) || value <= 0 || value > 1000000) return nullptr;
    return value;
}
}
Json parsePlayStationComputers(std::string_view html, std::string_view promoSlug) {
    if (html.size() > 8 * 1024 * 1024) throw std::runtime_error(fog::constants::error::PlayStation::PlayStationCatalogResponseIsTooLarge);
    Json result = Json::array();
    for (const auto& known : PlayStationComputers) {
        if (promoSlug != known.promoSlug) continue;
        Json computer{{"id", known.id}, {"name", known.name}, {"listed", false},
            {"availability", "unknown"}, {"launchable", false}, {"tariff_id", 0}, {"price", nullptr},
            {"cpu", ""}, {"gpu", ""}, {"ram", ""}, {"ip", ""}};
        std::size_t cursor = 0; Tag tag; bool duplicate = false;
        while (nextTag(html, cursor, tag)) {
            if (tag.closing || tag.name != "div" || !classHas(tag, "card-outside") ||
                !classHas(tag, "computer-" + std::to_string(known.id))) continue;
            if (computer["listed"].get<bool>()) { duplicate = true; break; }
            computer["listed"] = true;
            const auto card = inner(html, tag);
            const auto title = classText(card, "card__title");
            const bool nameMatches = title == known.name || title == std::string("Сервер: ") + known.name;
            computer["tariff_id"] = tariff(card, known.id);
            computer["price"] = price(card);
            computer["cpu"] = hardware(card, "card__system__item--cpu");
            computer["gpu"] = hardware(card, "card__system__item--gpu");
            computer["ram"] = hardware(card, "card__system__item--ram");
            computer["launchable"] = nameMatches && computer["tariff_id"].get<int>() > 0 && !computer["price"].is_null();
        }
        if (duplicate) computer["launchable"] = false;
        result.push_back(std::move(computer));
    }
    return result;
}
Json playStationComputerPlaceholders() {
    Json result = Json::array();
    for (const auto& known : PlayStationComputers) {
        auto source = parsePlayStationComputers({},known.promoSlug);
        for (auto& computer : source)
            if (computer.at("id") == known.id) result.push_back(std::move(computer));
    }
    return result;
}
std::string loadPlayStationPromoPages(std::string_view promoSlug, const PlayStationPromoPageLoader& load,
    const PlayStationCatalogProgress& progress, const PlayStationCancellation& cancelled) {
    std::string combined;
    for (int page = 1; page <= 20; ++page) {
        checkPlayStationCancellation(cancelled);
        std::string html;
        try {
            html = load(page);
            if (html.size() > 8*1024*1024-combined.size()) throw std::runtime_error(fog::constants::error::PlayStation::PlayStationPromoExceedsLimit);
        } catch (const std::exception&) {
            if (combined.empty()) throw;
            break;
        }
        checkPlayStationCancellation(cancelled);
        combined += html;
        const auto computers = parsePlayStationComputers(combined,promoSlug);
        if (progress) progress(computers);
        bool allFound = !computers.empty();
        for (const auto& computer : computers)
            if (!computer.at("listed").get<bool>()) allFound = false;
        if (allFound || !hasNextPage(html,page)) break;
    }
    return combined;
}
namespace {
using PagedCatalogLoader = std::function<std::string(std::string_view,const PlayStationCatalogProgress&)>;
void mergeOffers(Json& computers, const Json& offers) {
    for (auto& computer : computers)
        for (const auto& offer : offers)
            if (computer.at("id") == offer.at("id")) { computer = offer; break; }
}
Json fetchCatalog(const PagedCatalogLoader& load, const PlayStationCatalogProgress& progress,
                  const PlayStationCancellation& cancelled) {
    Json result = playStationComputerPlaceholders();
    std::vector<std::string_view> loadedSources;
    bool anyLoaded = false;
    for (const auto& known : PlayStationComputers) {
        checkPlayStationCancellation(cancelled);
        const std::string_view slug = known.promoSlug;
        if (std::find(loadedSources.begin(),loadedSources.end(),slug) != loadedSources.end()) continue;
        loadedSources.push_back(slug);
        Json computers;
        try {
            const auto html = load(slug,[&](const Json& offers) {
                checkPlayStationCancellation(cancelled);
                mergeOffers(result,offers);
                if (progress) progress(result);
            });
            checkPlayStationCancellation(cancelled);
            computers = parsePlayStationComputers(html,slug);
            anyLoaded = true;
        } catch (const std::exception&) {
            checkPlayStationCancellation(cancelled);
            computers = parsePlayStationComputers({},slug);
        }
        mergeOffers(result,computers);
        if (progress) progress(result);
    }
    if (!anyLoaded) throw std::runtime_error(fog::constants::error::PlayStation::CatalogLoadFailed);
    return result;
}
}
Json fetchPlayStationComputers(const PlayStationCatalogLoader& load,
    const PlayStationCatalogProgress& progress, const PlayStationCancellation& cancelled) {
    return fetchCatalog([&load](std::string_view slug,const PlayStationCatalogProgress&) {
        return load(slug);
    },progress,cancelled);
}
Json fetchPlayStationComputers(HttpClient& http, const PlayStationCatalogProgress& progress,
    const PlayStationCancellation& cancelled) {
    auto computers = fetchCatalog([&http,&cancelled](std::string_view promoSlug,const PlayStationCatalogProgress& sourceProgress) {
        const std::string slug(promoSlug);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        return loadPlayStationPromoPages(slug, [&http, &slug, deadline, &cancelled](int page) {
            checkPlayStationCancellation(cancelled);
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline-std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw std::runtime_error(fog::constants::error::PlayStation::PlayStationPromoDeadlineReached);
            const auto timeout = static_cast<long>(remaining);
            const auto response = http.request("GET", "/promo/computer/" + slug + "/",
                {{"slug", slug}, {"page", std::to_string(page)}}, "", "", {timeout, std::min(3000L,timeout)});
            if (response.status != 200) throw std::runtime_error(fog::constants::error::PlayStation::PlayStationPromoSourceUnavailable);
            return response.body;
        },sourceProgress,cancelled);
    },progress,cancelled);
    updatePlayStationAvailability(computers,http,progress,cancelled);
    return computers;
}
Json fetchPlayStationComputers() {
    HttpClient http;
    return fetchPlayStationComputers(http);
}
}
