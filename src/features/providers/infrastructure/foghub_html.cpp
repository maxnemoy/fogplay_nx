#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "features/providers/infrastructure/foghub_html.hpp"
#include "core/domain/repository_error.hpp"
#include "features/session/domain/session_rating.hpp"
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace fog::foghub {
namespace {
constexpr std::size_t maxBytes = 2 * 1024 * 1024;
constexpr std::size_t maxNodes = 60000;
constexpr unsigned maxDepth = 96;
using Nodes = std::vector<xmlNode*>;
using Doc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

[[noreturn]] void invalid() {
    throw RepositoryError(RepositoryErrorKind::InvalidResponse, "FogHub page could not be verified");
}
void require(bool value) { if (!value) invalid(); }
std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string normalized(const std::string& value) {
    std::string result;
    bool gap = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (std::isspace(c) || (c == 0xc2 && i + 1 < value.size() && value[i + 1] == '\xa0')) {
            if (c == 0xc2) ++i;
            gap = !result.empty();
        } else {
            if (gap) result += ' ';
            result += value[i];
            gap = false;
        }
    }
    return result;
}
std::string trimmed(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : std::string{};
}
bool tag(const xmlNode* node, const char* name) {
    return node && node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name);
}
std::string attr(xmlNode* node, const char* name) {
    auto* value = xmlGetProp(node, BAD_CAST name);
    if (!value) return {};
    std::string result(reinterpret_cast<const char*>(value));
    xmlFree(value);
    return result;
}
bool hasClass(xmlNode* node, const std::string& name) {
    const auto classes = " " + normalized(attr(node, "class")) + " ";
    return classes.find(" " + name + " ") != std::string::npos;
}
void appendText(xmlNode* node, std::string& result) {
    for (auto* child = node->children; child; child = child->next) {
        if (child->type == XML_TEXT_NODE && child->content) result += reinterpret_cast<const char*>(child->content);
        else if (tag(child, "br")) result += ' ';
        else if (child->type == XML_ELEMENT_NODE && !tag(child, "script") && !tag(child, "style")) appendText(child, result);
    }
}
std::string text(xmlNode* node) { std::string result; appendText(node, result); return normalized(result); }
std::string firstLine(xmlNode* node) {
    std::string result;
    for (auto* child = node->children; child && !tag(child, "br"); child = child->next) {
        if (child->type == XML_TEXT_NODE && child->content) result += reinterpret_cast<const char*>(child->content);
        else if (child->type == XML_ELEMENT_NODE) appendText(child, result);
    }
    return normalized(result);
}
Nodes select(xmlNode* node, const char* query) {
    std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)> context(xmlXPathNewContext(node->doc), xmlXPathFreeContext);
    require(static_cast<bool>(context));
    context->node = node;
    std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)> found(
        xmlXPathEvalExpression(BAD_CAST query, context.get()), xmlXPathFreeObject);
    require(found && found->type == XPATH_NODESET);
    Nodes result;
    if (found->nodesetval) {
        for (int i = 0; i < found->nodesetval->nodeNr; ++i) result.push_back(found->nodesetval->nodeTab[i]);
    }
    return result;
}
xmlNode* one(xmlNode* node, const char* query) {
    const auto found = select(node, query);
    require(found.size() == 1);
    return found.front();
}
bool digits(const std::string& value) {
    return !value.empty() && value.size() <= 18 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}
std::size_t number(const std::string& value) {
    require(digits(value));
    std::size_t result = 0;
    for (char c : value) { require(result <= 10000000); result = result * 10 + static_cast<unsigned>(c - '0'); }
    require(result <= 100000000);
    return result;
}
void identifier(const std::string& value) { require(digits(value) && value.front() != '0'); }
using Params = std::map<std::string, std::string>;
Params query(const std::string& href, const std::string& path) {
    require(href.size() <= 2048 && href.compare(0, path.size() + 1, path + "?") == 0);
    require(href.find_first_of("#\\\r\n\t ") == std::string::npos);
    Params result;
    std::size_t start = path.size() + 1;
    while (start < href.size()) {
        auto end = href.find('&', start);
        if (end == std::string::npos) end = href.size();
        const auto equal = href.find('=', start);
        require(equal != std::string::npos && equal < end && equal > start);
        const auto key = href.substr(start, equal - start);
        require(result.emplace(key, href.substr(equal + 1, end - equal - 1)).second);
        start = end + 1;
    }
    require(!result.empty());
    return result;
}
std::string linkId(xmlNode* link, const std::string& path) {
    auto params = query(attr(link, "href"), path);
    require(params.size() == 1 && params.count("id") == 1);
    identifier(params.at("id"));
    return params.at("id");
}
Nodes links(xmlNode* node, const char* path) {
    Nodes result;
    for (auto* link : select(node, ".//a[@href]")) {
        const auto href = attr(link, "href");
        if (href.compare(0, std::string(path).size(), path) == 0) result.push_back(link);
    }
    return result;
}
void bounds(xmlNode* node, unsigned depth, std::size_t& count) {
    for (auto* current = node; current; current = current->next) {
        require(++count <= maxNodes && depth <= maxDepth);
        if (current->children) bounds(current->children, depth + 1, count);
    }
}
struct ParseErrors { bool invalid{false}; };
bool toleratedError(const xmlError* error) {
    // libxml2's HTML4 tree builder reports these for actual site templates:
    // HTML5 main/nav and an optional p end tag after block content. Semantic
    // page/table/card checks below still require complete expected structure.
    return !error || error->level < XML_ERR_ERROR || error->code == XML_HTML_UNKNOWN_TAG ||
        (error->code == XML_ERR_TAG_NAME_MISMATCH && error->str1 && std::string(error->str1) == "p");
}
#if LIBXML_VERSION >= 21200
void parseError(void* data, const xmlError* error) {
#else
void parseError(void* data, xmlError* error) {
#endif
    auto* context = static_cast<xmlParserCtxt*>(data);
    auto* errors = static_cast<ParseErrors*>(context->_private);
    if (!toleratedError(error)) errors->invalid = true;
}
#if LIBXML_VERSION >= 21300
void contextParseError(void* data, const xmlError* error) {
    if (!toleratedError(error)) static_cast<ParseErrors*>(data)->invalid = true;
}
#endif
void quietError(void*, const char*, ...) {}
Doc document(const std::string& html) {
    require(!html.empty() && html.size() <= maxBytes && html.find('\0') == std::string::npos);
    const auto folded = lower(html);
    // No DTD/entity declarations are needed by these pages. The HTML parser is
    // memory-only, NONET, without NOENT/DTDLOAD or relaxed resource limits.
    require(folded.find("<!entity") == std::string::npos && folded.find("<![") == std::string::npos);
    std::unique_ptr<htmlParserCtxt, decltype(&htmlFreeParserCtxt)> context(htmlNewParserCtxt(), htmlFreeParserCtxt);
    require(static_cast<bool>(context));
    ParseErrors errors;
    context->_private = &errors;
    context->sax->serror = parseError;
    context->sax->warning = quietError;
    context->sax->error = quietError;
    context->sax->fatalError = quietError;
#if LIBXML_VERSION >= 21300
    xmlCtxtSetErrorHandler(context.get(), contextParseError, &errors);
#endif
    Doc doc(htmlCtxtReadMemory(context.get(), html.data(), static_cast<int>(html.size()), nullptr, "UTF-8",
        HTML_PARSE_NONET | HTML_PARSE_COMPACT | HTML_PARSE_NODEFDTD), xmlFreeDoc);
    require(doc && xmlDocGetRootElement(doc.get()));
    const auto* error = xmlCtxtGetLastError(context.get());
    // Older Switch libxml2 rejects HTML5 element names such as main/nav. They
    // remain normal tree nodes; all other reported parse errors fail closed.
    require(!errors.invalid && toleratedError(error));
    require(!doc->extSubset && (!doc->intSubset || !doc->intSubset->children));
    std::size_t count = 0;
    bounds(xmlDocGetRootElement(doc.get()), 0, count);
    return doc;
}
xmlNode* mainNode(xmlDoc* doc, const std::string& heading, bool protectedPage) {
    auto* root = xmlDocGetRootElement(doc);
    if (heading != "Вход" && !select(root, "//form[@id='login-form' or @action='/site/login']").empty()) {
        throw RepositoryError(RepositoryErrorKind::NotAuthenticated, "FogHub authentication is required");
    }
    // CAPTCHA/challenge and error documents must never become empty data.
    require(select(root, "//*[@id='cf-challenge-running' or @id='challenge-form' or @id='captcha' or contains(@id,'recaptcha') or contains(concat(' ',normalize-space(@class),' '),' g-recaptcha ')]").empty());
    auto* main = one(root, "//main");
    require(attr(main, "id") == "main" && attr(main, "role") == "main");
    if (!heading.empty()) require(text(one(main, ".//h1")) == heading);
    if (protectedPage) {
        auto* logout = one(root, "//form[@action='/site/logout']");
        require(lower(attr(logout, "method")) == "post");
        require(select(logout, ".//input[@name='_csrf' and @type='hidden']").size() == 1);
    }
    return main;
}
std::vector<std::size_t> numbers(const std::string& value) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] < '0' || value[i] > '9') { ++i; continue; }
        const auto start = i;
        while (i < value.size() && value[i] >= '0' && value[i] <= '9') ++i;
        result.push_back(number(value.substr(start, i - start)));
    }
    return result;
}
struct Paging { std::size_t total; std::string next; std::size_t firstIndex = 0; };
Paging pagination(xmlNode* container, std::size_t rows, const std::string& path, const std::string& gameId = {}) {
    const auto summaries = select(container, ".//div[contains(concat(' ',normalize-space(@class),' '),' summary ')]");
    const auto pagers = select(container, ".//ul[contains(concat(' ',normalize-space(@class),' '),' pagination ')]");
    if (rows == 0) {
        require(summaries.empty() && pagers.empty());
        return {0, {}};
    }
    require(summaries.size() == 1 && pagers.size() <= 1);
    const auto counts = numbers(text(summaries.front()));
    require(counts.size() == 3 && counts[0] > 0 && counts[1] >= counts[0] && counts[2] >= counts[1]);
    require(counts[1] - counts[0] + 1 == rows);
    const bool hasMore = counts[1] < counts[2];
    if (pagers.empty()) { require(!hasMore && counts[0] == 1); return {counts[2], {}}; }
    auto* pager = pagers.front();
    auto* current = one(pager, "./li[contains(concat(' ',normalize-space(@class),' '),' active ')]/a");
    const auto currentPage = number(attr(current, "data-page"));
    const auto currentParams = query(attr(current, "href"), path);
    require(currentParams.count("per-page") == 1);
    const auto pageSize = number(currentParams.at("per-page"));
    require(pageSize > 0 && pageSize <= 200 && currentPage <= 1000000);
    require(counts[0] == currentPage * pageSize + 1 && rows <= pageSize && (!hasMore || rows == pageSize));
    auto* next = one(pager, "./li[contains(concat(' ',normalize-space(@class),' '),' next ')]");
    const bool enabled = !hasClass(next, "disabled");
    require(enabled == hasMore);
    std::string nextPath;
    for (auto* link : select(pager, "./li/a")) {
        const auto params = query(attr(link, "href"), path);
        require(params.count("page") == 1 && number(params.at("page")) == number(attr(link, "data-page")) + 1);
        auto contextParams = params;
        auto expectedParams = currentParams;
        contextParams.erase("page");
        expectedParams.erase("page");
        require(contextParams == expectedParams);
        if (!gameId.empty()) require(params.count("id") == 1 && params.at("id") == gameId);
        if (path == "/session/list") {
            for (const auto& item : params) require(item.first == "page" || item.first == "per-page" || item.first == "sort");
        }
    }
    auto* nextLink = one(next, "./a");
    if (enabled) {
        require(number(attr(nextLink, "data-page")) == currentPage + 1);
        nextPath = attr(nextLink, "href");
    }
    return {counts[2], nextPath, counts[0]-1};
}
bool date(const std::string& value) {
    return value.size() == 10 && value[2] == '.' && value[5] == '.' &&
        digits(value.substr(0, 2)) && digits(value.substr(3, 2)) && digits(value.substr(6, 4)) &&
        number(value.substr(0, 2)) >= 1 && number(value.substr(0, 2)) <= 31 &&
        number(value.substr(3, 2)) >= 1 && number(value.substr(3, 2)) <= 12;
}
bool clock(const std::string& value) {
    return value.size() == 5 && value[2] == ':' && digits(value.substr(0, 2)) && digits(value.substr(3, 2)) &&
        number(value.substr(0, 2)) <= 23 && number(value.substr(3, 2)) <= 59;
}
void interval(xmlNode* cell, HistoryRow& row) {
    const auto period = text(one(cell, "./small"));
    require(period.size() >= 18 && date(period.substr(0, 10)) && period[10] == ' ' && clock(period.substr(11, 5)) && period[16] == '-');
    const auto end = period.substr(17);
    require(end == "?" || clock(end));
    row.startedAt = period.substr(0, 10) + " " + period.substr(11, 5);
    if (end != "?") {
        // The observed markup gives one date only. Preserve its end time
        // without assuming that the start date is also the end date.
        row.finishedAt = end;
    }
    const auto full = text(cell);
    const auto duration = full.substr(period.size());
    const auto parts = numbers(duration);
    require(parts.size() == 1 && normalized(duration) == std::to_string(parts.front()) + " мин.");
    row.durationMinutes = std::to_string(parts.front());
}
bool money(const std::string& value) {
    const auto dot = value.find('.');
    return dot != std::string::npos && dot + 3 == value.size() && digits(value.substr(0, dot)) && digits(value.substr(dot + 1));
}
xmlNode* listView(xmlNode* main) {
    return one(main, ".//div[contains(concat(' ',normalize-space(@class),' '),' list-view ')]");
}
Nodes cards(xmlNode* list) {
    return select(list, "./div[contains(concat(' ',normalize-space(@class),' '),' row ')]/div");
}
void emptyList(xmlNode* list) {
    auto* empty = one(list, ".//*[contains(concat(' ',normalize-space(@class),' '),' empty ')]");
    require(text(empty) == "Ничего не найдено.");
}
bool hasAttr(xmlNode* node, const char* name) { return xmlHasProp(node, BAD_CAST name) != nullptr; }
bool printable(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
xmlNode* rentalDocument(xmlDoc* doc) {
    auto* root = xmlDocGetRootElement(doc);
    if (!select(root, "//form[@id='login-form' or @action='/site/login']").empty())
        throw RepositoryError(RepositoryErrorKind::NotAuthenticated, "FogHub authentication is required");
    require(select(root, "//base | //*[@id='cf-challenge-running' or @id='challenge-form' or @id='captcha' or contains(@id,'recaptcha') or contains(concat(' ',normalize-space(@class),' '),' g-recaptcha ')]").empty());
    return root;
}
void noRoutingOverride(xmlNode* node) {
    for (const auto* name : {"onclick", "onsubmit", "formaction", "formmethod", "formenctype", "formtarget", "data-method", "data-params"})
        require(!hasAttr(node, name));
}
bool verifyBonusChoice(xmlNode* form) {
    const auto choices = select(form,".//select[@name='Pc[use_bonus]']");
    require(choices.size() <= 1);
    if (choices.empty()) return false;
    auto* choice = choices.front();
    require(!hasAttr(choice,"multiple") && !hasAttr(choice,"onchange") &&
        (!hasAttr(choice,"size") || attr(choice,"size") == "1"));
    const auto options = select(choice,"./option");
    require(options.size() == 2 && select(choice,".//*").size() == 2);
    require(attr(options[0],"value") == "0" && attr(options[1],"value") == "1");
    for (auto* option : options) {
        noRoutingOverride(option);
        require(!hasAttr(option,"disabled") && !text(option).empty());
    }
    // With no explicit selection, HTML defaults to the first option (0).
    // An unexpected spending default is not part of the verified form contract.
    require(!hasAttr(options[1],"selected"));
    return true;
}
bool verifiedSessionRating(xmlNode* cell) {
    const auto small = select(cell,"./small");
    if (small.size() != 1 || select(cell,"./*").size() != 1) return false;
    const auto content = select(small.front(),"./div");
    if (content.size() != 1 || select(small.front(),"./*").size() != 1) return false;
    // Observed rated rows replace the review link with this fixed first line;
    // Optional review lines follow the first break and are not state evidence.
    // The server may render textarea newlines as additional direct br elements.
    const auto breaks = select(content.front(),"./br");
    if (breaks.empty() || select(content.front(),"./*").size() != breaks.size()) return false;
    const auto line = firstLine(content.front());
    const std::string prefix = "Оценка:";
    if (line.rfind(prefix,0) != 0) return false;
    const auto rating = trimmed(line.substr(prefix.size()));
    return rating.size() == 1 && rating.front() >= '1' && rating.front() <= '5';
}
void rejectReviewRequired(xmlNode* root, const std::string& computerId) {
    constexpr const char* notice = "Вы не можете играть на этом ПК: сперва оцените предыдущую игру через Foghub! Оценить";
    xmlNode* blocker = nullptr;
    for (auto* paragraph : select(root, "//main//form//p[contains(concat(' ',normalize-space(@class),' '),' text-danger ')]")) {
        if (text(paragraph) != notice) continue;
        require(!blocker);
        blocker = paragraph;
    }
    if (!blocker) return;
    auto* form = one(blocker, "ancestor::form[1]");
    const auto base = "/pc/play?id=" + computerId;
    const auto action = attr(form, "action");
    require((action == base || action == base + "&resume=0") && lower(attr(form,"method")) == "post");
    noRoutingOverride(form);
    require(!hasAttr(form,"target") && !hasAttr(form,"enctype"));
    auto* link = one(blocker, ".//a[@href]");
    require(text(link) == "Оценить");
    noRoutingOverride(link);
    require(!hasAttr(link,"target") && !hasAttr(link,"download"));
    const auto href = attr(link,"href");
    require(!href.empty() && href.back() != '&');
    const auto params = query(href, "/session/review");
    require(params.size() == 2 && params.count("id") && params.count("fromPcId"));
    identifier(params.at("id"));
    identifier(params.at("fromPcId"));
    require(params.at("fromPcId") == computerId);
    throw SessionReviewRequired({ProviderId::FogHub,params.at("id")});
}
}

MoonlightEndpoint parseMoonlightEndpoint(const std::string& value) {
    require(!value.empty() && value.size() <= 320 && printable(value));
    require(value.find_first_of("/\\?#@% \t\r\n") == std::string::npos);
    MoonlightEndpoint endpoint;
    std::string port;
    if (value.front() == '[') {
        const auto end = value.find(']');
        require(end != std::string::npos && end > 1 && end + 2 < value.size() && value[end + 1] == ':');
        endpoint.host = value.substr(1, end - 1);
        in6_addr address{};
        require(inet_pton(AF_INET6, endpoint.host.c_str(), &address) == 1);
        port = value.substr(end + 2);
    } else {
        const auto colon = value.find(':');
        require(colon != std::string::npos && colon > 0 && value.find(':', colon + 1) == std::string::npos);
        endpoint.host = value.substr(0, colon);
        port = value.substr(colon + 1);
        require(endpoint.host.size() <= 253);
        const bool numericHost = std::all_of(endpoint.host.begin(), endpoint.host.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || c == '.'; });
        if (numericHost) {
            in_addr address{};
            require(inet_pton(AF_INET, endpoint.host.c_str(), &address) == 1);
        } else {
            // Only DNS labels, not URLs, IP literals in alternate encodings,
            // unqualified local names, shell syntax, or resolver search paths.
            require(endpoint.host.find('.') != std::string::npos);
            std::size_t start = 0;
            while (start < endpoint.host.size()) {
                auto end = endpoint.host.find('.', start);
                if (end == std::string::npos) end = endpoint.host.size();
                const auto label = endpoint.host.substr(start, end - start);
                require(!label.empty() && label.size() <= 63 && label.front() != '-' && label.back() != '-');
                require(std::all_of(label.begin(), label.end(), [](unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
                }));
                start = end + 1;
            }
            require(endpoint.host.back() != '.');
            const auto suffix = endpoint.host.substr(endpoint.host.rfind('.') + 1);
            // A numeric/hex final component may be interpreted as an alternate
            // IPv4 encoding by a resolver rather than as a DNS name.
            require((suffix.front() >= 'a' && suffix.front() <= 'z') || (suffix.front() >= 'A' && suffix.front() <= 'Z'));
        }
    }
    require(port.size() <= 5 && digits(port) && port.front() != '0');
    const auto portNumber = number(port);
    require(portNumber > 0 && portNumber <= 65535);
    endpoint.port = static_cast<std::uint16_t>(portNumber);
    return endpoint;
}

std::string freshPairingLink(const std::string& html, const std::string& expectedComputerId) {
    identifier(expectedComputerId);
    auto doc = document(html);
    auto* root = rentalDocument(doc.get());
    rejectReviewRequired(root, expectedComputerId);
    if (!select(root, "//form[.//input[@name='moonlight_url']]").empty()) return {};
    const auto expected = "/pc/play?id=" + expectedComputerId + "&resume=0";
    xmlNode* fresh = nullptr;
    for (auto* link : select(root,"//a[@href]")) {
        if (attr(link,"href") != expected) continue;
        require(!fresh); fresh = link;
    }
    require(fresh);
    noRoutingOverride(fresh);
    require(!hasAttr(fresh,"target") && !hasAttr(fresh,"download"));
    return expected;
}

ConnectForm parseConnectForm(const std::string& html, const std::string& expectedComputerId) {
    identifier(expectedComputerId);
    auto doc = document(html);
    auto* root = rentalDocument(doc.get());
    rejectReviewRequired(root, expectedComputerId);
    // Names/types/action/method are verified metadata. Form IDs, surrounding
    // headings and layout were not captured and must not be invented here.
    auto* form = one(root, "//form[.//input[@name='moonlight_url']]");
    noRoutingOverride(form);
    const auto base = "/pc/play?id=" + expectedComputerId;
    const auto action = attr(form,"action");
    require((action == base || action == base+"&resume=0") && lower(attr(form, "method")) == "post");
    require(!hasAttr(form, "target") && !hasAttr(form, "enctype"));
    require(select(root, "//*[@form]").empty());
    for (auto* control : select(form, ".//input | .//select | .//textarea | .//button")) {
        noRoutingOverride(control);
        require(!hasAttr(control, "disabled"));
        const auto name = attr(control, "name");
        if (name.empty()) continue;
        if (name == "Pc[use_bonus]") require(tag(control,"select"));
        else require(tag(control, "input") && (name == "_csrf" || name == "moonlight_url" || name == "Pc[pin]"));
    }
    auto* csrf = one(form, ".//input[@name='_csrf']");
    auto* address = one(form, ".//input[@name='moonlight_url']");
    auto* pin = one(form, ".//input[@name='Pc[pin]']");
    require(lower(attr(csrf, "type")) == "hidden" && lower(attr(address, "type")) == "text" && lower(attr(pin, "type")) == "text");
    ConnectForm result;
    result.hasBonusChoice = verifyBonusChoice(form);
    result.csrf = attr(csrf, "value");
    require(!result.csrf.empty() && result.csrf.size() <= 4096 && printable(result.csrf));
    result.actionPath = action;
    result.moonlightUrl = attr(address, "value");
    result.endpoint = parseMoonlightEndpoint(result.moonlightUrl);
    return result;
}

LaunchSuccess parseLaunchSuccess(const std::string& html, const std::string& expectedComputerId,
    const std::string& effectivePath) {
    identifier(expectedComputerId);
    require(effectivePath == "/pc/play-success?id=" + expectedComputerId + "&withResume=0");
    auto doc = document(html);
    auto* root = rentalDocument(doc.get());
    const auto review = select(root, "//a[contains(@href,'/session/review')]");
    require(review.size() == 1);
    noRoutingOverride(review.front());
    const auto rentalId = linkId(review.front(), "/session/review");
    require(attr(review.front(), "href") == "/session/review?id=" + rentalId);
    return {rentalId};
}

CloseAction parseCloseAction(const std::string& html, const std::string& expectedRentalId) {
    identifier(expectedRentalId);
    const auto history = parseHistory(html);
    const auto row = std::find_if(history.rows.begin(), history.rows.end(), [&](const HistoryRow& item) { return item.id == expectedRentalId; });
    require(row != history.rows.end() && row->state == HistoryState::Active);
    auto doc = document(html);
    auto* root = rentalDocument(doc.get());
    xmlNode* found = nullptr;
    for (auto* tr : select(root, "//table/tbody/tr[@data-key]")) {
        if (attr(tr, "data-key") != expectedRentalId) continue;
        require(!found);
        found = tr;
    }
    require(found);
    auto* close = one(found, ".//a[contains(@href,'/session/close')]");
    const auto path = "/session/close?id=" + expectedRentalId;
    require(attr(close, "href") == path);
    require(attr(close, "data-confirm") == "Вы уверены, что хотите завершить эту сессию?");
    noRoutingOverride(close);
    require(!hasAttr(close, "target") && !hasAttr(close, "download"));
    return {path, CloseMethod::Get};
}

LoginForm parseLogin(const std::string& html) {
    auto doc = document(html);
    auto* main = mainNode(doc.get(), "Вход", false);
    auto* form = one(main, ".//form[@id='login-form']");
    require(attr(form, "action") == "/site/login" && lower(attr(form, "method")) == "post");
    require(select(form, ".//input[@name='LoginForm[username]' and @type='text']").size() == 1);
    require(select(form, ".//input[@name='LoginForm[password]' and @type='password']").size() == 1);
    require(select(form, ".//input[@name='LoginForm[rememberMe]' and @type='checkbox']").size() == 1);
    auto csrf = attr(one(form, ".//input[@name='_csrf' and @type='hidden']"), "value");
    require(!csrf.empty() && csrf.size() <= 4096 && csrf.find_first_of("\r\n") == std::string::npos);
    return {csrf};
}

AccountPage parseAccount(const std::string& html) {
    auto doc = document(html);
    auto* main = mainNode(doc.get(), {}, true);
    // The current profile uses the document title and empty tab headings;
    // the older template has one meaningful "Информация" heading.
    std::size_t headings = 0;
    for (auto* heading : select(main, ".//h1")) {
        const auto title = text(heading);
        if (title.empty()) continue;
        require(title == "Информация" && ++headings == 1);
    }
    if (headings == 0)
        require(text(one(xmlDocGetRootElement(doc.get()), "/html/head/title")) == "Профиль");
    auto* table = one(main, ".//table[contains(concat(' ',normalize-space(@class),' '),' detail-view ')]");
    std::string login;
    for (auto* row : select(table, ".//tr")) {
        const auto headers = select(row, "./th");
        require(headers.size() == 1);
        if (text(headers.front()) == "Логин") {
            require(login.empty());
            auto* cell = one(row, "./td");
            require(select(cell, ".//*").empty());
            std::string rawLogin;
            appendText(cell, rawLogin);
            login = trimmed(rawLogin);
        }
        // In particular, never read the Api token row or its value.
    }
    require(!login.empty() && login.size() <= 256);
    return {"login:" + login, login};
}

HistoryPage parseHistory(const std::string& html) {
    auto doc = document(html);
    auto* main = mainNode(doc.get(), "Журнал сессий", true);
    auto* table = one(main, ".//table[thead and tbody]");
    const auto headers = select(table, "./thead/tr[1]/th");
    const std::vector<std::string> expected{"Комп.", "Игра", "Сроки", "Цена", "Статус", "Отзыв"};
    require(headers.size() == expected.size());
    for (std::size_t i = 0; i < headers.size(); ++i) require(text(headers[i]) == expected[i]);
    for (auto* link : select(table, "./thead//a[@href]")) {
        const auto params = query(attr(link, "href"), "/session/list");
        for (const auto& item : params) {
            if (item.first.compare(0, 13, "SessionSearch") == 0) require(item.second.empty());
        }
    }
    for (auto* option : select(table, ".//select[starts-with(@name,'SessionSearch[')]/option[@selected]")) require(attr(option, "value").empty());
    HistoryPage page;
    std::set<std::string> ids;
    const auto rows = select(table, "./tbody/tr");
    require(!rows.empty());
    if (rows.size() == 1 && attr(rows.front(), "data-key").empty()) {
        require(attr(one(rows.front(), "./td"), "colspan") == "6");
        emptyList(rows.front());
    } else for (auto* tr : rows) {
        HistoryRow row;
        row.id = attr(tr, "data-key");
        identifier(row.id);
        require(ids.insert(row.id).second);
        const auto cells = select(tr, "./td");
        require(cells.size() == 6);
        const auto pc = links(cells[0], "/pc/preview");
        require(pc.size() == 1 && links(tr, "/pc/preview").size() == 1);
        row.computerId = linkId(pc.front(), "/pc/preview");
        row.computerName = text(pc.front());
        require(!row.computerName.empty());
        interval(cells[2], row);
        row.amount = text(cells[3]);
        require(money(row.amount));
        const auto status = firstLine(one(cells[4], "./small"));
        require(!status.empty());
        const auto close = links(tr, "/session/close");
        const auto review = links(tr, "/session/review");
        require(close.size() <= 1 && review.size() <= 1);
        for (auto* link : close) require(linkId(link, "/session/close") == row.id);
        for (auto* link : review) require(linkId(link, "/session/review") == row.id);
        const bool rated = verifiedSessionRating(cells[5]);
        if (status == "Активна") {
            require(row.finishedAt.empty() && close.size() == 1 && review.empty() && !rated);
            row.state = HistoryState::Active;
        } else if (status == "Завершена") {
            require(!row.finishedAt.empty() && close.empty() &&
                ((review.size() == 1 && !rated) || (review.empty() && rated)));
            row.state = HistoryState::Finished;
        }
        page.rows.push_back(std::move(row));
    }
    const auto paging = pagination(main, page.rows.size(), "/session/list");
    page.nextPath = paging.next;
    page.totalItems = paging.total;
    return page;
}

GamesPage parseGames(const std::string& html) {
    auto doc = document(html);
    auto* main = mainNode(doc.get(), {}, false);
    const auto headings = select(main, ".//h1");
    require(headings.size() <= 1);
    if (!headings.empty()) require(text(headings.front()) == "Все игры");
    else {
        // The current catalog comments out its h1. Verify both the document
        // identity and catalog container before accepting that template.
        require(text(one(xmlDocGetRootElement(doc.get()), "/html/head/title")) == "Все игры");
        one(main, ".//div[contains(concat(' ',normalize-space(@class),' '),' game-index ')]");
    }
    require(attr(one(main, ".//form[@id='search-form']"), "action") == "/game/list");
    auto* list = listView(main);
    GamesPage page;
    std::set<std::string> ids;
    for (auto* card : cards(list)) {
        const auto gameLinks = links(card, "/game/preview");
        require(!gameLinks.empty() && gameLinks.size() <= 2);
        GameRow row;
        std::set<std::string> modes;
        for (auto* link : gameLinks) {
            const auto params = query(attr(link, "href"), "/game/preview");
            require(params.count("id") == 1 && (params.size() == 1 ||
                (params.size() == 2 && params.count("mode") == 1 && params.at("mode") == "5")));
            const auto& id = params.at("id");
            identifier(id);
            require(row.id.empty() || row.id == id);
            row.id = id;
            require(modes.insert(params.count("mode") ? params.at("mode") : "").second);
        }
        require(ids.insert(row.id).second);
        const auto images = select(card, "./div/div[1]/img");
        require(images.size() <= 1);
        if (images.empty()) row.name = text(one(card, "./div/div[1]/h5"));
        else {
            require(select(card, "./div/div[1]/h5").empty());
            row.name = normalized(attr(images.front(), "alt"));
            row.imageUrl = attr(images.front(), "src");
            require(!row.imageUrl.empty() && row.imageUrl.size() <= 2048);
            require((row.imageUrl.front() == '/' && row.imageUrl.compare(0, 2, "//") != 0) || row.imageUrl.compare(0, 8, "https://") == 0);
        }
        require(!row.name.empty() && row.name.size() <= 512);
        page.rows.push_back(std::move(row));
    }
    if (page.rows.empty()) emptyList(list);
    const auto paging = pagination(list, page.rows.size(), "/game/list");
    page.nextPath = paging.next;
    page.totalItems = paging.total;
    page.firstIndex = paging.firstIndex;
    return page;
}

ComputerRow parseComputerPreview(const std::string& html, const std::string& computerId) {
    identifier(computerId);
    auto doc = document(html);
    auto* main = mainNode(doc.get(), {}, false);
    // The PC detail page has one bordered summary card, independent from its
    // paginated games. Never use game play links as current PC price evidence.
    auto* card = one(main, ".//div[contains(concat(' ',normalize-space(@class),' '),' rounded ') and contains(concat(' ',normalize-space(@class),' '),' p-3 ')]/div");
    const auto previews = links(card, "/pc/preview");
    require(previews.size() == 1);
    const auto identity = query(attr(previews.front(), "href"), "/pc/preview");
    require(identity.count("id") && identity.at("id") == computerId &&
        (identity.size() == 1 || (identity.size() == 2 && identity.count("mode"))));
    ComputerRow row;
    row.id = computerId; row.name = text(previews.front());
    require(!row.name.empty() && row.name.size() <= 128);
    require(text(one(main, ".//h1")).rfind("Компьютер " + row.name + " ", 0) == 0);
    const auto owners = links(card, "/promo/");
    require(owners.size() == 1); row.owner = text(owners.front());
    row.description = text(one(card, "./div[2]/small"));
    for (auto* badge : select(card, ".//span[contains(concat(' ',normalize-space(@class),' '),' badge ')]")) {
        const auto label = text(badge);
        if (label == "Онлайн") row.online = true;
        if (label == "Занят") row.busy = true;
    }
    const auto play = links(card, "/pc/play");
    require(play.size() <= 1);
    if (!play.empty()) {
        require(linkId(play.front(), "/pc/play") == computerId);
        const auto label = text(play.front());
        const std::string prefix = "Играть FH ", suffix = "₽";
        require(label.rfind(prefix, 0) == 0 && label.size() > prefix.size()+suffix.size() &&
            label.compare(label.size()-suffix.size(),suffix.size(),suffix) == 0);
        row.pricePerHour = label.substr(prefix.size(),label.size()-prefix.size()-suffix.size());
        require(digits(row.pricePerHour) || money(row.pricePerHour));
        require(std::any_of(row.pricePerHour.begin(),row.pricePerHour.end(),
            [](unsigned char c) { return c >= '1' && c <= '9'; }));
        // A contradictory page must not make this PC launchable.
        row.fhAvailable = row.online && !row.busy;
    }
    return row;
}

ComputersPage parseComputers(const std::string& html, const std::string& gameId) {
    identifier(gameId);
    auto doc = document(html);
    auto* main = mainNode(doc.get(), {}, false);
    const auto subtitles = select(main, ".//h2");
    require(subtitles.size() <= 1);
    if (!subtitles.empty()) require(text(subtitles.front()) == "Все компьютеры с игрой");
    auto* search = one(main, ".//form[@id='search-form']");
    require(attr(search, "action") == "/game/preview");
    require(attr(one(search, ".//input[@name='id']"), "value") == gameId);
    auto* list = listView(main);
    ComputersPage page;
    auto* heading = one(main, ".//h1");
    require(heading->children && heading->children->type == XML_TEXT_NODE && heading->children->content);
    page.game.id = gameId;
    page.game.name = normalized(reinterpret_cast<const char*>(heading->children->content));
    require(!page.game.name.empty());
    std::set<std::string> ids;
    for (auto* card : cards(list)) {
        const auto pcLinks = links(card, "/pc/preview");
        require(pcLinks.size() == 1);
        ComputerRow row;
        row.id = linkId(pcLinks.front(), "/pc/preview");
        row.name = text(pcLinks.front());
        require(!row.name.empty() && ids.insert(row.id).second);
        const auto owner = links(card, "/promo/");
        require(owner.size() == 1);
        row.owner = text(owner.front());
        auto* hardware = one(card, "./div[2]/small");
        row.description = text(hardware);
        const auto badgeNodes = select(card, ".//span[contains(concat(' ',normalize-space(@class),' '),' badge ')]");
        for (auto* badge : badgeNodes) {
            const auto label = text(badge);
            if (label == "Онлайн") row.online = true;
            if (label == "Занят") row.busy = true;
        }
        const auto play = links(card, "/pc/play");
        require(play.size() <= 1);
        if (!play.empty()) {
            const auto params = query(attr(play.front(), "href"), "/pc/play");
            require(params.size() == 2 && params.count("id") && params.count("gameId") && params.at("id") == row.id && params.at("gameId") == gameId);
            const auto label = text(play.front());
            const std::string prefix = "Играть FH ";
            const std::string suffix = "₽";
            require(label.compare(0, prefix.size(), prefix) == 0 && label.size() > prefix.size() + suffix.size() && label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0);
            row.pricePerHour = label.substr(prefix.size(), label.size() - prefix.size() - suffix.size());
            require(digits(row.pricePerHour) || money(row.pricePerHour));
            require(row.online && !row.busy);
            row.fhAvailable = true;
        }
        page.rows.push_back(std::move(row));
    }
    if (page.rows.empty()) emptyList(list);
    const auto paging = pagination(list, page.rows.size(), "/game/preview", gameId);
    page.nextPath = paging.next;
    page.totalItems = paging.total;
    page.firstIndex = paging.firstIndex;
    return page;
}
}
