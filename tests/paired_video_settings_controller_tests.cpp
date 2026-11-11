#include "app/application_controller.hpp"
#include <atomic>
using namespace player_adapter_test;
using fogplay::ApplicationController;
namespace {
std::atomic<unsigned> saves{0};
std::atomic<bool> saveThrows{false};
fog::video::Settings saved;
std::thread::id writer;
player_test::Blocker* saveBlocker = nullptr;
fog::video::Settings changed(const ApplicationController& c, int bitrate = 30) {
    auto value = c.videoSettings_; value.codec = ::stream::VideoCodec::H264;
    value.bitrateMbps = bitrate; value.quality = value.keyframeSeconds = 0;
    return value;
}
bool notice(const ApplicationController& c, const std::string& text) { return c.videoSaveStatus_.find(text) != std::string::npos; }
void edit(ApplicationController& c, int bitrate = 30) { c.changeVideoSettings(changed(c,bitrate),true); c.settingsSaves_.flush(); }
Rental::Effects quality(ApplicationController& c, unsigned inspections = 1) {
    c.verifyRequest(Player::Purpose::Quality);
    const auto count = c.rental.fh.inspections;
    auto result = c.join();
    require(result.operation == Rental::Operation::QualityInspection && c.rental.fh.inspections == count + inspections,
        "quality uses real coordinator poll with expected ownership and stop-verification reads");
    return result;
}
void resume(ApplicationController& c, const Rental::Effects& result) {
    c.deliver(result); c.step(); c.joined(); c.effects(); c.step();
}
}
namespace fog::video {
void saveSettings(const std::string&, const Settings& value) {
    ++saves; writer = std::this_thread::get_id();
    const bool fail = saveThrows.load();
    if (saveBlocker) saveBlocker->block();
    if (fail) throw std::runtime_error("synthetic storage failure");
    saved = value;
}
}
static void persistenceAndCoalescing() {
    namespace Text = fog::constants::ui::Settings;
    {
        ApplicationController c(1h);
        const auto count = saves.load(); auto value = changed(c); value.framesPerSecond = 60;
        c.changeVideoSettings(value,true);
        require(fog::stage1::asyncSettings ? saves == count && notice(c,Text::Saving) : saves == count + 1 && notice(c,Text::Saved),
            "save status matches async or synchronous settings profile");
        require(c.pending.valid() && c.videoSettings_.bitrateMbps == 30 && c.starts() == 1,
            "in-memory preferences and quality inspection do not wait for storage");
        c.settingsSaves_.flush();
        require(saves == count + 1 && notice(c,Text::Saved) && notice(c,Text::FogHubReconnectNotice) &&
            notice(c,"60 FPS") && c.overlay_.saved == c.videoSaveStatus_,
            "owner completion preserves reconnect and next-FPS notices");
        if (fog::stage1::asyncSettings) require(writer != std::this_thread::get_id(), "real settings writer runs off owner thread");
    }
    {
        ApplicationController c; c.videoSettings_.quality = 4; c.videoSettings_.keyframeSeconds = 5;
        auto original = c.client(); edit(c);
        require(saved.quality == 4 && saved.keyframeSeconds == 5 && saved.bitrateMbps == 30,
            "FH normalized edits preserve shared MTS quality and keyframe preferences");
        require(c.pending.valid() && original->retries == 0 && original->closes == 0,
            "healthy transport remains alive until verified quality verdict");
        edit(c,40); require(c.dispatches == 1, "edits coalesce into single inspection");
        auto result = quality(c); c.deliver(result);
        require(original->retries == 0 && c.starts() == 1, "quality verdict is store-only until fresh admission");
        c.step(); c.joined();
        require(c.starts() == 1 && c.owner.snapshot().qualityClosing, "joined close never resumes before overlay input");
        c.step();
        require(c.starts() == 2 && c.client()->settings.bitrateMbps == 40 && c.client()->connection.resumeOnly &&
            c.client()->connection.rental == c.identity && original->retries == 1 && original->closes == 0 && c.stops.empty(),
            "real Player resumes exact rental with latest coalesced settings after nonfinal close");
        require(c.owner.snapshot().attempts == 0 && c.rental.fh.creates == 1, "quality reconnect neither spends recovery budget nor creates rental");
        c.deliver(result); c.step(); require(c.starts() == 2, "duplicate consumed result cannot resume twice");
    }
    {
        ApplicationController c; saveThrows = true; edit(c); saveThrows = false;
        require(notice(c,Text::NotSaved) && c.pending.valid() && c.videoSettings_.bitrateMbps == 30,
            "storage failure is visible while explicit in-memory quality intent remains valid");
    }
    {
        ApplicationController c; c.settingsSaves_.finish(); edit(c);
        require(notice(c,Text::NotSaved) && c.pending.valid(), "settings dispatch failure reports unsaved preferences without losing live intent");
    }
    if (fog::stage1::asyncSettings) {
        ApplicationController c; player_test::Blocker blocker; saveBlocker = &blocker; saveThrows = true;
        c.changeVideoSettings(changed(c),true);
        const bool entered = blocker.wait();
        saveThrows = false;
        c.changeVideoSettings(changed(c,40),true);
        const bool responsive = c.pending.valid() && c.videoSettings_.bitrateMbps == 40 && c.dispatches == 1 && notice(c,Text::Saving);
        blocker.release(); c.settingsSaves_.flush(); saveBlocker = nullptr;
        require(entered && responsive, "blocked settings write does not block owner edits or inspection coalescing");
        require(saved.bitrateMbps == 40 && notice(c,Text::Saved) && !notice(c,Text::NotSaved),
            "older failed async write cannot overwrite latest successful revision status");
    }
}
static void cancellationAndClose() {
    for (int intent = 0; intent < 8; ++intent) {
        ApplicationController c; player_test::Blocker blocker;
        c.client()->retryAction = [&] { blocker.block(); };
        edit(c); auto result = quality(c); c.deliver(result); c.step();
        const bool entered = blocker.wait();
        const bool blocked = c.owner.snapshot().teardown && c.starts() == 1;
        edit(c,40);
        const bool coalesced = c.dispatches == 1 && c.starts() == 1 && c.owner.snapshot().qualityClosing;
        blocker.release(); c.joined();
        require(entered && blocked && coalesced && c.starts() == 1,
            "quality edits coalesce during blocked real close and mailbox join never resumes");
        if (intent == 0) c.admission.stopRequested = true;
        if (intent == 1) c.admission.shuttingDown = true;
        if (intent == 2) c.admission.pairedPending = false;
        if (intent == 3) c.admission.rentalPending = false;
        if (intent == 4) c.admission.active = false;
        if (intent == 5) c.owner.cancel();
        if (intent == 6) c.owner.closeResolvedRental();
        c.step();
        if (intent == 7) {
            require(c.starts() == 2 && !c.owner.snapshot().qualityClosing && c.stops.empty() &&
                c.client()->settings.bitrateMbps == 40 && c.client()->connection.resumeOnly &&
                c.client()->connection.rental == c.identity,
                "fresh positive admission resumes exact rental with edits coalesced during blocked close");
            c.step();
            require(c.starts() == 2, "joined quality close is consumed exactly once");
        } else {
            require(c.starts() == 1 && !c.owner.snapshot().qualityClosing,
                "post-join overlay stop, shutdown, lost ownership/focus, final close or cancel prevents resume");
        }
    }
    {
        ApplicationController c; player_test::Blocker blocker;
        c.client()->retryAction = [&] { blocker.block(); };
        edit(c); c.deliver(quality(c)); c.step(); const bool entered = blocker.wait();
        c.now += 14999ms; c.step(); const bool alive = c.stops.empty();
        c.now += 1ms; c.step(); blocker.release(); c.joined();
        require(entered && alive && c.stops == std::vector<Player::StopReason>{Player::StopReason::QualityCloseDeadline} && c.starts() == 1,
            "quality close remains bounded by fifteen seconds even with blocked callback");
    }
    for (int intent = 0; intent < 3; ++intent) {
        ApplicationController c; edit(c); auto result = quality(c); auto original = c.client(); c.deliver(result);
        if (intent == 0) c.admission.stopRequested = true;
        if (intent == 1) c.admission.shuttingDown = true;
        if (intent == 2) c.admission.active = false;
        c.step();
        require(c.starts() == 1 && original->retries == 0 && original->closes == 0,
            "fresh overlay admission beats ready quality result before close begins");
    }
}
static void failuresAndContracts() {
    for (int invalid = 0; invalid < 7; ++invalid) {
        ApplicationController c; edit(c); auto result = quality(c);
        if (invalid == 0) result.resume->expected.value = "other";
        if (invalid == 1) result.resume->expected.provider = ProviderId::Mts;
        if (invalid == 2) result.resume->purpose = Rental::ResumePurpose::Transport;
        if (invalid == 3) result.resume->status = Rental::ResumeStatus::Refused;
        if (invalid == 4) result.resume.reset();
        if (invalid == 5) c.client()->disconnected = true;
        if (invalid == 6) c.client()->ready = false;
        c.deliver(result); c.step();
        require(c.starts() == 1 && c.stops.size() == 1, "invalid identity/purpose/evidence or failed live stream forbids quality resume");
    }
    for (int failure = 0; failure < 3; ++failure) {
        ApplicationController c; edit(c); auto result = quality(c); auto original = c.client();
        if (failure == 0) original->retryOk = false;
        if (failure == 1) { auto next = std::make_shared<player_test::ClientState>(); next->startOk = false; c.transports.next.push_back(next); }
        if (failure == 2) c.transports.fail = true;
        resume(c,result);
        require(c.stops.size() == 1 && (failure != 0 || original->closes == 0),
            "refused retry-close, failed resume or thrown factory requests owned stop");
    }
    {
        ApplicationController c; c.dispatchFails = true; edit(c);
        require(c.stops == std::vector<Player::StopReason>{Player::StopReason::InspectionDispatch} && !c.owner.snapshot().inspection,
            "quality dispatch failure clears pending intent and requires stop");
    }
    {
        ApplicationController c; edit(c); c.rental.fh.pairedTerminal = true; c.deliver(quality(c)); c.joined(); c.step();
        require(!c.owner.snapshot().ownsRental && c.starts() == 1 && c.stops.empty(),
            "terminal coordinator evidence closes resolved rental without quality restart");
    }
    {
        ApplicationController c; edit(c); c.rental.fh.inspectionFailure = true;
        const auto result = quality(c,2);
        require(c.rental.owner->requested() && c.rental.owner->state().paired.stopRequested && (!result.resume || result.resume->status != Rental::ResumeStatus::Retry),
            "real quality poll preserves Stage2 stop-on-inspection-error contract unlike transport inspectForResume");
        c.deliver(result); c.joined(); c.step();
        require(c.starts() == 1, "quality inspection outage cannot start an unverified stream");
    }
    {
        ApplicationController c; c.admission.serviceBusy = true; edit(c);
        require(c.stops.empty() && c.starts() == 1 && c.client()->retries == 0 && !c.pending.valid() &&
            notice(c,fog::constants::ui::Settings::FogHubReconnectBusyNotice), "busy service saves preferences and keeps healthy rental");
    }
}
static void handshakeAndFps() {
    namespace Text = fog::constants::ui::Settings;
    for (bool resumed : {false,true}) {
        ApplicationController c;
        if (resumed) {
            auto next = std::make_shared<player_test::ClientState>(); next->ready = false; c.transports.next.push_back(next);
            edit(c); resume(c,quality(c));
        } else c.client()->ready = false;
        const auto starts = c.starts(); const auto dispatches = c.dispatches; const auto active = c.client()->settings;
        edit(c,40);
        require(c.starts() == starts && c.dispatches == dispatches && !c.pending.valid() && c.stops.empty() &&
            c.client()->settings == active && c.client()->updates == 0 && c.videoSettings_.bitrateMbps == 40 && notice(c,Text::FogHubConnectingSavedNotice),
            "initial/resumed handshake saves next settings without another stop or reconnect");
    }
    for (auto provider : {ProviderId::Mts,ProviderId::FogHub}) {
        ApplicationController c(0ms,provider);
        const auto original = c.client()->settings; auto value = c.videoSettings_; value.framesPerSecond = 60;
        c.changeVideoSettings(value,true); c.settingsSaves_.flush();
        require(saved.framesPerSecond == 60 && c.owner.snapshot().framesPerSecond == 30 && c.client()->settings == original &&
            !c.pending.valid() && c.starts() == 1 && c.stops.empty() && notice(c,"60 FPS"),
            "FPS-only edit persists for next connection without changing live transport");
        require(c.client()->updates == 0, "FPS-only edit sends no live video settings request on either provider");
        value.bitrateMbps = 30; c.changeVideoSettings(value,true); c.settingsSaves_.flush();
        if (provider == ProviderId::Mts) {
            require(c.client()->updates == 1 && c.client()->settings.bitrateMbps == 30 && c.client()->settings.framesPerSecond == 30 &&
                saved.framesPerSecond == 60 && !c.pending.valid() && notice(c,Text::RequestSentNotice),
                "MTS live bitrate preserves active FPS and saves next FPS");
        } else {
            resume(c,quality(c));
            require(c.starts() == 2 && c.client()->settings.framesPerSecond == 60 && c.client()->connection.rental == c.identity,
                "explicit FH bitrate reconnect applies latest saved FPS to exact rental");
        }
    }
    {
        ApplicationController c(0ms,ProviderId::Mts,60); auto value = c.videoSettings_; value.framesPerSecond = 30; value.quality = 2;
        c.changeVideoSettings(value,true); c.settingsSaves_.flush();
        require(saved.framesPerSecond == 30 && c.client()->updates == 1 && c.client()->settings.framesPerSecond == 60 && c.client()->settings.quality == 2,
            "saved 30 FPS never implicitly downgrades live MTS 60 FPS");
    }
    {
        ApplicationController c(0ms,ProviderId::Mts); c.client()->configOk = false; edit(c);
        require(notice(c,Text::RequestNotSentNotice) && c.stops.empty(), "rejected MTS live request has explicit saved-but-not-applied notice");
    }
    {
        ApplicationController c; const auto count = saves.load(); auto value = changed(c); value.framesPerSecond = 17;
        c.changeVideoSettings(value,true); c.settingsSaves_.flush();
        require(saves == count && !c.pending.valid(), "invalid settings never persist or start inspection");
        c.changeVideoSettings(changed(c),false); c.settingsSaves_.flush();
        require(saved.bitrateMbps == 30 && !c.pending.valid() && c.client()->updates == 0 && c.client()->settings.bitrateMbps == 20,
            "non-live edit persists without touching transport");
    }
}
int main() {
    try {
        persistenceAndCoalescing(); cancellationAndClose(); failuresAndContracts(); handshakeAndFps();
        std::cout << "Player settings adapter: " << assertions << " assertions passed\n";
    } catch (const std::exception& error) { std::cerr << "Player settings adapter failed: " << error.what() << '\n'; return 1; }
}