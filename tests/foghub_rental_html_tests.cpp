#include "features/providers/infrastructure/foghub_html.hpp"
#include "core/domain/repository_error.hpp"
#include "features/session/domain/session_rating.hpp"
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
    if (!result) throw std::runtime_error("FogHub rental parser assertion failed at check " + std::to_string(checks));
}
std::string fixture(const std::string& name) {
    std::ifstream stream("tests/fixtures/foghub/" + name + ".html");
    if (!stream) throw std::runtime_error("FogHub rental fixture not found");
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}
std::string replace(std::string value, const std::string& from, const std::string& to) {
    const auto position = value.find(from);
    if (position == std::string::npos) throw std::runtime_error("FogHub rental fixture mutation not found");
    value.replace(position, from.size(), to);
    return value;
}
void rejects(const std::function<void()>& action, RepositoryErrorKind expected = RepositoryErrorKind::InvalidResponse) {
    try { action(); }
    catch (const RepositoryError& error) {
        if (error.kind() != expected) throw std::runtime_error("Unexpected repository error category: " + std::to_string(static_cast<int>(error.kind())) + " expected " + std::to_string(static_cast<int>(expected)) + " at check " + std::to_string(checks));
        check(error.kind() == expected);
        const std::string message = error.what();
        check(message.find("synthetic-csrf") == std::string::npos && message.find("192.0.2.42") == std::string::npos);
        return;
    }
    throw std::runtime_error("FogHub rental parser accepted invalid input at check " + std::to_string(checks));
}
void endpoints() {
    for (const auto& value : {"192.0.2.42:47989", "stream.example.com:1", "stream-42.example.com:65535", "[2001:db8::42]:47989"})
        check(parseMoonlightEndpoint(value).port != 0);
    check(parseMoonlightEndpoint("[2001:db8::42]:47989").host == "2001:db8::42");
    for (const auto& value : {"", "https://stream.example.com:47989", "foghub://stream.example.com:47989", "//stream.example.com:47989",
        "user:password@stream.example.com:47989", "stream.example.com:47989/path", "stream.example.com:47989?route=x",
        "stream.example.com:47989#x", "stream.example.com:0", "stream.example.com:65536", "stream.example.com:-1",
        "stream.example.com:047989", "stream.example.com", "stream.example.com:", "stream.example.com:abc",
        " stream.example.com:47989", "stream.example.com:47989\n", "stream.example.com:47989\t", "stream.example.com\\x:47989",
        "256.0.0.1:47989", "127.1:47989", "2130706433:47989", "010.0.0.1:47989", "0x7f.0x0.0x0.0x1:47989", "localhost:47989",
        "-bad.example.com:47989", "bad-.example.com:47989", "bad..example.com:47989", "bad.example.com.:47989",
        "bad_example.com:47989", "bad.123:47989", "2001:db8::42:47989", "[broken]:47989", "[2001:db8::42]",
        "[2001:db8::42]:47989:1", "[fe80::42%eth0]:47989", "https:47989", "stream.example.com:47989;evil"})
        rejects([&] { parseMoonlightEndpoint(value); });
    rejects([] { parseMoonlightEndpoint(std::string("stream.example.com:47989") + char(1)); });
    rejects([] { parseMoonlightEndpoint(std::string(254, 'a') + ".com:47989"); });
}
void forms() {
    const auto html = fixture("connect-metadata-synthetic");
    const auto parsed = parseConnectForm(html, "42");
    check(parsed.csrf == "synthetic-csrf-placeholder" && parsed.actionPath == "/pc/play?id=42");
    check(parsed.moonlightUrl == "192.0.2.42:47989" && parsed.endpoint.host == "192.0.2.42" && parsed.endpoint.port == 47989);
    check(parseConnectForm(replace(html,"/pc/play?id=42","/pc/play?id=42&amp;resume=0"),"42").actionPath == "/pc/play?id=42&resume=0");
    check(freshPairingLink(html,"42").empty());
    const std::string previous = "<!doctype html><html><body><form method='post' action='/pc/play?id=42'><button>Продолжить</button></form><a href='/pc/play?id=42&amp;resume=0'>подключите его снова</a></body></html>";
    check(freshPairingLink(previous,"42") == "/pc/play?id=42&resume=0");
    rejects([&] { freshPairingLink(previous,"43"); });
    rejects([&] { freshPairingLink(replace(previous,"resume=0","resume=1"),"42"); });
    rejects([&] { freshPairingLink(replace(previous,"href=","data-method='post' href="),"42"); });
    check(parseConnectForm(replace(html, "synthetic-csrf-placeholder", "synthetic&amp;csrf"), "42").csrf == "synthetic&csrf");
    rejects([&] { parseConnectForm(html, "43"); });
    rejects([&] { parseConnectForm(html, "42&other=1"); });
    for (const auto& action : {"https://evil.example/pc/play?id=42", "//foghub.ru/pc/play?id=42", "/pc/play?id=42&other=1", "/pc/play?id=42&", "/pc/preview?id=42"})
        rejects([&] { parseConnectForm(replace(html, "/pc/play?id=42", action), "42"); });
    for (const auto& attr : {"method=\"get\"", "method=\"post\" target=\"_blank\"", "method=\"post\" onsubmit=\"evil()\"", "method=\"post\" data-method=\"post\""})
        rejects([&] { parseConnectForm(replace(html, "method=\"post\"", attr), "42"); });
    rejects([&] { parseConnectForm(replace(html, "synthetic-csrf-placeholder", ""), "42"); });
    rejects([&] { parseConnectForm(replace(html, "synthetic-csrf-placeholder", "bad&#10;csrf"), "42"); });
    rejects([&] { parseConnectForm(replace(html, "name=\"_csrf\"", "name=\"missing\""), "42"); });
    rejects([&] { parseConnectForm(replace(html, "name=\"Pc[pin]\"", "name=\"pin\""), "42"); });
    rejects([&] { parseConnectForm(replace(html, "type=\"hidden\"", "type=\"text\""), "42"); });
    rejects([&] { parseConnectForm(replace(html, "name=\"moonlight_url\"", "disabled name=\"moonlight_url\""), "42"); });
    rejects([&] { parseConnectForm(replace(html, "name=\"moonlight_url\"", "form='elsewhere' name=\"moonlight_url\""), "42"); });
    rejects([&] { parseConnectForm(replace(html, "</form>", "<input type='hidden' name='_csrf' value='duplicate'></form>"), "42"); });
    rejects([&] { parseConnectForm(replace(html, "</form>", "<input type='hidden' name='unknown' value='1'></form>"), "42"); });
    rejects([&] { parseConnectForm(replace(html, "<button", "<button formaction='/pc/play?id=99'"), "42"); });
    rejects([&] { parseConnectForm(replace(html, "192.0.2.42:47989", "https://evil.example:47989"), "42"); });
    rejects([&] { parseConnectForm(html + html, "42"); });
    rejects([&] { parseConnectForm(replace(html, "</head>", "<base href='https://evil.example'></head>"), "42"); });
    rejects([&] { parseConnectForm(fixture("login"), "42"); }, RepositoryErrorKind::NotAuthenticated);
    rejects([&] { parseConnectForm(replace(html, "<body>", "<body><form id='challenge-form'></form>"), "42"); });
    rejects([&] { parseConnectForm("<!DOCTYPE html [<!ENTITY xxe SYSTEM 'file:///etc/passwd'>]>" + html, "42"); });
    rejects([&] { parseConnectForm(html + std::string(2 * 1024 * 1024, ' '), "42"); });
}
void success() {
    const auto html = fixture("launch-success-metadata-synthetic");
    const std::string path = "/pc/play-success?id=42&withResume=0";
    check(parseLaunchSuccess(html, "42", path).rentalId == "9000042");
    for (const auto& badPath : {"/pc/play-success?id=9000042&withResume=0", "/pc/play-success?id=42&withResume=1", "/pc/play-success?id=42", "https://evil.example/pc/play-success?id=42&withResume=0", "/pc/play-success?id=42&withResume=0&extra=1"})
        rejects([&] { parseLaunchSuccess(html, "42", badPath); });
    for (const auto& badLink : {"https://evil.example/session/review?id=9000042", "/session/review?id=0", "/session/review?id=no", "/session/review?id=9000042&other=1", "/session/review?id=9000042&", "/session/close?id=9000042"})
        rejects([&] { parseLaunchSuccess(replace(html, "/session/review?id=9000042", badLink), "42", path); });
    for (const auto& link : {"/session/review?id=9000042", "/session/review?id=9000043", "https://evil.example/session/review?id=9000043"})
        rejects([&] { parseLaunchSuccess(replace(html, "</body>", "<a href='" + std::string(link) + "'>Other</a></body>"), "42", path); });
    rejects([&] { parseLaunchSuccess(fixture("login"), "42", path); }, RepositoryErrorKind::NotAuthenticated);
}
void reviewRequired() {
    const auto html = fixture("connect-review-required-synthetic");
    rejects([&] { freshPairingLink(html,"42"); },RepositoryErrorKind::ReviewRequired);
    rejects([&] { parseConnectForm(html,"42"); },RepositoryErrorKind::ReviewRequired);
    try { (void)freshPairingLink(html,"42"); }
    catch (const SessionReviewRequired& error) {
        check(error.rental() == RentalId({ProviderId::FogHub,"9000042"}));
        check(std::string(error.what()).find("9000042") == std::string::npos);
    }
    for (const auto& link : {"/session/review?id=9000042&fromPcId=43", "/session/review?id=0&fromPcId=42",
        "/session/review?id=9000042&fromPcId=042", "/session/review?id=9000042",
        "/session/review?id=9000042&fromPcId=42&other=1", "/session/review?id=9000042&fromPcId=42&fromPcId=42",
        "/session/review?id=9000042&fromPcId=42&",
        "https://evil.example/session/review?id=9000042&fromPcId=42"}) {
        const auto changed = replace(html,"/session/review?id=9000042&amp;fromPcId=42",link);
        rejects([&] { freshPairingLink(changed,"42"); });
        rejects([&] { parseConnectForm(changed,"42"); });
    }
    rejects([&] { freshPairingLink(replace(html,"href=","onclick='unsafe()' href="),"42"); });
    rejects([&] { parseConnectForm(replace(html,"href=","data-method='post' href="),"42"); });
    rejects([&] { parseConnectForm(replace(html,">Оценить</a>",">Оценить</a><a href='/session/review?id=9000043&amp;fromPcId=42'>Оценить</a>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"/pc/play?id=42","/pc/play?id=43"),"42"); });
    const auto unknown = replace(html,"сперва оцените предыдущую игру через Foghub!","попробуйте позже!");
    rejects([&] { freshPairingLink(unknown,"42"); });
    rejects([&] { parseConnectForm(unknown,"42"); });
    rejects([&] { parseConnectForm(replace(html,"text-danger","other"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<form method=","<div method="),"42"); });
}
void bonusChoice() {
    const auto html = fixture("connect-bonus-metadata-synthetic");
    check(parseConnectForm(html,"42").hasBonusChoice);
    check(!parseConnectForm(fixture("connect-metadata-synthetic"),"42").hasBonusChoice);
    check(parseConnectForm(replace(html," selected",""),"42").hasBonusChoice);
    check(parseConnectForm(replace(html,"/pc/play?id=42","/pc/play?id=42&amp;resume=0"),"42").hasBonusChoice);
    for (const auto& control : {"name=\"Pc[use_bonus]\" multiple", "name=\"Pc[use_bonus]\" disabled",
        "name=\"Pc[use_bonus]\" size=\"2\"", "name=\"Pc[use_bonus]\" onchange=\"x()\"",
        "name=\"Pc[use_bonus]\" formmethod=\"get\"",
        "name=\"Pc[use_bonus]\" onclick=\"x()\"", "name=\"Pc[use_bonus]\" form=\"other\"",
        "name=\"Pc[unknown]\""})
        rejects([&] { parseConnectForm(replace(html,"name=\"Pc[use_bonus]\"",control),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<option value=\"0\" selected>","<option value=\"1\" selected>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<option value=\"1\">","<option value=\"0\">"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<option value=\"1\">","<option value=\"2\">"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<option value=\"1\">","<option value=\"1\" selected>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<option value=\"0\" selected>","<option value=\"0\" selected disabled>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"<option value=\"1\">","<option value=\"1\" data-method=\"post\">"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"</select>","<option value='2'>Unknown</option></select>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"</select>","</select><select name='Pc[use_bonus]'><option value='0'>Accumulate</option><option value='1'>Spend</option></select>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"</select>","</select><input name='Pc[use_bonus]' value='0'>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"</select>","</select><input name='unknown' value='0'>"),"42"); });
    rejects([&] { parseConnectForm(replace(html,"Накапливать бонусы",""),"42"); });
}
void closeActions() {
    const auto html = fixture("history-active");
    const auto close = parseCloseAction(html, "9000002");
    check(close.path == "/session/close?id=9000002" && close.method == CloseMethod::Get);
    rejects([&] { parseCloseAction(html, "9000001"); });
    rejects([&] { parseCloseAction(html, "9999999"); });
    rejects([&] { parseCloseAction(fixture("history-terminal"), "9000002"); });
    rejects([&] { parseCloseAction(fixture("history-reason-filter-empty"), "9000002"); });
    for (const auto& attr : {"data-method='post'", "data-method='get'", "data-method=''", "onclick='evil()'", "target='_blank'", "download", "data-params='{}'"})
        rejects([&] { parseCloseAction(replace(html, "href=\"/session/close", std::string(attr) + " href=\"/session/close"), "9000002"); });
    rejects([&] { parseCloseAction(replace(html, "/session/close?id=9000002", "/session/close?id=9000001"), "9000002"); });
    rejects([&] { parseCloseAction(replace(html, "/session/close?id=9000002", "https://evil.example/session/close?id=9000002"), "9000002"); });
    rejects([&] { parseCloseAction(replace(html, "Вы уверены, что хотите завершить эту сессию?", "Changed contract"), "9000002"); });
    rejects([&] { parseCloseAction(replace(html, "<small>Активна", "<small>Unknown"), "9000002"); });
    rejects([&] { parseCloseAction(fixture("login"), "9000002"); }, RepositoryErrorKind::NotAuthenticated);
}
void computerPreview() {
    const auto html = fixture("computer-preview-card-excerpt");
    const auto pc = parseComputerPreview(html,"1838");
    check(pc.id == "1838" && pc.name == "f4bX8" && pc.online && !pc.busy && pc.fhAvailable && pc.pricePerHour == "25");
    rejects([&] { parseComputerPreview(html,"1836"); });
    rejects([&] { parseComputerPreview(replace(html,"/pc/play?id=1838","/pc/play?id=1836"),"1838"); });
    rejects([&] { parseComputerPreview(replace(html,"Играть FH 25₽","Играть МТС 25₽"),"1838"); });
    rejects([&] { parseComputerPreview(replace(html,"Играть FH 25₽","Играть FH 0₽"),"1838"); });
    rejects([&] { parseComputerPreview(replace(html,"id=1838&amp;mode=","id=1838&amp;close=1"),"1838"); });
    check(!parseComputerPreview(replace(html,"Онлайн","Занят"),"1838").fhAvailable);
    check(!parseComputerPreview(replace(html,"Онлайн","Офлайн"),"1838").fhAvailable);
    check(!parseComputerPreview(replace(html,"href=\"/pc/play?id=1838\"","href=\"/pc/click-play?id=1838\""),"1838").fhAvailable);
}
}
int main() {
    try {
        endpoints(); forms(); success(); reviewRequired(); bonusChoice(); closeActions(); computerPreview();
        std::cout << "FogHub rental HTML parser tests passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (after " << checks << " checks)\n";
        return 1;
    }
}
