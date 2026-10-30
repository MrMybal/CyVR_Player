#include "cyvr/player/subtitles.hpp"
#include <algorithm>
#include <deque>
#include <mutex>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace cyvr::player {
struct SubtitleDecoder::Impl {
    const Demuxer& input;int stream;AVCodecContext* codec{};
    Impl(const Demuxer& in,int track):input(in),stream(track){
        const auto* p=in.codec_parameters(track);const auto* decoder=avcodec_find_decoder(p->codec_id);
        if(!decoder || p->codec_type!=AVMEDIA_TYPE_SUBTITLE)throw MediaError("Subtitle decoder unavailable");
        codec=avcodec_alloc_context3(decoder);
        if(!codec)throw std::bad_alloc();
        if(avcodec_parameters_to_context(codec,p)<0){avcodec_free_context(&codec);throw MediaError("Subtitle configuration failed");}
        codec->pkt_timebase={in.time_base_numerator(track),in.time_base_denominator(track)};
        if(avcodec_open2(codec,decoder,nullptr)<0){avcodec_free_context(&codec);throw MediaError("Cannot open subtitle decoder");}
    }
    ~Impl(){avcodec_free_context(&codec);}
};
SubtitleDecoder::SubtitleDecoder(const Demuxer& in,int track):impl_(std::make_unique<Impl>(in,track)){}
SubtitleDecoder::~SubtitleDecoder()=default;
std::string SubtitleDecoder::header()const{return impl_->codec->subtitle_header ? std::string(reinterpret_cast<const char*>(impl_->codec->subtitle_header),impl_->codec->subtitle_header_size):std::string{};}
std::shared_ptr<SubtitleCue> SubtitleDecoder::decode(const AVPacket* packet){
    AVSubtitle sub{};int got{};AVPacket empty{};
    // This legacy API is still FFmpeg's subtitle API. It does not own the packet.
    const int result=avcodec_decode_subtitle2(impl_->codec,&sub,&got,const_cast<AVPacket*>(packet?packet:&empty));
    struct Guard{AVSubtitle* value;~Guard(){avsubtitle_free(value);}}guard{&sub};
    if(result<0)throw MediaError("Subtitle packet decoding failed");
    if(!got)return {};
    auto cue=std::make_shared<SubtitleCue>();
    auto pts=packet?impl_->input.timestamp_us(impl_->stream,packet->pts):std::optional<std::int64_t>{};
    if(!pts && sub.pts!=AV_NOPTS_VALUE)pts=sub.pts-impl_->input.origin_us();
    if(!pts)return {};
    cue->start_us=*pts+std::int64_t(sub.start_display_time)*1000;
    const auto duration=packet?av_rescale_q(packet->duration,impl_->codec->pkt_timebase,AVRational{1,1000000}):0;
    cue->end_us=sub.end_display_time>sub.start_display_time && sub.end_display_time!=UINT32_MAX ? *pts+std::int64_t(sub.end_display_time)*1000 : cue->start_us+(duration>0?duration:30000000);
    cue->canvas_width=impl_->codec->width;cue->canvas_height=impl_->codec->height;
    if(!sub.format && (!packet || packet->duration<=0) && sub.end_display_time==UINT32_MAX)cue->end_us=INT64_MAX;
    std::size_t bytes=0;
    if(sub.num_rects>256)throw MediaError("Subtitle rectangle budget exceeded");
    for(unsigned n=0;n<sub.num_rects;++n){
        const auto& r=*sub.rects[n];
        if(r.ass){if(std::strlen(r.ass)>256*1024)throw MediaError("Subtitle text budget exceeded");cue->ass.emplace_back(r.ass);}
        else if(r.text){std::string text=r.text;if(text.size()>256*1024)throw MediaError("Subtitle text budget exceeded");cue->ass.push_back("0,0,Default,,0,0,0,,"+text);}
        if(r.type==SUBTITLE_BITMAP && r.w>0 && r.h>0 && r.data[0] && r.data[1]){
            const auto size=std::uint64_t(r.w)*r.h*4;
            if(size>32*1024*1024-bytes || r.linesize[0]<r.w || r.nb_colors<1 || r.nb_colors>256)throw MediaError("Subtitle bitmap budget exceeded");
            bytes+=std::size_t(size);SubtitleBitmap bitmap{r.x,r.y,r.w,r.h,{}};bitmap.rgba.resize(std::size_t(size));
            for(int y=0;y<r.h;++y)for(int x=0;x<r.w;++x){
                const auto index=r.data[0][y*r.linesize[0]+x];std::uint32_t color{};
                if(index<r.nb_colors)std::memcpy(&color,r.data[1]+index*4,4);
                auto* out=bitmap.rgba.data()+(std::size_t(y)*r.w+x)*4;
                out[0]=std::uint8_t(color>>16);out[1]=std::uint8_t(color>>8);out[2]=std::uint8_t(color);out[3]=std::uint8_t(color>>24);
            }
            cue->bitmaps.push_back(std::move(bitmap));
        }
    }
    return cue;
}
struct SubtitleTimeline::Impl {
    mutable std::mutex mutex;
    std::shared_ptr<const SubtitleConfig> config;
    struct Event{std::shared_ptr<const SubtitleCue> cue;std::int64_t end;};
    std::deque<Event> cues;
    std::size_t bytes{};
    static std::size_t size(const SubtitleCue& c){std::size_t n=sizeof(c);for(auto& b:c.bitmaps)n+=b.rgba.size();for(auto& a:c.ass)n+=a.size();return n;}
};
SubtitleTimeline::SubtitleTimeline(std::shared_ptr<const SubtitleConfig> config):impl_(std::make_unique<Impl>()){impl_->config=std::move(config);}
SubtitleTimeline::~SubtitleTimeline()=default;
void SubtitleTimeline::add(std::shared_ptr<SubtitleCue> cue){
    if(!cue)return;std::lock_guard<std::mutex> lock(impl_->mutex);
    // Bitmap formats use empty display sets to clear a previous image.
    if(cue->ass.empty())for(auto& old:impl_->cues)if(old.cue->ass.empty() && old.end>cue->start_us)old.end=cue->start_us;
    const auto bytes=Impl::size(*cue);
    if(bytes>64*1024*1024 || impl_->bytes>64*1024*1024-bytes || impl_->cues.size()>=100000)throw MediaError("Subtitle event budget exceeded");
    impl_->bytes+=bytes;const auto end=cue->end_us;impl_->cues.push_back({std::move(cue),end});
}
void SubtitleTimeline::prune(std::int64_t before){std::lock_guard<std::mutex> lock(impl_->mutex);while(!impl_->cues.empty() && impl_->cues.front().end<before){impl_->bytes-=Impl::size(*impl_->cues.front().cue);impl_->cues.pop_front();}}
SubtitleSnapshot SubtitleTimeline::at(std::int64_t position)const{
    std::lock_guard<std::mutex> lock(impl_->mutex);SubtitleSnapshot s{impl_->config,{},position};
    for(auto& event:impl_->cues){const auto& c=event.cue;if(c->start_us<=position && position<event.end && (!c->ass.empty() || !c->bitmaps.empty()))s.cues.push_back(c);}
    return s;
}
}
