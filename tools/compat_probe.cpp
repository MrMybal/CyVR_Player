// Decode actual compressed media through the same software path as the app.
#include "cyvr/player/video_bitstream.hpp"
#include "cyvr/player/multiview.hpp"
#include "cyvr/player/clearkey.hpp"
#include <chrono>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}
using namespace cyvr::player;
int main(int argc,char** argv){try{
    if(argc<2 || argc>3)throw std::runtime_error("compat_probe FILE [views]");
    StreamOptions options;
    if(argc==3 && std::string(argv[2])!="views")options.clear_keys=read_clear_keys(MediaSource::file(argv[2]),Cancellation{});
    Demuxer input;input.open(MediaSource::file(argv[1]),options);const int stream=input.best_video_stream();
    if(stream<0)throw MediaError("No video stream");
    VideoBitstream bits(input,stream);
    const int dependent=input.mvc_dependent_stream(stream);
    if(dependent>=0 && input.tracks().at(std::size_t(dependent)).companion_video!=stream)throw MediaError("MVC dependent track lost its base association");
    const bool views=(argc==3 && std::string(argv[2])=="views") || bits.configuration().multiview || dependent>=0;
    SoftwareDecoder decoder(input,stream,4,64*1024*1024,views);
    PacketPtr pending;bool end=false,eos=false,finished=false;unsigned frames=0,stereo=0;int width=0,height=0;
    std::int64_t first=AV_NOPTS_VALUE,last=AV_NOPTS_VALUE;std::uint64_t hashes[2]={1469598103934665603ull,1469598103934665603ull};
    const auto started=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-started<std::chrono::seconds(45)){
        auto result=decoder.receive();
        if(result.status==DecodeStatus::end){finished=true;break;}
        if(result.frame){
            auto& f=*result.frame;++frames;width=f.width;height=f.height;
            const bool pair=packed_stereo_frame(f);stereo+=pair;
            if(first==AV_NOPTS_VALUE)first=f.pts;last=f.pts;
            // Hash the visible luma per eye, without stride padding. Distinct
            // eye fixtures detect accidentally displaying the base view twice.
            const auto* desc=av_pix_fmt_desc_get(AVPixelFormat(f.format));const int bytes=desc->comp[0].depth>8?2:1;
            for(int eye=0;eye<(pair?2:1);++eye)for(int y=0;y<f.height;++y)for(int x=0;x<(pair?f.width/2:f.width)*bytes;++x){hashes[eye]^=f.data[0][std::ptrdiff_t(y)*f.linesize[0]+eye*f.width/2*bytes+x];hashes[eye]*=1099511628211ull;}
        }
        if(!pending && !end){auto packet=input.read();end=packet.status==ReadStatus::end;if(packet.status==ReadStatus::packet && (packet.packet->stream_index==stream || packet.packet->stream_index==dependent))pending=std::move(packet.packet);}
        if(pending && decoder.send(pending.get()))pending.reset();
        if(end && !pending && !eos && decoder.send(nullptr))eos=true;
    }
    if(!frames || !eos || !finished)throw MediaError("Decoder did not complete inside its budget");
    if(views && stereo!=frames)throw MediaError("Multiview frame lost an eye");
    std::cout<<"{\"frames\":"<<frames<<",\"stereo_frames\":"<<stereo<<",\"width\":"<<width<<",\"height\":"<<height<<",\"first_pts\":"<<first<<",\"last_pts\":"<<last<<",\"left_hash\":\""<<hashes[0]<<"\",\"right_hash\":\""<<hashes[1]<<"\",\"seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
