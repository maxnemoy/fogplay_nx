#include "features/providers/infrastructure/foghub_rental_repository.hpp"
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
        pending_.reset(); closeAttempts_.clear(); observedComputers_.clear(); pairedEndpoints_.clear();
        unresolvedCreate_ = false; unresolvedRental_.reset(); scope_ = current;
    }
    if (!session_.authenticated() || current.state != AuthState::Authenticated ||
        current.account.provider != ProviderId::FogHub || current.account.empty())
        fail(RepositoryErrorKind::NotAuthenticated,"FogHub sign-in required");
}
std::set<std::string> Rentals::idleBaseline(const Session::OwnHistory& history) {
    std::set<std::string> baseline;
    for (const auto& row : history.rows) {
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
    validateId(request.computerId); synchronize();
    if (request.ownAccount || !request.offerId.empty())
        fail(RepositoryErrorKind::Unsupported,"FogHub launch options are unsupported");
    if (!request.gameId.empty()) validateId(request.gameId);
    if (unresolvedCreate_) fail(RepositoryErrorKind::OutcomeUnknown,"FogHub creation outcome requires reconciliation");
    // Replace any old preparation immediately, including when a fresh read fails.
    pending_.reset();
    const auto computer = parseComputerPreview(session_.readPublic("/pc/preview?id="+request.computerId.value),request.computerId.value);
    if (!computer.fhAvailable || computer.pricePerHour.empty())
        fail(RepositoryErrorKind::Unavailable,"FogHub computer is not currently available for paired launch");
    const auto history = session_.ownHistory();
    auto baseline = idleBaseline(history);
    auto page = session_.readRentalPage("/pc/play?id="+request.computerId.value);
    const auto freshLink = freshPairingLink(page,request.computerId.value);
    if (!freshLink.empty()) page = session_.readRentalPage(freshLink);
    auto form = parseConnectForm(page,request.computerId.value);
    if (!freshLink.empty() && form.actionPath != freshLink)
        fail(RepositoryErrorKind::InvalidResponse,"FogHub fresh pairing action changed");
    const auto address = endpoint(form.endpoint);
    if (!sameAuth(history.owner,session_.auth())) fail(RepositoryErrorKind::NotAuthenticated,"FogHub account changed");
    EntityId handle{ProviderId::FogHub,"pair-"+std::to_string(++handleSequence)};
    pending_ = Pending{handle,request.computerId,history.owner,std::move(form),std::move(baseline),
        // Discovery/dispatch may consume 15s and the fresh account/history
        // preflight may consume a bounded HTTPS call. This is a local one-use
        // lease, not a claim about the server CSRF token lifetime.
        std::chrono::steady_clock::now()+std::chrono::seconds(120)};
    return {handle,request.computerId,address,{}};
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
    const Query fields{{"_csrf",pending.form.csrf},{"moonlight_url",pending.form.moonlightUrl},{"Pc[pin]",pin}};
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
}
