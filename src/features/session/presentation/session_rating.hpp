#pragma once
#include "features/session/domain/session_rating.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace fog::rating {
enum class State { Loading, Ready, LoadFailed, Submitting, Sent, None };
enum class Action { None, Comment, Send, Later, Reload };

// Application-thread draft. Loading never chooses or submits a score. The
// request identity belongs to the verified form, not the current catalog row.
class Presentation {
public:
    // The system applet's initial-text buffer is smaller than the website's
    // 2000-character textarea. Keep the entire draft safely editable on Switch.
    static constexpr std::size_t CommentLimit = 200;
    void loading(std::optional<RentalId> requested) {
        if (requested && *requested != rental_) clearDraft();
        requested_ = std::move(requested);
        form_.reset(); state_ = State::Loading; focus_ = 0; error_.clear();
    }
    bool loaded(std::optional<SessionRatingForm> form) {
        if (!form) { form_.reset(); clearDraft(); state_ = State::None; error_.clear(); return true; }
        if (form->rental.empty() || form->rental.provider != ProviderId::FogHub ||
            (requested_ && form->rental != *requested_)) return false;
        if (form->rental != rental_) clearDraft();
        rental_ = form->rental;
        form_ = std::move(form);
        error_.clear(); focus_ = 0;
        if (form_->alreadyRated) {
            state_ = State::Sent; alreadyRated_ = true;
            score_ = form_->existingScore.value_or(0);
        } else {
            state_ = State::Ready; alreadyRated_ = false;
            if (!form_->commentSupported) comment_.clear();
        }
        return true;
    }
    void loadFailed(std::string error) { state_ = State::LoadFailed; error_ = std::move(error); }
    void submitFailed(std::string error) { state_ = State::Ready; error_ = std::move(error); }
    bool sent(const SessionRatingResult& result) {
        if (!form_ || result.rental != rental_ || result.confirmedScore < 1 || result.confirmedScore > 5) return false;
        score_ = result.confirmedScore; state_ = State::Sent; alreadyRated_ = false; error_.clear();
        return true;
    }
    void submitting() { state_ = State::Submitting; error_.clear(); }
    void up() { if (editable() && focus_) --focus_; }
    void down() { if (editable() && focus_ + 1 < rows()) ++focus_; }
    void left() { changeScore(-1); }
    void right() { changeScore(1); }
    Action activate() {
        if (state_ == State::LoadFailed) return Action::Reload;
        if (state_ == State::Sent || state_ == State::None) return Action::Later;
        if (!editable()) return Action::None;
        if (!focus_) { changeScore(1); return Action::None; }
        if (commentSupported() && focus_ == 1) return Action::Comment;
        if (focus_ == sendRow()) return canSend() ? Action::Send : Action::None;
        return Action::Later;
    }
    bool comment(std::string value) {
        if (!editable() || !commentSupported()) return false;
        const auto units = utf16Units(value);
        if (!units || *units > commentLimit()) return false;
        comment_ = std::move(value); error_.clear(); return true;
    }
    std::optional<SessionRatingRequest> request() const {
        if (!canSend()) return std::nullopt;
        return SessionRatingRequest{rental_,form_->preparation,score_,comment_};
    }
    State state() const { return state_; }
    bool editable() const { return state_ == State::Ready; }
    bool canSend() const { return editable() && form_ && !form_->alreadyRated && score_ >= 1 && score_ <= 5; }
    bool commentSupported() const { return form_ && form_->commentSupported; }
    std::size_t commentLimit() const { return commentSupported() ? std::min(form_->commentMaxLength,CommentLimit) : 0; }
    std::size_t focus() const { return focus_; }
    std::size_t sendRow() const { return commentSupported() ? 2 : 1; }
    std::size_t laterRow() const { return sendRow() + 1; }
    int score() const { return score_; }
    bool alreadyRated() const { return alreadyRated_; }
    const std::string& comment() const { return comment_; }
    const std::string& error() const { return error_; }
    const std::optional<SessionRatingForm>& form() const { return form_; }
    const std::optional<RentalId>& requestedRental() const { return requested_; }
private:
    static std::optional<std::size_t> utf16Units(const std::string& value) {
        std::size_t units = 0;
        for (std::size_t offset = 0; offset < value.size();) {
            const auto lead = static_cast<unsigned char>(value[offset++]);
            unsigned continuation = 0; std::uint32_t point = lead;
            if (lead >= 0xc2 && lead <= 0xdf) { continuation = 1; point &= 0x1f; }
            else if (lead >= 0xe0 && lead <= 0xef) { continuation = 2; point &= 0xf; }
            else if (lead >= 0xf0 && lead <= 0xf4) { continuation = 3; point &= 7; }
            else if (lead >= 0x80 || !lead) return std::nullopt;
            else if ((lead < 0x20 && lead != '\n' && lead != '\r' && lead != '\t') || lead == 0x7f)
                return std::nullopt;
            if (offset + continuation > value.size()) return std::nullopt;
            for (unsigned i = 0; i < continuation; ++i) {
                const auto next = static_cast<unsigned char>(value[offset++]);
                if ((next & 0xc0) != 0x80) return std::nullopt;
                point = (point << 6) | (next & 0x3f);
            }
            if ((continuation == 2 && point < 0x800) || (continuation == 3 && point < 0x10000) ||
                (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) return std::nullopt;
            units += point >= 0x10000 ? 2 : 1;
        }
        return units;
    }
    void clearDraft() { score_ = 0; comment_.clear(); alreadyRated_ = false; }
    void changeScore(int direction) {
        if (!editable()) return;
        if (focus_ == sendRow() && direction > 0) { focus_ = laterRow(); return; }
        if (focus_ == laterRow() && direction < 0) { focus_ = sendRow(); return; }
        if (focus_) return;
        score_ = score_ ? std::clamp(score_ + direction,1,5) : 1;
        error_.clear();
    }
    std::size_t rows() const { return laterRow() + 1; }
    State state_ = State::None;
    std::optional<RentalId> requested_;
    RentalId rental_;
    std::optional<SessionRatingForm> form_;
    int score_ = 0;
    std::size_t focus_ = 0;
    bool alreadyRated_ = false;
    std::string comment_, error_;
};
}
