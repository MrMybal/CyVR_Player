#include "cyvr/player/audio_queue.hpp"
#include <algorithm>
#include <iostream>
#include <vector>
#include <cmath>
#ifdef CYVR_TEST_AUDIO_CONVERSION
#include "cyvr/player/audio_convert.hpp"
extern "C" {
#include <libavutil/frame.h>
}
#endif
using namespace cyvr::player;
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(){try{
    AudioQueue queue;std::vector<float> output(192*2),samples(1024*2);
    for(unsigned i=0;i<1024;++i){samples[i*2]=float(i)/1024;samples[i*2+1]=-samples[i*2];}
    queue.render(output.data(),192);
    require(queue.media_at(192)==0,"Starvation incorrectly advanced the media clock");
    require(queue.push(samples.data(),1024,0),"Cannot queue initial samples");
    queue.render(output.data(),192);
    require(std::equal(output.begin(),output.end(),samples.begin()),"Late first block lost its initial samples");
    require(queue.media_at(288)==96 && queue.media_at(384)==192,"Device/media timestamp mapping lost inserted silence");
    queue.render(output.data(),192,false);
    require(std::all_of(output.begin(),output.end(),[](float v){return v==0;}),"Paused callback emitted audio");
    require(queue.media_at(500)==192,"Paused callback advanced media");
    queue.render(output.data(),192);
    require(std::equal(output.begin(),output.end(),samples.begin()+384),"Resume skipped queued audio");
    AudioQueue gaps;require(gaps.push(samples.data(),1024,96),"Cannot queue gap fixture");gaps.render(output.data(),192);
    require(std::all_of(output.begin(),output.begin()+192,[](float x){return x==0;}),"Declared timestamp gap was not silent");
    require(std::equal(output.begin()+192,output.end(),samples.begin()),"Gap destroyed following samples");
    require(gaps.media_at(192)==192 && gaps.stats().gap==96,"Declared gap did not advance presentation time");
    AudioQueue bounded;for(int i=0;i<128;++i)require(bounded.push(samples.data(),1024,i*1024),"Queue filled too early");require(!bounded.push(samples.data(),1024,128*1024),"Queue grew beyond its budget");
    AudioQueue tail;tail.finish();tail.render(output.data(),192);require(tail.media_at(192)==192 && !tail.stats().underrun,"Known audio tail stalled video or counted starvation");
#ifdef CYVR_TEST_AUDIO_CONVERSION
    for(int channels:{2,6,8}){
        AudioConverter converter;StereoSamples converted;
        AVFrame* frame=av_frame_alloc();require(frame,"Cannot allocate audio fixture");
        frame->sample_rate=48000;frame->nb_samples=1024;frame->format=AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&frame->ch_layout,channels);require(av_frame_get_buffer(frame,0)>=0,"Cannot allocate PCM fixture");
        for(int channel=0;channel<channels;++channel)std::fill_n(reinterpret_cast<float*>(frame->extended_data[channel]),1024,.95f);
        std::int64_t expected=0;
        for(int i=0;i<100;++i){
            // Matroska timestamps rounded to milliseconds; AAC frames are 21 1/3 ms.
            converter.convert(*frame,std::int64_t(std::llround(i*1024.*1000/48000))*1000,converted);
            require(converted.start==expected,"Rounded PTS split continuous PCM");expected+=converted.samples.size()/2;
            const auto expected_level=channels>2?.95f*.891250938f:.95f;
            require(std::all_of(converted.samples.begin(),converted.samples.end(),[&](float s){return std::isfinite(s)&&std::abs(s-expected_level)<1e-5f;}),"Downmix distorted/clipped a correlated full-level signal");
        }
        require(!converter.stats().limited && !converter.stats().invalid,"Downmix needed safety clipping");
        if(channels==2)require(converted.samples[0]==.95f && converted.samples[1]==.95f,"Stereo unity gain changed");
        converter.convert(*frame,3'000'000,converted);require(converted.start==144000,"Genuine timestamp discontinuity was ignored");
        av_frame_free(&frame);
    }
    AudioConverter resampling;StereoSamples converted;std::int64_t frames=0;
    AVFrame* frame=av_frame_alloc();frame->sample_rate=44100;frame->nb_samples=441;frame->format=AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&frame->ch_layout,2);require(av_frame_get_buffer(frame,0)>=0,"Cannot allocate resampling fixture");
    for(int n=0;n<100;++n){
        auto* pcm=reinterpret_cast<std::int16_t*>(frame->data[0]);
        for(int i=0;i<441;++i){const auto sample=std::int16_t(std::sin((n*441+i)*2*3.141592653589793*440/44100)*16000);pcm[i*2]=sample;pcm[i*2+1]=-sample;}
        resampling.convert(*frame,n*10000,converted);require(converted.start==frames,"Resampling broke continuity");frames+=converted.samples.size()/2;
        for(std::size_t i=0;i<converted.samples.size();i+=2)require(std::abs(converted.samples[i]+converted.samples[i+1])<1e-6f,"Resampling lost stereo polarity");
    }
    resampling.drain(converted);require(converted.start==frames,"Resampler tail is discontinuous");frames+=converted.samples.size()/2;
    require(frames==48000 && !resampling.stats().limited,"44.1 kHz did not produce one clean second at 48 kHz");av_frame_free(&frame);
#endif
    std::cout<<"Audio starvation recovery, stereo samples, device/media clock, gaps, pause/resume, bounded queues, normalized surround and resampling passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
