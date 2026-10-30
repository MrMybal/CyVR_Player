#include "cyvr/player/multiview.hpp"
#include "cyvr/player/video_bitstream.hpp"
#include <algorithm>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/stereo3d.h>
#include <edge264mvc.h>
}
namespace cyvr::player {
namespace {
void mark_stereo(AVFrame& frame) {
    av_frame_remove_side_data(&frame, AV_FRAME_DATA_VIEW_ID);
    av_frame_remove_side_data(&frame, AV_FRAME_DATA_STEREO3D);
    auto* stereo=av_stereo3d_create_side_data(&frame);
    if(!stereo)throw std::bad_alloc();
    stereo->type=AV_STEREO3D_SIDEBYSIDE;
    stereo->view=AV_STEREO3D_VIEW_PACKED;
}
int view_id(const AVFrame& frame) {
    const auto* side=av_frame_get_side_data(&frame,AV_FRAME_DATA_VIEW_ID);
    int id=-1;
    if(side && side->size>=sizeof(id))std::memcpy(&id,side->data,sizeof(id));
    return id;
}
int eye(const AVFrame& frame) {
    const auto* side=av_frame_get_side_data(&frame,AV_FRAME_DATA_STEREO3D);
    if(side && side->size>=sizeof(AVStereo3D)) {
        const auto* stereo=reinterpret_cast<const AVStereo3D*>(side->data);
        if(stereo->view==AV_STEREO3D_VIEW_LEFT)return 0;
        if(stereo->view==AV_STEREO3D_VIEW_RIGHT)return 1;
    }
    return -1;
}
std::size_t prefix(const std::uint8_t* data,std::size_t size,std::size_t at) {
    if(at+3<=size && !data[at] && !data[at+1]) {
        if(data[at+2]==1)return 3;
        if(at+4<=size && !data[at+2] && data[at+3]==1)return 4;
    }
    return 0;
}
}
bool packed_stereo_frame(const AVFrame& frame) noexcept {
    const auto* side=av_frame_get_side_data(&frame,AV_FRAME_DATA_STEREO3D);
    if(!side || side->size<sizeof(AVStereo3D))return false;
    const auto* s=reinterpret_cast<const AVStereo3D*>(side->data);
    return s->type==AV_STEREO3D_SIDEBYSIDE && s->view==AV_STEREO3D_VIEW_PACKED;
}
FramePtr pack_stereo_frames(const AVFrame& left,const AVFrame& right) {
    if(left.width!=right.width || left.height!=right.height || left.format!=right.format ||
       left.width<=0 || left.height<=0 || std::int64_t(left.width)*left.height>32*1024*1024)
        throw MediaError("Incompatible or oversized stereoscopic views");
    const auto format=AVPixelFormat(left.format);
    const auto* desc=av_pix_fmt_desc_get(format);
    if(!desc || (desc->flags&(AV_PIX_FMT_FLAG_HWACCEL|AV_PIX_FMT_FLAG_PAL)) ||
       (desc->log2_chroma_w && (left.width&((1<<desc->log2_chroma_w)-1))))
        throw MediaError("Unsupported stereoscopic pixel layout");
    FramePtr out(av_frame_alloc());if(!out)throw std::bad_alloc();
    out->width=left.width*2;out->height=left.height;out->format=left.format;
    if(av_frame_get_buffer(out.get(),32)<0 || av_frame_copy_props(out.get(),&left)<0)throw std::bad_alloc();
    const int planes=av_pix_fmt_count_planes(format);
    for(int p=0;p<planes;++p) {
        const int bytes=av_image_get_linesize(format,left.width,p);
        const int rows=(p==1 || p==2) && !(desc->flags&AV_PIX_FMT_FLAG_RGB)
            ? (left.height+(1<<desc->log2_chroma_h)-1)>>desc->log2_chroma_h : left.height;
        if(bytes<=0 || !left.data[p] || !right.data[p])throw MediaError("Invalid stereo image planes");
        for(int y=0;y<rows;++y) {
            auto* dst=out->data[p]+std::ptrdiff_t(y)*out->linesize[p];
            std::memcpy(dst,left.data[p]+std::ptrdiff_t(y)*left.linesize[p],bytes);
            std::memcpy(dst+bytes,right.data[p]+std::ptrdiff_t(y)*right.linesize[p],bytes);
        }
    }
    mark_stereo(*out);return out;
}
FramePtr StereoFrameAssembler::push(FramePtr frame) {
    const int id=view_id(*frame);
    if(id<0)throw MediaError("Multiview decoder omitted its view identifier");
    const auto pts=frame->pts;
    if(pts==AV_NOPTS_VALUE)throw MediaError("Multiview video needs presentation timestamps");
    auto match=std::find_if(pending_.begin(),pending_.end(),[&](const auto& f){return f->pts==pts && view_id(*f)!=id;});
    if(match!=pending_.end()) {
        auto other=std::move(*match);pending_.erase(match);
        const int position=eye(*frame),other_position=eye(*other);
        if(position>=0 && position==other_position)throw MediaError("Multiview eyes have conflicting metadata");
        const bool first_left=position==0 || (position<0 && (other_position==1 || (other_position<0 && id<view_id(*other))));
        return first_left?pack_stereo_frames(*frame,*other):pack_stereo_frames(*other,*frame);
    }
    if(pending_.size()>=8)throw MediaError("Multiview video has unmatched views beyond the pairing budget");
    pending_.push_back(std::move(frame));return {};
}
struct MvcDecoder::Impl {
    Edge264MvcDecoder* decoder{};
    VideoBitstream bits;
    PacketPtr packet;
    std::size_t cursor{};
    bool end_sent{}, drained{};
    int dependent{-1};
    bool bits_pending{};
    AVCodecParameters* parameters{};
    Impl(const Demuxer& input,int stream,int threads,std::int64_t max_pixels):bits(input,stream) {
        dependent=input.mvc_dependent_stream(stream);
        try{
        parameters=avcodec_parameters_alloc();if(!parameters)throw std::bad_alloc();
        if(avcodec_parameters_copy(parameters,input.codec_parameters(stream))<0)throw std::bad_alloc();
        Edge264MvcSettings settings;edge264mvc_default_settings(&settings);
        settings.n_threads=threads;settings.max_frame_pixels=int(std::min<std::int64_t>(max_pixels,32*1024*1024));
        if(edge264mvc_open(&decoder,&settings)!=EDGE264MVC_OK)throw MediaError("Cannot initialize MVC software decoder");
        for(const auto& headers:bits.configuration().codec_data){
            for(std::size_t at=0;at<headers.size();){const auto length=prefix(headers.data(),headers.size(),at);if(!length){++at;continue;}const auto begin=at+length;auto end=begin;while(end<headers.size() && !prefix(headers.data(),headers.size(),end))++end;if(end>begin && edge264mvc_send_nal(decoder,headers.data()+begin,end-begin,AV_NOPTS_VALUE,0)<EDGE264MVC_AGAIN)throw MediaError("MVC codec headers are unsupported");at=end;}
        }
        }catch(...){edge264mvc_close(&decoder);avcodec_parameters_free(&parameters);throw;}
    }
    ~Impl(){edge264mvc_close(&decoder);avcodec_parameters_free(&parameters);}
    FrameRead receive() {
        for(int work=0;work<1024;++work) {
            Edge264MvcFrame decoded{};
            const int result=edge264mvc_receive_frame(decoder,&decoded);
            if(result==EDGE264MVC_END)return {DecodeStatus::end,{}};
            if(result==EDGE264MVC_OK) {
                struct Release {Edge264MvcDecoder* decoder;Edge264MvcFrame* frame;~Release(){edge264mvc_release_frame(decoder,frame);}} release{decoder,&decoded};
                if(decoded.bit_depth_Y!=8 || decoded.bit_depth_C!=8 || decoded.width_C!=(decoded.width_Y+1)/2 || decoded.height_C!=(decoded.height_Y+1)/2)
                    throw MediaError("MVC currently requires 8-bit 4:2:0 video");
                if(!decoded.views[0].planes[0] || !decoded.views[1].planes[0])
                    throw MediaError("MVC access unit is missing one stereoscopic view");
                AVFrame views[2]{};
                for(int v=0;v<2;++v) {
                    auto& f=views[v];f.format=AV_PIX_FMT_YUV420P;f.width=decoded.width_Y;f.height=decoded.height_Y;
                    f.pts=f.best_effort_timestamp=decoded.views[v].pts;
                    f.color_primaries=parameters->color_primaries;f.color_trc=parameters->color_trc;
                    f.colorspace=parameters->color_space;f.color_range=parameters->color_range;f.sample_aspect_ratio=parameters->sample_aspect_ratio;
                    for(int p=0;p<3;++p){f.data[p]=const_cast<std::uint8_t*>(decoded.views[v].planes[p]);f.linesize[p]=p?decoded.stride_C:decoded.stride_Y;}
                }
                return {DecodeStatus::frame,pack_stereo_frames(views[0],views[1])};
            }
            if(result!=EDGE264MVC_AGAIN)throw MediaError("MVC decoder failed to receive a frame");
            if(!packet) {
                auto next=bits.receive();
                if(next.status==ReadStatus::again)return {DecodeStatus::again,{}};
                if(next.status==ReadStatus::end) {
                    if(!drained){edge264mvc_send_end(decoder);drained=true;continue;}
                    return {DecodeStatus::again,{}};
                }
                packet=std::move(next.packet);cursor=0;bits_pending=false;
                if(packet->size<0 || packet->size>32*1024*1024)throw MediaError("MVC access unit exceeds the input budget");
            }
            const auto size=std::size_t(packet->size);
            while(cursor<size && !prefix(packet->data,size,cursor))++cursor;
            if(cursor>=size){packet.reset();continue;}
            const auto begin=cursor+prefix(packet->data,size,cursor);
            auto end=begin;while(end<size && !prefix(packet->data,size,end))++end;
            if(end==begin){cursor=end;continue;}
            const int accepted=edge264mvc_send_nal(decoder,packet->data+begin,end-begin,packet->pts,0);
            if(accepted==EDGE264MVC_AGAIN)continue;
            if(accepted==EDGE264MVC_NOMEM)throw std::bad_alloc();
            if(accepted==EDGE264MVC_INVALID)throw MediaError("MVC decoder rejected the access unit");
            const auto type=packet->data[begin]&31;
            if(accepted==EDGE264MVC_UNSUPPORTED && (type==1 || type==5 || type==7 || type==8 || type==15 || type==20))
                throw MediaError("Unsupported MVC profile, frame size or interlaced coding");
            cursor=end;if(cursor==size)packet.reset();
        }
        return {DecodeStatus::again,{}};
    }
};
MvcDecoder::MvcDecoder(const Demuxer& d,int stream,int threads,std::int64_t max_pixels):impl_(std::make_unique<Impl>(d,stream,threads,max_pixels)){}
MvcDecoder::~MvcDecoder()=default;
bool MvcDecoder::send(const AVPacket* packet) {
    if(impl_->packet || impl_->bits_pending)return false;
    if(packet && packet->stream_index==impl_->dependent){
        impl_->packet.reset(av_packet_clone(packet));if(!impl_->packet)throw std::bad_alloc();
        if(packet->size<0 || packet->size>32*1024*1024)throw MediaError("MVC dependent access unit exceeds the input budget");
        impl_->cursor=0;return true;
    }
    const bool accepted=impl_->bits.send(packet);
    if(accepted && packet)impl_->bits_pending=true;
    if(accepted && !packet)impl_->end_sent=true;
    return accepted;
}
FrameRead MvcDecoder::receive(){return impl_->receive();}
void MvcDecoder::flush(){impl_->bits.flush();impl_->packet.reset();impl_->cursor=0;impl_->drained=impl_->end_sent=impl_->bits_pending=false;edge264mvc_flush(impl_->decoder);}
}
