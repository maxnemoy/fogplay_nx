#include "features/providers/infrastructure/foghub_rental_repository.hpp"
#include "features/providers/infrastructure/foghub_review_html.hpp"
#include <algorithm>
#include <atomic>

namespace fog::foghub {
namespace {
[[noreturn]] void fail(RepositoryErrorKind kind,const char* message) { throw RepositoryError(kind,message); }
bool sameAuth(const AuthData& a,const AuthData& b) {
    return a.account == b.account && a.generation == b.generation && a.state == b.state;
}
bool numeric(const std::string& value) {
    return !value.empty() && value.size() <= 20 && value.front() != '0' &&
        std::all_of(value.begin(),value.end(),[](unsigned char c){ return c >= '0' && c <= '9'; });
}
std::string localLocation(const std::string& location) {
    const std::string origin = "https://foghub.ru";
    if (location.rfind(origin+"/",0) == 0) return location.substr(origin.size());
    return location;
}
PairedEndpoint endpoint(const MoonlightEndpoint& source) {
    // moonlight_url specifies the base HTTP port only. The production pairing
    // client must discover HTTPS through GameStream serverinfo before using it;
    // zero explicitly represents an unspecified HTTPS port, never a fallback.
    return {source.host,0,source.port};
}
std::atomic<std::uint64_t> handleSequence{0};
}
void Rentals::validateId(const EntityId& id) {
    if (id.provider != ProviderId::FogHub || !numeric(id.value))
        fail(RepositoryErrorKind::InvalidRequest,"Invalid FogHub rental or computer identifier");
}
void Rentals::synchronize() {
    const auto current = session_.auth();
    if (!sameAuth(current,scope_)) {
        pending_.reset(); ratingPending_.reset(); closeAttempts_.clear(); observedComputers_.clear(); pairedEndpoints_.clear();
        unresolvedCreate_ = false; unresolvedRental_.reset(); scope_ = current;
    }
    if (!session_.authenticated() || current.state != AuthState::Authenticated ||
        current.account.provider != ProviderId::FogHub || current.account.empty())
        fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
}
std::set<std::string> Rentals::idleBaseline(const Session::OwnHistory& history) {
    std::set<std::string> baseline;
    for (const auto& row : history.rows) {
        if (row.state == HistoryState::Active)
            fail(RepositoryErrorKind::ActiveRentalExists,"FogHub has an active rental");
        if (!terminal(row)) fail(RepositoryErrorKind::OutcomeUnknown,"FogHub has an active or unverified rental");
        baseline.insert(row.id);
    }
    return baseline;
}
const HistoryRow* Rentals::find(const Session::OwnHistory& history,const RentalId& id) {
    const auto match = std::find_if(history.rows.begin(),history.rows.end(),[&](const HistoryRow& row){ return row.id == id.value; });
    return match == history.rows.end() ? nullptr : &*match;
}
bool Rentals::terminal(const HistoryRow& row) {
    return row.state == HistoryState::Finished && !row.finishedAt.empty();
}
RentalInspection Rentals::inspection(const Session::OwnHistory& history,const RentalId& id) {
    if (!sameAuth(history.owner,session_.auth()) || !sameAuth(history.owner,scope_)) return {};
    const auto* row = find(history,id);
    if (!row || !numeric(row->computerId)) return {};
    const EntityId computer{ProviderId::FogHub,row->computerId};
    const auto known = observedComputers_.find(id.value);
    if (known != observedComputers_.end() && known->second != computer) return {};
    observedComputers_[id.value] = computer;
    RentalInspection result;
    result.rental = id; result.account = history.owner.account; result.computer = computer;
    if (terminal(*row)) result.state = RentalInspectionState::Terminal;
    // The provider's resume/app-selection contract has not been validated.
    // Active history proves rental activity, not an established stream endpoint.
    else if (row->state == HistoryState::Active) {
        result.state = RentalInspectionState::Preparing;
        const auto address = pairedEndpoints_.find(id.value);
        if (address != pairedEndpoints_.end()) {
            result.state = RentalInspectionState::Ready;
            result.endpoint = address->second;
        }
    }
    return result;
}
PairedPreparation Rentals::preparePairedRental(const StartRentalRequest& request) {
    auto stage = PairedPreparationStage::Validation;
    try {
        validateId(request.computerId); synchronize();
        if (request.ownAccount || !request.offerId.empty())
            fail(RepositoryErrorKind::Unsupported,"FogHub launch options are unsupported");
        if (!request.gameId.empty()) validateId(request.gameId);
        if (unresolvedCreate_) fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation outcome requires reconciliation");
        // Replace any old preparation immediately, including when a fresh read fails.
        pending_.reset();
        stage = PairedPreparationStage::ComputerPreview;
        const auto computer = parseComputerPreview(session_.readPublic("/pc/preview?id="+request.computerId.value),request.computerId.value);
        if (!computer.fhAvailable || computer.pricePerHour.empty())
            fail(RepositoryErrorKind::Unavailable,"FogHub computer is not currently available for paired launch");
        stage = PairedPreparationStage::AccountHistory;
        const auto history = session_.ownHistory();
        stage = PairedPreparationStage::IdleBaseline;
        auto baseline = idleBaseline(history);
        stage = PairedPreparationStage::ConnectionPage;
        auto page = session_.readRentalPage("/pc/play?id="+request.computerId.value);
        stage = PairedPreparationStage::FreshPairingLink;
        const auto freshLink = freshPairingLink(page,request.computerId.value);
        if (!freshLink.empty()) {
            stage = PairedPreparationStage::FreshPairingPage;
            page = session_.readRentalPage(freshLink);
        }
        stage = PairedPreparationStage::ConnectionForm;
        auto form = parseConnectForm(page,request.computerId.value);
        if (!freshLink.empty() && form.actionPath != freshLink)
            fail(RepositoryErrorKind::InvalidResponse,"FogHub fresh pairing action changed");
        const auto address = endpoint(form.endpoint);
        stage = PairedPreparationStage::AccountRecheck;
        if (!sameAuth(history.owner,session_.auth())) fail(RepositoryErrorKind::NotAuthenticated,"FogHub account changed");
        EntityId handle{ProviderId::FogHub,"pair-"+std::to_string(++handleSequence)};
        pending_ = Pending{handle,request.computerId,history.owner,std::move(form),std::move(baseline),
            // Discovery and the fresh account/history preflight consume bounded
            // calls. This is a one-use local lease, not a server CSRF lifetime.
            std::chrono::steady_clock::now()+std::chrono::seconds(120)};
        return {handle,request.computerId,address,{}};
    } catch (const SessionReviewRequired& error) {
        throw PairedPreparationError(stage,error.kind(),error.rental());
    } catch (const RepositoryError& error) {
        throw PairedPreparationError(stage,error.kind());
    } catch (...) {
        throw PairedPreparationError(stage,RepositoryErrorKind::InternalFailure);
    }
}
PairedLaunchReceipt Rentals::createPairedRental(const EntityId& handle,const std::string& pin) {
    return createPairedRental(handle,pin,[] { return true; });
}
PairedLaunchReceipt Rentals::createPairedRental(const EntityId& handle,const std::string& pin,
        const PairedSubmissionCheck& canSubmit) {
    synchronize();
    if (pin.size() != 4 || !std::all_of(pin.begin(),pin.end(),[](unsigned char c){ return c >= '0' && c <= '9'; }))
        fail(RepositoryErrorKind::InvalidRequest,"Invalid Moonlight pairing PIN");
    if (!pending_ || pending_->handle != handle)
        fail(RepositoryErrorKind::InvalidRequest,"FogHub preparation is missing or already used");
    auto pending = std::move(*pending_); pending_.reset();
    if (!sameAuth(pending.owner,session_.auth()) || std::chrono::steady_clock::now() >= pending.expires)
        fail(RepositoryErrorKind::InvalidRequest,"FogHub preparation has expired");
    if (unresolvedCreate_) fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation outcome requires reconciliation");
    const auto before = session_.ownHistory();
    if (!sameAuth(before.owner,pending.owner) || idleBaseline(before) != pending.baseline)
        fail(RepositoryErrorKind::OutcomeUnknown,"FogHub history changed before creation");
    if (std::chrono::steady_clock::now() >= pending.expires)
        fail(RepositoryErrorKind::InvalidRequest,"FogHub preparation has expired");
    Query fields{{"_csrf",pending.form.csrf},{"moonlight_url",pending.form.moonlightUrl},{"Pc[pin]",pin}};
    if (pending.form.hasBonusChoice) fields.emplace_back("Pc[use_bonus]","0");
    bool allowed = false;
    try { allowed = canSubmit && canSubmit(); } catch (...) {}
    if (!allowed) throw PairedSubmissionCancelled();
    // The caller persists its create intent before entering this method. Any
    // response loss leaves this in-memory scope blocked; there is no POST retry.
    unresolvedCreate_ = true;
    RentalId identified{ProviderId::FogHub,{}};
    try {
        const bool freshPair = pending.form.actionPath == "/pc/play?id="+pending.computer.value+"&resume=0";
        const auto submitted = session_.submitPairing(pending.computer.value,fields,freshPair);
        const auto expected = "/pc/play-success?id="+pending.computer.value+"&withResume=0";
        if ((submitted.status != 302 && submitted.status != 303) || localLocation(submitted.location) != expected)
            fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation did not return a verified success route");
        const auto success = parseLaunchSuccess(session_.readRentalPage(expected),pending.computer.value,expected);
        identified.value = success.rentalId;
        // A receipt pointing to a baseline rental is not evidence of this create.
        if (pending.baseline.count(identified.value))
            fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation returned an existing rental");
        observedComputers_[identified.value] = pending.computer;
        pairedEndpoints_[identified.value] = endpoint(pending.form.endpoint);
        unresolvedRental_ = identified;
        // Return the exact receipt before any further network work. The
        // coordinator journals this ID, then inspectRental verifies ownership.
        return {identified,false};
    } catch (const RepositoryError& error) {
        // Preserve the exact receipt even if local bookkeeping fails.
        if (!identified.empty() && !pending.baseline.count(identified.value)) return {identified,true};
        if (error.kind() == RepositoryErrorKind::Unsupported) {
            unresolvedCreate_ = false;
            throw;
        }
        fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation outcome is unknown; do not retry");
    } catch (...) {
        if (!identified.empty() && !pending.baseline.count(identified.value)) return {identified,true};
        fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation outcome is unknown; do not retry");
    }
}
RentalInspection Rentals::inspectRental(const RentalId& id) {
    validateId(id);
    try {
        synchronize();
        const auto first = session_.ownHistory();
        auto result = inspection(first,id);
        if (result.state == RentalInspectionState::Terminal) {
            const auto second = inspection(session_.ownHistory(),id);
            if (second.rental != result.rental || second.account != result.account || second.computer != result.computer ||
                second.state != RentalInspectionState::Terminal) return {};
        }
        // An active receipt remains outstanding even if a later history page
        // temporarily omits it. Only confirmed terminal evidence unblocks create.
        if (unresolvedRental_ && *unresolvedRental_ == id && result.state == RentalInspectionState::Terminal) {
            unresolvedCreate_ = false;
            unresolvedRental_.reset();
        }
        return result;
    } catch (const RepositoryError&) { return {}; }
}
std::vector<Rental> Rentals::activeSessions(bool) {
    synchronize();
    const auto history = session_.ownHistory();
    std::vector<Rental> result;
    for (const auto& row : history.rows) {
        if (row.state == HistoryState::Unknown || (row.state == HistoryState::Finished && !terminal(row)))
            fail(RepositoryErrorKind::OutcomeUnknown,"FogHub history has an unverified rental");
        if (row.state == HistoryState::Active) result.push_back({{ProviderId::FogHub,row.id},row.computerName,
            EntityId{ProviderId::FogHub,row.computerId}});
    }
    return result;
}
bool Rentals::finishSession(const RentalId& id) {
    return finishSession(id,[] { return true; });
}
bool Rentals::finishSession(const RentalId& id,const PairedSubmissionCheck& canSubmit) {
    validateId(id); synchronize();
    const auto history = session_.ownHistory();
    const auto verified = inspection(history,id);
    if (verified.rental != id || verified.state == RentalInspectionState::Unknown)
        fail(RepositoryErrorKind::OutcomeUnknown,"FogHub rental ownership or state is unverified");
    if (verified.state == RentalInspectionState::Terminal) {
        const auto second = inspection(session_.ownHistory(),id);
        const bool confirmed = second.rental == id && second.account == verified.account && second.computer == verified.computer &&
            second.state == RentalInspectionState::Terminal;
        if (confirmed && unresolvedRental_ && *unresolvedRental_ == id) {
            unresolvedCreate_ = false;
            unresolvedRental_.reset();
        }
        return confirmed;
    }
    const auto page = history.rowPages.find(id.value);
    if (page == history.rowPages.end() || page->second >= history.pages.size())
        fail(RepositoryErrorKind::InvalidResponse,"FogHub close evidence is missing");
    const auto close = parseCloseAction(history.pages[page->second],id.value);
    if (close.method != CloseMethod::Get || close.path != "/session/close?id="+id.value)
        fail(RepositoryErrorKind::InvalidResponse,"Unsupported FogHub close action");
    if (closeAttempts_.count(id.value))
        fail(RepositoryErrorKind::OutcomeUnknown,"FogHub close was already attempted; reconcile without replay");
    bool allowed = false;
    try { allowed = canSubmit && canSubmit(); } catch (...) {}
    if (!allowed) throw PairedSubmissionCancelled();
    closeAttempts_.insert(id.value);
    try {
        const auto response = session_.closeRental(id.value);
        if ((response.status == 302 || response.status == 303) && localLocation(response.location) == "/session/list") return true;
        if (response.status == 200 && response.contentType.rfind("text/html",0) == 0) return true;
    } catch (const RepositoryError& error) {
        if (error.kind() == RepositoryErrorKind::Unsupported) throw;
        fail(RepositoryErrorKind::OutcomeUnknown,"FogHub close outcome is unknown; do not retry");
    }
    fail(RepositoryErrorKind::OutcomeUnknown,"FogHub close outcome is unknown; do not retry");
}
CreationReceipt Rentals::createOrder(const StartRentalRequest&) {
    fail(RepositoryErrorKind::Unsupported,"FogHub does not support MTS order creation");
}
RentalPreparation Rentals::orderStatus(const CreationId&) {
    fail(RepositoryErrorKind::Unsupported,"FogHub does not support MTS order status");
}
std::optional<SessionRatingForm> Rentals::ratingForm(const std::optional<RentalId>& requested) {
    if (requested) validateId(*requested);
    synchronize(); ratingPending_.reset();
    const auto history=session_.ownHistory();
    if (!sameAuth(history.owner,scope_) || !sameAuth(history.owner,session_.auth()))
        fail(RepositoryErrorKind::NotAuthenticated,"FogHub account changed");
    const HistoryRow* selected=nullptr;
    std::optional<int> score;
    std::string latest;
    bool ambiguous=false;
    // Parse each bounded history page once, including for a long prior-run history.
    std::map<std::size_t,std::map<std::string,std::optional<int>>> pageRatings;
    for (const auto& row:history.rows) {
        if (requested && row.id!=requested->value) continue;
        if (!terminal(row)) {
            if (requested) fail(RepositoryErrorKind::InvalidRequest,"Only finished sessions can be rated");
            continue;
        }
        const auto page=history.rowPages.find(row.id);
        if (page==history.rowPages.end() || page->second>=history.pages.size())
            fail(RepositoryErrorKind::InvalidResponse,"FogHub review history is incomplete");
        auto ratings=pageRatings.find(page->second);
        if(ratings==pageRatings.end()) ratings=pageRatings.emplace(page->second,sessionHistoryRatings(history.pages[page->second])).first;
        const auto entry=ratings->second.find(row.id);
        if(entry==ratings->second.end()) fail(RepositoryErrorKind::InvalidResponse,"FogHub review row is missing");
        const auto rating=entry->second;
        if (requested) { selected=&row; score=rating; break; }
        if (rating) continue;
        // The verified history date is DD.MM.YYYY HH:mm. Do not assume row or ID order.
        const auto& time=row.startedAt;
        if (time.size()!=16) fail(RepositoryErrorKind::InvalidResponse,"FogHub review date is invalid");
        const auto key=time.substr(6,4)+time.substr(3,2)+time.substr(0,2)+time.substr(11,5);
        if (!selected || key>latest) { selected=&row;latest=key;ambiguous=false; }
        else if(key==latest) ambiguous=true;
    }
    if (ambiguous) fail(RepositoryErrorKind::InvalidResponse,"Latest FogHub review is ambiguous");
    if (!selected) {
        if(requested) fail(RepositoryErrorKind::InvalidRequest,"Finished FogHub session was not found");
        return std::nullopt;
    }
    const RentalId rental{ProviderId::FogHub,selected->id};
    const EntityId computer{ProviderId::FogHub,selected->computerId}; validateId(computer);
    SessionRatingForm result; result.rental=rental;result.computerName=selected->computerName;
    result.sessionDate=selected->startedAt;
    result.alreadyRated=score.has_value();result.existingScore=score;
    if(score) return result;
    (void)parseSessionReviewForm(session_.readRentalPage("/session/review?id="+rental.value),rental.value);
    if(!sameAuth(history.owner,session_.auth())) fail(RepositoryErrorKind::NotAuthenticated,"FogHub account changed");
    result.preparation={ProviderId::FogHub,"review-"+std::to_string(++handleSequence)};
    result.commentSupported=true;result.commentMaxLength=2000;
    ratingPending_=RatingPending{rental,result.preparation,computer,history.owner};
    return result;
}
SessionRatingResult Rentals::submitRating(const SessionRatingRequest& request) {
    validateId(request.rental); synchronize();
    if(request.score<1 || request.score>5 || !validSessionReviewMessage(request.comment))
        fail(RepositoryErrorKind::InvalidRequest,"Invalid session rating or comment");
    if(!ratingPending_ || ratingPending_->handle!=request.preparation || ratingPending_->rental!=request.rental ||
        !sameAuth(ratingPending_->owner,session_.auth()))
        fail(RepositoryErrorKind::InvalidRequest,"FogHub review preparation has expired");
    const auto pending=*ratingPending_;
    const auto before=session_.ownHistory();
    const auto* row=find(before,request.rental);
    if(!sameAuth(before.owner,pending.owner) || !row || !terminal(*row) || row->computerId!=pending.computer.value)
        fail(RepositoryErrorKind::InvalidResponse,"FogHub review ownership changed");
    const auto score=sessionHistoryRating(before.pages.at(before.rowPages.at(row->id)),row->id);
    if(score) {
        if(*score!=request.score) fail(RepositoryErrorKind::InvalidRequest,"FogHub session already has a different rating");
        ratingPending_.reset(); return {request.rental,*score};
    }
    // Fetch a fresh CSRF after editing. No form URL/token is exposed to presentation.
    const auto form=parseSessionReviewForm(session_.readRentalPage("/session/review?id="+request.rental.value),request.rental.value);
    if(!sameAuth(pending.owner,session_.auth())) fail(RepositoryErrorKind::NotAuthenticated,"FogHub account changed");
    const Query fields{{"_csrf",form.csrf},{"Review[rating]",std::to_string(request.score)},{"Review[message]",request.comment}};
    // One dispatch per explicit action. HTTP acknowledgement is insufficient;
    // even a lost response can be reconciled by the exact owned history row.
    try { (void)session_.submitSessionRating(request.rental.value,fields); }
    catch (...) { /* Reconcile a possibly accepted request without replaying it. */ }
    try {
        const auto after=session_.ownHistory();
        const auto* confirmed=find(after,request.rental);
        if(sameAuth(after.owner,pending.owner) && confirmed && terminal(*confirmed) && confirmed->computerId==pending.computer.value) {
            const auto rating=sessionHistoryRating(after.pages.at(after.rowPages.at(confirmed->id)),confirmed->id);
            if(rating && *rating==request.score) {ratingPending_.reset();return {request.rental,*rating};}
        }
    } catch (...) { /* Preserve an unknown submission outcome with a fixed error. */ }
    fail(RepositoryErrorKind::OutcomeUnknown,"FogHub rating could not be confirmed");
}
}
