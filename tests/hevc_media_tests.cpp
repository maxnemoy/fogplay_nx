#include "features/streaming/domain/h264_rtp.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include <SDL.h>
#include <opus/opus.h>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
using Bytes = std::vector<std::byte>;
using Frame = std::vector<Bytes>;
static void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
static unsigned byte(std::byte b) { return std::to_integer<unsigned>(b); }
static std::vector<Frame> load(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    auto unit = fog::detail::inspectVideo(bytes.data(), bytes.size(), stream::VideoCodec::H265);
    std::vector<Frame> frames;
    for (const auto& nal : unit.nals) {
        if (nal.type == 35) frames.emplace_back();
        check(!frames.empty(), "fixture needs access-unit delimiters");
        if (frames.size() > 1 && (nal.type >= 32 && nal.type <= 34)) continue;
        Bytes copy;
        for (std::size_t i=0; i<nal.size; ++i) copy.push_back(std::byte(nal.data[i]));
        frames.back().push_back(std::move(copy));
    }
    return frames;
}
static std::vector<Bytes> packetize(const Frame& frame, std::uint16_t& sequence, std::uint32_t timestamp) {
    std::vector<Bytes> result;
    for (const auto& nal : frame) {
        std::vector<Bytes> payloads;
        if (nal.size() <= 600) payloads.push_back(nal);
        else {
            for (std::size_t pos=2; pos<nal.size();) {
                const auto count=std::min(std::size_t(597),nal.size()-pos);
                Bytes payload{std::byte{98},nal[1],
                    std::byte(((byte(nal[0])>>1)&63)|(pos==2?128:0)|(pos+count==nal.size()?64:0))};
                payload.insert(payload.end(),nal.begin()+pos,nal.begin()+pos+count);
                payloads.push_back(std::move(payload)); pos+=count;
            }
        }
        for (auto& payload : payloads) {
            const auto seq=sequence++;
            Bytes packet{std::byte{0x80},std::byte{96},std::byte(seq>>8),std::byte(seq),
                std::byte(timestamp>>24),std::byte(timestamp>>16),std::byte(timestamp>>8),std::byte(timestamp),
                std::byte{0},std::byte{0},std::byte{0},std::byte{1}};
            packet.insert(packet.end(),payload.begin(),payload.end()); result.push_back(std::move(packet));
        }
    }
    result.back()[1]=std::byte{0xe0}; return result;
}
static Bytes audioPacket() {
    int error = 0;
    auto* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    check(encoder && error == OPUS_OK, "Opus fixture encoder");
    std::array<opus_int16, 1920> pcm{}; std::array<unsigned char, 4096> encoded{};
    const auto count = opus_encode(encoder, pcm.data(), 960, encoded.data(), encoded.size());
    opus_encoder_destroy(encoder); check(count > 0, "Opus fixture packet");
    Bytes result; for (int i = 0; i < count; ++i) result.push_back(std::byte(encoded[i])); return result;
}
int main(int argc,char** argv) {
    check(argc==2,"fixture path required");
    SDL_setenv("SDL_VIDEODRIVER","dummy",1); SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)==0,"SDL init");
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,640,360,32,SDL_PIXELFORMAT_ARGB8888);
    check(surface!=nullptr,"surface"); auto* renderer=SDL_CreateSoftwareRenderer(surface);
    check(renderer!=nullptr,"renderer");
    {
        auto frames=load(argv[1]); check(frames.size()>=30,"need 30 fixture frames");
        stream::H264RtpAssembler assembler(stream::VideoCodec::H265); fog::Media media(renderer);
        const auto audio = audioPacket();
        media.audio(audio.data(), audio.size());
        media.setVideoCodec(stream::VideoCodec::H265);
        check(media.audioPackets() == 1, "codec switch lost audio");
        std::uint16_t sequence=65520; std::uint64_t now=1000;
        std::size_t decoded=0; bool removed=false, reordered=false;
        for (std::size_t i=0;i<frames.size();++i) {
            auto packets=packetize(frames[i],sequence,90000+static_cast<std::uint32_t>(i)*3000);
            if (i==7) {
                // Lose a middle FU fragment of an actual coded slice, preserving sequence gap.
                for(std::size_t j=1;j+1<packets.size();++j) {
                    if (((byte(packets[j][12])>>1)&63)==49 && !(byte(packets[j][14])&0xc0)) {
                        packets.erase(packets.begin()+j); removed=true; break;
                    }
                }
                check(removed,"fixture needs middle fragment");
            }
            if (i==17 && packets.size()>2) {
                std::swap(packets[packets.size()-1],packets[packets.size()-2]); reordered=true;
            }
            for (const auto& packet:packets) assembler.receive(packet.data(),packet.size(),now);
            assembler.tick(now+25);
            stream::H264RtpAssembler::Frame out;
            while(assembler.poll(out)) {
                check(out.timestamp!=90000+7*3000,"damaged frame escaped");
                const auto before=media.videoFrames();
                media.video(out.data.data(),out.data.size());
                decoded+=media.videoFrames()-before;
            }
            now+=33;
        }
        check(removed&&reordered,"loss/reorder injection");
        check(assembler.stats().missing==1,"exact one missing RTP packet");
        check(assembler.stats().recovered>=2,"initial and post-loss IDR");
        check(decoded>=15,"video did not resume after loss");
        check(!assembler.waiting(),"receiver still quarantined");
        check(media.draw(),"draw decoded frame");
        check(media.diagnostics().find("codec=hevc") != std::string::npos, "actual HEVC decoder diagnostic");
        const auto before = media.videoFrames();
        media.resetVideoDecoder();
        // A delta AU cannot end decoder recovery. IDR restoration remains owned
        // by the parameter cache even if the sender never repeats its config.
        for (std::size_t index : {std::size_t(6), std::size_t(5)}) {
            std::vector<std::byte> unit;
            for (const auto& nal : frames[index]) {
                unit.insert(unit.end(), {std::byte{0},std::byte{0},std::byte{0},std::byte{1}});
                unit.insert(unit.end(), nal.begin(), nal.end());
            }
            media.video(unit.data(), unit.size());
            if (index == 6) check(media.videoFrames() == before && media.waitingForVideoKeyframe(), "delta escaped reset");
        }
        check(media.videoFrames() > before && !media.waitingForVideoKeyframe(), "HEVC cached reset recovery");
        media.setVideoCodec(stream::VideoCodec::H264);
        check(media.diagnostics().find("codec=h264") != std::string::npos && media.waitingForVideoKeyframe(), "codec switch back");
        media.setVideoCodec(stream::VideoCodec::H265);
        std::vector<std::byte> bareIdr;
        for (const auto& nal : frames[5]) {
            bareIdr.insert(bareIdr.end(), {std::byte{0},std::byte{0},std::byte{0},std::byte{1}});
            bareIdr.insert(bareIdr.end(), nal.begin(), nal.end());
        }
        const auto switched = media.videoFrames();
        media.video(bareIdr.data(), bareIdr.size());
        check(media.waitingForVideoKeyframe() && media.videoFrames() == switched, "codec switch retained stale parameter chain");
        media.audio(audio.data(), audio.size());
        check(media.audioPackets() == 2, "audio failed after HEVC recovery and codec switches");
        std::cout<<"PASS actual HEVC: first-AU-only VPS/SPS/PPS, sequence wrap, lost FU fragment, next IDR, early marker reorder; decoded="<<decoded<<'\n';
        std::cout<<assembler.diagnostics()<<'\n';
    }
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
}
