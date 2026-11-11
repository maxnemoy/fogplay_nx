#include "app/stream_telemetry_logging.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <new>
#include <unistd.h>
namespace {
thread_local bool producerFailure=false,calibrateNewline=false;
thread_local unsigned producerFailures=0;
thread_local std::size_t lastAllocationBytes=0,failSizeOnce=0;
std::atomic<unsigned> newlineFailures{0};
}
void* operator new(std::size_t bytes) {
    if(producerFailure){++producerFailures;throw std::bad_alloc();}
    if(calibrateNewline)lastAllocationBytes=bytes;
    if(failSizeOnce&&bytes==failSizeOnce){failSizeOnce=0;++newlineFailures;throw std::bad_alloc();}
    if(void* memory=std::malloc(bytes?bytes:1))return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory)noexcept{std::free(memory);}
void operator delete(void* memory,std::size_t)noexcept{std::free(memory);}
namespace {
struct Directory {
    std::string path;
    Directory(){char name[]="/tmp/fog-telemetry-allocation-XXXXXX";const auto created=mkdtemp(name);assert(created);path=created;}
    ~Directory(){std::filesystem::remove_all(path);}
};
std::string read(const std::string& path){std::ifstream input(path);return {std::istreambuf_iterator<char>(input),{}};}
class Source final:public stream::StreamTelemetrySource{};
void factoryFailure(unsigned kind){
    Directory directory;auto& log=fogplay::diagnostics::runLog;assert(log.start(directory.path,1234));
    Source source;producerFailures=0;producerFailure=true;
    if(kind==0)fogplay::diagnostics::diagnosticTransport(source,stream::telemetry::Line::Transport);
    else if(kind==1)fogplay::diagnostics::diagnosticInput(source);
    else fogplay::diagnostics::diagnosticPlayback({});
    producerFailure=false; // fault persisted for the entire helper, including its catch
    assert(producerFailures==1);
    assert(log.append("after-factory-failure"));assert(!log.flush());
    const auto stats=log.stats();log.close();
    assert(stats.droppedDeferred==1&&stats.droppedLines==1&&stats.acceptedDeferred==0&&stats.formatFailures==0);
    const auto text=read(directory.path+"/last-run.log");
    assert(text.find("after-factory-failure\n")!=std::string::npos);
    assert(text.find("[RunLog] dropped 1 diagnostic lines (enqueue rejection)")!=std::string::npos);
    assert(text.find("telemetry-capture failure")==std::string::npos);
}
std::mutex gateMutex;std::condition_variable changed;bool entered=false,released=false;
class Gate final:public fog::RunLog::DeferredRecord{
public:
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 8;}
    void format(fog::RunLog::DeferredSink& sink)const override{
        std::unique_lock lock(gateMutex);entered=true;changed.notify_all();changed.wait(lock,[]{return released;});sink.append("gate");
    }
};
class NewlineFault final:public fog::RunLog::DeferredRecord{
public:
    NewlineFault(std::size_t length,std::size_t allocation):length_(length),allocation_(allocation){payload_.fill('A');}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return length_;}
    void format(fog::RunLog::DeferredSink& sink)const override{
        sink.append(std::string_view(payload_.data(),length_));failSizeOnce=allocation_;
    }
private:
    std::array<char,1024> payload_{};std::size_t length_,allocation_;
};
// Select the real host string growth boundary, rather than depending on one
// libstdc++ SSO capacity. Arm only after formatter success on the writer thread.
std::pair<std::size_t,std::size_t> newlineBoundary(const std::string& prefix){
    const std::array<char,1024> payload{};
    for(std::size_t length=32;length<payload.size();++length){
        std::string batch;batch+=prefix;
        batch.append(payload.data(),length);
        if(batch.size()!=batch.capacity())continue;
        lastAllocationBytes=0;calibrateNewline=true;batch+='\n';calibrateNewline=false;
        if(lastAllocationBytes)return {length,lastAllocationBytes};
    }
    assert(false&&"no newline growth boundary found");return {};
}
void newlineFailure(bool withPrefix){
    const std::string prefix=withPrefix?"before-format-failure\n":"";
    const auto boundary=newlineBoundary(prefix);Directory directory;fog::RunLog log;assert(log.start(directory.path,1234));
    if(withPrefix){
        {std::lock_guard lock(gateMutex);entered=released=false;}
        assert(log.appendDeferred(std::make_unique<Gate>()));log.requestFlush();
        std::unique_lock lock(gateMutex);assert(changed.wait_for(lock,std::chrono::seconds(2),[]{return entered;}));
    }
    // When the writer is gated, all three entries are guaranteed to enter its
    // next batch together. Existing complete prefix must survive the rollback.
    if(withPrefix)assert(log.append("before-format-failure"));
    assert(log.appendDeferred(std::make_unique<NewlineFault>(boundary.first,boundary.second)));
    if(withPrefix)assert(log.append("after-format-failure"));
    if(withPrefix){std::lock_guard lock(gateMutex);released=true;changed.notify_all();}
    assert(!log.flush());
    if(!withPrefix){assert(log.append("after-format-failure"));assert(!log.flush());}
    const auto stats=log.stats();log.close();const auto text=read(directory.path+"/last-run.log");
    const std::string expected=(withPrefix?"gate\n":"")+prefix+"[RunLog] deferred-format failure\nafter-format-failure\n";
    assert(text==expected);assert(text.find('A')==std::string::npos);
    assert(stats.formatFailures==1&&stats.droppedDeferred==1&&stats.droppedLines==1);
    assert(stats.writtenLines==unsigned(withPrefix?3:1)&&stats.residentBytes==0&&stats.deferredRecords==0);
}
}
int main(){
    std::set_terminate([]{constexpr char message[]="unexpected terminate during injected allocation failure\n";::write(2,message,sizeof(message)-1);std::_Exit(73);});
    for(unsigned kind=0;kind<3;++kind)factoryFailure(kind);
    newlineFailure(false);newlineFailure(true);assert(newlineFailures==2);
    std::cout<<"Actual persistent producer allocation failures (3 helpers) and newline batch rollback (empty/prior lines) passed\n";
}
