#include "features/providers/infrastructure/foghub_repositories.hpp"
#include <nlohmann/json.hpp>
#include "core/domain/repository_error.hpp"
#include <cassert>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>

using namespace fog;
using namespace fog::foghub;
namespace {
std::string fixture(const char* name) {
    std::ifstream file(std::string("tests/fixtures/foghub/")+name);
    assert(file.good());
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
void replace(std::string& value,const std::string& from,const std::string& to) {
    const auto at = value.find(from); assert(at != std::string::npos); value.replace(at,from.size(),to);
}
std::string historyPage(bool last, bool duplicate=false) {
    auto page=fixture("history-terminal.html");
    if(last) {
        const auto begin=page.find("<tr data-key=\"9000001\""); assert(begin!=std::string::npos);
        const auto end=page.find("</tr>",begin); assert(end!=std::string::npos);
        page.erase(begin,end+5-begin);
        if(!duplicate)page=std::regex_replace(page,std::regex("9000002"),"9000003");
        replace(page,"1–2 из 2","3–3 из 3");
    } else replace(page,"1–2 из 2","1–2 из 3");
    const std::string current=last ? "2" : "1", index=last ? "1" : "0";
    replace(page,"</main>","<ul class='pagination'><li class='active'><a href='/session/list?page="+current+
        "&amp;per-page=2' data-page='"+index+"'>Page</a></li><li class='next"+(last ? " disabled" : "")+
        "'><a href='/session/list?page=2&amp;per-page=2' data-page='1'>Next</a></li></ul></main>");
    return page;
}
std::string secondGamesPage() {
    auto page=fixture("games.html");
    replace(page,"1–24 из 6767","25–48 из 6767");
    page=std::regex_replace(page,std::regex("/game/preview\\?id=([0-9]+)"),"/game/preview?id=90$1");
    const auto begin=page.find("<ul class=\"pagination\""); assert(begin!=std::string::npos);
    const auto end=page.find("</ul>",begin); assert(end!=std::string::npos);
    page.replace(begin,end+5-begin,"<ul class='pagination'><li class='active'><a href='/game/list?page=2&amp;per-page=24' data-page='1'>2</a></li><li class='next'><a href='/game/list?page=3&amp;per-page=24' data-page='2'>Next</a></li></ul>");
    return page;
}
WebResponse html(std::string body) { return {200,std::move(body),{},"text/html; charset=UTF-8"}; }
WebResponse balance(std::string value="250.00") { return {200,nlohmann::json{{"balance",value}}.dump(),{},"application/json"}; }
struct FakeTransport final : WebTransport {
    struct Expected { std::string path; WebResponse response; };
    std::deque<Expected> responses;
    std::vector<std::string> jar{"fixture-cookie"};
    Query submitted;
    unsigned posts=0,gets=0,clears=0;
    bool cancelled=false;
    bool rejectCookies=false;
    WebResponse loginResponse{302,{},"/user/info",{}};
    WebResponse get(const std::string& path) override {
        assert(!cancelled); assert(!responses.empty()); assert(responses.front().path==path);
        auto response=responses.front().response; responses.pop_front(); ++gets; return response;
    }
    WebResponse submitLogin(const Query& fields) override { ++posts; submitted=fields; jar={"new-cookie"}; return loginResponse; }
    std::vector<std::string> cookies() override { return jar; }
    void restoreCookies(const std::vector<std::string>& values) override {
        if(rejectCookies)throw TransportError(TransportFailure::CookieRejected);
        jar=values;
    }
    void clearCookies() override { jar.clear(); ++clears; }
    void cancel() noexcept override { cancelled=true; }
    void profile(std::string page=fixture("account.html"),std::string amount="250.00") {
        responses.push_back({"/user/info",html(std::move(page))});
        responses.push_back({"/user/balance",balance(std::move(amount))});
    }
};
struct MemoryStore final : CookieStore {
    std::optional<SavedCookies> saved;
    bool failSave=false,failClear=false;
    unsigned saves=0,clears=0;
    std::optional<SavedCookies> load() override { return saved; }
    void save(const SavedCookies& value) override {
        ++saves; if(failSave)throw RepositoryError(RepositoryErrorKind::StorageFailure,"fixture storage failure"); saved=value;
    }
    void clear() override {
        ++clears; if(failClear)throw RepositoryError(RepositoryErrorKind::StorageFailure,"fixture storage failure"); saved.reset();
    }
};
struct Setup {
    FakeTransport web;
    MemoryStore store;
    std::vector<std::string> diagnostics;
    Session session{web,store,[this](const std::string& message) { diagnostics.push_back(message); }};
    Auth auth{session};
    Account account{session};
    fog::foghub::Catalog catalog{session};
    void login(std::string page=fixture("account.html")) {
        web.responses.push_back({"/site/login",html(fixture("login.html"))}); web.profile(std::move(page));
        const auto result=auth.begin({"fixture-user","fixture-password"});
        assert(result.step==AuthStep::Success && auth.authenticated()); assert(web.posts==1);
    }
};
template<class F> void error(RepositoryErrorKind kind,F&& action) {
    bool seen=false;
    try{action();}catch(const RepositoryError& e){seen=true;assert(e.kind()==kind);assert(std::string(e.what()).find("fixture-password")==std::string::npos);}
    assert(seen);
}
void loginAndProfile() {
    const auto current=fixture("account-profile-synthetic.html");
    Setup modern; modern.login(current);
    assert(modern.store.saved && modern.store.saved->account=="login:fixture-user");
    modern.web.profile(current,"123.45");
    assert(modern.account.profile().balance->minorUnits==12345);
    Setup restored; restored.store.saved=modern.store.saved; restored.web.profile(current);
    assert(restored.auth.restore().restored && restored.auth.authenticated() && restored.web.posts==0);
    auto wrongOwner=current; replace(wrongOwner,"fixture-user","another-user");
    restored.web.profile(wrongOwner);
    error(RepositoryErrorKind::NotAuthenticated,[&]{restored.account.profile();});
    assert(!restored.auth.authenticated() && !restored.store.saved);
    Setup s; assert(s.auth.capabilities().method==LoginMethod::Credentials);
    s.login(); assert(s.auth.auth().account==AccountId({ProviderId::FogHub,"login:fixture-user"}));
    assert(s.store.saved && s.store.saved->account==s.auth.auth().account.value);
    assert(s.web.submitted.size()==4);
    assert(s.web.submitted[0].first=="_csrf" && !s.web.submitted[0].second.empty());
    assert(s.web.submitted[1]==std::make_pair(std::string("LoginForm[username]"),std::string("fixture-user")));
    assert(s.web.submitted[2].first=="LoginForm[password]");
    s.web.profile(fixture("account.html"),"123.45");
    const auto profile=s.account.profile(); assert(profile.displayName=="fixture-user");
    assert(profile.balance && profile.balance->minorUnits==12345 && profile.balance->currency=="RUB");
    const auto generation=s.auth.auth().generation;
    s.auth.forget(); assert(!s.auth.authenticated() && !s.store.saved && s.web.jar.empty());
    assert(s.auth.auth().generation>generation && s.auth.auth().account.provider==ProviderId::FogHub);
    error(RepositoryErrorKind::NotAuthenticated,[&]{s.account.profile();});
}
void restoreAndFailures() {
    Setup s; s.store.saved=SavedCookies{"login:fixture-user",{"saved-cookie"}}; s.web.profile();
    assert(s.auth.restore().restored && s.auth.authenticated() && s.web.posts==0);
    Setup expired; expired.store.saved=SavedCookies{"login:fixture-user",{"saved-cookie"}};
    expired.web.responses.push_back({"/user/info",{302,{},"/site/login",{}}});
    assert(!expired.auth.restore().restored && !expired.store.saved && !expired.auth.authenticated());
    Setup outage; outage.store.saved=SavedCookies{"login:fixture-user",{"saved-cookie"}};
    outage.web.responses.push_back({"/user/info",{503,{},{},{}}});
    error(RepositoryErrorKind::Unavailable,[&]{outage.auth.restore();});
    assert(outage.store.saved && !outage.auth.authenticated() && outage.web.posts==0);
    Setup mismatch; mismatch.store.saved=SavedCookies{"login:another-user",{"saved-cookie"}}; mismatch.web.profile();
    assert(!mismatch.auth.restore().restored && !mismatch.store.saved);
    Setup invalidCookies; invalidCookies.store.saved=SavedCookies{"login:fixture-user",{"foreign-cookie"}}; invalidCookies.web.rejectCookies=true;
    error(RepositoryErrorKind::StorageFailure,[&]{invalidCookies.auth.restore();});
    assert(!invalidCookies.auth.authenticated() && !invalidCookies.auth.storageError().empty() && invalidCookies.web.gets==0);
    Setup networkLogin; networkLogin.web.responses.push_back({"/site/login",html(fixture("login.html"))});
    networkLogin.web.loginResponse={503,{},{},{}};
    error(RepositoryErrorKind::Unavailable,[&]{networkLogin.auth.begin({"fixture-user","fixture-password"});});
    assert(networkLogin.web.posts==1 && !networkLogin.auth.authenticated() && networkLogin.web.jar.empty());
}
void securityAndStorage() {
    Setup s; error(RepositoryErrorKind::InvalidRequest,[&]{s.auth.begin({"", "fixture-password"});});
    assert(s.web.gets==0 && s.web.posts==0 && s.store.clears==0);
    Setup redirect; redirect.store.saved=SavedCookies{"login:fixture-user",{"saved-cookie"}};
    redirect.web.responses.push_back({"/user/info",{302,{},"https://evil.invalid/user/info",{}}});
    error(RepositoryErrorKind::InvalidResponse,[&]{redirect.auth.restore();}); assert(redirect.web.gets==1 && redirect.store.saved);
    Setup broken; broken.store.failSave=true; broken.login(); assert(!broken.auth.storageError().empty() && broken.auth.authenticated());
    broken.store.failClear=true; error(RepositoryErrorKind::StorageFailure,[&]{broken.auth.forget();}); assert(!broken.auth.authenticated());
    Setup clearFailure; clearFailure.store.failClear=true;
    error(RepositoryErrorKind::StorageFailure,[&]{clearFailure.auth.begin({"fixture-user","fixture-password"});});
    assert(clearFailure.web.posts==0 && clearFailure.web.gets==0);
    Setup changed; changed.login(); auto page=fixture("account.html"); replace(page,"fixture-user","another-user"); changed.web.profile(page);
    error(RepositoryErrorKind::NotAuthenticated,[&]{changed.account.profile();}); assert(!changed.auth.authenticated() && !changed.store.saved);
}
void historyAndAmounts() {
    Setup s; s.login(); s.web.profile(); s.web.responses.push_back({"/session/list",html(fixture("history-active.html"))});
    const auto rows=s.account.history(); assert(rows.size()==2);
    assert(rows[0].id==EntityId({ProviderId::FogHub,"9000002"}) && rows[0].computerId==EntityId({ProviderId::FogHub,"1836"}));
    assert(rows[0].status==HistoryStatus::Active && rows[1].status==HistoryStatus::Finished);
    assert(rows[1].total && rows[1].total->minorUnits==0);
    s.web.profile(); s.web.responses.push_back({"/session/list",html(fixture("login.html"))});
    error(RepositoryErrorKind::NotAuthenticated,[&]{s.account.history();}); assert(!s.auth.authenticated() && !s.store.saved);
    Setup filtered; filtered.login(); filtered.web.profile(); filtered.web.responses.push_back({"/session/list",html(fixture("history-reason-filter-empty.html"))});
    error(RepositoryErrorKind::InvalidResponse,[&]{filtered.account.history();});
}
void optionalBalanceAndPrecision() {
    for (const auto& value : std::vector<std::pair<std::string,std::int64_t>>{
            {"123.45",12345},{"123.459999999999999999999",12345},{"0.009",0},
            {"-0.001",-1},{"-1.234",-124},{"-1.2300",-123},{"1.2",120},
            {"1000000000000.999",100000000000099LL}}) {
        Setup s; s.login(); s.web.profile(fixture("account.html"),value.first);
        const auto profile=s.account.profile();
        assert(profile.balance && profile.balance->minorUnits==value.second);
        assert(s.auth.authenticated() && s.store.saved);
        const auto& diagnostic=s.diagnostics.back();
        assert(diagnostic.find("status=available type=string length=")!=std::string::npos);
        assert(diagnostic.find(value.first)==std::string::npos);
    }
    for(const auto& invalid: {"nan","1e3","1,20","99999999999999999","1x","1.",".25","+1.00","1000000000001","-",""}) {
        Setup bad; bad.store.saved=SavedCookies{"login:fixture-user",{"saved-cookie"}}; bad.web.profile(fixture("account.html"),invalid);
        assert(bad.auth.restore().restored && bad.store.saved && bad.auth.authenticated());
        bad.web.profile(fixture("account.html"),invalid);
        assert(!bad.account.profile().balance && bad.auth.authenticated());
        assert(bad.diagnostics.back().find("status=unavailable reason=invalid-response type=string length=")!=std::string::npos);
    }
    for (const auto& response : std::vector<WebResponse>{
            {503,{},{},{}},{200,"<html>private-secret-body</html>",{},"text/html"},
            {200,"private-secret-body",{},"application/json"},
            {200,"{\"balance\":\"private-secret-body\"}",{},"application/json"},
            {200,"{\"balance\":12.345}",{},"application/json"},
            {200,"{\"balance\":null}",{},"application/json"},
            {200,"{\"secret\":\"private-secret-body\"}",{},"application/json"}}) {
        Setup s;
        s.web.responses.push_back({"/site/login",html(fixture("login.html"))});
        s.web.responses.push_back({"/user/info",html(fixture("account.html"))});
        s.web.responses.push_back({"/user/balance",response});
        assert(s.auth.begin({"fixture-user","fixture-password"}).step==AuthStep::Success);
        assert(s.auth.authenticated() && s.store.saved);
        s.web.responses.push_back({"/user/info",html(fixture("account.html"))});
        s.web.responses.push_back({"/user/balance",response});
        assert(!s.account.profile().balance && s.auth.authenticated());
        for (const auto& message : s.diagnostics) {
            assert(message.find("private-secret-body")==std::string::npos);
            assert(message.find("fixture-")==std::string::npos && message.find("12.345")==std::string::npos);
        }
        s.web.profile();
        assert(s.account.profile().balance && s.auth.authenticated());
    }
    Setup preciseLogin;
    preciseLogin.web.responses.push_back({"/site/login",html(fixture("login.html"))});
    preciseLogin.web.profile(fixture("account.html"),"123.45999");
    assert(preciseLogin.auth.begin({"fixture-user","fixture-password"}).step==AuthStep::Success);
    assert(preciseLogin.auth.authenticated() && preciseLogin.store.saved);
    assert(preciseLogin.diagnostics.back()=="FogHub balance: status=available type=string length=9 decimal_digits=5");
    preciseLogin.web.profile(fixture("account.html"),"0."+std::string(127,'0'));
    assert(!preciseLogin.account.profile().balance && preciseLogin.auth.authenticated());
    // Diagnostics are optional and a broken sink cannot undo a verified login.
    FakeTransport web; MemoryStore store;
    Session session{web,store,[](const std::string&) { throw std::runtime_error("private diagnostic failure"); }};
    web.responses.push_back({"/site/login",html(fixture("login.html"))}); web.profile();
    assert(session.login({"fixture-user","fixture-password"}).step==AuthStep::Success && session.authenticated());
    Setup expired; expired.login();
    expired.web.responses.push_back({"/user/info",html(fixture("account.html"))});
    expired.web.responses.push_back({"/user/balance",{302,{},"/site/login",{}}});
    error(RepositoryErrorKind::NotAuthenticated,[&]{expired.account.profile();});
    assert(!expired.auth.authenticated() && !expired.store.saved);
    Setup strict; strict.login(); strict.web.profile();
    auto history=fixture("history-active.html"); replace(history,"0.00","0.001");
    strict.web.responses.push_back({"/session/list",html(history)});
    error(RepositoryErrorKind::InvalidResponse,[&]{strict.account.history();});
}
void groupedBalanceAmounts() {
    // Matches the observed wire shape without preserving the actual account balance.
    for (const auto& value : std::vector<std::pair<std::string,std::int64_t>>{
            {"1,234.56",123456},{"1,234,567.89",123456789},{"12,345.6",1234560},
            {"123,456",12345600},{"-1,234.567",-123457},{"-1,234.5600",-123456},
            {"1,000,000,000,000.999",100000000000099LL}}) {
        Setup s; s.web.responses.push_back({"/site/login",html(fixture("login.html"))});
        s.web.profile(fixture("account.html"),value.first);
        assert(s.auth.begin({"fixture-user","fixture-password"}).step==AuthStep::Success);
        assert(s.auth.authenticated() && s.store.saved);
        s.web.profile(fixture("account.html"),value.first);
        const auto profile=s.account.profile();
        assert(profile.balance && profile.balance->minorUnits==value.second && profile.balance->currency=="RUB");
        assert(s.diagnostics.back().find(value.first)==std::string::npos);
    }
    for (const auto& invalid : {"1,23.45","12,34,567.89","1234,567.89",",123.45","123,.45",
            "1,,234.56","1,234,","1,234.5,6","1,234,56","1.234,56","1 234.56",
            "1\u00a0234.56","1,234.56 ","+1,234.56","1,234.56₽","RUB1,234.56","1,000,000,000,001.00"}) {
        Setup s; s.login(); s.web.profile(fixture("account.html"),invalid);
        assert(!s.account.profile().balance && s.auth.authenticated() && s.store.saved);
        assert(s.diagnostics.back().find("status=unavailable reason=invalid-response")!=std::string::npos);
    }
    Setup restored; restored.store.saved=SavedCookies{"login:fixture-user",{"saved-cookie"}};
    restored.web.profile(fixture("account.html"),"1,234.56");
    assert(restored.auth.restore().restored && restored.auth.authenticated());
    // Grouping support is restricted to balance; history remains an exact amount.
    Setup strict; strict.login(); strict.web.profile();
    auto history=fixture("history-active.html"); replace(history,"0.00","1,234.56");
    strict.web.responses.push_back({"/session/list",html(history)});
    error(RepositoryErrorKind::InvalidResponse,[&]{strict.account.history();});
    // Limits apply to the original input, even if separators/leading zeros add no value.
    strict.web.profile(fixture("account.html"),std::string(127,'0')+",000.00");
    assert(!strict.account.profile().balance && strict.auth.authenticated());
}
void catalogs() {
    Setup s;
    error(RepositoryErrorKind::InvalidRequest,[&]{s.catalog.game({ProviderId::Mts,"70"});});
    error(RepositoryErrorKind::InvalidRequest,[&]{s.catalog.games({"",0,24,0});});
    error(RepositoryErrorKind::Unsupported,[&]{s.catalog.games({"Spider-Man",1,24,0});});
    error(RepositoryErrorKind::Unsupported,[&]{s.catalog.games({"",1,24,173});});
    assert(s.web.gets==0 && s.web.posts==0);
    s.web.responses.push_back({"/game/list?page=1&per-page=24",html(fixture("games.html"))});
    auto games=s.catalog.games({"",1,24,0}); assert(games.items.size()==24);
    for(const auto& game:games.items)assert(game.id.provider==ProviderId::FogHub && !game.name.empty());
    s.web.responses.push_back({"/game/preview?id=70&page=1&per-page=24",html(fixture("computers.html"))});
    auto computers=s.catalog.computers({{ProviderId::FogHub,"70"},1,24,false}); assert(!computers.items.empty());
    for(const auto& pc:computers.items)assert(pc.id.provider==ProviderId::FogHub && !pc.launchable);
    assert(s.web.posts==0);
}
void pagedHistory() {
    Setup s; s.login(); s.web.profile();
    s.web.responses.push_back({"/session/list",html(historyPage(false))});
    s.web.responses.push_back({"/session/list?page=2&per-page=2",html(historyPage(true))});
    const auto rows=s.account.history();
    assert(rows.size()==3 && rows.back().id.value=="9000003" && s.web.responses.empty());
    Setup expired; expired.login(); expired.web.profile();
    expired.web.responses.push_back({"/session/list",html(historyPage(false))});
    expired.web.responses.push_back({"/session/list?page=2&per-page=2",{302,{},"/site/login",{}}});
    error(RepositoryErrorKind::NotAuthenticated,[&]{expired.account.history();});
    assert(!expired.auth.authenticated() && !expired.store.saved);
    for(bool duplicate:{true,false}) {
        Setup bad; bad.login(); bad.web.profile();
        auto first=historyPage(false);
        if(!duplicate)replace(first,"1–2 из 3","1–2 из 4");
        bad.web.responses.push_back({"/session/list",html(first)});
        bad.web.responses.push_back({"/session/list?page=2&per-page=2",html(historyPage(true,duplicate))});
        error(RepositoryErrorKind::InvalidResponse,[&]{bad.account.history();});
        assert(bad.auth.authenticated());
    }
}
void resizedPages() {
    Setup s;
    s.web.responses.push_back({"/game/list?page=1&per-page=24",html(fixture("games.html"))});
    const auto first=s.catalog.games({"",1,16,0}); assert(first.items.size()==16);
    s.web.responses.push_back({"/game/list?page=1&per-page=24",html(fixture("games.html"))});
    s.web.responses.push_back({"/game/list?page=2&per-page=24",html(secondGamesPage())});
    const auto second=s.catalog.games({"",2,16,0}); assert(second.items.size()==16);
    assert(first.items.back().id!=second.items.front().id);
    assert(second.items[7].id.value.rfind("90",0)!=0 && second.items[8].id.value.rfind("90",0)==0);
    s.web.responses.push_back({"/game/list?page=2&per-page=24",html(secondGamesPage())});
    const auto third=s.catalog.games({"",3,16,0}); assert(third.items.size()==16);
    assert(second.items.back().id!=third.items.front().id && s.web.responses.empty());
    auto repeated=secondGamesPage();
    replace(repeated,"/game/preview?id=90"+first.items.front().id.value,
        "/game/preview?id="+first.items.front().id.value);
    s.web.responses.push_back({"/game/list?page=1&per-page=24",html(fixture("games.html"))});
    s.web.responses.push_back({"/game/list?page=2&per-page=24",html(repeated)});
    error(RepositoryErrorKind::InvalidResponse,[&]{s.catalog.games({"",2,16,0});});
    s.web.responses.push_back({"/game/list?page=1&per-page=24",html(fixture("games.html"))});
    s.web.responses.push_back({"/game/list?page=2&per-page=24",html(secondGamesPage())});
    assert(s.catalog.games({"",1,25,0}).items.size()==25);
    auto last=secondGamesPage();
    replace(last,"25–48 из 6767","25–48 из 48");
    replace(last,"class='next'","class='next disabled'");
    replace(last,"page=3&amp;per-page=24' data-page='2'","page=2&amp;per-page=24' data-page='1'");
    s.web.responses.push_back({"/game/list?page=2&per-page=24",html(last)});
    assert(s.catalog.games({"",3,17,0}).items.size()==14);
    s.web.responses.push_back({"/game/list?page=3&per-page=24",html(last)});
    assert(s.catalog.games({"",4,17,0}).items.empty());
    s.web.responses.push_back({"/game/list?page=1&per-page=24",html(fixture("games.html"))});
    s.web.responses.push_back({"/game/list?page=2&per-page=24",html(last)});
    error(RepositoryErrorKind::InvalidResponse,[&]{s.catalog.games({"",2,16,0});});
}
void curatedComputers() {
    Setup s;
    auto second=fixture("computer-preview-card-excerpt.html");
    replace(second,"id=1838","id=1836"); replace(second,"id=1838","id=1836");
    // All identity-bearing text on the page must agree with the selected card.
    replace(second,"f4bX8","f4bX10"); replace(second,"f4bX8","f4bX10");
    s.web.responses.push_back({"/pc/preview?id=1838",html(fixture("computer-preview-card-excerpt.html"))});
    s.web.responses.push_back({"/pc/preview?id=1836",html(second)});
    const std::vector<std::pair<std::string,std::string>> added{
        {"1233","10623X9"},{"1232","10623X2"},{"1231","10623X3"},
        {"1235","10623X7"},{"1237","10623X10"},{"1230","10623X4"}};
    for (const auto& [id,name] : added) {
        auto page=fixture("computer-preview-card-excerpt.html");
        page=std::regex_replace(page,std::regex("1838"),id);
        page=std::regex_replace(page,std::regex("f4bX8"),name);
        s.web.responses.push_back({"/pc/preview?id="+id,html(page)});
    }
    unsigned updates=0;
    const auto cards=s.catalog.playStationComputers([&](const std::vector<Computer>& rows) {
        ++updates; assert(rows.size()==updates);
    });
    assert(cards.size()==8 && cards[0].id.value=="1838" && cards[1].id.value=="1836" && updates==8);
    for (std::size_t i=0;i<added.size();++i)
        assert(cards[i+2].id.value==added[i].first && cards[i+2].name==added[i].second);
    for(const auto& card:cards) assert(card.launchable && card.hourlyPrice && card.hourlyPrice->minorUnits==2500);
    assert(s.web.posts==0);
    Setup cancelled;
    error(RepositoryErrorKind::Unavailable,[&]{cancelled.catalog.playStationComputers({},[]{return true;});});
    assert(cancelled.web.gets==0);
    Setup busy;
    auto offline=fixture("computer-preview-card-excerpt.html"); replace(offline,"Онлайн","Занят");
    busy.web.responses.push_back({"/pc/preview?id=1838",html(offline)});
    busy.web.responses.push_back({"/pc/preview?id=1836",html(second)});
    for (const auto& [id,name] : added) {
        auto page=fixture("computer-preview-card-excerpt.html");
        page=std::regex_replace(page,std::regex("1838"),id);
        page=std::regex_replace(page,std::regex("f4bX8"),name);
        busy.web.responses.push_back({"/pc/preview?id="+id,html(page)});
    }
    const auto unavailable=busy.catalog.playStationComputers();
    assert(!unavailable[0].launchable && unavailable[0].availability==Availability::Busy && unavailable[1].launchable);
}
void publicCatalogWithoutAccountStorage() {
    // The production composition must not construct FileCookieStore("") merely
    // to obtain an anonymous catalog. Construction/cancel never makes a request.
    auto first = makePublicCatalog();
    auto second = makePublicCatalog();
    assert(first && second && first.get() != second.get());
    first->cancel();
    second->cancel();
}
void authenticationPageDiagnostics() {
    for (bool accountStage : {false, true}) {
        Setup s;
        s.web.responses.push_back({"/site/login",html(accountStage ? fixture("login.html") : "<main></main>")});
        if (accountStage) s.web.responses.push_back({"/user/info",html("<main></main>")});
        bool rejected = false;
        try { s.auth.begin({"fixture-user","fixture-password"}); }
        catch (const RepositoryError& e) {
            rejected = true;
            assert(e.kind() == RepositoryErrorKind::InvalidResponse);
            assert(std::string(e.what()) == (accountStage
                ? "FogHub page could not be verified (stage=account)"
                : "FogHub page could not be verified (stage=login-form)"));
        }
        assert(rejected && !s.auth.authenticated() && !s.store.saved && s.web.jar.empty());
        assert(s.web.posts == (accountStage ? 1U : 0U));
    }
    Setup expired;
    expired.web.responses.push_back({"/site/login",html(fixture("login.html"))});
    expired.web.responses.push_back({"/user/info",html(fixture("login.html"))});
    error(RepositoryErrorKind::NotAuthenticated,[&]{expired.auth.begin({"fixture-user","fixture-password"});});
    assert(!expired.auth.authenticated() && expired.web.jar.empty());
}
}
int main(){loginAndProfile();restoreAndFailures();securityAndStorage();historyAndAmounts();optionalBalanceAndPrecision();groupedBalanceAmounts();catalogs();pagedHistory();resizedPages();curatedComputers();publicCatalogWithoutAccountStorage();authenticationPageDiagnostics();std::cout<<"FogHub repository tests passed\n";}
