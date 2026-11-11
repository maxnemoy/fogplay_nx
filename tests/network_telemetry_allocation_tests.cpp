#include "features/streaming/application/network_telemetry_logging.hpp"
#include <cassert>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <new>
#include <unistd.h>
namespace {thread_local bool producerFailure=false;thread_local unsigned failures=0;}
void* operator new(std::size_t n){if(producerFailure){++failures;throw std::bad_alloc();}if(auto p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void* p)noexcept{std::free(p);}void operator delete(void* p,std::size_t)noexcept{std::free(p);}
namespace net=stream::telemetry::network;
class EventSource final:public stream::StreamNetworkTelemetrySource{
public:bool pending=true;unsigned uncertain=0;
bool peekNetworkEvent(net::NetworkEvent& out)const noexcept override{if(!pending)return false;out.journalSequence=1;return true;}
bool retireNetworkEvent(std::uint64_t seq,bool admitted)noexcept override{assert(seq==1);pending=false;if(!admitted)++uncertain;return true;}
};
int main(){
 std::set_terminate([]{constexpr char text[]="network helper terminated on allocation failure\n";::write(2,text,sizeof(text)-1);std::_Exit(73);});
 char dir[]="/tmp/fog-network-alloc-XXXXXX";assert(mkdtemp(dir));fog::RunLog log;assert(log.start(dir));net::NetworkSnapshot value;
 failures=0;producerFailure=true;const bool enqueued=net::enqueueNetwork(log,42,net::Line::Video,value);producerFailure=false;
 assert(!enqueued&&failures==1);EventSource source;
 failures=0;producerFailure=true;const auto result=net::drainNetworkEvents(source,log,43,1);producerFailure=false;
 assert(failures==1&&result.uncertain==1&&result.accepted==0&&!source.pending&&source.uncertain==1);
 assert(log.append("after-network-allocation-failure"));assert(!log.flush());log.close();
 assert(log.stats().droppedDeferred==2&&log.stats().acceptedDeferred==0);std::filesystem::remove_all(dir);
 std::cout<<"Actual persistent allocation failures in network snapshot/event helpers preserve accounted uncertainty and continuation\n";
}
