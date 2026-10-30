#include "cyvr/player/audio_convert.hpp"
#include "cyvr/player/demuxer.hpp"
#include <algorithm>
#include <cmath>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
namespace cyvr::player {
struct AudioConverter::Impl {
    SwrContext* context{};AVChannelLayout layout{};
    int rate,input_rate{},format{-1};std::int64_t next{};bool anchored{};
    AudioConversionStats counters;
    explicit Impl(int output):rate(output){if(rate<=0 || rate>384000)throw MediaError("Invalid audio output rate");}
    ~Impl(){swr_free(&context);av_channel_layout_uninit(&layout);}
    void safety(StereoSamples& out){
        for(auto& sample:out.samples){
            if(!std::isfinite(sample)){sample=0;++counters.invalid;continue;}
            counters.peak=std::max(counters.peak,std::abs(sample));
            if(std::abs(sample)>1){sample=std::clamp(sample,-1.f,1.f);++counters.limited;}
        }
    }
};
AudioConverter::AudioConverter(int rate):impl_(std::make_unique<Impl>(rate)){}
AudioConverter::~AudioConverter()=default;
void AudioConverter::convert(const AVFrame& frame,std::optional<std::int64_t> pts,StereoSamples& out){
    auto& p=*impl_;
    if(frame.sample_rate<=0 || frame.ch_layout.nb_channels<=0 || frame.nb_samples<=0)
        throw MediaError("Audio track has incomplete sample metadata");
    if(!p.context || p.input_rate!=frame.sample_rate || p.format!=frame.format || av_channel_layout_compare(&p.layout,&frame.ch_layout)){
        swr_free(&p.context);av_channel_layout_uninit(&p.layout);
        if(av_channel_layout_copy(&p.layout,&frame.ch_layout)<0)throw MediaError("Cannot copy audio channel layout");
        AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        // Float downmixes otherwise permit sums above full scale. Reserve 1 dB
        // for codec/resampling overshoot when folding surround into stereo.
        if(swr_alloc_set_opts2(&p.context,&stereo,AV_SAMPLE_FMT_FLT,p.rate,&p.layout,AVSampleFormat(frame.format),frame.sample_rate,0,nullptr)<0 ||
           av_opt_set_double(p.context,"rematrix_maxval",1.0,0)<0 ||
           av_opt_set_double(p.context,"rematrix_volume",p.layout.nb_channels>2?0.8912509381337456:1.0,0)<0 || swr_init(p.context)<0)
            throw MediaError("Audio resampling configuration failed");
        p.input_rate=frame.sample_rate;p.format=frame.format;
    }
    const auto delay=swr_get_delay(p.context,p.input_rate);
    const auto capacity=av_rescale_rnd(delay+frame.nb_samples,p.rate,p.input_rate,AV_ROUND_UP);
    if(capacity<=0 || capacity>1048576)throw MediaError("Audio frame exceeds the conversion budget");
    out.start=p.next;
    if(pts){
        const auto candidate=av_rescale_q(*pts,AVRational{1,1000000},AVRational{1,p.rate})-
            av_rescale_rnd(delay,p.rate,p.input_rate,AV_ROUND_NEAR_INF);
        // MKV millisecond PTS and resampler delay rounding are not missing samples.
        // Preserve genuine gaps/discontinuities of more than 100 ms.
        if(!p.anchored || std::abs(candidate-p.next)>p.rate/10)out.start=candidate;
        p.anchored=true;
    }
    out.samples.resize(std::size_t(capacity)*2);
    std::uint8_t* planes[]{reinterpret_cast<std::uint8_t*>(out.samples.data())};
    const auto count=swr_convert(p.context,planes,int(capacity),const_cast<const std::uint8_t**>(frame.extended_data),frame.nb_samples);
    if(count<0)throw MediaError("Audio sample conversion failed");
    out.samples.resize(std::size_t(count)*2);p.next=out.start+count;p.safety(out);
}
void AudioConverter::drain(StereoSamples& out){
    auto& p=*impl_;out.samples.clear();out.start=p.next;if(!p.context)return;
    const auto capacity=av_rescale_rnd(swr_get_delay(p.context,p.input_rate),p.rate,p.input_rate,AV_ROUND_UP);
    if(capacity<=0)return;if(capacity>1048576)throw MediaError("Audio flush exceeds the conversion budget");
    out.samples.resize(std::size_t(capacity)*2);std::uint8_t* planes[]{reinterpret_cast<std::uint8_t*>(out.samples.data())};
    const auto count=swr_convert(p.context,planes,int(capacity),nullptr,0);
    if(count<0)throw MediaError("Audio flush failed");out.samples.resize(std::size_t(count)*2);p.next+=count;p.safety(out);
}
AudioConversionStats AudioConverter::stats()const{return impl_->counters;}
}
