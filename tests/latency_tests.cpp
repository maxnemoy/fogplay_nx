#include "features/catalog/infrastructure/latency.hpp"
#include "features/catalog/infrastructure/mts_catalog_mapper.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int assertions=0;
void require(bool value,const char* label) { ++assertions;if(!value)throw std::runtime_error(label); }
template<class Predicate> void wait(Predicate predicate,const char* label) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate() && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(predicate(),label);
}
}
int main() {
    try {
        using fog::LatencyService;
        require(LatencyService::connectionMilliseconds(0.5,0.7)==200,"exclude DNS from TCP estimate");
        require(LatencyService::connectionMilliseconds(0,0.0001)==1,"submillisecond minimum");
        require(!LatencyService::connectionMilliseconds(1,0.5),"invalid clock order rejected");
        require(!LatencyService::connectionMilliseconds(0,0),"missing connect timing rejected");
        require(!LatencyService::connectionMilliseconds(0,4),"outlier rejected");
        require(LatencyService::validEndpoint("turn-nsk1.remoteplay.mts.ru:443"),"MTS endpoint");
        require(LatencyService::validEndpoint("unit-test-fixture.keenetic.link:443/version.js"),"legacy HEAD endpoint");
        require(LatencyService::routeKey("turn-nsk1.remoteplay.mts.ru:443")=="turn-nsk1_remoteplay_mts_ru","exact API route key");
        for(const auto* endpoint:{"https://turn-nsk1.remoteplay.mts.ru/","127.0.0.1:443","localhost:443","turn-nsk1.remoteplay.mts.ru:444","x.mts.ru.evil.test:443","x.keenetic.link@evil.test","x.keenetic.link:443/?token=x","x.keenetic.link:443/#x","x..keenetic.link:443"})
            require(!LatencyService::validEndpoint(endpoint),"untrusted endpoint rejected");
        std::atomic_int calls{0},active{0},maximum{0};
        LatencyService service([](const auto&){return std::vector<std::string>{"turn-nsk1.remoteplay.mts.ru:443","turn-msk1.remoteplay.mts.ru:443","turn-spb1.remoteplay.mts.ru:443","turn-spb1.remoteplay.mts.ru:443"};},
            [&](const auto& endpoint,const auto&)->std::optional<int> {
                ++calls;
                const auto current=++active;
                auto old=maximum.load();while(old<current && !maximum.compare_exchange_weak(old,current)){}
                std::this_thread::sleep_for(std::chrono::milliseconds(2));--active;
                if(endpoint.find("msk")!=std::string::npos)return {};
                return endpoint.find("nsk")!=std::string::npos ? 40:80;
            });
        const auto computer=fog::mapMtsComputer(nlohmann::json{{"id","1"},{"ping",{{"turn-nsk1_remoteplay_mts_ru",20},{"turn-msk1_remoteplay_mts_ru",1},{"turn-spb1_remoteplay_mts_ru",4}}}});
        require(!service.estimate(computer),"unmeasured is unknown");
        service.request();wait([&]{return !service.busy();},"samples complete");
        require(service.estimate(computer)==60,"best matching measured route plus PC segment");
        require(calls==9 && maximum<=3,"deduplicate nodes and bounded concurrency three attempts");
        service.request();wait([&]{return !service.busy();},"cached refresh complete");
        require(calls==9,"fresh samples reused");
        require(!service.estimate(fog::mapMtsComputer(nlohmann::json{{"id",1},{"ping",{{"turn-nsk1_remoteplay_mts_ru",32767}}}})),"unreachable sentinel never treated as zero");
        for(const auto& malformed:{nlohmann::json(),nlohmann::json{{"ping",nullptr}},nlohmann::json{{"ping",{{"turn-nsk1_remoteplay_mts_ru",-1}}}},nlohmann::json{{"ping",{{"turn-nsk1_remoteplay_mts_ru","2"}}}},nlohmann::json{{"ping",{{"turn-nsk1_remoteplay_mts_ru",18446744073709551615ull}}}}})
            require(!service.estimate(fog::mapMtsComputer(nlohmann::json{{"id",1},{"ping",malformed.is_object() && malformed.contains("ping") ? malformed.at("ping") : nlohmann::json{}}})),"invalid published metric unknown");
        service.stop();service.request();require(!service.busy(),"stopped cannot restart");

        std::atomic_int entered{0};
        std::atomic_bool delay{true};
        LatencyService stale([](const auto&){return std::vector<std::string>{"turn-nsk1.remoteplay.mts.ru:443"};},
            [&](const auto&,const auto& cancelled)->std::optional<int>{const auto index=++entered;while(delay.load() && !cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(1));return index==1?1:100;});
        stale.request();wait([&]{return entered.load()>0;},"cancel fixture active");
        stale.cancel();delay=false;stale.request();wait([&]{return !stale.busy();},"replacement job completes");
        require(stale.estimate(computer)==120,"cancelled generation cannot publish stale fast estimate");
        std::atomic_int medianAttempts{0};
        LatencyService median([](const auto&){return std::vector<std::string>{"turn-nsk1.remoteplay.mts.ru:443"};},
            [&](const auto&,const auto&)->std::optional<int>{constexpr int samples[]={200,10,30};return samples[medianAttempts++];});
        median.request();wait([&]{return !median.busy();},"median fixture complete");
        require(median.estimate(computer)==50,"median rejects cold-connect outlier");
        LatencyService fail([](const auto&)->std::vector<std::string>{throw std::runtime_error("fixture");},{});
        fail.request();wait([&]{return !fail.busy();},"loader failure contained");require(!fail.estimate(computer),"failure stays unknown");
        std::cout<<"Latency tests PASS ("<<assertions<<" assertions)\n";
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
