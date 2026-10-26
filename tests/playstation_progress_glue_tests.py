"""Actual PlayStation coordinator methods with synthetic workers; no network/billing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/features/playstation/presentation/playstation_controller.cpp').read_text()
def method(name):
    start = source.index('void ApplicationController::' + name + '(')
    brace = source.index('{',start)
    depth, end = 1, brace+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]

harness = r'''
#include "features/playstation/application/catalog_progress.hpp"
#include "features/playstation/domain/computer.hpp"
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
namespace fog {
std::vector<Computer> playStationComputerPlaceholders() {
    std::vector<Computer> result;
    for(const auto& pc:PlayStationComputers) {
        Computer value; value.id={ProviderId::Mts,std::to_string(pc.id)}; value.name=pc.name;
        result.push_back(std::move(value));
    }
    return result;
}
struct FakeRepository;
std::function<std::vector<Computer>(FakeRepository&,const CatalogProgress&,const CatalogCancellation&)> loader;
struct FakeRepository final : CatalogRepository {
    std::atomic_bool wasCancelled{false};
    Page<Game> games(const GameQuery&) override {return {};}
    Game game(const EntityId&) override {return {};}
    Page<Computer> computers(const ComputerQuery&) override {return {};}
    AutomaticComputers recommend(const ComputerQuery&) override {return {};}
    std::vector<Computer> playStationComputers(const CatalogProgress& progress,const CatalogCancellation& cancelled) override {
        return loader(*this,progress,cancelled);
    }
    void cancel() noexcept override {wasCancelled=true;}
};
struct FakeServices {
    std::shared_ptr<CatalogRepository> publicCatalog() {return std::make_shared<FakeRepository>();}
};
}
std::string safeText(const std::string& value) {return value;}
void diagnostic(const std::string&) {}
namespace fogplay {
enum class Screen {PlayStation,PlayStationDetails,Games,Checkout};
struct ApplicationController {
    Screen screen_=Screen::Games;
    bool sessionPending_=false;
    std::future<int> pending_;
    struct Latency {void cancel() {}} latency_;
    int nav_=0;
    std::size_t selection_=0;
    std::vector<fog::Computer> playStationComputers_;
    std::unique_ptr<fog::FakeServices> services_=std::make_unique<fog::FakeServices>();
    std::chrono::steady_clock::time_point playStationLoadedAt_{};
    std::string playStationError_,status_;
    std::future<std::vector<fog::Computer>> playStationPending_;
    std::shared_ptr<fog::CatalogRepository> playStationRepository_;
    std::shared_ptr<fog::PlayStationCatalogProgressChannel> playStationUpdates_;
    void loadPlayStation();
    void pollPlayStation();
    void cancelPlayStation();
    ~ApplicationController() {cancelPlayStation();if(playStationPending_.valid()) playStationPending_.wait();}
};
'''
harness += '\n'.join(method(name) for name in ['loadPlayStation','pollPlayStation','cancelPlayStation'])
harness += r'''
}
int main() {
    using fogplay::ApplicationController;
    using fogplay::Screen;
    for(int scenario=0;scenario<5;++scenario) {
        std::promise<void> published,finish;
        auto finishGate=finish.get_future().share();
        auto batch=fog::playStationComputerPlaceholders();
        for(auto& pc:batch) {pc.listed=true;pc.launchable=true;pc.price="35";}
        std::swap(batch[0],batch[6]);
        auto final=batch; final[0].price="40";
        fog::loader=[&,finishGate](fog::FakeRepository&,const fog::PlayStationCatalogProgress& progress,const fog::PlayStationCancellation&) {
            progress(batch);published.set_value();finishGate.wait();
            if(scenario==3) throw std::runtime_error("synthetic total outage after progress");
            return final;
        };
        ApplicationController app;
        app.loadPlayStation();
        assert(app.screen_==Screen::PlayStation && app.playStationComputers_.size()==7 && app.playStationPending_.valid());
        assert(app.playStationLoadedAt_==std::chrono::steady_clock::time_point{});
        app.selection_=6;
        published.get_future().wait();
        if(scenario==1 || scenario==4) {app.screen_=scenario==1?Screen::Games:Screen::Checkout;app.selection_=92;app.status_="other page status";}
        if(scenario==2) {app.cancelPlayStation();assert(std::static_pointer_cast<fog::FakeRepository>(app.playStationRepository_)->wasCancelled && app.playStationUpdates_->cancelled());}
        app.pollPlayStation();
        assert(app.playStationPending_.valid());
        if(scenario==0 || scenario==3) assert(app.selection_==0 && app.playStationComputers_[0].id.value=="5510" && app.playStationComputers_[0].price=="35");
        if(scenario==1 || scenario==4) assert(app.selection_==92 && app.status_=="other page status");
        if(scenario==0) {
            auto channel=app.playStationUpdates_;
            app.loadPlayStation();assert(app.playStationUpdates_==channel && app.selection_==0);
            app.pending_=std::async(std::launch::deferred,[]{return 0;});
            app.pollPlayStation();assert(app.selection_==0 && app.pending_.valid());app.pending_.get();
        }
        finish.set_value();app.playStationPending_.wait();app.pollPlayStation();
        assert(!app.playStationPending_.valid() && !app.playStationUpdates_ && !app.playStationRepository_);
        if(scenario==0) assert(app.playStationLoadedAt_!=std::chrono::steady_clock::time_point{} && app.playStationComputers_[0].price=="40" && app.selection_==0);
        if(scenario==1 || scenario==4) assert(app.selection_==92 && app.status_=="other page status" && app.screen_==(scenario==1?Screen::Games:Screen::Checkout));
        if(scenario==2) assert(app.playStationLoadedAt_==std::chrono::steady_clock::time_point{} && app.playStationComputers_[0].price.empty());
        if(scenario==3) assert(app.playStationLoadedAt_==std::chrono::steady_clock::time_point{} && !app.playStationError_.empty() && app.playStationComputers_[0].price=="35");
    }
    {
        fog::loader=[](fog::FakeRepository&,const fog::PlayStationCatalogProgress&,const fog::PlayStationCancellation&)->std::vector<fog::Computer> {throw std::runtime_error("all merchants failed");};
        ApplicationController app;app.loadPlayStation();app.playStationPending_.wait();app.pollPlayStation();
        assert(app.playStationComputers_.size()==7 && !app.playStationError_.empty() && app.playStationLoadedAt_==std::chrono::steady_clock::time_point{});
    }
    {
        std::promise<void> started;
        fog::loader=[&](fog::FakeRepository&,const fog::PlayStationCatalogProgress& progress,const fog::PlayStationCancellation& cancelled) {
            started.set_value();while(!cancelled()) std::this_thread::yield();
            progress(fog::playStationComputerPlaceholders());return std::vector<fog::Computer>{};
        };
        std::weak_ptr<fog::PlayStationCatalogProgressChannel> weak;
        {ApplicationController app;app.loadPlayStation();weak=app.playStationUpdates_;started.get_future().wait();}
        assert(weak.expired());
    }
    std::cout<<"PlayStation actual coordinator: immediate placeholders, independent progress, selected ID, offscreen/checkout isolation, cancellation, failure and shutdown join PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-playstation-progress-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'src'),'-I'+str(root/'third_party'),
                    str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)