#include "features/providers/infrastructure/foghub_rental_repository.hpp"
#include "features/providers/infrastructure/foghub_review_html.hpp"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
using namespace fog;
using namespace fog::foghub;
namespace {
unsigned checks=0;
void check(bool value) {++checks;assert(value);}
std::string fixture(const std::string& name) {
    std::ifstream file("tests/fixtures/foghub/"+name+".html");assert(file.good());return {std::istreambuf_iterator<char>(file),{}};
}
std::string replace(std::string value,const std::string& from,const std::string& to) {
    const auto at=value.find(from);assert(at!=std::string::npos);value.replace(at,from.size(),to);return value;
}
std::string rated(std::string page,int score=4) {
    return replace(std::move(page),"<a class=\"btn btn-sm btn-warning\" href=\"/session/review?id=9000002\" pjax=\"0\">Оценить</a>",
        "<small><div>Оценка: "+std::to_string(score)+"<br>Синтетический отзыв</div></small>");
}
template<class Action> void rejects(RepositoryErrorKind kind,Action action) {
    bool caught=false;try{action();}catch(const RepositoryError& error) {
        caught=true;check(error.kind()==kind);const std::string message=error.what();
        check(message.find("synthetic-review-csrf")==std::string::npos && message.find("private text")==std::string::npos);
    }check(caught);
}
WebResponse html(const std::string& body) {return {200,body,{},"text/html; charset=UTF-8"};}
struct Web final:WebTransport {
    std::string history=fixture("history-terminal"),review=fixture("review-form-synthetic"),profile=fixture("account");
    unsigned posts=0,reviewReads=0,closes=0,pairs=0;bool accept=true,lose=false,failAfter=false,wrongScore=false,multiline=false;
    Query fields;std::function<void()> formAction;
    WebResponse get(const std::string& path) override {
        if(path=="/user/info")return html(profile);
        if(path=="/user/balance")return {200,"{\"balance\":\"250.00\"}",{},"application/json"};
        if(path=="/session/list") {if(posts&&failAfter)throw TransportError(TransportFailure::Network);return html(history);}
        if(path=="/session/review?id=9000002" || path=="/session/review?id=9000001") {
            ++reviewReads;if(formAction)formAction();return html(path.back()=='1'?replace(review,"id=9000002","id=9000001"):review);
        }
        assert(false && "Unexpected request");return {};
    }
    WebResponse submitSessionRating(const std::string& rental,const Query& sent) override {
        check(rental=="9000002");++posts;fields=sent;
        if(accept) {
            history=rated(history,wrongScore?5:4);
            if(multiline)history=replace(history,"<br>Синтетический отзыв","<br>Первая строка<br>Оценка: 5<br>Последняя строка");
        }
        if(lose)throw TransportError(TransportFailure::Network);
        return {302,{},"/session/list",{}};
    }
    WebResponse submitPairing(const std::string&,const Query&) override {++pairs;assert(false);return {};}
    WebResponse closeRental(const std::string&) override {++closes;assert(false);return {};}
    WebResponse submitLogin(const Query&) override {assert(false);return {};}
    std::vector<std::string> cookies() override{return {"synthetic-cookie"};}
    void restoreCookies(const std::vector<std::string>&) override{}
    void clearCookies() override{}
    void cancel() noexcept override{}
};
struct Store final:CookieStore {
    std::optional<SavedCookies> load()override{return SavedCookies{"login:fixture-user",{"synthetic-cookie"}};}
    void save(const SavedCookies&)override{}void clear()override{}
};
struct Setup {Web web;Store store;Session session{web,store};Rentals rentals{session};Setup(){assert(session.restore().restored);}
    SessionRatingForm form(){auto result=rentals.ratingForm(RentalId{ProviderId::FogHub,"9000002"});check(result.has_value());return *result;}
    SessionRatingRequest request(){const auto loaded=form();return {loaded.rental,loaded.preparation,4,"private text\nвторая строка"};}
};
void parser() {
    const auto source=fixture("review-form-synthetic");check(parseSessionReviewForm(source,"9000002").csrf=="synthetic-review-csrf");
    const std::pair<std::string,std::string> mutations[]={
        {"action=\"/session/review?id=9000002\"","action=\"/session/review?id=9000001\""},
        {"action=\"/session/review?id=9000002\"","action=\"https://foghub.ru/session/review?id=9000002\""},
        {"action=\"/session/review?id=9000002\"","action=\"/session/review?id=9000002&amp;x=1\""},
        {"id=\"review-form\"","id=\"review-form\" target=\"_blank\""},
        {"id=\"review-form\"","id=\"review-form\" enctype=\"multipart/form-data\""},
        {"id=\"review-form\"","id=\"review-form\" onsubmit=\"other()\""},
        {"Review[rating]\" value=\"1\"","Review[rating]\" value=\"2\""},
        {"value=\"5\" checked","value=\"5\""},
        {"maxlength=\"2000\"","maxlength=\"3000\""},
        {"maxlength=\"2000\"","maxlength=\"2000\" required"},
        {"name=\"_csrf\" value=\"synthetic-review-csrf\"","name=\"_csrf\" value=\"\""},
        {"name=\"Review[rating]\" value=\"\"","name=\"Review[rating]\" value=\"0\""},
        {"<button type=\"submit\">","<button type=\"submit\" formaction=\"/session/close?id=9000002\">"},
        {"</main>","<input form=\"review-form\" name=\"extra\"></main>"},
        {"</textarea>","</textarea><input name=\"unexpected\" value=\"1\">"},
        {"Оцените игру:","Другая страница"},
        {"<head>","<head><base href=\"https://invalid.example\">"},
        {"<input type=\"radio\" name=\"Review[rating]\" value=\"1\">",""},
        {"<textarea name=", "<textarea disabled name="}
    };
    for(const auto& change:mutations)rejects(RepositoryErrorKind::InvalidResponse,[&]{parseSessionReviewForm(replace(source,change.first,change.second),"9000002");});
    rejects(RepositoryErrorKind::InvalidResponse,[&]{parseSessionReviewForm(source,"9000002&x=1");});
    rejects(RepositoryErrorKind::InvalidResponse,[&]{parseSessionReviewForm(source+"<form></form>","9000002");});
    rejects(RepositoryErrorKind::NotAuthenticated,[&]{parseSessionReviewForm(replace(source,"/site/logout","/site/login"),"9000002");});
    check(!sessionHistoryRating(fixture("history-terminal"),"9000002"));
    check(sessionHistoryRating(rated(fixture("history-terminal")),"9000002")==4);
    rejects(RepositoryErrorKind::InvalidResponse,[&]{sessionHistoryRating(fixture("history-active"),"9000002");});
    for(int score=1;score<=5;++score)check(sessionHistoryRating(rated(fixture("history-terminal"),score),"9000002")==score);
    check(validSessionReviewMessage(std::string(2000,'a')));check(!validSessionReviewMessage(std::string(2001,'a')));
    check(validSessionReviewMessage("Привет\n\tмир"));check(!validSessionReviewMessage(std::string("a\0b",3)));
    check(!validSessionReviewMessage("\xc0\xaf"));check(!validSessionReviewMessage("\xed\xa0\x80"));
    check(!validSessionReviewMessage("\xf4\x90\x80\x80"));
    std::string emoji;for(int i=0;i<1000;++i)emoji+="😀";check(validSessionReviewMessage(emoji));check(!validSessionReviewMessage(emoji+"a"));
}
void repository() {
    {Setup s;check(s.rentals.sessionRatingSupported());const auto form=s.form();check(form.rental.value=="9000002" && !form.alreadyRated && !form.preparation.empty());
     check(form.commentSupported && form.commentMaxLength==2000 && !form.existingScore);check(s.web.posts==0 && s.web.closes==0 && s.web.pairs==0);}
    {Setup s;const auto form=s.form();check(form.sessionDate=="01.01.2026 12:00" && form.computerName=="f4bX10");}
    {Setup s;const auto form=s.rentals.ratingForm();check(form && form->rental.value=="9000002");check(s.web.posts==0);}
    {Setup s;s.web.history=replace(s.web.history,"11:00-11:03","13:00-13:03");
     const auto form=s.rentals.ratingForm();check(form && form->rental.value=="9000001");check(s.web.posts==0);}
    {Setup s;s.web.history=replace(s.web.history,"11:00-11:03","12:00-12:03");
     rejects(RepositoryErrorKind::InvalidResponse,[&]{s.rentals.ratingForm();});check(s.web.posts==0 && s.web.reviewReads==0);}
    {Setup s;s.web.history=rated(s.web.history);s.web.history=replace(s.web.history,"<a class=\"btn btn-sm btn-warning\" href=\"/session/review?id=9000001\" pjax=\"0\">Оценить</a>","<small><div>Оценка: 5<br></div></small>");
     check(!s.rentals.ratingForm());check(s.web.reviewReads==0 && s.web.posts==0);const auto form=s.form();check(form.alreadyRated && form.existingScore==4 && form.preparation.empty());}
    {Setup s;auto request=s.request();const auto result=s.rentals.submitRating(request);check(result.rental==request.rental && result.confirmedScore==4);check(s.web.posts==1 && s.web.reviewReads==2);
     check(s.web.fields==Query({{"_csrf","synthetic-review-csrf"},{"Review[rating]","4"},{"Review[message]",request.comment}}));
     rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==1);}
    {Setup s;auto request=s.request();s.web.lose=true;check(s.rentals.submitRating(request).confirmedScore==4);check(s.web.posts==1);}
    {Setup s;auto request=s.request();s.web.multiline=true;check(s.rentals.submitRating(request).confirmedScore==4);check(s.web.posts==1);}
    {Setup s;auto request=s.request();s.web.accept=false;rejects(RepositoryErrorKind::OutcomeUnknown,[&]{s.rentals.submitRating(request);});check(s.web.posts==1);
     s.web.accept=true;check(s.rentals.submitRating(request).confirmedScore==4);check(s.web.posts==2);}
    {Setup s;auto request=s.request();s.web.failAfter=true;rejects(RepositoryErrorKind::OutcomeUnknown,[&]{s.rentals.submitRating(request);});check(s.web.posts==1);}
    {Setup s;auto request=s.request();s.web.wrongScore=true;rejects(RepositoryErrorKind::OutcomeUnknown,[&]{s.rentals.submitRating(request);});check(s.web.posts==1);
     rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==1);}
    {Setup s;auto request=s.request();s.web.history=rated(s.web.history);check(s.rentals.submitRating(request).confirmedScore==4);check(s.web.posts==0);}
    {Setup s;auto request=s.request();s.web.history=fixture("history-active");rejects(RepositoryErrorKind::InvalidResponse,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;auto request=s.request();s.web.history=replace(s.web.history,"id=1836","id=42");rejects(RepositoryErrorKind::InvalidResponse,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;auto request=s.request();s.session.forget();rejects(RepositoryErrorKind::NotAuthenticated,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;auto request=s.request();s.session.forget();check(s.session.restore().restored);
     rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;auto request=s.request();request.rental={ProviderId::FogHub,"9000001"};
     rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.ratingForm(RentalId{ProviderId::FogHub,"9000003"});});check(s.web.posts==0);}
    {Setup s;auto request=s.request();s.form();rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;auto request=s.request();s.web.formAction=[&]{s.session.forget();};rejects(RepositoryErrorKind::NotAuthenticated,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    for(int score:{0,6}){Setup s;auto request=s.request();request.score=score;rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;auto request=s.request();request.comment=std::string(2001,'a');rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.submitRating(request);});check(s.web.posts==0);}
    {Setup s;rejects(RepositoryErrorKind::InvalidRequest,[&]{s.rentals.ratingForm(RentalId{ProviderId::Mts,"9000002"});});check(s.web.posts==0);}
    {Setup s;s.web.review=replace(s.web.review,"value=\"1\"","value=\"2\"");rejects(RepositoryErrorKind::InvalidResponse,[&]{s.form();});check(s.web.posts==0);}
}
}
int main(){parser();repository();std::cout<<"FogHub review: "<<checks<<" checks passed\n";}
