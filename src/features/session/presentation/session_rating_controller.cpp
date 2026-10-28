#include "app/application_controller.hpp"
#include "features/session/presentation/session_rating_text.hpp"
#include "features/session/presentation/session_rating_view.hpp"

namespace fogplay {
bool ApplicationController::offerSessionRating(const fog::PairedCheckoutResult& result) {
    if (ratingFocusLost_ || exitAfterStop_ || shutdownDone_ || sessionPending_) return false;
    if (result.preparationError == fog::RepositoryErrorKind::ReviewRequired)
        return result.requiredRating && openSessionRating(*result.requiredRating);
    return result.state == fog::PairedCheckoutState::Terminal && !result.rental.empty() &&
        openSessionRating(result.rental);
}
bool ApplicationController::openSessionRating(std::optional<fog::RentalId> rental) {
    if (sessionPending_ || pending_.valid() || exitAfterStop_ || shutdownDone_ ||
        appletGetFocusState() != AppletFocusState_InFocus) return false;
    if (!pairedServices_ || !pairedServices_->rentals().sessionRatingSupported()) {
        status_ = fog::rating::text::Unavailable; return false;
    }
    if (!pairedServices_->auth().authenticated()) {
        status_ = fog::rating::text::SignIn; return false;
    }
    if (rental && (rental->empty() || rental->provider != fog::ProviderId::FogHub)) return false;
    rating_.loading(rental);
    sidebar_ = false; screen_ = Screen::SessionRating;
    try {
        start(Task::SessionRatingLoad,[this,rental]() -> TaskResult {
            return pairedServices_->rentals().ratingForm(rental);
        });
    } catch (...) { rating_.loadFailed(fog::rating::text::LoadFailed); }
    return true;
}
void ApplicationController::completeSessionRatingLoad(std::optional<fog::SessionRatingForm> form) {
    if (!rating_.loaded(std::move(form))) rating_.loadFailed(fog::rating::text::WrongSession);
    status_.clear();
}
void ApplicationController::completeSessionRatingSubmit(const fog::SessionRatingResult& result) {
    if (!rating_.sent(result)) rating_.submitFailed(fog::rating::text::WrongSession);
    status_.clear();
}
void ApplicationController::submitSessionRating() {
    if (sessionPending_ || pending_.valid() || exitAfterStop_ || shutdownDone_ || !pairedServices_) return;
    const auto request = rating_.request();
    if (!request) return;
    rating_.submitting();
    try {
        start(Task::SessionRatingSubmit,[this,request = *request]() -> TaskResult {
            return pairedServices_->rentals().submitRating(request);
        });
    } catch (...) { rating_.submitFailed(fog::rating::text::SubmitFailed); }
}
void ApplicationController::inputSessionRating(u64 keys) {
    if (pending_.valid()) return;
    if ((keys & HidNpadButton_B) && rating_.state() != fog::rating::State::Submitting) {
        screen_ = Screen::PlayStation; status_.clear(); return;
    }
    if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) rating_.up();
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) rating_.down();
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) rating_.left();
    if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight)) rating_.right();
    if (!(keys & HidNpadButton_A)) return;
    switch (rating_.activate()) {
        case fog::rating::Action::None: return;
        case fog::rating::Action::Comment: {
            const auto limit = rating_.commentLimit();
            if (!limit) return;
            if (auto value = keyboard(fog::rating::text::Comment,rating_.comment(),static_cast<int>(limit))) {
                if (!rating_.comment(std::move(*value))) rating_.submitFailed(fog::rating::text::CommentTooLong);
            }
            return;
        }
        case fog::rating::Action::Send: submitSessionRating(); return;
        case fog::rating::Action::Reload: openSessionRating(rating_.requestedRental()); return;
        case fog::rating::Action::Later: screen_ = Screen::PlayStation; status_.clear(); return;
    }
}
void ApplicationController::drawSessionRating() { fog::rating::draw(ui_,rating_); }
}
