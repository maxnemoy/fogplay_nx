#include "core/logging/run_log.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <type_traits>
#include <unistd.h>

namespace {
using Log=fog::RunLog;
static_assert(!std::is_copy_constructible_v<Log::DeferredSink>);
static_assert(!std::is_copy_assignable_v<Log::DeferredSink>);
static_assert(!std::is_move_constructible_v<Log::DeferredSink>);
static_assert(!std::is_move_assignable_v<Log::DeferredSink>);
struct Directory {
    std::string path;
    Directory(){char name[]="/tmp/fog-runlog-batches-XXXXXX";const auto value=mkdtemp(name);assert(value);path=value;}
    ~Directory(){std::filesystem::remove_all(path);}
};
std::string read(const std::string& path){std::ifstream input(path);return {std::istreambuf_iterator<char>(input),{}};}
std::mutex gateMutex;std::condition_variable changed;bool entered=false,released=false;
std::mutex ioMutex;std::condition_variable ioChanged;bool ioBlocked=false,ioEntered=false;
void releaseIo(){std::lock_guard lock(ioMutex);ioBlocked=false;ioChanged.notify_all();}
struct ReleaseIo {~ReleaseIo(){releaseIo();}};
void release(){std::lock_guard lock(gateMutex);released=true;changed.notify_all();}
struct Release {~Release(){release();}};
class Gate final:public Log::DeferredRecord {
public:
    Gate(std::size_t lines=1,std::size_t output=64,std::size_t raw=sizeof(void*)*4):lines_(lines),output_(output),raw_(raw){}
    std::size_t rawBytes()const noexcept override{return raw_;}
    std::size_t maxOutputBytes()const noexcept override{return output_;}
    std::size_t lineCount()const noexcept override{return lines_;}
    void format(Log::DeferredSink& sink)const override{
        {std::unique_lock lock(gateMutex);entered=true;changed.notify_all();changed.wait(lock,[]{return released;});}
        for(std::size_t line=0;line<lines_;++line){if(line)sink.nextLine();sink.append("gate");}
    }
private:const std::size_t lines_,output_,raw_;
};
void block(Log& log,std::size_t lines=1,std::size_t output=64,std::size_t raw=32){
    {std::lock_guard lock(gateMutex);entered=released=false;}
    assert(log.appendDeferred(std::make_unique<Gate>(lines,output,raw)));log.requestFlush();
    std::unique_lock lock(gateMutex);assert(changed.wait_for(lock,std::chrono::seconds(2),[]{return entered;}));
}
struct Capture {std::uint64_t capturedAt=0,value=0;};
class Snapshot final:public Log::DeferredRecord {
public:
    explicit Snapshot(Capture value):value_(value){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return 512;}
    std::size_t lineCount()const noexcept override{return 3;}
    void format(Log::DeferredSink& sink)const override{
        for(unsigned line=0;line<3;++line){
            if(line)sink.nextLine();
            sink.append("["+std::to_string(value_.capturedAt)+" ms] final group="+std::to_string(line)+" value="+std::to_string(value_.value));
        }
    }
private:const Capture value_;
};
class Sized final:public Log::DeferredRecord {
public:
    Sized(std::size_t lines,std::size_t width,std::size_t output,std::size_t lastWidth=0,std::size_t raw=64)
        :lines_(lines),width_(width),output_(output),lastWidth_(lastWidth?lastWidth:width),raw_(raw){}
    std::size_t rawBytes()const noexcept override{return raw_;}
    std::size_t maxOutputBytes()const noexcept override{return output_;}
    std::size_t lineCount()const noexcept override{return lines_;}
    void format(Log::DeferredSink& sink)const override{
        std::array<char,8193> bytes{};bytes.fill('x');
        for(std::size_t line=0;line<lines_;++line){if(line)sink.nextLine();sink.append({bytes.data(),line+1==lines_?lastWidth_:width_});}
    }
private:const std::size_t lines_,width_,output_,lastWidth_,raw_;
};
enum class Fault {ExceptionSecond,EmbeddedLf,EmbeddedCr,LongLine,TooFew,TooMany,Total,Swallowed};
class Bad final:public Log::DeferredRecord {
public:
    explicit Bad(Fault fault):fault_(fault){}
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{return fault_==Fault::LongLine?20000:fault_==Fault::Total?8:128;}
    std::size_t lineCount()const noexcept override{return 2;}
    void format(Log::DeferredSink& sink)const override{
        sink.append(fault_==Fault::Total?"1234":"FIRST-CANARY");
        if(fault_==Fault::TooFew)return;
        sink.nextLine();
        if(fault_==Fault::ExceptionSecond)throw std::runtime_error("SECRET-CANARY");
        if(fault_==Fault::EmbeddedLf||fault_==Fault::Swallowed){
            try{sink.append("a\nb");}catch(...){if(fault_!=Fault::Swallowed)throw;}
        }else if(fault_==Fault::EmbeddedCr)sink.append("a\rb");
        else if(fault_==Fault::LongLine){std::array<char,8193> bytes{};sink.append({bytes.data(),bytes.size()});}
        else sink.append("1234");
        if(fault_==Fault::TooMany)sink.nextLine();
    }
private:const Fault fault_;
};
std::atomic<unsigned> countQueries{0},outputQueries{0};
std::atomic<unsigned> commits{0};
bool commit(){++commits;return true;}
class FrozenDeclaration final:public Log::DeferredRecord {
public:
    std::size_t rawBytes()const noexcept override{return sizeof(*this);}
    std::size_t maxOutputBytes()const noexcept override{++outputQueries;return 3;}
    std::size_t lineCount()const noexcept override{return ++countQueries==1?2:3;}
    void format(Log::DeferredSink& sink)const override{sink.append("a");sink.nextLine();sink.append("b");}
};
void ownedFifoAndFlush(){
    Directory directory;Log log;assert(log.start(directory.path,123,commit));Release guard;block(log);
    assert(log.append("prior-controller"));
    {Capture owner{17,111};assert(log.appendDeferred(std::make_unique<Snapshot>(owner)));owner.value=999;}
    assert(log.append("next-controller"));
    const auto beforeCommit=commits.load();auto flush=std::async(std::launch::async,[&]{return log.flush();});
    assert(flush.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
    assert(commits==beforeCommit);release();assert(flush.get());assert(commits>beforeCommit);log.close();
    const auto text=read(directory.path+"/last-run.log");
    assert(text=="gate\nprior-controller\n[17 ms] final group=0 value=111\n[17 ms] final group=1 value=111\n[17 ms] final group=2 value=111\nnext-controller\n");
    assert(read(log.archivePath())==text);const auto stats=log.stats();
    assert(stats.acceptedLines==6&&stats.writtenLines==6&&stats.acceptedDeferred==2&&!stats.droppedLines);
    assert(!stats.residentBytes&&!stats.deferredRecords&&!stats.queuedLines);
}
void exactBounds(){
    Directory directory;Log log;assert(log.start(directory.path));
    assert(log.appendDeferred(std::make_unique<Sized>(1,8192,8192)));assert(log.flush());
    assert(log.appendDeferred(std::make_unique<Sized>(2,8192,16385,0,Log::maxDeferredRawBytes)));assert(log.flush());
    assert(log.appendDeferred(std::make_unique<Sized>(8,8192,65536,8185)));assert(log.flush());
    assert(log.appendDeferred(std::make_unique<Sized>(128,0,127)));assert(log.flush());
    countQueries=outputQueries=0;assert(log.appendDeferred(std::make_unique<FrozenDeclaration>()));assert(log.flush());
    assert(countQueries==1&&outputQueries==1);
    const auto before=log.stats();assert(before.writtenLines==141);
    // Invalid declarations have no trustworthy declared count and count one
    // rejected request. Otherwise rejection charges every declared line.
    assert(!log.appendDeferred(std::make_unique<Sized>(0,1,1)));
    assert(!log.appendDeferred(std::make_unique<Sized>(129,1,257)));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,65537)));
    assert(!log.appendDeferred(std::make_unique<Sized>(1,1,8193)));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3,0,16385)));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,0)));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3,0,0)));
    assert(!log.flush());log.close();const auto stats=log.stats();
    assert(stats.droppedLines==11&&stats.droppedDeferred==7&&!stats.formatFailures);
    assert(stats.rejected.invalidOrProducerLoss==2&&stats.rejected.raw==2&&stats.rejected.rendered==3);
    const auto text=read(directory.path+"/last-run.log");
    assert(text.find(std::string(8192,'x')+'\n')==0);
    assert(text.find("a\nb\n")!=std::string::npos);
}
void formatFailures(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log);
    assert(log.append("prefix"));
    assert(log.appendDeferred(std::make_unique<Bad>(Fault::ExceptionSecond)));
    assert(log.append("suffix"));release();assert(!log.flush());
    for(const auto fault:{Fault::EmbeddedLf,Fault::EmbeddedCr,Fault::LongLine,Fault::TooFew,Fault::TooMany,Fault::Total,Fault::Swallowed}){
        assert(log.appendDeferred(std::make_unique<Bad>(fault)));assert(!log.flush());
    }
    assert(log.appendDeferred(std::make_unique<Snapshot>(Capture{31,777})));assert(!log.flush());
    log.close();const auto text=read(directory.path+"/last-run.log");
    assert(text=="gate\nprefix\n[RunLog] deferred-format failure\nsuffix\n"+
        std::string("[RunLog] deferred-format failure\n")+"[RunLog] deferred-format failure\n"+
        "[RunLog] deferred-format failure\n[RunLog] deferred-format failure\n[RunLog] deferred-format failure\n"+
        "[RunLog] deferred-format failure\n[RunLog] deferred-format failure\n"+
        "[31 ms] final group=0 value=777\n[31 ms] final group=1 value=777\n[31 ms] final group=2 value=777\n");
    const auto stats=log.stats();assert(stats.formatFailures==8&&stats.droppedDeferred==8&&stats.droppedLines==16);
    assert(stats.writtenLines==6&&stats.acceptedLines==22&&!stats.residentBytes&&!stats.deferredRecords);
}
void recordBudget(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log);
    for(unsigned i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<Sized>(2,1,3)));
    assert(log.stats().deferredRecords==4&&log.stats().queuedLines==6);
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3)));
    release();assert(!log.flush());log.close();
    assert(log.stats().writtenLines==7&&log.stats().droppedLines==2&&log.stats().droppedDeferred==1);
    assert(log.stats().rejected.records==1);
    assert(read(directory.path+"/last-run.log").find("rejectedRequestsRecords=1")!=std::string::npos);
}
void terminalRecordReserve(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log);
    for(unsigned i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<Snapshot>(Capture{61,i})));
    assert(log.stats().deferredRecords==Log::maxDeferredRecords);
    // Repeat sessions cannot reset this reserve. All three terminal entries remain
    // charged while the first (ordinary) entry is blocked in the writer.
    for(unsigned i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<Snapshot>(Capture{62,i}),Log::Admission::Terminal));
    assert(log.stats().deferredRecords==Log::maxTerminalDeferredRecords);
    assert(!log.appendDeferred(std::make_unique<Snapshot>(Capture{63,99})));
    assert(!log.appendDeferred(std::make_unique<Snapshot>(Capture{64,99}),Log::Admission::Terminal));
    assert(log.stats().rejected.records==2&&log.stats().droppedDeferred==2&&log.stats().droppedLines==6);
    release();assert(!log.flush());log.close();
    const auto text=read(directory.path+"/last-run.log");
    assert(text.find("[62 ms] final group=2 value=2")!=std::string::npos);
    assert(text.find("[63 ms]")==std::string::npos&&text.find("[64 ms]")==std::string::npos);
    assert(!log.stats().deferredRecords&&!log.stats().residentBytes);
}
void terminalByteReserve(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log,8,65536,16384);
    for(unsigned i=0;i<2;++i)assert(log.appendDeferred(std::make_unique<Sized>(8,1,65536,0,16384)));
    assert(log.stats().residentBytes==245763);
    for(unsigned i=0;i<2;++i)assert(log.appendDeferred(std::make_unique<Sized>(8,1,65536,0,16384),Log::Admission::Terminal));
    assert(log.stats().residentBytes==409605&&log.stats().deferredRecords==5);
    // Ordinary prepared lines also use TOTAL residency. This catches unsigned
    // subtraction wrap once terminal admission goes beyond the regular cap.
    assert(!log.append("ordinary-cannot-borrow"));
    assert(!log.appendDeferred(std::make_unique<Sized>(8,1,65536,0,16384),Log::Admission::Terminal));
    const auto remaining=Log::maxTerminalQueuedBytes-log.stats().residentBytes;
    assert(log.appendDeferred(std::make_unique<Sized>(8,1,remaining-64-1,0,64),Log::Admission::Terminal));
    assert(log.stats().residentBytes==Log::maxTerminalQueuedBytes&&log.stats().deferredRecords==6);
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3),Log::Admission::Terminal));
    assert(log.stats().rejected.bytes==3&&log.stats().droppedDeferred==2&&log.stats().droppedLines==11);
    release();assert(!log.flush());log.close();
    assert(log.stats().highWaterBytes==Log::maxTerminalQueuedBytes&&!log.stats().residentBytes);
}
void terminalDeclarationsStillBounded(){
    Directory directory;Log log;assert(log.start(directory.path));
    assert(!log.appendDeferred({},Log::Admission::Terminal));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3,0,16385),Log::Admission::Terminal));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,65537),Log::Admission::Terminal));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3),static_cast<Log::Admission>(99)));
    assert(log.stats().rejected.invalidOrProducerLoss==2&&log.stats().rejected.raw==1&&log.stats().rejected.rendered==1);
    assert(log.stats().droppedDeferred==4&&log.stats().droppedLines==7);
    assert(!log.flush());log.close();
    const auto text=read(directory.path+"/last-run.log");
    assert(text.find("rejectedRequestsInvalidOrProducerLoss=")!=std::string::npos);
    assert(text.find("rejectedRequestsInvalid=")==std::string::npos);
}
void lineBudget(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log,128,1024);
    for(unsigned i=0;i<384;++i)assert(log.append("ordinary"));
    assert(log.stats().queuedLines==384&&log.stats().deferredRecords==1);
    assert(!log.append("over-resident-lines"));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3)));
    assert(!log.appendDeferred(std::make_unique<Sized>(2,1,3),Log::Admission::Terminal));
    release();assert(!log.flush());log.close();
    assert(log.stats().rejected.lines==3);
    assert(log.stats().acceptedLines==512&&log.stats().writtenLines==512&&log.stats().droppedLines==5);
}
void byteBudget(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log,8,65536,16384);
    for(unsigned i=0;i<2;++i)assert(log.appendDeferred(std::make_unique<Sized>(8,1,65536,0,16384)));
    assert(log.stats().residentBytes==3*(16384+65536+1)&&log.stats().deferredRecords==3);
    assert(!log.appendDeferred(std::make_unique<Sized>(8,1,65536,0,16384)));
    release();assert(!log.flush());log.close();assert(log.stats().writtenLines==24&&log.stats().droppedLines==8);
}
void writingRemainsResident(){
    Directory directory;Log log;assert(log.start(directory.path));ReleaseIo guard;
    {std::lock_guard lock(ioMutex);ioBlocked=true;ioEntered=false;}
    assert(log.appendDeferred(std::make_unique<Snapshot>(Capture{55,888})));log.requestFlush();
    {std::unique_lock lock(ioMutex);assert(ioChanged.wait_for(lock,std::chrono::seconds(2),[]{return ioEntered;}));}
    const auto charge=sizeof(Snapshot)+512+1;
    assert(log.stats().deferredRecords==1&&!log.stats().queuedLines&&log.stats().residentBytes==charge);
    for(unsigned i=0;i<3;++i)assert(log.appendDeferred(std::make_unique<Snapshot>(Capture{55,i})));
    assert(log.stats().deferredRecords==4&&log.stats().queuedLines==9&&log.stats().residentBytes==4*charge);
    assert(!log.appendDeferred(std::make_unique<Snapshot>(Capture{55,999})));
    releaseIo();assert(!log.flush());log.close();
    assert(log.stats().writtenLines==12&&log.stats().droppedLines==3&&!log.stats().residentBytes&&!log.stats().deferredRecords);
}
void closeBarrier(){
    Directory directory;Log log;assert(log.start(directory.path));Release guard;block(log);
    assert(log.appendDeferred(std::make_unique<Snapshot>(Capture{23,456})));assert(log.append("profile-next"));
    auto closing=std::async(std::launch::async,[&]{log.close();});
    assert(closing.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
    release();closing.get();const auto text=read(directory.path+"/last-run.log");
    assert(text.find("[23 ms] final group=2 value=456\nprofile-next\n")!=std::string::npos);
    assert(log.stats().writtenLines==5&&!log.stats().residentBytes&&!log.stats().deferredRecords);
}
}
extern "C" ssize_t __real_write(int,const void*,std::size_t);
extern "C" ssize_t __wrap_write(int fd,const void* bytes,std::size_t count){
    {std::unique_lock lock(ioMutex);if(ioBlocked){ioEntered=true;ioChanged.notify_all();ioChanged.wait(lock,[]{return !ioBlocked;});}}
    return __real_write(fd,bytes,count);
}
int main(){
    ownedFifoAndFlush();exactBounds();formatFailures();recordBudget();terminalRecordReserve();terminalByteReserve();terminalDeclarationsStillBounded();lineBudget();byteBudget();writingRemainsResident();closeBarrier();
    std::cout<<"RunLog immutable multiline batches: FIFO/capture/flush/close, exact per-line/total/count limits, frozen declarations, atomic rollback and resident ordinary/terminal byte/line/record caps and rejection reasons PASS\n";
}
