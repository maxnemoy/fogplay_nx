#include "app/application_controller.hpp"
#include "features/session/presentation/session_rating_text.hpp"
#include <iostream>
static unsigned assertions = 0, keyboardCalls = 0;
static int keyboardLimit = 0;
static std::optional<std::string> keyboardValue;
static void check(bool value,const char* message) { ++assertions; if (!value) throw std::runtime_error(message); }
static fog::RentalId rental(const char* value="owned-finished") { return {fog::ProviderId::FogHub,value}; }
static fog::SessionRatingForm form() {
    fog::SessionRatingForm result; result.rental = rental();
    result.preparation = {fog::ProviderId::FogHub,"opaque-local-handle"};
    result.computerName = "Проверенный компьютер"; result.commentSupported = true; result.commentMaxLength = 2000;
    return result;
}
namespace fogplay {
std::optional<std::string> keyboard(const char*,const std::string&,int maximum,bool) {
    ++keyboardCalls; keyboardLimit = maximum; return keyboardValue;
}
}
int main() {
    using namespace fogplay;
    using fog::rating::State;
    try {
        { ApplicationController c; c.service.repository.response = form();
          c.openSessionRating(rental()); c.inputSessionRating(HidNpadButton_B);
          check(c.screen_ == Screen::SessionRating && c.pending_.valid(),"bounded read-only load keeps ordinary busy input behavior");
          c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join()));
          c.inputSessionRating(HidNpadButton_B);
          check(c.screen_ == Screen::PlayStation && c.service.repository.sends == 0,"Back works after load and never posts"); }
        { ApplicationController c; c.service.repository.response = form();
          check(c.openSessionRating(rental()) && c.pending_.valid() && c.screen_ == Screen::SessionRating && !c.sidebar_,
              "exact finished session opens async read-only form");
          c.inputSessionRating(HidNpadButton_A); c.submitSessionRating();
          check(c.dispatches == 1 && c.service.repository.loads == 0 && c.service.repository.sends == 0,
              "pending form cannot submit or execute IO on UI thread");
          c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join()));
          check(c.service.repository.requested == rental() && !c.rating_.canSend() && c.rating_.score() == 0,
              "verified form is bound to exact rental and never chooses default five");
          c.inputSessionRating(HidNpadButton_Right); c.inputSessionRating(HidNpadButton_Right);
          c.inputSessionRating(HidNpadButton_Down); keyboardValue = "Хорошая игра";
          c.inputSessionRating(HidNpadButton_A);
          check(keyboardCalls == 1 && keyboardLimit == 200 && c.rating_.comment() == *keyboardValue,
              "comment uses safe system-editor limit and preserves Unicode");
          c.inputSessionRating(HidNpadButton_Down); c.inputSessionRating(HidNpadButton_A);
          c.inputSessionRating(HidNpadButton_A);
          check(c.rating_.state() == State::Submitting && c.dispatches == 2 && c.service.repository.sends == 0,
              "only explicit Send dispatches exactly one mutation");
          c.completeSessionRatingSubmit(std::get<fog::SessionRatingResult>(c.join()));
          check(c.service.repository.sends == 1 && c.service.repository.submitted.rental == rental() &&
              c.service.repository.submitted.score == 2 && c.service.repository.submitted.comment == "Хорошая игра" &&
              c.rating_.state() == State::Sent,"worker sends copied exact-session draft and applies confirmation");
          c.inputSessionRating(HidNpadButton_A);
          check(c.screen_ == Screen::PlayStation && c.dispatches == 2,"completion never auto-starts a rental"); }
        { ApplicationController c; c.service.repository.response = form();
          check(c.openSessionRating(),"profile can explicitly discover pending rating from previous run");
          c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join()));
          check(!c.service.repository.requested,"manual discovery does not guess a rental ID");
          c.inputSessionRating(HidNpadButton_Right); c.inputSessionRating(HidNpadButton_B);
          check(c.screen_ == Screen::PlayStation && c.service.repository.sends == 0,"Back skips without posting");
          c.openSessionRating(); c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join()));
          check(c.rating_.score() == 1,"same-session draft survives later/manual reopen"); }
        for (int blocked=0; blocked<6; ++blocked) {
            ApplicationController c;
            if (blocked==0) c.sessionPending_=true;
            if (blocked==1) c.exitAfterStop_=true;
            if (blocked==2) c.shutdownDone_=true;
            if (blocked==3) c.service.authentication.active=false;
            if (blocked==4) c.service.repository.supported=false;
            if (blocked==5) ratingTestFocused=false;
            check(!c.openSessionRating(rental()) && !c.pending_.valid() && !c.service.repository.sends,
                "active rental, HOME/shutdown, signed-out or unsupported provider cannot open or post");
            ratingTestFocused=true;
        }
        { ApplicationController c; c.dispatchThrows=true;
          check(c.openSessionRating(rental()) && c.rating_.state()==State::LoadFailed,"dispatch failure remains in retryable form");
          c.dispatchThrows=false; c.service.repository.response=form(); c.inputSessionRating(HidNpadButton_A);
          check(c.pending_.valid(),"load retry requires explicit action");
          c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join()));
          c.rating_.right(); c.rating_.comment("Сохранить при ошибке"); c.rating_.down(); c.rating_.down();
          c.service.repository.sendThrows=true; c.submitSessionRating();
          bool threw=false; try { c.join(); } catch (...) { threw=true; c.rating_.submitFailed(fog::rating::text::SubmitFailed); }
          check(threw && c.rating_.score()==1 && c.rating_.comment()=="Сохранить при ошибке" && c.rating_.canSend(),
              "failed mutation retains draft and requires explicit retry");
          c.service.repository.sendThrows=false; c.submitSessionRating();
          c.completeSessionRatingSubmit(std::get<fog::SessionRatingResult>(c.join()));
          check(c.service.repository.sends==2 && c.rating_.state()==State::Sent,"explicit retry receives confirmed result"); }
        { ApplicationController c; auto other=form(); other.rental=rental("other"); c.service.repository.response=other;
          c.openSessionRating(rental()); c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join()));
          check(c.rating_.state()==State::LoadFailed && !c.rating_.request(),"wrong returned rental never becomes sendable"); }
        { ApplicationController c; auto existing=form(); existing.alreadyRated=true; existing.existingScore=5;
          c.service.repository.response=existing; c.openSessionRating(rental());
          c.completeSessionRatingLoad(std::get<std::optional<fog::SessionRatingForm>>(c.join())); c.submitSessionRating();
          check(c.rating_.alreadyRated() && c.rating_.state()==State::Sent && !c.pending_.valid(),
              "already-rated evidence shows success without another POST"); }
        { ApplicationController c; fog::PairedCheckoutResult ended;
          ended.state=fog::PairedCheckoutState::Terminal; ended.rental=rental();
          c.stopAfterTask_=true; c.ratingFocusLost_=true; ratingTestFocused=true;
          check(!c.offerSessionRating(ended) && !c.pending_.valid(),"HOME stop never auto-loads even after focus returns");
          c.stopAfterTask_=false;
          check(!c.offerSessionRating(ended) && !c.pending_.valid(),"clearing stopAfterTask after confirmed stop retains HOME suppression");
          check(c.openSessionRating(rental()),"manual profile can offer deferred HOME rating later"); c.join(); }
        { ApplicationController c; fog::PairedCheckoutResult blocked;
          blocked.preparationError=fog::RepositoryErrorKind::ReviewRequired;
          check(!c.offerSessionRating(blocked) && !c.pending_.valid(),"unknown blocker never falls back to another pending session");
          blocked.requiredRating=rental("exact-blocker");
          check(c.offerSessionRating(blocked),"verified blocker opens exact target"); c.join();
          check(c.service.repository.requested==rental("exact-blocker"),"preflight rating uses exact blocker, not latest pending"); }
        std::cout << "Rating production controller: " << assertions << " assertions passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
