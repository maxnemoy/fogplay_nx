#include "features/catalog/infrastructure/recent_games.hpp"
#include "features/catalog/presentation/home_catalog.hpp"
#include <nlohmann/json.hpp>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using Json = nlohmann::json;
namespace {
bool failInstall = false, failFlush = false;
unsigned failFlushAt = 0;
std::string installPath;
std::string artwork(int id) { return "https://fogplay.mts.ru/image/preview/game-"+std::to_string(id)+".webp"; }
fog::RecentGame game(int id) { return {id,"Игра "+std::to_string(id),artwork(id)}; }
Json page(int first=1,int count=16) {
    auto result=Json::array();
    for(int i=first;i<first+count;++i) result.push_back(game(i).catalogItem());
    return result;
}
void write(const std::string& path,const std::string& bytes) { std::ofstream(path,std::ios::binary)<<bytes; }
std::string read(const std::string& path) { std::ifstream input(path); return {(std::istreambuf_iterator<char>(input)),{}}; }
}
extern "C" int __real_rename(const char*,const char*);
extern "C" int __wrap_rename(const char* from,const char* to) {
    if(failInstall && from==installPath+".tmp" && to==installPath) { errno=EIO; return -1; }
    struct stat info{};
    if(lstat(to,&info)==0) { errno=EEXIST; return -1; }
    return __real_rename(from,to);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if(failFlush || (failFlushAt && --failFlushAt==0)) { failFlush=false; errno=EIO; return -1; }
    return __real_fsync(fd);
}
int main() {
    const auto dir=std::filesystem::temp_directory_path()/("fog-recent-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto path=(dir/"recent-games.json").string();
    {
        const auto snapshotPath=(dir/"snapshot.json").string();
        fog::RecentGames live(snapshotPath);
        assert(live.update(game(1)) && !std::filesystem::exists(snapshotPath));
        auto snapshot=live;
        assert(live.update(game(2)) && snapshot.save());
        fog::RecentGames disk(snapshotPath);
        assert(disk.load() && disk.items().front().id==1);
        assert(live.items().front().id==2 && live.save());
        assert(disk.load() && disk.items().front().id==2);
    }
    {
        fog::RecentGames store(path);
        assert(store.load() && store.items().empty() && !std::filesystem::exists(path));
        for(int id=1;id<=6;++id) assert(store.remember(game(id)));
        assert(store.items().size()==5 && store.items().front().id==6 && store.items().back().id==2);
        auto updated=game(4);updated.name="Новое название";
        assert(store.remember(updated) && store.items().front().id==4 && store.items().front().name==updated.name);
        assert(store.items()[1].id==6 && store.items().size()==5);
        fog::RecentGames restarted(path);assert(restarted.load() && restarted.items().size()==5);
        assert(restarted.items().front().name==updated.name && restarted.items().front().artwork==artwork(4));
        const auto saved=Json::parse(read(path));
        assert(saved.size()==2 && saved.at("games").size()==5 && saved.at("games")[0].size()==3);
        assert(!saved.contains("account") && !saved.contains("hash") && !saved.contains("token"));
        assert(!store.remember({-1,"Invalid",""}) && store.items().front().id==4);
        assert(fog::RecentGame::fromCatalog(game(1).catalogItem())->artwork==artwork(1));
        assert(fog::RecentGame::fromCatalog(Json{{"id",1},{"name","No image"},{"images",{"https://evil.test/private-token"}}})->artwork.empty());
        const auto mixed = fog::RecentGame::fromCatalog(Json{{"id",7},{"name","Mixed covers"},
            {"images",{"https://evil.test/private-token",artwork(7),"https://fogplay.mts.ru/image/preview/a.jpg",
                "https://fogplay.mts.ru/image/preview/b.png",artwork(7)}}});
        assert(mixed && mixed->artworkCandidates().size() == 3 && mixed->catalogItem().at("images").size() == 3);
        const auto alternatePath = (dir/"alternates.json").string();
        fog::RecentGames alternateStore(alternatePath); assert(alternateStore.remember(*mixed));
        fog::RecentGames alternateRestart(alternatePath); assert(alternateRestart.load());
        assert(alternateRestart.items()[0].artworkCandidates() == mixed->artworkCandidates());
        for(const auto& bad:std::vector<Json>{Json{{"id",0},{"name","bad"}},Json{{"id",2147483648ULL},{"name","bad"}},
            Json{{"id",1.0},{"name","bad"}},Json{{"id",true},{"name","bad"}},Json{{"id",1},{"name","\n"}},
            Json{{"id",1},{"name",std::string(257,'x')}},Json{{"id",1},{"name","   "}},Json{{"id",1},{"name",7}}})
            assert(!fog::RecentGame::fromCatalog(bad));
        auto invalidUtf=game(1);invalidUtf.name=std::string(1,static_cast<char>(0xff));assert(!store.remember(invalidUtf));
        for(const auto& bytes:std::vector<std::string>{"{","null","[]","{}","",std::string(fog::RecentGames::MaxBytes+1,' ')}) {
            write(path,bytes);assert(!store.load() && store.items().front().id==4 && !store.error().empty());
        }
        std::vector<Json> corrupt;
        auto bad=saved;bad["version"]=2;corrupt.push_back(bad);
        bad=saved;bad["version"]=1.0;corrupt.push_back(bad);
        bad=saved;bad["token"]="secret";corrupt.push_back(bad);
        bad=saved;bad["games"][0]["hash"]="secret";corrupt.push_back(bad);
        bad=saved;bad["games"][0]["id"]=4294967297ULL;corrupt.push_back(bad);
        bad=saved;bad["games"][0]["artwork"]="https://evil.test/token";corrupt.push_back(bad);
        bad=saved;bad["games"].push_back(bad["games"][0]);corrupt.push_back(bad);
        bad=saved;bad["games"][1]=bad["games"][0];corrupt.push_back(bad);
        for(const auto& value:corrupt) {write(path,value.dump());assert(!store.load() && store.items().front().id==4);}
        write(path,saved.dump());assert(store.load());
        // Interrupted replace recovers only authoritative backup when primary is missing.
        std::filesystem::rename(path,path+".bak");write(path+".tmp","{private-staged}");
        assert(restarted.load() && restarted.items().front().id==4 && !std::filesystem::exists(path+".bak"));
        // A failed install retains the old durable record; the current run still shows its new recent.
        installPath=path;failInstall=true;
        assert(!store.remember(game(9)) && store.items().front().id==9);
        failInstall=false;
        assert(restarted.load() && restarted.items().front().id==4);
        failFlush=true;
        assert(!store.remember(game(10)) && store.items().front().id==10);
        assert(restarted.load() && restarted.items().front().id==4);
        failFlushAt=3; // Temporary flush, backup-directory commit, installed-primary commit.
        assert(!store.remember(game(12)) && store.items().front().id==12);
        assert(restarted.load() && restarted.items().front().id==12); // Present primary remains authoritative.
        assert(store.remember(game(11)) && store.error().empty());
        assert(restarted.load() && restarted.items().front().id==11);
        std::filesystem::remove(path);std::filesystem::create_directory(path);assert(!restarted.load());
        std::filesystem::remove(path);std::filesystem::create_symlink("missing-target",path);assert(!restarted.load());
        std::filesystem::remove(path);mkfifo(path.c_str(),0600);assert(!restarted.load());std::filesystem::remove(path);
        fog::RecentGames unavailable((dir/"missing-parent"/"recent-games.json").string());
        assert(!unavailable.remember(game(1)) && unavailable.items().size()==1 && !unavailable.error().empty());
    }
    {
        fog::RecentLaunch launch;
        assert(!launch.decoded());
        launch.prepare(game(1));assert(!launch.decoded()); // Click/create is not playback.
        launch.stream(true);assert(launch.decoded()->id==1 && !launch.decoded());
        launch.prepare(game(2));launch.stream(false);assert(!launch.decoded()); // Reconnect cannot inherit catalog selection.
        launch.stream(true);assert(!launch.decoded());
        launch.prepare(game(3));launch.close();launch.stream(true);assert(!launch.decoded()); // Failed/cancelled startup.
        launch.prepare(game(4));launch.prepare(game(5));launch.stream(true);assert(launch.decoded()->id==5);
        launch.prepare(std::nullopt);launch.stream(true);assert(!launch.decoded());
        auto catalogItem=game(6).catalogItem();
        const auto metadata=fog::RecentGame::fromCatalog(catalogItem);
        catalogItem=Json{{"description","replacement detail without catalog artwork"}};
        launch.prepare(metadata);launch.stream(true);
        const auto snapshot=launch.decoded();assert(snapshot && snapshot->id==6 && snapshot->artwork==artwork(6));
    }
    {
        using Direction=fog::Catalog::Direction;
        fog::Catalog catalog;catalog.reset("");auto request=catalog.begin();assert(request);
        assert(catalog.complete(*request,page()) && catalog.items().size()==16);
        fog::HomeCatalogNavigation nav;nav.reset(5,true);
        assert(nav.stripVisible(catalog,5) && nav.recentFocused(catalog,5));
        assert(nav.move(catalog,5,Direction::Left));assert(nav.selected()==0); // Rail/back retains recent focus.
        for(int i=0;i<5;++i) nav.move(catalog,5,Direction::Right);
        assert(nav.selected()==4 && nav.first()==1); // Fifth card scrolls into four-card viewport.
        nav.move(catalog,5,Direction::Down);assert(!nav.recentFocused(catalog,5) && catalog.selected()==3);
        assert(nav.stripVisible(catalog,5));
        nav.move(catalog,5,Direction::Up);assert(nav.recentFocused(catalog,5) && nav.selected()==4 && nav.first()==1);
        nav.move(catalog,5,Direction::Down);nav.move(catalog,5,Direction::Down);
        assert(catalog.selected()==7 && !nav.stripVisible(catalog,5) && !catalog.begin());
        nav.move(catalog,5,Direction::Down);assert(catalog.selected()==11 && catalog.first()==4);
        request=catalog.begin();assert(request && request->page==2 && request->query.empty());
        assert(catalog.complete(*request,page(17)) && catalog.items().size()==32);
        const auto remembered=catalog.selected(), first=catalog.first();
        // Opening/closing detail does not reset either model; asynchronous append also preserves focus.
        assert(catalog.selected()==remembered && catalog.first()==first);
        nav.move(catalog,5,Direction::Up);nav.move(catalog,5,Direction::Up);nav.move(catalog,5,Direction::Up);
        assert(nav.recentFocused(catalog,5) && nav.selected()==4);
        catalog.reset("search");nav.reset(5,false);request=catalog.begin();assert(request && request->page==1);
        assert(catalog.complete(*request,page()));
        assert(!nav.stripVisible(catalog,5) && !nav.recentFocused(catalog,5));
        nav.move(catalog,5,Direction::Up);assert(catalog.selected()==0);
        nav.move(catalog,5,Direction::Right);assert(catalog.selected()==1);
        nav.move(catalog,5,Direction::Down);assert(catalog.selected()==5);
        catalog.reset("");nav.reset(0,true);assert(!nav.stripVisible(catalog,0) && nav.move(catalog,0,Direction::Left));
        nav.reset(5,true);nav.move(catalog,5,Direction::Down);assert(nav.recentFocused(catalog,5)); // Catalog still loading.
    }
    std::filesystem::remove_all(dir);
    std::cout<<"Recent games: bounded validated persistence/restart/recovery/faults, first-frame launch eligibility, strip/grid/search/sidebar/pagination navigation passed\n";
}
