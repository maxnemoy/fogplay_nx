#include "features/playstation/infrastructure/catalog.hpp"
#include "features/playstation/infrastructure/availability.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
using Json=nlohmann::json;
std::string card(int id,const std::string& name) {
    return "<div class='card-outside computer-"+std::to_string(id)+"'><p class='card__title'>"+name+
        "</p><div class='card__price'>35 ₽/час</div><a class='js-play' href='/platform/comp/"+
        std::to_string(id)+"/?tariff_id=50001'>play</a></div>";
}
fog::Computer sequence(int value) { fog::Computer result;result.id={fog::ProviderId::Mts,std::to_string(value)};return result; }
int main() {
    const auto initial=fog::playStationComputerPlaceholders();
    assert(initial.size()==7);
    for(const auto& computer:initial) assert(computer.availability == fog::Availability::Unknown && !computer.launchable && computer.hourlyPrice.has_value()==false);
    std::vector<std::vector<fog::Computer>> snapshots;
    int merchantCalls=0;
    const auto result=fog::fetchPlayStationComputers([&](std::string_view slug) {
        ++merchantCalls;
        if(merchantCalls==2) assert(!snapshots.empty() && snapshots.back().size()==7 && snapshots.back()[0].launchable==true && snapshots.back()[3].launchable==false);
        return slug=="MamkinZabiyaka"?card(11573,"3A7X47"):slug=="Karahun"?card(13025,"F4BX8"):card(7705,"61EFX15");
    },[&](const std::vector<fog::Computer>& value){snapshots.push_back(value);});
    assert(merchantCalls==3 && snapshots.size()==3 && result[4].launchable==true);
    int promoCalls=0;
    snapshots.clear();
    fog::loadPlayStationPromoPages("CrazyGames",[&](int page) {
        ++promoCalls;
        if(page==2) assert(snapshots.size()==1 && snapshots[0][0].launchable==true && snapshots[0][1].launchable==false);
        return page==1?card(7705,"61EFX15")+"<a class='pagination__next' href='?page=2'>next</a>":card(5632,"61EFX4")+card(5510,"61EFX3");
    },[&](const std::vector<fog::Computer>& value){snapshots.push_back(value);});
    assert(promoCalls==2 && snapshots.size()==2 && snapshots[1][2].launchable==true);
    auto computers=initial;
    snapshots.clear();
    fog::updatePlayStationAvailability(computers,[&](int page,int) {
        if(page==2) assert(snapshots.size()==1 && snapshots[0][0].availability == fog::Availability::Online && snapshots[0][1].availability == fog::Availability::Unknown);
        return page==1?Json{{"computers",{{{"id",11573}},{{"id",42}}}}}:Json{{"computers",{{{"id",13025}}}}};
    },2,50,[&](const std::vector<fog::Computer>& value){snapshots.push_back(value);});
    assert(computers[0].availability == fog::Availability::Online && computers[3].availability == fog::Availability::Online && computers[6].availability == fog::Availability::Offline);
    assert(snapshots[1][6].availability == fog::Availability::Unknown && snapshots.back()[6].availability == fog::Availability::Offline);
    snapshots.clear();
    fog::updatePlayStationAvailability(computers,[](int page,int)->Json {
        if(page==2) throw std::runtime_error("synthetic outage");
        return {{"computers",{{{"id",11573}},{{"id",42}}}}};
    },2,50,[&](const std::vector<fog::Computer>& value){snapshots.push_back(value);});
    assert(snapshots.front()[6].availability == fog::Availability::Unknown && snapshots.back()[0].availability == fog::Availability::Online && snapshots.back()[6].availability == fog::Availability::Unknown);
    bool cancelled=false,threw=false;
    merchantCalls=0;
    try {
        fog::fetchPlayStationComputers([&](std::string_view){++merchantCalls;return card(11573,"3A7X47");},
            [&](const std::vector<fog::Computer>&){cancelled=true;},[&]{return cancelled;});
    } catch(const std::runtime_error&) {threw=true;}
    assert(threw && merchantCalls==1);
    cancelled=false; threw=false; int statusCalls=0;
    try {
        fog::updatePlayStationAvailability(computers,[&](int,int){++statusCalls;return Json{{"computers",{{{"id",11573}},{{"id",42}}}}};},
            2,50,[&](const std::vector<fog::Computer>& value){assert(value[6].availability == fog::Availability::Unknown);cancelled=true;},[&]{return cancelled;});
    } catch(const std::runtime_error&) {threw=true;}
    assert(threw && statusCalls==1);
    std::size_t selected=6;
    computers=initial;
    auto reordered=initial;
    std::swap(reordered[0],reordered[6]);
    fog::applyPlayStationCatalogSnapshot(computers,selected,reordered);
    assert(selected==0 && computers[selected].id.value == "5510");
    fog::applyPlayStationCatalogSnapshot(computers,selected,{}); assert(selected==0);
    fog::PlayStationCatalogProgressChannel latest;
    for(int i=0;i<100;++i) latest.publish({sequence(i)});
    assert(latest.take()->at(0).id.value=="99" && !latest.take());
    constexpr int iterations=2000;
    std::atomic_bool done{false};
    auto channel=std::make_shared<fog::PlayStationCatalogProgressChannel>();
    auto producer=std::async(std::launch::async,[channel,&done] {
        for(int i=1;i<=iterations;++i) channel->publish({sequence(i)});
        done.store(true,std::memory_order_release);
    });
    int previous=0;
    while(!done.load(std::memory_order_acquire)) {
        if(auto snapshot=channel->take()) {const int next=std::stoi(snapshot->at(0).id.value); assert(next>previous); previous=next;}
        std::this_thread::yield();
    }
    producer.get();
    if(auto snapshot=channel->take()) {assert(snapshot->at(0).id.value==std::to_string(iterations));previous=iterations;}
    assert(previous==iterations && !channel->take());
    channel->publish(initial); channel->cancel(); channel->publish(result);
    assert(channel->cancelled() && !channel->take());
    std::weak_ptr<fog::PlayStationCatalogProgressChannel> weak=channel;
    std::function<void()> lateWriter=[channel,result] {channel->publish(result);};
    channel.reset(); assert(!weak.expired()); lateWriter(); lateWriter={}; assert(weak.expired());
    std::cout<<"PlayStation progressive merchant/page delivery, terminal-only offline, failure/cancellation, ID selection and concurrent bounded channel PASS\n";
}
