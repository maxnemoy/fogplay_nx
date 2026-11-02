"""Exercise direct Moonlight video admission and exact core-handle completion."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/features/streaming/infrastructure/moonlight/moonlight_session.cpp').read_text()


def extract_function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


assert 'std::deque<stream::Packet> video' not in source, 'FH must not retain an intermediate video queue'
for signature in ('std::string videoDiagnostics()', 'std::string gapDiagnostics()'):
    diagnostic = extract_function(signature)
    gated_clock = r'state_->coreDiagnosticsReady\s*\?\s*LiGetMicroseconds\(\)\s*:\s*0'
    assert len(re.findall(gated_clock, diagnostic)) == 1
    assert 'LiGetMicroseconds' not in re.sub(gated_clock, '', diagnostic)

harness = r'''
#include "features/streaming/domain/streaming_client.hpp"
#include "features/streaming/domain/video_limits.hpp"
#include "features/streaming/infrastructure/moonlight/frame_diagnostics.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
constexpr int DR_OK=0, DR_NEED_IDR=-1, FRAME_TYPE_IDR=1, FRAME_TYPE_PFRAME=2;
namespace detail=moonlight::detail;
std::uint64_t fakeClockUs=1000000;
std::uint64_t LiGetMicroseconds(){return fakeClockUs++;}
std::uint64_t nowMs(){return 123456;}
struct BUFFER_ENTRY {BUFFER_ENTRY* next; char* data; int length;};
struct DECODE_UNIT {
    int fullLength,frameType; std::uint32_t rtpTimestamp; BUFFER_ENTRY* bufferList;
    std::uint32_t frameNumber; std::uint64_t receiveTimeUs,enqueueTimeUs; std::uint16_t frameHostProcessingLatency;
};
using PDECODE_UNIT=DECODE_UNIT*;
struct Frame {
    char bytes[2]; BUFFER_ENTRY second,first; DECODE_UNIT unit;
    Frame(unsigned id,bool idr):bytes{char(id),char(id+1)},second{nullptr,&bytes[1],1},first{&second,&bytes[0],1},
        unit{2,idr?FRAME_TYPE_IDR:FRAME_TYPE_PFRAME,id,&first,id,900000+id*100,900050+id*100,7}{}
};
using VIDEO_FRAME_HANDLE=Frame*;
struct State {
    std::atomic<bool> stop{false},failed{false},terminated{false},assemblyWaiting{true},fatalFailure{false};
    std::atomic<int> code{0};
    std::atomic<std::uint64_t> frames{0},videoDrops{0},lastVideoMs{0},videoInvalid{0},videoWaitDrops{0},videoResyncs{0};
    std::mutex mutex,pullMutex; std::condition_variable pullChanged,changed;
    bool pullOpen=true,pullActive=false; std::uint64_t generation=42;
    std::shared_ptr<stream::MediaSignals> mediaSignals=std::make_shared<stream::MediaSignals>();
    detail::FrameDiagnostics trace;
};
struct Core {
    std::deque<std::unique_ptr<Frame>> pending; std::unique_ptr<Frame> inFlight;
    std::vector<std::pair<unsigned,int>> completed; unsigned polls=0,waits=0;
    std::function<void()> onPull;
    void add(unsigned id,bool idr=false,bool malformed=false){
        auto frame=std::make_unique<Frame>(id,idr);if(malformed)frame->unit.fullLength=3;pending.push_back(std::move(frame));
    }
} core;
bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* handle,PDECODE_UNIT* unit){
    ++core.polls;assert(!core.inFlight);if(core.pending.empty())return false;
    core.inFlight=std::move(core.pending.front());core.pending.pop_front();
    *handle=core.inFlight.get();*unit=&core.inFlight->unit;
    if(core.onPull)core.onPull();
    return true;
}
bool LiWaitForNextVideoFrame(VIDEO_FRAME_HANDLE* handle,PDECODE_UNIT* unit){++core.waits;return LiPollNextVideoFrame(handle,unit);}
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle,int result){
    assert(core.inFlight&&core.inFlight.get()==handle);core.completed.emplace_back(handle->unit.rtpTimestamp,result);
    core.inFlight.reset();if(result==DR_NEED_IDR)core.pending.clear();
}
'''
harness += extract_function('bool copyFrame(') + '\n'
harness += extract_function('class CoreFrameCompletion') + ';\n'
harness += extract_function('bool pullVideo(') + '\n'
harness += r'''
void reset(){assert(!core.inFlight);core=Core{};fakeClockUs=1000000;}
bool take(State& state,stream::Packet& packet,bool wait=false){return pullVideo(state,packet,wait,state.mediaSignals->video.sequence());}
int main(){
    // One direct packet per call; no application producer queue or batch delay.
    reset();State fifo;stream::Packet packet;
    for(unsigned i=1;i<=8;++i)core.add(i,i==1);
    for(unsigned i=1;i<=8;++i){
        assert(take(fifo,packet)&&packet.kind==stream::Kind::Video&&packet.timestamp==i);
        assert(packet.videoGeneration==42&&packet.data==std::vector<std::byte>({std::byte(i),std::byte(i+1)}));
        assert(!fifo.pullActive&&!core.inFlight&&core.completed.size()==i&&core.completed.back().second==DR_OK);
    }
    assert(fifo.frames==8&&!fifo.assemblyWaiting&&fifo.videoDrops==0);
    // Rejection invalidates once; dependent frames cannot bypass IDR.
    reset();State damaged;core.add(1,true);assert(take(damaged,packet));
    core.add(2,false,true);assert(!take(damaged,packet));
    assert(damaged.generation==43&&damaged.assemblyWaiting&&damaged.videoResyncs==1&&damaged.videoInvalid==1);
    assert(core.completed.size()==2&&core.completed.back().second==DR_NEED_IDR);
    core.add(3);assert(!take(damaged,packet)&&damaged.videoWaitDrops==1&&damaged.generation==43);
    core.add(4,true);assert(take(damaged,packet)&&packet.videoGeneration==43&&!damaged.assemblyWaiting);
    assert(core.completed.size()==4&&core.completed.back().second==DR_OK&&!core.inFlight);
    // Revisions protect a wake before entry; a closed gate never calls the core.
    reset();State gate;core.add(1,true);const auto old=gate.mediaSignals->video.sequence();gate.mediaSignals->video.notify();
    assert(!pullVideo(gate,packet,true,old)&&core.waits==0&&core.polls==0);
    gate.pullOpen=false;assert(!take(gate,packet)&&core.polls==0);
    gate.pullOpen=true;gate.pullActive=true;assert(!take(gate,packet)&&core.polls==0);gate.pullActive=false;
    gate.stop=true;assert(!take(gate,packet)&&core.polls==0);
    // Shutdown as the core returns a frame still completes it exactly once.
    reset();State stopping;core.add(1,true);core.onPull=[&]{stopping.stop=true;};
    assert(!take(stopping,packet,true)&&core.completed.size()==1&&!core.inFlight&&!stopping.pullActive);
    for(unsigned scenario=0;scenario<4;++scenario){
        reset();State timestamp;core.add(1,true);
        if(scenario==1)core.pending.front()->unit.receiveTimeUs=0;
        if(scenario==2)core.pending.front()->unit.receiveTimeUs=fakeClockUs+10000;
        if(scenario==3){fakeClockUs=9000000000000;core.pending.front()->unit.receiveTimeUs=1;}
        assert(take(timestamp,packet));
        assert(scenario==0?packet.firstPacketMs==nowMs()-99:packet.firstPacketMs==0);
        assert(timestamp.trace.completion.count==1&&timestamp.trace.copy.count==1);
    }
    std::cout<<"Moonlight direct pull: FIFO, exact-once completion, NEED_IDR/generation, wake revision, gate/stop and receipt age PASS\n";
}
'''
with tempfile.TemporaryDirectory(prefix='fog-moonlight-pull-') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(harness)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-I'+str(root/'src'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True,timeout=15)
