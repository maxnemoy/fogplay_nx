#include "features/providers/infrastructure/foghub_repositories.hpp"
#include "features/providers/infrastructure/foghub_html.hpp"
#include "core/domain/repository_error.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace fog::foghub {
namespace {
[[noreturn]] void fail(RepositoryErrorKind kind, const char* message) { throw RepositoryError(kind,message); }
bool clean(const std::string& value, std::size_t limit) {
    return !value.empty() && value.size() <= limit && std::none_of(value.begin(),value.end(),
        [](unsigned char c){ return c < 32 || c == 127; });
}
bool numericId(const std::string& id) {
    return !id.empty() && id.size() <= 20 && id.front() != '0' &&
        std::all_of(id.begin(),id.end(),[](unsigned char c){ return c >= '0' && c <= '9'; });
}
void requireId(const EntityId& id) {
    if (id.provider != ProviderId::FogHub || !numericId(id.value))
        fail(RepositoryErrorKind::InvalidRequest,"Invalid FogHub entity identifier");
}
void requirePage(int page, int size) {
    if (page < 1 || page > 10000 || size < 1 || size > 100)
        fail(RepositoryErrorKind::InvalidRequest,"Invalid FogHub page request");
}
std::string encode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
Money money(const std::string& value, bool availableBalance = false) {
    if (value.empty() || value.size() > (availableBalance ? 128U : 18U))
        fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub amount");
    std::size_t i = value.front() == '-' ? 1 : 0;
    const bool negative = i != 0;
    std::int64_t whole = 0, fraction = 0;
    bool discardedNonzero = false;
    bool grouped = false;
    std::size_t groupDigits = 0;
    const auto start = i;
    while (i < value.size()) {
        if (value[i] >= '0' && value[i] <= '9') {
            whole = whole * 10 + value[i++] - '0';
            ++groupDigits;
            if (whole > 1000000000000LL) fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub amount");
        } else if (availableBalance && value[i] == ',') {
            // The balance endpoint formats thousands with ASCII commas. Verify
            // every group before ignoring separators; other money stays exact.
            if (!groupDigits || (grouped ? groupDigits != 3 : groupDigits > 3))
                fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub amount");
            grouped = true; groupDigits = 0; ++i;
        } else break;
    }
    if (i == start || (grouped && groupDigits != 3))
        fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub amount");
    if (i < value.size()) {
        if (value[i++] != '.' || i == value.size() || (!availableBalance && value.size()-i > 2))
            fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub amount");
        const auto digits = value.size()-i;
        const auto fractionStart = i;
        for (;i < value.size();++i) {
            if (value[i] < '0' || value[i] > '9') fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub amount");
            if (i-fractionStart < 2) fraction = fraction*10 + value[i]-'0';
            else if (value[i] != '0') discardedNonzero = true;
        }
        if (digits == 1) fraction *= 10;
    }
    // Available funds are rounded toward negative infinity, never upward.
    // Other monetary fields retain their exact, at-most-two-decimal contract.
    const auto minor = whole*100+fraction;
    return {negative ? -minor-(discardedNonzero ? 1 : 0) : minor,"RUB"};
}
std::string balanceShape(const nlohmann::json& data) {
    if (!data.is_object()) return "type=response-invalid length=0 decimal_digits=0";
    const auto found = data.find("balance");
    if (found == data.end()) return "type=missing length=0 decimal_digits=0";
    std::size_t length = 0, decimals = 0;
    if (found->is_string()) {
        const auto& value = found->get_ref<const std::string&>();
        length = value.size();
        const auto point = value.find('.');
        if (point != std::string::npos)
            for (auto i=point+1;i<value.size() && value[i]>='0' && value[i]<='9';++i) ++decimals;
    }
    // type_name is library-controlled. Never include a response value or body.
    return std::string("type=")+found->type_name()+" length="+std::to_string(length)+
        " decimal_digits="+std::to_string(decimals);
}
std::string relativeLocation(const std::string& location) {
    static const std::string origin = "https://foghub.ru";
    std::string path = location;
    if (path.rfind(origin+"/",0) == 0) path.erase(0,origin.size());
    if (!clean(path,4096) || path[0] != '/' || path.rfind("//",0) == 0 || path.find_first_of("\\#") != std::string::npos)
        fail(RepositoryErrorKind::InvalidResponse,"Unexpected FogHub redirect");
    return path;
}
std::string route(const std::string& path) { return path.substr(0,path.find('?')); }
void successful(const WebResponse& response) {
    if (response.status == 401) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    if (response.status == 403 || response.status == 429 || response.status >= 500)
        fail(RepositoryErrorKind::Unavailable,"FogHub is temporarily unavailable");
    if (response.status != 200) fail(RepositoryErrorKind::InvalidResponse,"Unexpected FogHub response");
}
template<class F> auto transportCall(F&& call) -> decltype(call()) {
    try { return call(); }
    catch (const TransportError& error) {
        switch (error.kind()) {
            case TransportFailure::Unsupported:
                fail(RepositoryErrorKind::Unsupported,"FogHub transport operation is unsupported");
            case TransportFailure::InvalidRequest: case TransportFailure::CookieRejected:
                fail(RepositoryErrorKind::InvalidRequest,"Invalid FogHub transport request");
            case TransportFailure::ResponseTooLarge:
                fail(RepositoryErrorKind::InvalidResponse,"FogHub response is too large");
            default: fail(RepositoryErrorKind::NetworkFailure,"FogHub connection failed");
        }
    }
}
void htmlResponse(const WebResponse& response) {
    if (response.contentType.rfind("text/html",0) != 0)
        fail(RepositoryErrorKind::InvalidResponse,"Expected FogHub HTML page");
}
template<class Parser> auto verifiedAuthPage(const std::string& body, Parser parser, const char* failure)
    -> decltype(parser(body)) {
    try { return parser(body); }
    catch (const RepositoryError& error) {
        if (error.kind() != RepositoryErrorKind::InvalidResponse) throw;
        fail(RepositoryErrorKind::InvalidResponse, failure);
    }
}
Game mapGame(const GameRow& row) {
    Game game;
    game.id = {ProviderId::FogHub,row.id}; game.name = row.name;
    if (!row.imageUrl.empty()) game.artworkUrls.push_back(row.imageUrl.front() == '/' ? "https://foghub.ru"+row.imageUrl : row.imageUrl);
    return game;
}
template<class Row, class Loader> std::vector<Row> catalogPage(int page, int size, Loader&& load) {
    // FogHub's Yii lists use 24 rows even when a different per-page is supplied.
    constexpr std::size_t nativeSize = 24;
    const std::size_t offset = static_cast<std::size_t>(page-1)*size;
    std::size_t nativePage = offset/nativeSize+1;
    std::vector<Row> result;
    std::set<std::string> ids;
    std::optional<std::size_t> total;
    while (result.size() < static_cast<std::size_t>(size)) {
        const auto data = load(nativePage);
        if (total && *total != data.totalItems) fail(RepositoryErrorKind::InvalidResponse,"FogHub catalog changed during reading");
        total = data.totalItems;
        if (offset >= *total) return {};
        const auto first = (nativePage-1)*nativeSize;
        if (data.firstIndex != first || first >= *total || data.rows.size() != std::min(nativeSize,*total-first))
            fail(RepositoryErrorKind::InvalidResponse,"Unexpected FogHub catalog page range");
        for (const auto& row : data.rows)
            if (!ids.insert(row.id).second) fail(RepositoryErrorKind::InvalidResponse,"Duplicate FogHub catalog identifier");
        const auto skip = result.empty() ? offset-first : 0;
        for (std::size_t i=skip;i<data.rows.size() && result.size()<static_cast<std::size_t>(size);++i) {
            result.push_back(data.rows[i]);
        }
        if (data.nextPath.empty()) break;
        ++nativePage;
    }
    return result;
}
}

Session::Session(WebTransport& transport, CookieStore& storage, Diagnostic diagnostic)
    : transport_(transport),storage_(storage),diagnostic_(std::move(diagnostic)) {}
void Session::balanceDiagnostic(const std::string& message) const noexcept {
    try { if (diagnostic_) diagnostic_("FogHub balance: "+message); } catch (...) {}
}
void Session::validateLogin(const LoginRequest& request) const {
    if (!clean(request.identifier,128) || !clean(request.secret,1024))
        fail(RepositoryErrorKind::InvalidRequest,"Enter FogHub login and password");
}
void Session::invalidate(bool removeSaved) {
    authenticated_.store(false);
    auth_.state = AuthState::SignedOut; auth_.account = {ProviderId::FogHub,{}}; ++auth_.generation;
    transportCall([&]{ transport_.clearCookies(); });
    if (removeSaved) {
        try { storage_.clear(); storageError_.clear(); }
        catch (...) { storageError_ = "Cannot remove saved FogHub sign-in"; fail(RepositoryErrorKind::StorageFailure,"Cannot remove saved FogHub sign-in"); }
    }
}
void Session::accept(const UserProfile& profile) {
    auth_.account = profile.account; auth_.state = AuthState::Authenticated; ++auth_.generation;
    authenticated_.store(true);
}
void Session::persist() {
    try { storage_.save({auth_.account.value,transport_.cookies()}); storageError_.clear(); }
    catch (...) { storageError_ = "FogHub sign-in is active but could not be saved"; }
}
WebResponse Session::get(const std::string& path, bool requiresAuth) {
    if (requiresAuth && !authenticated()) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    std::string next = path;
    for (int attempt=0; attempt<4; ++attempt) {
        auto response = transportCall([&]{ return transport_.get(next); });
        if (response.status == 301 || response.status == 302 || response.status == 303 || response.status == 307 || response.status == 308) {
            next = relativeLocation(response.location);
            if (route(next) == "/site/login" && route(path) != "/site/login")
                fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
            if (route(next) != route(path)) fail(RepositoryErrorKind::InvalidResponse,"Unexpected FogHub redirect");
            continue;
        }
        successful(response); return response;
    }
    fail(RepositoryErrorKind::InvalidResponse,"Too many FogHub redirects");
}
UserProfile Session::verifiedProfile() {
    const auto response = get("/user/info",false); htmlResponse(response);
    const auto page = verifiedAuthPage(response.body, parseAccount,
        "FogHub page could not be verified (stage=account)");
    UserProfile profile{{ProviderId::FogHub,page.accountId},page.displayName,{}};
    std::string shape = "type=unread length=0 decimal_digits=0";
    try {
        const auto balance = get("/user/balance",false);
        if (balance.contentType.rfind("application/json",0) != 0)
            fail(RepositoryErrorKind::InvalidResponse,"Expected FogHub balance response");
        const auto data = nlohmann::json::parse(balance.body,nullptr,false);
        shape = balanceShape(data);
        if (!data.is_object() || !data.contains("balance") || !data["balance"].is_string())
            fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub balance response");
        profile.balance = money(data["balance"].get_ref<const std::string&>(),true);
        balanceDiagnostic("status=available "+shape);
    } catch (const RepositoryError& error) {
        // A verified account is independent of this optional balance endpoint.
        // Explicit session expiry must still invalidate authentication.
        balanceDiagnostic(std::string("status=unavailable reason=")+repositoryErrorKindName(error.kind())+" "+shape);
        if (error.kind() == RepositoryErrorKind::NotAuthenticated) throw;
    }
    return profile;
}
AuthResult Session::login(const LoginRequest& request) {
    validateLogin(request);
    invalidate(true);
    try {
        const auto response = get("/site/login",false); htmlResponse(response);
        const auto form = verifiedAuthPage(response.body, parseLogin,
            "FogHub page could not be verified (stage=login-form)");
        const Query fields{{"_csrf",form.csrf},{"LoginForm[username]",request.identifier},
            {"LoginForm[password]",request.secret},{"LoginForm[rememberMe]","1"}};
        const auto submitted = transportCall([&]{ return transport_.submitLogin(fields); });
        if (submitted.status == 301 || submitted.status == 302 || submitted.status == 303) {
            // Validate the destination, but authenticate via a fresh profile read.
            const auto destination = route(relativeLocation(submitted.location));
            if (destination != "/" && destination != "/site/index" && destination != "/user/info" && destination != "/site/login")
                fail(RepositoryErrorKind::InvalidResponse,"Unexpected FogHub login redirect");
        } else successful(submitted);
        const auto profile = verifiedProfile();
        accept(profile); persist();
        return {AuthStep::Success,auth_,{}};
    } catch (...) { invalidate(false); throw; }
}
RestoreAuthResult Session::restore() {
    invalidate(false);
    std::optional<SavedCookies> saved;
    try { saved = storage_.load(); }
    catch (...) { storageError_ = "Cannot read saved FogHub sign-in"; fail(RepositoryErrorKind::StorageFailure,"Cannot read saved FogHub sign-in"); }
    if (!saved) return {false};
    try {
        try { transportCall([&]{ transport_.restoreCookies(saved->cookies); }); }
        catch (const RepositoryError& error) {
            if (error.kind() != RepositoryErrorKind::InvalidRequest) throw;
            storageError_ = "Saved FogHub cookies are invalid";
            fail(RepositoryErrorKind::StorageFailure,"Saved FogHub cookies are invalid");
        }
        const auto profile = verifiedProfile();
        if (profile.account.value != saved->account) {
            invalidate(true);
            fail(RepositoryErrorKind::NotAuthenticated,"Saved FogHub account does not match");
        }
        accept(profile); persist(); return {true};
    } catch (const RepositoryError& error) {
        invalidate(error.kind() == RepositoryErrorKind::NotAuthenticated);
        if (error.kind() == RepositoryErrorKind::NotAuthenticated) return {false};
        throw;
    }
}
void Session::forget() { invalidate(true); }
UserProfile Session::profile() {
    if (!authenticated()) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    try {
        auto profile = verifiedProfile();
        if (profile.account != auth_.account) fail(RepositoryErrorKind::NotAuthenticated,"FogHub account changed");
        persist(); return profile;
    } catch (const RepositoryError& error) {
        if (error.kind() == RepositoryErrorKind::NotAuthenticated) invalidate(true);
        throw;
    }
}
Session::OwnHistory Session::ownHistory() {
    // Also verifies the account attached to the cookie session before reading its history.
    const auto profile = this->profile();
    try {
        OwnHistory result;
        result.owner = auth_;
        result.balance = profile.balance;
        std::set<std::string> ids, paths;
        std::string next = "/session/list";
        std::optional<std::size_t> expected;
        std::size_t evidenceBytes = 0;
        while (!next.empty()) {
            if (paths.size() >= 100 || !paths.insert(next).second)
                fail(RepositoryErrorKind::InvalidResponse,"Incomplete FogHub history pagination");
            const auto response = get(next,true); htmlResponse(response);
            if (response.body.size() > 8*1024*1024-evidenceBytes)
                fail(RepositoryErrorKind::InvalidResponse,"FogHub history evidence limit exceeded");
            evidenceBytes += response.body.size();
            const auto page = parseHistory(response.body);
            if (page.totalItems > 10000 || (expected && *expected != page.totalItems))
                fail(RepositoryErrorKind::InvalidResponse,"FogHub history changed during reading");
            expected = page.totalItems;
            for (const auto& row : page.rows) {
                if (!ids.insert(row.id).second) fail(RepositoryErrorKind::InvalidResponse,"Duplicate FogHub rental identifier");
                // Validate amounts for both read-only and rental consumers.
                money(row.amount);
                result.rowPages.emplace(row.id,result.pages.size());
                result.rows.push_back(row);
            }
            result.pages.push_back(response.body);
            next = page.nextPath;
            if (!next.empty() && route(relativeLocation(next)) != "/session/list")
                fail(RepositoryErrorKind::InvalidResponse,"Unexpected FogHub history pagination");
        }
        if (!expected || result.rows.size() != *expected) fail(RepositoryErrorKind::InvalidResponse,"Incomplete FogHub history");
        persist(); return result;
    } catch (const RepositoryError& error) {
        if (error.kind() == RepositoryErrorKind::NotAuthenticated) invalidate(true);
        throw;
    }
}
std::vector<HistoryEntry> Session::history() {
    const auto snapshot = ownHistory();
    std::vector<HistoryEntry> result;
    for (const auto& row : snapshot.rows) {
        HistoryEntry entry;
        entry.id = {ProviderId::FogHub,row.id}; entry.computerId = {ProviderId::FogHub,row.computerId};
        entry.computerName = row.computerName; entry.startedAt = row.startedAt;
        entry.finishedAt = row.finishedAt; entry.durationMinutes = row.durationMinutes;
        entry.amount = row.amount; entry.total = money(row.amount);
        entry.status = row.state == HistoryState::Active ? HistoryStatus::Active :
            row.state == HistoryState::Finished ? HistoryStatus::Finished : HistoryStatus::Unknown;
        result.push_back(std::move(entry));
    }
    return result;
}
std::string Session::readRentalPage(const std::string& path) {
    if (!authenticated()) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    // A rental page must correspond to this exact PC query. Do not let a
    // same-route redirect silently change its identity or replay a command.
    try {
        const auto response = transportCall([&]{ return transport_.get(path); });
        if ((response.status == 301 || response.status == 302 || response.status == 303 ||
             response.status == 307 || response.status == 308) && route(relativeLocation(response.location)) == "/site/login")
            fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
        successful(response); htmlResponse(response); return response.body;
    } catch (const RepositoryError& error) {
        if (error.kind() == RepositoryErrorKind::NotAuthenticated) invalidate(true);
        throw;
    }
}
WebResponse Session::submitPairing(const std::string& computer,const Query& fields,bool freshPair) {
    if (!authenticated()) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    return transportCall([&]{ return transport_.submitPairing(computer,fields,freshPair); });
}
WebResponse Session::closeRental(const std::string& rental) {
    if (!authenticated()) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    return transportCall([&]{ return transport_.closeRental(rental); });
}
WebResponse Session::submitSessionRating(const std::string& rental,const Query& fields) {
    if (!authenticated()) fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
    return transportCall([&]{ return transport_.submitSessionRating(rental,fields); });
}
std::string Session::readPublic(const std::string& path) {
    const auto response = get(path,false); htmlResponse(response); return response.body;
}
AuthResult Auth::answer(const AuthAnswer&) { fail(RepositoryErrorKind::Unsupported,"FogHub requires login and password"); }

Page<Game> Catalog::games(const GameQuery& query) {
    requirePage(query.page,query.pageSize);
    if (query.genreId != 0) fail(RepositoryErrorKind::Unsupported,"FogHub genre mapping is not available");
    if (!query.search.empty() && !clean(query.search,256)) fail(RepositoryErrorKind::InvalidRequest,"Invalid FogHub search");
    if (!query.search.empty()) fail(RepositoryErrorKind::Unsupported,"FogHub game matching is not available");
    const auto rows = catalogPage<GameRow>(query.page,query.pageSize,[&](std::size_t nativePage) {
        return parseGames(session_.readPublic("/game/list?page="+std::to_string(nativePage)+"&per-page=24"));
    });
    Page<Game> result;
    for (const auto& row : rows) result.items.push_back(mapGame(row));
    return result;
}
Game Catalog::game(const EntityId& id) {
    requireId(id);
    return mapGame(parseComputers(session_.readPublic("/game/preview?id="+encode(id.value)),id.value).game);
}
Page<Computer> Catalog::computers(const ComputerQuery& query) {
    requireId(query.gameId); requirePage(query.page,query.pageSize);
    if (query.ownSteamAccount) fail(RepositoryErrorKind::Unsupported,"FogHub account filter is not available");
    const auto rows = catalogPage<ComputerRow>(query.page,query.pageSize,[&](std::size_t nativePage) {
        return parseComputers(session_.readPublic("/game/preview?id="+encode(query.gameId.value)+
            "&page="+std::to_string(nativePage)+"&per-page=24"),query.gameId.value);
    });
    Page<Computer> result;
    for (const auto& row : rows) {
        Computer computer;
        computer.id = {ProviderId::FogHub,row.id}; computer.name = row.name;
        computer.listed = true;
        computer.availability = row.busy ? Availability::Busy : row.online ? Availability::Online : Availability::Offline;
        if (!row.pricePerHour.empty()) {
            computer.hourlyPrice = money(row.pricePerHour);
            if (computer.hourlyPrice->minorUnits < 0) fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub hourly price");
            computer.price = row.pricePerHour;
        }
        // Reading availability is not permission to launch a paid session.
        computer.launchable = false;
        result.items.push_back(std::move(computer));
    }
    return result;
}
AutomaticComputers Catalog::recommend(const ComputerQuery&) { fail(RepositoryErrorKind::Unsupported,"FogHub recommendations are not available"); }
std::vector<Computer> Catalog::playStationComputers(const CatalogProgress& progress,const CatalogCancellation& cancelled) {
    // The shared home-section API predates providers. FogHub supplies the
    // explicitly selected ordinary PCs here; the UI labels its own section.
    std::vector<Computer> result;
    for (const auto* id : {"1838", "1836", "1233", "1232", "1231", "1235", "1237", "1230"}) {
        if (cancelled && cancelled()) fail(RepositoryErrorKind::Unavailable,"FogHub catalog cancelled");
        const auto row = parseComputerPreview(session_.readPublic(std::string("/pc/preview?id=")+id),id);
        if (cancelled && cancelled()) fail(RepositoryErrorKind::Unavailable,"FogHub catalog cancelled");
        Computer computer;
        computer.id = {ProviderId::FogHub,row.id}; computer.name = row.name;
        computer.listed = true;
        computer.availability = row.busy ? Availability::Busy : row.online ? Availability::Online : Availability::Offline;
        if (!row.pricePerHour.empty()) {
            computer.hourlyPrice = money(row.pricePerHour);
            if (computer.hourlyPrice->minorUnits <= 0) fail(RepositoryErrorKind::InvalidResponse,"Invalid FogHub hourly price");
            computer.price = row.pricePerHour;
        }
        computer.launchable = row.fhAvailable && computer.hourlyPrice.has_value();
        result.push_back(std::move(computer));
        if (progress) progress(result);
    }
    return result;
}
ReadRepositories::ReadRepositories(const std::string& cookiePath) : transport_(makeWebTransport()),storage_(makeCookieStore(cookiePath)),
    session_(*transport_,*storage_),auth_(session_),account_(session_),catalog_(session_) {}
namespace {
class EphemeralCookies final : public CookieStore {
public:
    std::optional<SavedCookies> load() override { return {}; }
    void save(const SavedCookies&) override {}
    void clear() override {}
};
struct PublicCatalog {
    std::unique_ptr<WebTransport> transport = makeWebTransport();
    EphemeralCookies storage;
    Session session{*transport,storage};
    Catalog catalog{session};
};
}
std::shared_ptr<CatalogRepository> makePublicCatalog() {
    auto owner = std::make_shared<PublicCatalog>();
    return {owner,&owner->catalog};
}
std::shared_ptr<CatalogRepository> ReadRepositories::publicCatalog() { return makePublicCatalog(); }
}
