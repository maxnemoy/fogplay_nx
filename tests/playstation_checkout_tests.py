"""Exercise the actual checkout glue with synthetic PlayStation offers; no network or billing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def method(source, name):
    start = source.index('void ApplicationController::' + name + '(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#include "core/constants/ui_text.hpp"
namespace uiText = fog::constants::ui;
#include "features/session/domain/service_models.hpp"
#include "features/playstation/domain/computer.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <variant>
using Json = nlohmann::json;
struct VerifiedOffer { fog::StartRentalRequest request; };
struct ChangedOffer { std::vector<fog::Computer> computers; };
using TaskResult = std::variant<std::monostate,VerifiedOffer,ChangedOffer>;
fog::EntityId identity(int id) { return {fog::ProviderId::Mts,std::to_string(id)}; }
void mutate(fog::Computer& pc,const Json& values) {
    if(values.contains("listed")) pc.listed=values["listed"].get<bool>();
    if(values.contains("launchable")) pc.launchable=values["launchable"].get<bool>();
    if(values.contains("id")) pc.id=identity(values["id"].get<int>());
    if(values.contains("tariff_id")) pc.offerId=values["tariff_id"]==0 ? fog::EntityId{} : identity(values["tariff_id"].get<int>());
    if(values.contains("price")) {
        if(values["price"].is_null()) { pc.hourlyPrice.reset(); pc.price.clear(); }
        else { pc.hourlyPrice=fog::Money{static_cast<int64_t>(values["price"].get<double>()*100),"RUB"}; pc.price=values["price"].dump(); }
    }
}

namespace fog {
std::vector<Computer> currentOffers;
bool failFetch = false;
std::function<void()> duringFetch;
std::vector<Computer> fetchPlayStationComputers() {
    if (duringFetch) duringFetch();
    if (failFetch) throw std::runtime_error("fixture fetch failure");
    return currentOffers;
}
}
constexpr int AppletFocusState_InFocus=1;
int focusState=AppletFocusState_InFocus;
int appletGetFocusState() { return focusState; }
enum class Screen { Details, PlayStationDetails, PlayStation, Consent, Checkout };
enum class Task { Create };
struct ApplicationController {
    Screen screen_ = Screen::PlayStationDetails, loginReturn_ = Screen::Details;
    bool sessionPending_=false, selectedPlayStation_=false, stopAfterTask_=false, ownSteam_=true, exitAfterStop_=false;
    struct Auth { bool signedIn=true; bool authenticated() const { return signedIn; } } auth_;
    struct Catalog { auto playStationComputers() { return fog::fetchPlayStationComputers(); } } catalogRepository_;
    struct Checkout {
        int calls=0;
        fog::StartRentalRequest request{};
        void create(const fog::StartRentalRequest& order) { ++calls; request=order; }
    } checkout_;
    struct Recent {
        std::optional<int> value;
        void prepare(std::optional<int> input) { value=input; }
    } recentLaunch_;
    std::vector<fog::Computer> playStationComputers_, computers_;
    std::chrono::steady_clock::time_point playStationLoadedAt_=std::chrono::steady_clock::now();
    std::size_t selection_=0;
    fog::EntityId selectedGameId_=identity(987);
    std::optional<int> selectedRecentGame_=987;
    std::string status_, checkoutStatus_;
    bool storageChecked=false, armed=false;
    int refreshes=0;
    std::function<TaskResult()> worker;
    void checkStorage() { storageChecked=true; }
    void armExitHandling() { armed=true; }
    void loadPlayStation() { ++refreshes; screen_=Screen::PlayStation; }
    void start(Task,std::function<TaskResult()> work) { worker=std::move(work); }
    bool hasJournal=false, disarmed=false;
    int finishRequests=0;
    void syncPending() { sessionPending_=hasJournal; }
    void disarmExitHandling() { disarmed=true; armed=false; }
    void beginFinish(const char*) { ++finishRequests; }
    void acceptCreateResult(TaskResult data);
    void createSession();
};
'''
harness += method((root/'src/features/session/presentation/rental_controller.cpp').read_text(), 'createSession')
poll_source = (root/'src/app/application.cpp').read_text()
harness += '\nvoid ApplicationController::acceptCreateResult(TaskResult data) { const auto kind=Task::Create;\n'
for marker in ['VerifiedOffer','ChangedOffer']:
    marker_start = poll_source.index('    if (kind == Task::Create && std::holds_alternative<' + marker + '>(data)) {')
    marker_brace = poll_source.index('{', marker_start)
    marker_depth, marker_end = 1, marker_brace + 1
    while marker_depth:
        marker_depth += (poll_source[marker_end] == '{') - (poll_source[marker_end] == '}')
        marker_end += 1
    harness += poll_source[marker_start:marker_end] + '\n'
harness += '}\n'

harness += r'''
fog::Computer offer(int id=11573) {
    fog::Computer pc;pc.id=identity(id);pc.name=id==13025?"F4BX8":"3A7X47";
    pc.listed=pc.launchable=true;pc.offerId=identity(57229);pc.hourlyPrice=fog::Money{4900,"RUB"};pc.price="49.0";
    return pc;
}
ApplicationController fixture(int id=11573) {
    ApplicationController app;
    app.playStationComputers_.push_back(offer(id));
    fog::currentOffers=app.playStationComputers_;
    fog::failFetch=false; fog::duringFetch={}; focusState=AppletFocusState_InFocus;
    return app;
}
int main() {
    for (const int id : {11573,13025,7705,5632,5510}) {
        auto app=fixture(id); app.createSession();
        assert(app.armed && app.storageChecked && app.sessionPending_ && app.worker);
        assert(app.screen_==Screen::Checkout && app.selectedPlayStation_);
        const auto result=app.worker();
        assert(std::holds_alternative<VerifiedOffer>(result) && app.checkout_.calls==0);
        app.acceptCreateResult(result);
        assert(app.worker && app.sessionPending_ && app.armed && !app.disarmed);
        assert(std::holds_alternative<std::monostate>(app.worker()) && app.checkout_.calls==1);
        assert(app.checkout_.request.computerId==identity(id) && app.checkout_.request.offerId==identity(57229));
        assert(app.checkout_.request.gameId.empty() && !app.checkout_.request.ownAccount);
        assert(!app.recentLaunch_.value);
    }
    for (int cancellation=0; cancellation<3; ++cancellation) {
        auto app=fixture(); app.createSession();
        fog::duringFetch=[&app,cancellation] {
            if (cancellation==0) app.stopAfterTask_=true; // Back queued while fetching.
            if (cancellation==1) focusState=0; // HOME before the result is accepted.
            if (cancellation==2) app.exitAfterStop_=true;
        };
        const auto result=app.worker();
        assert(std::holds_alternative<VerifiedOffer>(result) && app.checkout_.calls==0);
        app.worker={};
        app.acceptCreateResult(result);
        assert(!app.worker && app.checkout_.calls==0 && !app.sessionPending_ && app.disarmed);
        assert(app.screen_==Screen::PlayStation && !app.stopAfterTask_);
    }
    {
        auto app=fixture(); app.createSession();
        const auto result=app.worker(); app.worker={}; app.hasJournal=true;
        app.acceptCreateResult(result);
        assert(!app.worker && app.checkout_.calls==0 && app.sessionPending_ && app.finishRequests==1 && !app.disarmed);
    }
    for (const auto& mutation : {Json{{"listed",false}},Json{{"launchable",false}},
                                Json{{"id",42}},Json{{"tariff_id",0}}}) {
        auto app=fixture(); mutate(app.playStationComputers_[0],mutation); app.createSession();
        assert(!app.worker && !app.armed && !app.sessionPending_ && app.checkout_.calls==0);
    }
    for (const bool missingTimestamp : {false,true}) {
        auto app=fixture();
        app.playStationLoadedAt_=missingTimestamp ? std::chrono::steady_clock::time_point{} :
            std::chrono::steady_clock::now()-std::chrono::seconds(61);
        app.createSession();
        assert(app.refreshes==1 && !app.worker && !app.armed && app.checkout_.calls==0);
        assert(app.screen_==Screen::PlayStation);
    }
    for (const auto& mutation : {Json{{"listed",false}},Json{{"launchable",false}},
                                Json{{"tariff_id",57230}},Json{{"price",50.0}},Json{{"price",nullptr}}}) {
        auto app=fixture(); app.createSession();
        mutate(fog::currentOffers[0],mutation);
        const auto result=app.worker();
        assert(std::holds_alternative<ChangedOffer>(result) && app.checkout_.calls==0);
        assert(std::get<ChangedOffer>(result).computers.size()==fog::currentOffers.size());
    }
    {
        auto app=fixture(); app.createSession(); fog::currentOffers.clear();
        assert(std::holds_alternative<ChangedOffer>(app.worker()) && app.checkout_.calls==0);
    }
    {
        auto app=fixture(); app.createSession(); fog::failFetch=true;
        bool failed=false; try { app.worker(); } catch (const std::runtime_error&) { failed=true; }
        assert(failed && app.checkout_.calls==0); fog::failFetch=false;
    }
    {
        auto app=fixture(); app.auth_.signedIn=false; app.createSession();
        assert(app.screen_==Screen::Consent && app.loginReturn_==Screen::PlayStationDetails);
        assert(!app.worker && !app.armed && !app.sessionPending_);
    }
    {
        auto app=fixture(); app.screen_=Screen::Details; app.computers_=app.playStationComputers_;
        app.selectedPlayStation_=true; app.createSession();
        assert(!app.selectedPlayStation_ && app.worker);
        app.worker();
        assert(app.checkout_.calls==1 && app.checkout_.request.gameId==identity(987) && app.checkout_.request.ownAccount);
        assert(app.recentLaunch_.value==987);
    }
    {
        auto app=fixture(); app.sessionPending_=true; app.createSession();
        assert(!app.worker && !app.armed && app.checkout_.calls==0);
    }
    for (const bool stopQueued : {false,true}) {
        auto app=fixture(); app.createSession(); app.stopAfterTask_=stopQueued;
        mutate(fog::currentOffers[0],Json{{"price",55.0}});
        auto result=app.worker();
        app.acceptCreateResult(std::move(result));
        assert(!app.sessionPending_ && app.disarmed && !app.armed && !app.stopAfterTask_);
        assert(app.screen_==Screen::PlayStation && app.playStationComputers_[0].hourlyPrice->minorUnits==5500);
        assert(app.checkout_.calls==0 && app.finishRequests==0);
        assert(app.playStationLoadedAt_!=std::chrono::steady_clock::time_point{});
    }
    {
        auto app=fixture(); app.createSession(); app.hasJournal=true;
        mutate(fog::currentOffers[0],Json{{"price",55.0}});
        app.acceptCreateResult(app.worker());
        assert(app.sessionPending_ && app.finishRequests==1 && !app.disarmed);
        assert(app.playStationComputers_[0].hourlyPrice->minorUnits==4900 && app.checkout_.calls==0);
    }
    std::cout << "PlayStation checkout: original/fourth PC fresh offer dispatch, Back/HOME/exit during fetch, tariff/price changes, stale/missing list, absent/invalid PC, fetch failure, auth, journal guard and normal-mode isolation PASS (actual glue; no paid requests)\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-playstation-checkout-') as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'src'),'-I'+str(root/'third_party'),
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)

