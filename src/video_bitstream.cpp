#include "cyvr/player/video_bitstream.hpp"
extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
}

namespace cyvr::player {
namespace {
void check(int result,const char* action){
    if(result>=0) return;
    char text[AV_ERROR_MAX_STRING_SIZE]{};av_strerror(result,text,sizeof text);
    throw MediaError(std::string(action)+": "+text);
}
std::size_t start_code(const std::uint8_t* p,std::size_t size,std::size_t index){
    if(index+3<=size && p[index]==0 && p[index+1]==0){
        if(p[index+2]==1) return 3;
        if(index+4<=size && p[index+2]==0 && p[index+3]==1) return 4;
    }
    return 0;
}
void append(std::vector<std::uint8_t>& target,const std::uint8_t* begin,const std::uint8_t* end){
    if(std::size_t(end-begin)>262144 || target.size()+std::size_t(end-begin)+4>262144)
        throw MediaError("Codec parameter sets exceed the startup budget");
    target.insert(target.end(),{0,0,0,1});target.insert(target.end(),begin,end);
}
}
struct VideoBitstream::Impl {
    AVBSFContext* filter=nullptr;
    VideoConfiguration configuration;
    bool avc=false,vps=false,sps=false,pps=false;
    bool ready() const noexcept {return sps&&pps&&(avc||vps);}
    void collect(const std::uint8_t* data,std::size_t size){
        const bool had_vps=vps,had_sps=sps,had_pps=pps;
        auto& config=configuration;
        for(std::size_t i=0;i<size;){
            const auto prefix=start_code(data,size,i);
            if(!prefix){++i;continue;}
            const auto begin=i+prefix;auto end=begin;
            while(end<size&&!start_code(data,size,end))++end;
            if(begin<end){
                const auto type=avc ? data[begin]&31 : (data[begin]>>1)&63;
                if(avc&&type==7&&!had_sps){append(config.codec_data[0],data+begin,data+end);sps=true;}
                if(avc&&type==8&&!had_pps){append(config.codec_data[1],data+begin,data+end);pps=true;}
                if(!avc&&((type==32&&!had_vps)||(type==33&&!had_sps)||(type==34&&!had_pps))){
                    append(config.codec_data[0],data+begin,data+end);vps=vps||type==32;sps=sps||type==33;pps=pps||type==34;
                }
            }
            i=end;
        }
    }
    ~Impl(){av_bsf_free(&filter);}
};
VideoBitstream::VideoBitstream(const Demuxer& source,int stream) : impl_(std::make_unique<Impl>()) {
    const auto* parameters=source.codec_parameters(stream);
    const bool avc=parameters->codec_id==AV_CODEC_ID_H264;
    const bool hevc=parameters->codec_id==AV_CODEC_ID_HEVC;
    impl_->avc=avc;
    if(!avc && !hevc) throw MediaError("Current native hardware bridge accepts AVC and HEVC");
    const auto* filter=av_bsf_get_by_name(avc ? "h264_mp4toannexb" : "hevc_mp4toannexb");
    if(!filter) throw MediaError("Annex B filter is absent from this FFmpeg build");
    check(av_bsf_alloc(filter,&impl_->filter),"Create bitstream filter");
    check(avcodec_parameters_copy(impl_->filter->par_in,parameters),"Copy codec parameters");
    impl_->filter->time_base_in={source.time_base_numerator(stream),source.time_base_denominator(stream)};
    check(av_bsf_init(impl_->filter),"Initialize bitstream filter");
    auto& config=impl_->configuration;
    config.mime=avc ? "video/avc" : "video/hevc";
    config.width=parameters->width;config.height=parameters->height;
    config.codec_data.resize(avc ? 2 : 1);
    const auto* data=impl_->filter->par_out->extradata;
    if(data&&impl_->filter->par_out->extradata_size>0)
        impl_->collect(data,std::size_t(impl_->filter->par_out->extradata_size));
}
VideoBitstream::~VideoBitstream()=default;
const VideoConfiguration& VideoBitstream::configuration() const noexcept {return impl_->configuration;}
bool VideoBitstream::configuration_ready() const noexcept {return impl_->ready();}
bool VideoBitstream::send(const AVPacket* packet){
    PacketPtr copy;
    if(packet){
        copy.reset(av_packet_alloc());if(!copy) throw std::bad_alloc();
        check(av_packet_ref(copy.get(),packet),"Reference compressed packet");
    }
    const int result=av_bsf_send_packet(impl_->filter,copy.get());
    if(result==AVERROR(EAGAIN)) return false;
    check(result,"Filter compressed packet");return true;
}
PacketRead VideoBitstream::receive(){
    PacketPtr packet(av_packet_alloc());if(!packet) throw std::bad_alloc();
    const int result=av_bsf_receive_packet(impl_->filter,packet.get());
    if(result==AVERROR(EAGAIN)) return {ReadStatus::again,{}};
    if(result==AVERROR_EOF) return {ReadStatus::end,{}};
    check(result,"Read filtered packet");
    if(!impl_->ready())impl_->collect(packet->data,std::size_t(packet->size));
    return {ReadStatus::packet,std::move(packet)};
}
void VideoBitstream::flush(){av_bsf_flush(impl_->filter);}
} // namespace cyvr::player
