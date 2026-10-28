#include "features/providers/infrastructure/foghub_html.hpp"
#include "core/domain/repository_error.hpp"
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace fog;
using namespace fog::foghub;
namespace {
int checks = 0;
void check(bool result) {
    ++checks;
    if (!result) throw std::runtime_error("FogHub parser assertion failed at check " + std::to_string(checks));
}
std::string fixture(const std::string& name) {
    std::ifstream stream("tests/fixtures/foghub/" + name + ".html");
    if (!stream) throw std::runtime_error("FogHub fixture not found");
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}
std::string replace(std::string value, const std::string& from, const std::string& to) {
    const auto position = value.find(from);
    if (position == std::string::npos) throw std::runtime_error("FogHub fixture mutation not found: " + from);
    value.replace(position, from.size(), to);
    return value;
}
void rejects(const std::function<void()>& action, RepositoryErrorKind kind = RepositoryErrorKind::InvalidResponse) {
    try { action(); }
    catch (const RepositoryError& error) {
        check(error.kind() == kind);
        const std::string message = error.what();
        check(message.find("fixture-user") == std::string::npos && message.find("fixture-csrf") == std::string::npos && message.find("secret-not-captured") == std::string::npos);
        return;
    }
    throw std::runtime_error("FogHub parser accepted an unsafe fixture at check " + std::to_string(checks));
}
std::string historyPager(const std::string& html) {
    auto value = replace(html, "1–2 из 2", "1–2 из 3");
    return replace(value, "</main>",
        "<ul class='pagination'><li class='active'><a href='/session/list?page=1&amp;per-page=2' data-page='0'>1</a></li>"
        "<li class='next'><a href='/session/list?page=2&amp;per-page=2' data-page='1'>Next</a></li></ul></main>");
}
std::string accountWithTabs(std::string html) {
    // Synthetic full-page layout: the account tabs contain empty headings in
    // the live template. All account values still come from the safe fixture.
    html = replace(html, "<body>", "<body><header><nav>Fixture navigation</nav></header>");
    html = replace(html, "<h1>Информация</h1>", "<div class='user-view'><h1>Информация</h1>");
    return replace(html, "</main>", "<div class='tab-content'>"
        "<div class='tab-pane' id='operations'><div class='operation-list'><h1></h1></div></div>"
        "<div class='tab-pane' id='users'><div class='user-index'><h1> \n </h1></div></div>"
        "</div></div></main><footer>Fixture footer</footer>");
}
void loginAndAccount() {
    const auto login = fixture("login");
    check(parseLogin(login).csrf == "fixture-csrf-placeholder");
    rejects([&] { parseLogin(replace(login, "value=\"fixture-csrf-placeholder\"", "value=\"\"")); });
    rejects([&] { parseLogin(replace(login, "action=\"/site/login\"", "action=\"https://example.org/site/login\"")); });
    rejects([&] { parseLogin(replace(login, "type=\"password\"", "type=\"text\"")); });
    rejects([&] { parseLogin(replace(login, "</form>", "<input name='_csrf' type='hidden' value='duplicate'></form>")); });
    check(parseLogin(replace(login, "fixture-csrf-placeholder", "fixture&amp;placeholder")).csrf == "fixture&placeholder");
    const auto account = fixture("account");
    const auto profile = parseAccount(account);
    check(profile.accountId == "login:fixture-user" && profile.displayName == "fixture-user");
    const auto current = fixture("account-profile-synthetic");
    check(parseAccount(current).accountId == profile.accountId);
    check(parseAccount(current).displayName == profile.displayName);
    rejects([&] { parseAccount(replace(current,"<title>Профиль</title>","<title>Ошибка</title>")); });
    rejects([&] { parseAccount(replace(current,"<title>Профиль</title>","")); });
    rejects([&] { parseAccount(replace(current,"</head>","<title>Профиль</title></head>")); });
    rejects([&] { parseAccount(replace(current,"<h1></h1>","<h1>Ошибка</h1>")); });
    rejects([&] { parseAccount(replace(current,"/site/logout","/site/other")); });
    rejects([&] { parseAccount(replace(current,"<th>Логин</th>","<th>Другой</th>")); });
    rejects([&] { parseAccount(replace(current,"fixture-user","<span>fixture-user</span>")); });
    rejects([&] { parseAccount(replace(current,"</main>","<table class='detail-view'></table></main>")); });
    rejects([&] { parseAccount(replace(current,"</main>","<div id='captcha'></div></main>")); });
    check(parseAccount(replace(current,"secret-not-captured","<a>ignored</a>")).accountId == profile.accountId);
    const auto tabbedAccount = accountWithTabs(account);
    const auto tabbedProfile = parseAccount(tabbedAccount);
    check(tabbedProfile.accountId == profile.accountId && tabbedProfile.displayName == profile.displayName);
    rejects([&] { parseAccount(replace(tabbedAccount, "<h1>Информация</h1>", "<h1></h1>")); });
    rejects([&] { parseAccount(replace(tabbedAccount, "<h1>Информация</h1>", "<h1>Ошибка</h1>")); });
    rejects([&] { parseAccount(replace(tabbedAccount, "<h1></h1>", "<h1>Информация</h1>")); });
    rejects([&] { parseAccount(replace(tabbedAccount, "<h1></h1>", "<h1>Ошибка</h1>")); });
    rejects([&] { parseAccount(replace(tabbedAccount, "/site/logout", "/site/other")); });
    rejects([&] { parseAccount(replace(tabbedAccount, "</table>", "<tr><th>Логин</th><td>another-user</td></tr></table>")); });
    rejects([&] { parseAccount(replace(tabbedAccount, "<h1>Информация</h1>", "<h1 id='captcha'>Информация</h1>")); });
    // Other pages retain the existing requirement for exactly one h1.
    rejects([&] { parseLogin(replace(login, "</main>", "<h1></h1></main>")); });
    check(parseAccount(replace(account, "fixture-user", "fixture&amp;user")).accountId == "login:fixture&user");
    check(parseAccount(replace(account, "fixture-user", "  fixture  user  ")).accountId == "login:fixture  user");
    check(parseAccount(replace(account, "secret-not-captured", "<a href='/not-an-account-id'>ignored</a>")).accountId == profile.accountId);
    rejects([&] { parseAccount(replace(account, "/site/logout", "/site/other")); });
    rejects([&] { parseAccount(replace(account, "</table>", "<tr><th>Логин</th><td>another-user</td></tr></table>")); });
    rejects([&] { parseAccount(replace(account, "fixture-user", "<a href='/user/123'>fixture-user</a>")); });
    rejects([&] { parseAccount(login); }, RepositoryErrorKind::NotAuthenticated);
    rejects([&] { parseHistory(login); }, RepositoryErrorKind::NotAuthenticated);
}
void history() {
    const auto active = fixture("history-active");
    const auto terminal = fixture("history-terminal");
    const auto page = parseHistory(active);
    check(page.rows.size() == 2 && page.totalItems == 2 && page.nextPath.empty());
    const auto& first = page.rows[0];
    check(first.id == "9000002" && first.computerId == "1836" && first.computerName == "f4bX10");
    check(first.state == HistoryState::Active && first.startedAt == "01.01.2026 12:00" && first.finishedAt.empty());
    check(first.durationMinutes == "0" && first.amount == "0.00");
    check(page.rows[1].state == HistoryState::Finished && page.rows[1].finishedAt == "11:03");
    check(parseHistory(terminal).rows[0].state == HistoryState::Finished);
    const auto rated = fixture("history-rated-synthetic");
    const auto ratedPage = parseHistory(rated);
    check(ratedPage.rows.size() == 1 && ratedPage.rows[0].state == HistoryState::Finished &&
        ratedPage.rows[0].finishedAt == "12:03");
    for (const auto* rating : {"1","2","3","4","5"}) {
        check(parseHistory(replace(rated,"Оценка: 5",std::string("Оценка: ")+rating)).rows[0].state == HistoryState::Finished);
    }
    check(parseHistory(replace(rated,"Оценка: 5","Оценка:5")).rows[0].state == HistoryState::Finished);
    check(parseHistory(replace(rated,"Оценка: 5<br>","Оценка: 5<br>Синтетический отзыв")).rows[0].state == HistoryState::Finished);
    check(parseHistory(replace(rated,"Оценка: 5<br>","Оценка: 5<br>Первая строка<br>Вторая строка")).rows[0].state == HistoryState::Finished);
    check(parseHistory(replace(rated,"Оценка: 5<br>","Оценка: 5<br>Оценка: 1<br>Текст отзыва")).rows[0].state == HistoryState::Finished);
    rejects([&] { parseHistory(replace(rated,"Оценка: 5<br>","Оценка: 6<br>Оценка: 5<br>Текст отзыва")); });
    rejects([&] { parseHistory(replace(rated,"Оценка: 5<br>","Текст отзыва<br>Оценка: 5")); });
    rejects([&] { parseHistory(replace(rated,"Оценка: 5<br>","Оценка: 5<br><span>Текст</span>")); });
    for (const auto* invalidRating : {"0","6","55","5?","unknown"})
        rejects([&] { parseHistory(replace(rated,"Оценка: 5",std::string("Оценка: ")+invalidRating)); });
    rejects([&] { parseHistory(replace(rated,"Оценка: 5","Unknown: 5")); });
    rejects([&] { parseHistory(replace(rated,"Оценка: 5<br>","Оценка: 5")); });
    rejects([&] { parseHistory(replace(rated,"12:00-12:03","12:00-?")); });
    auto contradictoryRated = replace(replace(rated,"Завершена","Активна"),"12:00-12:03","12:00-?");
    contradictoryRated = replace(contradictoryRated,"Бонус: 1мин.","<a href='/session/close?id=9000031'>Завершить</a>");
    rejects([&] { parseHistory(contradictoryRated); });
    check(parseHistory(replace(rated,"Завершена","Неизвестный статус")).rows[0].state == HistoryState::Unknown);
    auto unknown = replace(terminal, "<small>Завершена", "<small>Неизвестный статус");
    check(parseHistory(unknown).rows[0].state == HistoryState::Unknown);
    unknown = replace(active, "<small>Активна", "<small>Ожидание");
    check(parseHistory(unknown).rows[0].state == HistoryState::Unknown);
    check(parseHistory(replace(unknown, "/session/close?id=9000002", "/session/not-close?id=9000002")).rows[0].state == HistoryState::Unknown);
    check(parseHistory(replace(active, ">f4bX10</a>", ">f4bX&amp;10</a>")).rows[0].computerName == "f4bX&10");
    rejects([&] { parseHistory(replace(active, "/session/close?id=9000002", "/session/close?id=9000999")); });
    rejects([&] { parseHistory(replace(terminal, "/session/review?id=9000002", "/session/review?id=9000999")); });
    rejects([&] { parseHistory(replace(active, "/pc/preview?id=1836", "/pc/preview?id=not-a-pc")); });
    rejects([&] { parseHistory(replace(active, "/pc/preview?id=1836", "https://example.org/pc/preview?id=1836")); });
    rejects([&] { parseHistory(replace(active, "data-key=\"9000001\"", "data-key=\"9000002\"")); });
    rejects([&] { parseHistory(replace(terminal, "12:00-12:03", "12:00-?")); });
    rejects([&] { parseHistory(replace(active, "12:00-?", "12:00-12:03")); });
    rejects([&] { parseHistory(replace(active, "/session/close?id=9000002", "/session/not-close?id=9000002")); });
    rejects([&] { parseHistory(replace(active, "Журнал сессий", "Ошибка")); });
    rejects([&] { parseHistory(replace(active, ">Цена</th>", ">Другая цена</th>")); });
    rejects([&] { parseHistory(replace(active, "12:00-?", "25:00-?")); });
    const auto overnight = parseHistory(replace(terminal, "12:00-12:03", "23:59-00:03"));
    check(overnight.rows[0].state == HistoryState::Finished && overnight.rows[0].startedAt == "01.01.2026 23:59" && overnight.rows[0].finishedAt == "00:03");
    rejects([&] { parseHistory(replace(active, "<td>0.00</td>", "<td>неизвестно</td>")); });
    rejects([&] { parseHistory(replace(active, "</tbody>", "<tr data-key='9000999'><td>truncated</tbody>")); });
    const auto filtered = fixture("history-reason-filter-empty");
    rejects([&] { parseHistory(filtered); });
    auto empty = replace(filtered, "value=\"1\" selected=\"\"", "value=\"1\"");
    const std::string filteredQuery = "SessionSearch%5Bcomment_id%5D=1";
    while (empty.find(filteredQuery) != std::string::npos) empty = replace(empty, filteredQuery, "SessionSearch%5Bcomment_id%5D=");
    check(parseHistory(empty).rows.empty() && parseHistory(empty).totalItems == 0);
    rejects([&] { parseHistory(replace(empty, "Ничего не найдено.", "Ошибка сервера")); });
    // A valid nonterminal page has an explicit, consistent next link.
    const auto paged = historyPager(active);
    check(parseHistory(paged).nextPath == "/session/list?page=2&per-page=2" && parseHistory(paged).totalItems == 3);
    rejects([&] { parseHistory(replace(active, "1–2 из 2", "1–2 из 3")); });
    rejects([&] { parseHistory(replace(paged, "/session/list?page=2", "https://example.org/session/list?page=2")); });
    rejects([&] { parseHistory(replace(paged, "/session/list?page=2", "/session/close?page=2")); });
    rejects([&] { parseHistory(replace(paged, "page=2&amp;per-page=2", "page=2&amp;page=3&amp;per-page=2")); });
    rejects([&] { parseHistory(replace(paged, "data-page='1'", "data-page='0'")); });
    rejects([&] { parseHistory(replace(paged, "href='/session/list?page=1&amp;per-page=2' data-page='0'", "href='/session/list?page=2&amp;per-page=2' data-page='1'")); });
    rejects([&] { parseHistory(replace(paged, "class='next'", "class='next disabled'")); });
    rejects([&] { parseHistory(replace(paged, "page=2&amp;per-page=2", "page=2&amp;SessionSearch%5Bpc_id%5D=1836")); });
    const auto last = replace(replace(replace(paged, "1–2 из 3", "3–4 из 4"), "class='next'", "class='next disabled'"),
        "href='/session/list?page=1&amp;per-page=2' data-page='0'", "href='/session/list?page=2&amp;per-page=2' data-page='1'");
    check(parseHistory(last).nextPath.empty() && parseHistory(last).totalItems == 4);
}
void catalog() {
    const auto games = fixture("games");
    const auto page = parseGames(games);
    check(page.rows.size() == 24 && page.totalItems == 6767 && page.nextPath == "/game/list?page=2&per-page=24");
    check(page.rows[0].id == "18726" && page.rows[1].name == "Cyberpunk 2077");
    check(page.rows[1].imageUrl.find("https://shared.akamai.steamstatic.com/") == 0);
    bool foundNoImage = false;
    for (const auto& game : page.rows) if (game.id == "17796") foundNoImage = game.imageUrl.empty() && !game.name.empty();
    check(foundNoImage);
    rejects([&] { parseGames(replace(games, "/game/preview?id=70", "/game/preview?id=18726")); });
    rejects([&] { parseGames(replace(games, "<h1>Все игры</h1>", "<h1>Error</h1>")); });
    rejects([&] { parseGames(replace(games, "/game/list?page=2", "/site/login?page=2")); });
    const auto computers = fixture("computers");
    const auto pcs = parseComputers(computers, "70");
    check(pcs.game.id == "70" && pcs.game.name == "Cyberpunk 2077");
    check(pcs.rows.size() == 24 && pcs.totalItems == 325 && pcs.nextPath == "/game/preview?id=70&page=2&per-page=24");
    check(pcs.rows[0].id == "148" && pcs.rows[0].name == "d066X2" && pcs.rows[0].owner == "mihanik");
    check(pcs.rows[0].online && pcs.rows[0].fhAvailable && pcs.rows[0].pricePerHour == "20");
    check(!pcs.rows.back().fhAvailable && pcs.rows.back().pricePerHour.empty());
    rejects([&] { parseComputers(computers, "71"); });
    rejects([&] { parseComputers(replace(computers, "/pc/play?id=148", "/pc/play?id=151"), "70"); });
    rejects([&] { parseComputers(replace(computers, "Играть FH 20", "Играть МТС 20"), "70"); });
    rejects([&] { parseComputers(replace(computers, "id=70&amp;page=2", "id=71&amp;page=2"), "70"); });
    rejects([&] { parseComputers(fixture("login"), "70"); }, RepositoryErrorKind::NotAuthenticated);
    rejects([&] { parseGames(fixture("login")); }, RepositoryErrorKind::NotAuthenticated);
}
void malformedAndLimits() {
    const auto login = fixture("login");
    rejects([] { parseLogin(""); });
    rejects([] { parseHistory("<html><body>upstream error</body></html>"); });
    rejects([] { parseHistory("<html><h1>Checking your browser</h1><form id='challenge-form'></form></html>"); });
    rejects([&] { parseLogin(replace(login, "<h1>", "<h1 id='captcha'>")); });
    rejects([&] { parseLogin("<!DOCTYPE html [<!ENTITY xxe SYSTEM 'file:///etc/passwd'>]>" + login); });
    rejects([&] { parseLogin("<!DOCTYPE html [<!ENTITY a 'secret-not-captured'>]>" + login); });
    rejects([&] { parseLogin(login + std::string(2 * 1024 * 1024, ' ')); });
    rejects([&] { parseLogin(login + std::string(1, '\0')); });
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "<div>";
    for (int i = 0; i < 100; ++i) deep += "</div>";
    rejects([&] { parseLogin(replace(login, "</main>", deep + "</main>")); });
    std::string many;
    for (int i = 0; i < 60001; ++i) many += "<i></i>";
    rejects([&] { parseLogin(replace(login, "</main>", many + "</main>")); });
    // Memory parser does not fetch an external DTD and does not evaluate scripts.
    check(parseLogin("<!DOCTYPE html SYSTEM 'https://127.0.0.1:9/not-loaded'>" + replace(login, "<!doctype html>", "")).csrf == "fixture-csrf-placeholder");
    rejects([&] { parseLogin(replace(login, "action=\"/site/login\"", "action=\"/site/login")); });
}
}
int main() {
    try {
        loginAndAccount();
        history();
        catalog();
        malformedAndLimits();
        std::cout << "FogHub HTML parser tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (after " << checks << " checks)\n";
        return 1;
    }
}
