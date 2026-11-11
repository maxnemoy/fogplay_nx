#pragma once
#include "features/streaming/domain/compressed_media_source.hpp"
#include "features/streaming/domain/media_telemetry.hpp"
#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
struct SDL_Renderer {};
namespace fog {
class VideoDecodeError : public std::runtime_error {
public:
    VideoDecodeError(int code,bool recoverable,const std::string& message):std::runtime_error(message),code_(code),recoverable_(recoverable){}
    int code()const{return code_;}bool recoverable()const{return recoverable_;}
private:int code_;bool recoverable_;
};
struct PlayerMediaAdapterState {
    SDL_Renderer* renderer=nullptr;
    ::stream::CompressedMediaSource* source=nullptr;
    ::stream::VideoCodec codec=::stream::VideoCodec::H265;
    bool deferred=false,hevc=true,workers=false,mts=false,prepare=true,draw=true,codecFailure=false,videoFailure=false,resetFailure=false,prepareFailure=false;
    int fps=0;
    unsigned resets=0,videos=0,audios=0,stops=0,draws=0,presents=0,destroyed=0;
    std::uint64_t firstMs=0,enqueueMs=0;
    std::thread::id destroyedOn;
    std::function<void(const std::string&)> watchdog;
    std::function<void(const media::telemetry::StallEvent&)> typedWatchdog;
    std::exception_ptr error;
    ::stream::Packet packet;
    bool hasPacket=false;
    std::chrono::steady_clock::time_point slot;
};
inline PlayerMediaAdapterState playerMediaAdapter;
class Media {
public:
    enum class WorkerPolicy { FogHub,Mts };
    struct WorkerError {std::exception_ptr error;::stream::Packet packet;bool hasPacket=false;};
    struct WorkerVideoLoad {std::uint64_t decodeUs,convertUs,generation;};
    Media(SDL_Renderer* renderer,::stream::VideoCodec codec,bool deferred){playerMediaAdapter.renderer=renderer;playerMediaAdapter.codec=codec;playerMediaAdapter.deferred=deferred;}
    ~Media(){++playerMediaAdapter.destroyed;playerMediaAdapter.destroyedOn=std::this_thread::get_id();}
    static bool supportsH265(){return playerMediaAdapter.hevc;}
    void setVideoStallObserver(std::function<void(const media::telemetry::StallEvent&)> value){playerMediaAdapter.typedWatchdog=std::move(value);}
    void setVideoStallReporter(std::function<void(const std::string&)> value){playerMediaAdapter.watchdog=std::move(value);}
    void setVideoCodec(::stream::VideoCodec value){if(playerMediaAdapter.codecFailure)throw VideoDecodeError(-10,true,"codec");playerMediaAdapter.codec=value;}
    void setPresentationFrameRate(int fps){playerMediaAdapter.fps=fps;}
    void startWorkers(::stream::CompressedMediaSource& source,int fps,WorkerPolicy policy){playerMediaAdapter.source=&source;playerMediaAdapter.workers=true;playerMediaAdapter.fps=fps;playerMediaAdapter.mts=policy==WorkerPolicy::Mts;}
    void stopWorkers()noexcept{++playerMediaAdapter.stops;playerMediaAdapter.workers=false;playerMediaAdapter.source=nullptr;}
    bool workersRunning()const{return playerMediaAdapter.workers;}
    bool takeWorkerError(WorkerError& error){if(!playerMediaAdapter.error)return false;error={playerMediaAdapter.error,playerMediaAdapter.packet,playerMediaAdapter.hasPacket};playerMediaAdapter.error={};return true;}
    std::uint64_t workerVideoGeneration()const{return 7;}
    WorkerVideoLoad workerVideoLoad()const{return {100,200,7};}
    void resetVideoDecoder(){++playerMediaAdapter.resets;if(playerMediaAdapter.resetFailure)throw VideoDecodeError(-11,false,"reset");}
    void video(const std::byte*,std::size_t,std::uint64_t first,std::uint64_t enqueue){++playerMediaAdapter.videos;playerMediaAdapter.firstMs=first;playerMediaAdapter.enqueueMs=enqueue;if(playerMediaAdapter.videoFailure)throw VideoDecodeError(-12,true,"video");}
    void audio(const std::byte*,std::size_t,std::uint64_t enqueue){++playerMediaAdapter.audios;playerMediaAdapter.enqueueMs=enqueue;}
    void checkAudioRecovery()const{}
    bool preparePresentation(std::chrono::steady_clock::time_point slot){playerMediaAdapter.slot=slot;if(playerMediaAdapter.prepareFailure)throw VideoDecodeError(-13,true,"prepare");return playerMediaAdapter.prepare;}
    bool draw(){++playerMediaAdapter.draws;return playerMediaAdapter.draw;}
    void present(){++playerMediaAdapter.presents;}
    bool waitingForVideoKeyframe()const{return false;}
    std::uint64_t videoFrames()const{return 11;}std::uint64_t uploadedFrames()const{return 10;}std::uint64_t presentedFrames()const{return 9;}
    std::uint64_t supersededFrames()const{return 2;}std::uint64_t audioPackets()const{return 20;}
    std::uint64_t audioUnderruns()const{return 3;}std::uint64_t audioQueueResets()const{return 4;}
    int videoWidth()const{return 1280;}int videoHeight()const{return 720;}bool hardwareVideo()const{return true;}
    media::telemetry::Snapshot captureTelemetry()const{return {};}
    std::string diagnostics()const{return "video";}std::string audioDiagnostics()const{return "audio";}
    std::string imageDiagnostics()const{return "image";}std::string latencyDiagnostics()const{return "latency";}
};
}
