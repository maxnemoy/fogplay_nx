#include "features/session/application/paired_checkout.hpp"
#include <algorithm>

namespace fog {
namespace {
class PinScope {
public:
    std::string value;
    ~PinScope() { std::fill(value.begin(), value.end(), '\0'); }
};
bool validEndpoint(const PairedEndpoint& endpoint) {
    return !endpoint.host.empty() && endpoint.host.size() <= 253 && endpoint.httpPort != 0 &&
        std::all_of(endpoint.host.begin(), endpoint.host.end(),
            [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':'; });
}
constexpr auto preparingLimit = std::chrono::seconds(90);
}
PairedCheckoutGuard::PairedCheckoutGuard(RentalRepository& api, PairingClient& pairing,
        PairingPinGenerator& pins, std::unique_ptr<PairedCheckoutJournal> journal, Clock clock)
    : api_(api), pairing_(pairing), pins_(pins), journal_(std::move(journal)), clock_(std::move(clock)) {
    try {
        if (!journal_) { invalid_ = true; return; }
        const auto saved = journal_->load();
        record_ = saved.record;
        pending_ = saved.pending;
        invalid_ = saved.invalid || (pending_ && !validRecord());
        if (pending_ || invalid_) {
            result_.rental = record_.rental;
            result_.stopRequested = record_.stopRequested;
            recovery("Recorded rental requires explicit recovery");
        }
        const auto auth = api_.auth();
        if (auth.authenticated() && auth.account == record_.account) generation_ = auth.generation;
    } catch (...) {
        invalid_ = true;
        recovery("Rental journal could not be read");
    }
}
bool PairedCheckoutGuard::validRecord() const {
    return !record_.account.empty() && !record_.computer.empty() &&
        record_.account.provider == record_.provider && record_.computer.provider == record_.provider &&
        (record_.rental.empty() || (record_.rental.provider == record_.provider && validRentalId(record_.rental))) &&
        (!record_.stopAttempted || (record_.stopRequested && !record_.rental.empty() &&
            record_.phase == PairedCheckoutPhase::StopRequested)) &&
        (record_.phase != PairedCheckoutPhase::Intent || record_.rental.empty()) &&
        (record_.phase == PairedCheckoutPhase::Intent || record_.phase == PairedCheckoutPhase::StopRequested ||
            !record_.rental.empty()) &&
        (record_.phase == PairedCheckoutPhase::Intent || record_.phase == PairedCheckoutPhase::RentalKnown ||
         record_.phase == PairedCheckoutPhase::Pairing || record_.phase == PairedCheckoutPhase::Streaming ||
         record_.phase == PairedCheckoutPhase::StopRequested);
}
bool PairedCheckoutGuard::sameAccount() const {
    const auto auth = api_.auth();
    return auth.authenticated() && auth.account == record_.account && auth.generation == generation_;
}
void PairedCheckoutGuard::persist() {
    if (!journal_) throw JournalError("Rental journal is unavailable");
    journal_->write(record_);
}
PairedCheckoutResult PairedCheckoutGuard::recovery(const char* message) {
    result_.state = PairedCheckoutState::RecoveryRequired;
    result_.rental = record_.rental;
    result_.stopRequested = record_.stopRequested;
    result_.endpoint.reset();
    result_.message = message;
    return result_;
}
bool PairedCheckoutGuard::exact(const RentalInspection& evidence) const {
    return evidence.rental == record_.rental && evidence.account == record_.account &&
        evidence.computer == record_.computer && evidence.state != RentalInspectionState::Unknown;
}
bool PairedCheckoutGuard::inspect(RentalInspection& evidence) {
    if (!sameAccount()) return false;
    evidence = api_.inspectRental(record_.rental);
    return sameAccount() && exact(evidence);
}
bool PairedCheckoutGuard::validPreparation(const PairedPreparation& prepared, bool reconnect) const {
    return !prepared.id.empty() && prepared.id.provider == record_.provider &&
        prepared.computer == record_.computer && validEndpoint(prepared.endpoint) &&
        (reconnect ? prepared.rental == record_.rental : prepared.rental.empty());
}
bool PairedCheckoutGuard::startPairing(const PairedPreparation& prepared, std::string& pin) {
    pin = pins_.generate();
    if (pin.size() != 4 || !std::all_of(pin.begin(), pin.end(),
            [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
    return !stopSignal_.load() && pairing_.begin(prepared.endpoint, pin) && pairing_.waitReady() &&
        !stopSignal_.load() && sameAccount();
}
PairedCheckoutResult PairedCheckoutGuard::terminal() {
    pairing_.cancelAndJoin();
    paired_ = false;
    pairedEndpoint_.reset();
    preparingDeadline_.reset();
    result_.endpoint.reset();
    try { journal_->clear(); }
    catch (...) { return recovery("Rental ended; journal cleanup failed"); }
    pending_ = false;
    result_.state = PairedCheckoutState::Terminal;
    result_.rental = record_.rental;
    result_.stopRequested = record_.stopRequested;
    result_.message = "Exact rental termination confirmed";
    return result_;
}
PairedCheckoutResult PairedCheckoutGuard::stopKnown() {
    pairing_.cancelAndJoin();
    paired_ = false;
    pairedEndpoint_.reset();
    record_.stopRequested = true;
    record_.phase = PairedCheckoutPhase::StopRequested;
    bool storageFailed = false;
    try { persist(); } catch (...) { storageFailed = true; }
    if (record_.rental.empty())
        return recovery(storageFailed ? "Stop intent could not be saved; creation outcome is unknown" :
            "Creation outcome is unknown; verify the official account");
    try {
        RentalInspection evidence;
        if (!inspect(evidence)) return recovery("Exact rental ownership could not be verified; stop remains unresolved");
        if (evidence.state == RentalInspectionState::Terminal) return terminal();
        if (record_.stopAttempted) return recovery("Stop was already attempted; exact termination remains unconfirmed");
        bool commandFailed = false;
        try { api_.finishSession(record_.rental,[this,&storageFailed] {
            if (!sameAccount() || record_.stopAttempted) return false;
            // The provider invokes this only after its read-only preflight,
            // immediately before close dispatch. A preflight network failure
            // must leave the known-unsubmitted command eligible for retry.
            record_.stopAttempted = true;
            // An SD failure cannot leave a verified paid rental billing forever.
            // Keep the marker in memory and retry its persistence after close.
            try { persist(); } catch (...) { storageFailed = true; }
            if (!sameAccount()) {
                record_.stopAttempted = false;
                try { persist(); } catch (...) { storageFailed = true; }
                return false;
            }
            return true;
        }); }
        catch (...) { commandFailed = true; }
        if (storageFailed) { try { persist(); } catch (...) {} }
        // An ACK, a timeout, and an absent row cannot establish termination.
        if (inspect(evidence) && evidence.state == RentalInspectionState::Terminal) return terminal();
        return recovery(storageFailed ? "Stop was attempted; journal write and termination verification failed" :
            commandFailed ? "Stop request failed; exact termination remains unconfirmed" :
            "Stop requested; exact termination remains unconfirmed");
    } catch (...) {
        return recovery(storageFailed ? "Stop intent could not be saved and termination is unconfirmed" :
            "Exact termination could not be verified");
    }
}
PairedCheckoutResult PairedCheckoutGuard::failPaid(const char* message) {
    const auto stopped = stopKnown();
    if (stopped.state == PairedCheckoutState::Terminal) return stopped;
    result_.message = std::string(message) + "; " + stopped.message;
    return result_;
}
PairedCheckoutResult PairedCheckoutGuard::cancelUnsubmitted() {
    pairing_.cancelAndJoin();
    // Only a live stack with explicit pre-dispatch evidence may clear this.
    try { journal_->clear(); }
    catch (...) { return recovery("Submission was cancelled before the paid request; journal cleanup failed"); }
    pending_ = false;
    record_ = {};
    result_ = {};
    result_.message = "Rental submission cancelled before the paid request";
    return result_;
}
PairedCheckoutResult PairedCheckoutGuard::create(const StartRentalRequest& request) {
    if (pending_ || invalid_) return recovery("Resolve the recorded rental before creating another");
    stopSignal_.store(false);
    pairing_.cancelAndJoin();
    paired_ = false;
    pairedEndpoint_.reset();
    preparingDeadline_.reset();
    try {
        const auto auth = api_.auth();
        if (!auth.authenticated() || auth.account.empty())
            return recovery("Authentication is required");
        if (!api_.pairedLaunchSupported()) return recovery("Paired launch is unsupported");
        if (request.computerId.empty() || request.computerId.provider != auth.account.provider)
            return recovery("Invalid computer selection");
        record_ = {auth.account.provider, auth.account, request.computerId, {},
                   PairedCheckoutPhase::Intent, false};
        generation_ = auth.generation;
        const auto prepared = api_.preparePairedRental(request);
        if (!sameAccount() || !validPreparation(prepared, false))
            return recovery("Prepared computer or account could not be verified");
        PinScope pin;
        if (!startPairing(prepared, pin.value)) {
            pairing_.cancelAndJoin();
            return recovery("Local pairing did not become ready; rental was not submitted");
        }
        // From this point an interrupted/crashed process must never repeat the paid request.
        try { persist(); }
        catch (...) {
            pairing_.cancelAndJoin();
            invalid_ = true;
            return recovery("Durable rental intent could not be saved; rental was not submitted");
        }
        pending_ = true;
        if (!sameAccount() || stopSignal_.load()) {
            // Only this live stack knows the paid call has not started. Never apply
            // this cleanup to an intent loaded after a crash or an unknown POST.
            return cancelUnsubmitted();
        }
        PairedLaunchReceipt receipt;
        // Include the paid POST and receipt read in the waiting budget. Starting
        // this only after the response could add another full network timeout.
        preparingDeadline_ = clock_() + preparingLimit;
        try { receipt = api_.createPairedRental(prepared.id, pin.value, [this] {
            return !stopSignal_.load() && sameAccount() && pairing_.submissionReady();
        }); }
        catch (const PairedSubmissionCancelled&) { return cancelUnsubmitted(); }
        catch (...) {
            pairing_.cancelAndJoin();
            record_.stopRequested = true;
            try { persist(); } catch (...) {
                return recovery("Creation outcome unknown and stop intent could not be saved");
            }
            return recovery("Creation outcome unknown; do not retry creation");
        }
        if (receipt.rental.provider != record_.provider || !validRentalId(receipt.rental)) {
            pairing_.cancelAndJoin();
            record_.stopRequested = true;
            try { persist(); } catch (...) {
                return recovery("Creation returned no valid rental ID and stop intent could not be saved");
            }
            return recovery("Creation returned no valid rental ID; verify the official account");
        }
        record_.rental = receipt.rental;
        record_.phase = PairedCheckoutPhase::RentalKnown;
        try { persist(); }
        catch (...) { return failPaid("Created rental could not be journaled"); }
        if (stopSignal_.load()) return failPaid("Stop requested during creation");
        if (preparingDeadline_ && clock_() >= *preparingDeadline_)
            return failPaid("Rental preparation deadline exceeded");
        if (receipt.reportedError) return failPaid("Provider reported a partial creation failure");
        RentalInspection evidence;
        if (!inspect(evidence)) return failPaid("Created rental ownership could not be verified");
        if (evidence.state == RentalInspectionState::Terminal) return terminal();
        if (stopSignal_.load()) return stopKnown();
        record_.phase = PairedCheckoutPhase::Pairing;
        try { persist(); }
        catch (...) { return failPaid("Pairing phase could not be journaled"); }
        if (!pairing_.complete() || !sameAccount() || stopSignal_.load()) return failPaid("Pairing did not complete");
        pairing_.cancelAndJoin();
        paired_ = true;
        pairedEndpoint_ = prepared.endpoint;
        return poll();
    } catch (...) {
        pairing_.cancelAndJoin();
        if (pending_) return failPaid("Rental preparation or pairing failed");
        return recovery("Rental preparation or pairing failed before submission");
    }
}
PairedCheckoutResult PairedCheckoutGuard::poll() {
    if (invalid_) return recovery("Rental journal is invalid");
    if (!pending_) return result_;
    if (record_.stopRequested || stopSignal_.load()) return stopKnown();
    if (record_.rental.empty()) return recovery("Creation outcome unknown; do not retry creation");
    try {
        RentalInspection evidence;
        if (!inspect(evidence)) return failPaid("Exact rental ownership could not be verified");
        if (evidence.state == RentalInspectionState::Terminal) return terminal();
        if (stopSignal_.load()) return stopKnown();
        if (!paired_) return recovery("Explicit same-rental reconnect is required");
        if (preparingDeadline_ && clock_() >= *preparingDeadline_)
            return failPaid("Rental preparation deadline exceeded");
        if (evidence.state != RentalInspectionState::Ready) {
            result_.state = PairedCheckoutState::Preparing;
            result_.rental = record_.rental;
            result_.endpoint.reset();
            result_.message = "Waiting for the exact rental to become ready";
            return result_;
        }
        if (!evidence.endpoint || !validEndpoint(*evidence.endpoint) || !pairedEndpoint_ ||
            evidence.endpoint->host != pairedEndpoint_->host ||
            evidence.endpoint->httpPort != pairedEndpoint_->httpPort ||
            evidence.endpoint->httpsPort != pairedEndpoint_->httpsPort)
            return failPaid("Ready rental endpoint does not match the paired host");
        record_.phase = PairedCheckoutPhase::Streaming;
        try { persist(); }
        catch (...) { return failPaid("Streaming phase could not be journaled"); }
        if (stopSignal_.load()) return stopKnown();
        if (!sameAccount()) return failPaid("Account changed before stream handoff");
        result_.state = PairedCheckoutState::Ready;
        preparingDeadline_.reset();
        result_.rental = record_.rental;
        result_.stopRequested = false;
        result_.endpoint = evidence.endpoint;
        result_.message = "Pairing and exact rental ownership verified";
        return result_;
    } catch (...) { return failPaid("Rental inspection failed"); }
}
PairedCheckoutResult PairedCheckoutGuard::reconnect() {
    if (!pending_ || invalid_ || record_.rental.empty())
        return recovery("No exact recorded rental is available for reconnect");
    stopSignal_.store(false);
    pairing_.cancelAndJoin();
    paired_ = false;
    pairedEndpoint_.reset();
    preparingDeadline_ = clock_() + preparingLimit;
    try {
        const auto auth = api_.auth();
        if (!auth.authenticated() || auth.account != record_.account)
            return recovery("Restore the original rental account");
        generation_ = auth.generation;
        if (record_.stopRequested || stopSignal_.load()) return stopKnown();
        RentalInspection evidence;
        if (!inspect(evidence)) return recovery("Exact rental ownership could not be verified");
        if (evidence.state == RentalInspectionState::Terminal) return terminal();
        if (stopSignal_.load()) return stopKnown();
        // Never call createPairedRental here, even if reconnect is Unsupported.
        const auto prepared = api_.preparePairedReconnect(record_.rental);
        if (stopSignal_.load()) return stopKnown();
        if (!sameAccount() || !validPreparation(prepared, true))
            return recovery("Same-rental reconnect preparation could not be verified");
        PinScope pin;
        if (!startPairing(prepared, pin.value)) {
            pairing_.cancelAndJoin();
            if (stopSignal_.load()) return stopKnown();
            return recovery("Same-rental pairing did not become ready");
        }
        record_.phase = PairedCheckoutPhase::Pairing;
        try { persist(); }
        catch (...) { return failPaid("Reconnect phase could not be journaled"); }
        if (stopSignal_.load()) return stopKnown();
        if (!sameAccount()) return failPaid("Account changed before reconnect");
        api_.completePairedReconnect(prepared.id, pin.value);
        if (!sameAccount() || !pairing_.complete()) return failPaid("Same-rental pairing did not complete");
        pairing_.cancelAndJoin();
        paired_ = true;
        pairedEndpoint_ = prepared.endpoint;
        return poll();
    } catch (const RepositoryError& error) {
        pairing_.cancelAndJoin();
        if (error.kind() == RepositoryErrorKind::Unsupported && stopSignal_.load()) return stopKnown();
        if (error.kind() == RepositoryErrorKind::Unsupported)
            return recovery("Provider does not support verified same-rental reconnect");
        return failPaid("Same-rental reconnect failed");
    } catch (...) {
        pairing_.cancelAndJoin();
        return failPaid("Same-rental reconnect failed");
    }
}
PairedCheckoutResult PairedCheckoutGuard::stop() {
    if (invalid_) return recovery("Rental journal is invalid; do not guess a rental ID");
    if (!pending_) return result_;
    return stopKnown();
}
PairedCheckoutResult PairedCheckoutGuard::bindAuthenticatedAccount() {
    if (invalid_) return recovery("Rental journal is invalid; do not guess a rental ID");
    if (!pending_) return result_;
    pairing_.cancelAndJoin();
    paired_ = false;
    pairedEndpoint_.reset();
    result_.endpoint.reset();
    try {
        const auto auth = api_.auth();
        if (!auth.authenticated() || auth.account != record_.account)
            return recovery("Restore the original rental account");
        generation_ = auth.generation;
        if (record_.rental.empty()) return recovery("Creation outcome unknown; do not retry creation");
        RentalInspection evidence;
        if (!inspect(evidence)) return recovery("Exact rental ownership could not be verified");
        if (evidence.state == RentalInspectionState::Terminal) return terminal();
        return recovery(record_.stopRequested ? "Recorded stop can now be reconciled" :
            "Original rental account verified; explicit stop or reconnect is required");
    } catch (...) { return recovery("Original rental account could not be verified"); }
}
}
