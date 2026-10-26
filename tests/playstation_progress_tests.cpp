#include "features/playstation/infrastructure/catalog.hpp"
#include "features/playstation/infrastructure/availability.hpp"
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
using fog::Json;
std::string card(int id,const std::string& name) {
    return "<div class='card-outside computer-"+std::to_string(id)+"'><p class='card__title'>"+name+
        "</p><div class='card__price'>35 ₽/час</div><a class='js-play' href='/platform/comp/"+
        std::to_string(id)+"/?tariff_id=50001'>play</a></div>";
}
int main() {
    const auto initial=fog::playStationComputerPlaceholders();
    assert(initial.size()==7);
    for(const auto& computer:initial) assert(computer["availability"]=="unknown" && !computer["launchable"].get<bool>() && computer["price"].is_null());
    std::vector<Json> snapshots;
    int merchantCalls=0;
    const auto result=fog::fetchPlayStationComputers([&](std::string_view slug) {
        ++merchantCalls;
        if(merchantCalls==2) assert(!snapshots.empty() && snapshots.back().size()==7 && snapshots.back()[0]["launchable"]==true && snapshots.back()[3]["launchable"]==false);
        return slug=="MamkinZabiyaka"?card(11573,"3A7X47"):slug=="Karahun"?card(13025,"F4BX8"):card(7705,"61EFX15");
    },[&](const Json& value){snapshots.push_back(value);});
    assert(merchantCalls==3 && snapshots.size()==3 && result[4]["launchable"]==true);
    int promoCalls=0;
    snapshots.clear();
    fog::loadPlayStationPromoPages("CrazyGames",[&](int page) {
        ++promoCalls;
        if(page==2) assert(snapshots.size()==1 && snapshots[0][0]["launchable"]==true && snapshots[0][1]["launchable"]==false);
        return page==1?card(7705,"61EFX15")+"<a class='pagination__next' href='?page=2'>next</a>":card(5632,"61EFX4")+card(5510,"61EFX3");
    },[&](const Json& value){snapshots.push_back(value);});
    assert(promoCalls==2 && snapshots.size()==2 && snapshots[1][2]["launchable"]==true);
    auto computers=initial;
    snapshots.clear();
    fog::updatePlayStationAvailability(computers,[&](int page,int) {
        if(page==2) assert(snapshots.size()==1 && snapshots[0][0]["availability"]=="online" && snapshots[0][1]["availability"]=="unknown");
        return page==1?Json{{"computers",{{{"id",11573}},{{"id",42}}}}}:Json{{"computers",{{{"id",13025}}}}};
    },2,50,[&](const Json& value){snapshots.push_back(value);});
    assert(computers[0]["availability"]=="online" && computers[3]["availability"]=="online" && computers[6]["availability"]=="offline");
    assert(snapshots[1][6]["availability"]=="unknown" && snapshots.back()[6]["availability"]=="offline");
    snapshots.clear();
    fog::updatePlayStationAvailability(computers,[](int page,int)->Json {
        if(page==2) throw std::runtime_error("synthetic outage");
        return {{"computers",{{{"id",11573}},{{"id",42}}}}};
    },2,50,[&](const Json& value){snapshots.push_back(value);});
    assert(snapshots.front()[6]["availability"]=="unknown" && snapshots.back()[0]["availability"]=="online" && snapshots.back()[6]["availability"]=="unknown");
    bool cancelled=false,threw=false;
    merchantCalls=0;
    try {
        fog::fetchPlayStationComputers([&](std::string_view){++merchantCalls;return card(11573,"3A7X47");},
            [&](const Json&){cancelled=true;},[&]{return cancelled;});
    } catch(const std::runtime_error&) {threw=true;}
    assert(threw && merchantCalls==1);
    cancelled=false; threw=false; int statusCalls=0;
    try {
        fog::updatePlayStationAvailability(computers,[&](int,int){++statusCalls;return Json{{"computers",{{{"id",11573}},{{"id",42}}}}};},
            2,50,[&](const Json& value){assert(value[6]["availability"]=="unknown");cancelled=true;},[&]{return cancelled;});
    } catch(const std::runtime_error&) {threw=true;}
    assert(threw && statusCalls==1);
    std::size_t selected=6;
    computers=initial;
    auto reordered=initial;
    std::swap(reordered[0],reordered[6]);
    fog::applyPlayStationCatalogSnapshot(computers,selected,reordered);
    assert(selected==0 && computers[selected]["id"]==5510);
    fog::applyPlayStationCatalogSnapshot(computers,selected,Json::array()); assert(selected==0);
    fog::PlayStationCatalogProgressChannel latest;
    for(int i=0;i<100;++i) latest.publish(Json{{"sequence",i}});
    assert(latest.take()->at("sequence")==99 && !latest.take());
    constexpr int iterations=2000;
    std::atomic_bool done{false};
    auto channel=std::make_shared<fog::PlayStationCatalogProgressChannel>();
    auto producer=std::async(std::launch::async,[channel,&done] {
        for(int i=1;i<=iterations;++i) channel->publish(Json{{"sequence",i}});
        done.store(true,std::memory_order_release);
    });
    int previous=0;
    while(!done.load(std::memory_order_acquire)) {
        if(auto snapshot=channel->take()) {const int next=snapshot->at("sequence"); assert(next>previous); previous=next;}
        std::this_thread::yield();
    }
    producer.get();
    if(auto snapshot=channel->take()) {assert(snapshot->at("sequence")==iterations);previous=iterations;}
    assert(previous==iterations && !channel->take());
    channel->publish(initial); channel->cancel(); channel->publish(result);
    assert(channel->cancelled() && !channel->take());
    std::weak_ptr<fog::PlayStationCatalogProgressChannel> weak=channel;
    std::function<void()> lateWriter=[channel,result] {channel->publish(result);};
    channel.reset(); assert(!weak.expired()); lateWriter(); lateWriter={}; assert(weak.expired());
    std::cout<<"PlayStation progressive merchant/page delivery, terminal-only offline, failure/cancellation, ID selection and concurrent bounded channel PASS\n";
}
