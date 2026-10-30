#include "cyvr/player/audio_convert.hpp"
#include "cyvr/player/demuxer.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
extern "C" {
#include <libavutil/frame.h>
#include <libavcodec/packet.h>
}
using namespace cyvr::player;
int main(int argc,char** argv){try{
    if(argc!=2)throw MediaError("FILE expected");
    Demuxer input;input.open(MediaSource::file(argv[1]));
    const int track=input.best_audio_stream();if(track<0)throw MediaError("No audio track");
    SoftwareDecoder decoder(input,track,1);AudioConverter converter;StereoSamples samples;
    std::uint64_t count{},hash=14695981039346656037ull,discontinuities{};double square[2]{};std::int64_t end{};bool have=false,eos=false;
    auto consume=[&]{
        if(samples.samples.empty())return;
        if(have && samples.start!=end)++discontinuities;have=true;end=samples.start+samples.samples.size()/2;
        for(std::size_t i=0;i<samples.samples.size();++i){const auto value=samples.samples[i];square[i%2]+=double(value)*value;
            // Quantized fingerprint is stable across scalar/SIMD resamplers.
            const auto q=std::int32_t(std::llround(value*1000000));
            for(unsigned byte=0;byte<4;++byte){hash^=std::uint8_t(std::uint32_t(q)>>(byte*8));hash*=1099511628211ull;}}
        count+=samples.samples.size()/2;
    };
    PacketPtr pending;
    for(;;){
        auto result=decoder.receive();
        if(result.frame){auto pts=input.timestamp_us(track,result.frame->best_effort_timestamp);if(!pts)pts=input.timestamp_us(track,result.frame->pts);converter.convert(*result.frame,pts,samples);consume();continue;}
        if(result.status==DecodeStatus::end)break;
        if(!pending && !eos){auto packet=input.read();if(packet.status==ReadStatus::end)eos=true;else if(packet.status==ReadStatus::packet && packet.packet->stream_index==track)pending=std::move(packet.packet);else continue;}
        if(pending){if(decoder.send(pending.get()))pending.reset();}
        else if(eos)decoder.send(nullptr);
    }
    converter.drain(samples);consume();const auto s=converter.stats();
    std::cout<<"{\"frames\":"<<count<<",\"hash\":"<<hash<<",\"peak\":"<<s.peak<<",\"limited\":"<<s.limited<<",\"invalid\":"<<s.invalid<<",\"discontinuities\":"<<discontinuities
        <<",\"rms_left\":"<<std::sqrt(square[0]/std::max<std::uint64_t>(1,count))<<",\"rms_right\":"<<std::sqrt(square[1]/std::max<std::uint64_t>(1,count))<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
