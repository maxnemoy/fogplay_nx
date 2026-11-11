#include "core/logging/run_log.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <new>
#include <unistd.h>

namespace {
thread_local unsigned failAllocations=0;
thread_local std::size_t failSizeOnce=0,lastAllocation=0;
thread_local bool calibrating=false;
std::atomic<unsigned> failures{0};
}
void* operator new(std::size_t bytes){
    if(calibrating)lastAllocation=bytes;
    if(failAllocations){--failAllocations;++failures;throw std::bad_alloc();}
    if(failSizeOnce&&bytes==failSizeOnce){failSizeOnce=0;++failures;throw std::bad_alloc();}
    if(auto* value=std::malloc(bytes?bytes:1))return value;
    throw std::bad_alloc();
}
void operator delete(void* value)noexcept{std::free(value);}
void operator delete(void* value,std::size_t)noexcept{std::free(value);}
namespace {
using Log=fog::RunLog;
struct Directory{
    std::string path;
    Directory(){char name[]="/tmp/fog-runlog-batch-oom-XXXXXX";const auto value=mkdtemp(name);assert(value);path=value;}
    ~Directory(){std::filesystem::remove_all(path);}
};
std::string read(const std::string& path){std::ifstream input(path);return {std::istreambuf_iterator<char>(input),{}};}
std::mutex gateMutex;std::condition_variable changed;bool entered=false,released=false;
class Gate final:public Log::DeferredRecord{
public:
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 8;}
    void format(Log::DeferredSink& sink)const override{
        std::unique_lock lock(gateMutex);entered=true;changed.notify_all();changed.wait(lock,[]{return released;});sink.append("gate");
    }
};
void block(Log& log){
    {std::lock_guard lock(gateMutex);entered=released=false;}
    assert(log.appendDeferred(std::make_unique<Gate>()));log.requestFlush();
    std::unique_lock lock(gateMutex);assert(changed.wait_for(lock,std::chrono::seconds(2),[]{return entered;}));
}
void release(){std::lock_guard lock(gateMutex);released=true;changed.notify_all();}
struct Release{~Release(){release();}};
class SecondLineOom final:public Log::DeferredRecord{
public:
    explicit SecondLineOom(unsigned count):count_(count){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 1024;}
    std::size_t lineCount()const noexcept override{return 3;}
    void format(Log::DeferredSink& sink)const override{
        sink.append("FIRST-LINE-CANARY");sink.nextLine();sink.append("SECOND-PARTIAL-CANARY");
        failAllocations=count_;
        const std::string allocation(700,'Q');sink.append(allocation);
        sink.nextLine();sink.append("never");
    }
private:const unsigned count_;
};
class NewlineOom final:public Log::DeferredRecord{
public:
    NewlineOom(std::size_t length,std::size_t allocation):length_(length),allocation_(allocation){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return length_;}
    std::size_t lineCount()const noexcept override{return 2;}
    void format(Log::DeferredSink& sink)const override{
        std::array<char,1024> bytes{};bytes.fill('Z');sink.append({bytes.data(),length_-2});sink.nextLine();sink.append("Z");
        failSizeOnce=allocation_;
    }
private:const std::size_t length_,allocation_;
};
std::pair<std::size_t,std::size_t> newlineBoundary(const std::string& prefix){
    const std::array<char,1024> bytes{};
    for(std::size_t length=32;length<bytes.size();++length){
        std::string batch;batch+=prefix;batch.append(bytes.data(),length);
        if(batch.size()!=batch.capacity())continue;
        lastAllocation=0;calibrating=true;batch+='\n';calibrating=false;
        if(lastAllocation)return {length,lastAllocation};
    }
    assert(false&&"no newline allocation boundary");return {};
}
void secondLine(bool prefix,unsigned injected){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log);
    if(prefix)assert(log.append("prefix-before-failure"));
    assert(log.appendDeferred(std::make_unique<SecondLineOom>(injected)));assert(log.append("suffix-after-failure"));
    const auto before=failures.load();release();assert(!log.flush());const auto stats=log.stats();
    assert(log.archiveHealthy()&&log.mirrorHealthy());log.close();
    const std::string expected=std::string("gate\n")+(prefix?"prefix-before-failure\n":"")+"[RunLog] deferred-format failure\nsuffix-after-failure\n";
    assert(read(directory.path+"/last-run.log")==expected&&read(log.archivePath())==expected);
    assert(failures-before==injected);
    assert(stats.droppedLines==3&&stats.formatFailures==1&&stats.droppedDeferred==1);
    assert(stats.acceptedLines==unsigned(prefix?6:5)&&stats.writtenLines==unsigned(prefix?3:2));
    assert(!stats.residentBytes&&!stats.deferredRecords);
}
void finalNewline(bool prefix){
    const std::string prior=prefix?"prefix-before-failure\n":"";const auto boundary=newlineBoundary(prior);
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log);
    if(prefix)assert(log.append("prefix-before-failure"));
    assert(log.appendDeferred(std::make_unique<NewlineOom>(boundary.first,boundary.second)));assert(log.append("suffix-after-failure"));
    const auto before=failures.load();release();assert(!log.flush());const auto stats=log.stats();
    assert(log.archiveHealthy()&&log.mirrorHealthy());log.close();
    assert(read(directory.path+"/last-run.log")=="gate\n"+prior+"[RunLog] deferred-format failure\nsuffix-after-failure\n");
    assert(failures-before==1&&stats.droppedLines==2&&stats.formatFailures==1&&stats.droppedDeferred==1);
    assert(stats.writtenLines==unsigned(prefix?3:2)&&!stats.residentBytes&&!stats.deferredRecords);
}
}
int main(){
    std::set_terminate([]{constexpr char message[]="unexpected terminate during batch OOM injection\n";::write(2,message,sizeof(message)-1);std::_Exit(73);});
    for(const bool prefix:{false,true}){secondLine(prefix,1);secondLine(prefix,2);finalNewline(prefix);}
    std::cout<<"RunLog real allocation failures: second multiline line, failed failure-notice allocation, final newline; empty/existing FIFO prefixes preserved, whole declared record dropped PASS\n";
}
