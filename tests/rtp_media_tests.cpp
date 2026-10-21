#include "features/streaming/domain/h264_rtp.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include <SDL.h>
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
    auto unit = fog::detail::inspectVideo(bytes.data(), bytes.size());
    std::vector<Frame> frames;
    for (const auto& nal : unit.nals) {
        if (nal.type == 9) frames.emplace_back();
        check(!frames.empty(), "fixture needs access-unit delimiters");
        if (frames.size() > 1 && (nal.type == 7 || nal.type == 8)) continue;
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
            for (std::size_t pos=1; pos<nal.size();) {
                const auto count=std::min(std::size_t(598),nal.size()-pos);
                Bytes payload{std::byte((byte(nal[0])&0xe0)|28),
                    std::byte((byte(nal[0])&31)|(pos==1?128:0)|(pos+count==nal.size()?64:0))};
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
int main(int argc,char** argv) {
    check(argc==2,"fixture path required");
    SDL_setenv("SDL_VIDEODRIVER","dummy",1); SDL_setenv("SDL_AUDIODRIVER","dummy",1);
    check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)==0,"SDL init");
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,640,360,32,SDL_PIXELFORMAT_ARGB8888);
    check(surface!=nullptr,"surface"); auto* renderer=SDL_CreateSoftwareRenderer(surface);
    check(renderer!=nullptr,"renderer");
    {
        auto frames=load(argv[1]); check(frames.size()>=30,"need 30 fixture frames");
        stream::H264RtpAssembler assembler; fog::Media media(renderer);
        std::uint16_t sequence=65520; std::uint64_t now=1000;
        std::size_t decoded=0; bool removed=false, reordered=false;
        for (std::size_t i=0;i<frames.size();++i) {
            auto packets=packetize(frames[i],sequence,90000+static_cast<std::uint32_t>(i)*3000);
            if (i==7) {
                // Lose a middle FU fragment of an actual coded slice, preserving sequence gap.
                for(std::size_t j=1;j+1<packets.size();++j) {
                    if ((byte(packets[j][12])&31)==28 && !(byte(packets[j][13])&0xc0)) {
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
        std::cout<<"PASS actual H264: first-AU-only SPS/PPS, sequence wrap, lost FU fragment, next IDR, early marker reorder; decoded="<<decoded<<'\n';
        std::cout<<assembler.diagnostics()<<'\n';
    }
    SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); SDL_Quit();
}
