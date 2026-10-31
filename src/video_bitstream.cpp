#include "cyvr/player/video_bitstream.hpp"
#include <algorithm>
extern "C" {
#include <libavcodec/bsf.h>
#include <libavutil/error.h>
#include <libavutil/mastering_display_metadata.h>
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
    PacketPtr spare; // Polling usually finds no packet; keep its allocation.
    AVBSFContext* filter=nullptr;
    VideoConfiguration configuration;
    bool avc=false,vps=false,sps=false,pps=false,annex=true;
    bool inspected_first_packet{};
    bool ready() const noexcept {return !annex || (sps&&pps&&(avc||vps));}
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
                if(avc && (type==15 || type==20 || (type==7 && end>begin+1 && (data[begin+1]==118 || data[begin+1]==128 || data[begin+1]==134))))config.multiview=true;
                if(!avc && type==32 && end>=begin+4 && (((data[begin+2]&3)<<4)|(data[begin+3]>>4)))config.multiview=true;
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
    impl_->annex=avc||hevc;
    const auto* filter=av_bsf_get_by_name(avc ? "h264_mp4toannexb" : hevc ? "hevc_mp4toannexb" : "null");
    if(!filter) throw MediaError("Annex B filter is absent from this FFmpeg build");
    check(av_bsf_alloc(filter,&impl_->filter),"Create bitstream filter");
    check(avcodec_parameters_copy(impl_->filter->par_in,parameters),"Copy codec parameters");
    impl_->filter->time_base_in={source.time_base_numerator(stream),source.time_base_denominator(stream)};
    check(av_bsf_init(impl_->filter),"Initialize bitstream filter");
    auto& config=impl_->configuration;
    switch(parameters->codec_id){
    case AV_CODEC_ID_H264:config.mime="video/avc";break;
    case AV_CODEC_ID_HEVC:config.mime="video/hevc";break;
    case AV_CODEC_ID_VP9:config.mime="video/x-vnd.on2.vp9";break;
    case AV_CODEC_ID_VP8:config.mime="video/x-vnd.on2.vp8";break;
    case AV_CODEC_ID_AV1:config.mime="video/av01";break;
    case AV_CODEC_ID_MPEG2VIDEO:config.mime="video/mpeg2";break;
    case AV_CODEC_ID_MPEG4:config.mime="video/mp4v-es";break;
    case AV_CODEC_ID_H263:config.mime="video/3gpp";break;
    case AV_CODEC_ID_MJPEG:config.mime="video/mjpeg";break;
    default:break;
    }
    config.width=parameters->width;config.height=parameters->height;
    config.multiview=(avc && (parameters->profile==118 || parameters->profile==128 || parameters->profile==134)) || (hevc && parameters->profile==6);
    config.color.standard=parameters->color_primaries==AVCOL_PRI_BT2020 || parameters->color_space==AVCOL_SPC_BT2020_NCL || parameters->color_space==AVCOL_SPC_BT2020_CL ? 6 : parameters->color_space==AVCOL_SPC_BT470BG || parameters->color_primaries==AVCOL_PRI_BT470BG ? 2 : parameters->color_space==AVCOL_SPC_SMPTE170M || parameters->color_primaries==AVCOL_PRI_SMPTE170M ? 4 : 1;
    config.color.transfer=parameters->color_trc==AVCOL_TRC_SMPTE2084 ? 6 : parameters->color_trc==AVCOL_TRC_ARIB_STD_B67 ? 7 : parameters->color_trc==AVCOL_TRC_LINEAR ? 1 : 3;
    config.color.range=parameters->color_range==AVCOL_RANGE_JPEG ? 1 : parameters->color_range==AVCOL_RANGE_MPEG ? 2 : 0;
    if(const auto* side=av_packet_side_data_get(parameters->coded_side_data,parameters->nb_coded_side_data,AV_PKT_DATA_MASTERING_DISPLAY_METADATA)){
        if(side->size>=sizeof(AVMasteringDisplayMetadata)){
            const auto* hdr=reinterpret_cast<const AVMasteringDisplayMetadata*>(side->data);
            if(hdr->has_luminance && hdr->max_luminance.den>0)config.color.peak_nits=float(av_q2d(hdr->max_luminance));
        }
    }
    config.color.peak_nits=std::clamp(config.color.peak_nits,203.f,10000.f);
    if(impl_->annex)config.codec_data.resize(avc ? 2 : 1);
    const auto* data=impl_->filter->par_out->extradata;
    if(data&&impl_->filter->par_out->extradata_size>0){
        if(impl_->annex)impl_->collect(data,std::size_t(impl_->filter->par_out->extradata_size));
        else config.codec_data.emplace_back(data,data+impl_->filter->par_out->extradata_size);
    }
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
    PacketPtr packet=std::move(impl_->spare);
    if(!packet)packet.reset(av_packet_alloc());
    if(!packet) throw std::bad_alloc();
    const int result=av_bsf_receive_packet(impl_->filter,packet.get());
    if(result==AVERROR(EAGAIN)){impl_->spare=std::move(packet);return {ReadStatus::again,{}};}
    if(result==AVERROR_EOF) return {ReadStatus::end,{}};
    check(result,"Read filtered packet");
    if(impl_->annex && (!impl_->ready() || !impl_->inspected_first_packet))impl_->collect(packet->data,std::size_t(packet->size));
    impl_->inspected_first_packet=true;
    return {ReadStatus::packet,std::move(packet)};
}
void VideoBitstream::flush(){av_bsf_flush(impl_->filter);}
} // namespace cyvr::player
