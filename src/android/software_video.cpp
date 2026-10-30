#include "cyvr/player/android/software_video.hpp"
#include <algorithm>
#include <unistd.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
namespace cyvr::player::android {
struct SoftwareVideo::Impl {
  struct Buffer {
    AHardwareBuffer* value{};
    ~Buffer(){if(value)AHardwareBuffer_release(value);}
  };
  const Demuxer& input;int stream;
  SoftwareDecoder decoder;
  VideoColor color;
  std::uint64_t epoch;
  std::int64_t next_pts{},period{40000};
  bool end{};
  FramePtr pending;
  SwsContext* scale{};
  int width{},height{},format{};
  std::vector<std::shared_ptr<Buffer>> pool;
  Impl(const Demuxer& in,int track,VideoColor c,std::uint64_t e,std::int64_t start):input(in),stream(track),decoder(in,track,4),color(c),epoch(e),next_pts(start){for(const auto& t:in.tracks())if(t.index==track && t.nominal_fps>0)period=std::max<std::int64_t>(1,std::int64_t(1000000/t.nominal_fps));}
  ~Impl(){sws_freeContext(scale);}
  ImageLease receive(){
    if(!pending){auto result=decoder.receive();end=result.status==DecodeStatus::end;pending=std::move(result.frame);}
    if(!pending)return {};
    const auto* description=av_pix_fmt_desc_get(AVPixelFormat(pending->format));
    if(!description)throw MediaError("Software decoder returned an unknown pixel format");
    const bool deep=description->comp[0].depth>8;
    const int target=deep ? AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    if(width!=pending->width || height!=pending->height || format!=target){pool.clear();width=pending->width;height=pending->height;format=target;}
    std::shared_ptr<Buffer> buffer;
    for(const auto& candidate:pool)if(candidate.use_count()==1){buffer=candidate;break;}
    if(!buffer && pool.size()<4){
      buffer=std::make_shared<Buffer>();
      AHardwareBuffer_Desc desc{};desc.width=width;desc.height=height;desc.layers=1;desc.format=target;
      desc.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
      if(AHardwareBuffer_allocate(&desc,&buffer->value))throw MediaError("Software video texture allocation failed");
      pool.push_back(buffer);
    }
    if(!buffer)return {};
    const auto output=deep ? AV_PIX_FMT_X2BGR10LE : AV_PIX_FMT_RGBA;
    scale=sws_getCachedContext(scale,width,height,AVPixelFormat(pending->format),width,height,output,SWS_BILINEAR,nullptr,nullptr,nullptr);
    if(!scale)throw MediaError("Software video color conversion is unavailable");
    const int matrix=pending->colorspace==AVCOL_SPC_BT2020_NCL || pending->colorspace==AVCOL_SPC_BT2020_CL ? SWS_CS_BT2020 : pending->colorspace==AVCOL_SPC_BT470BG || pending->colorspace==AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601 : SWS_CS_ITU709;
    sws_setColorspaceDetails(scale,sws_getCoefficients(matrix),pending->color_range==AVCOL_RANGE_JPEG,sws_getCoefficients(matrix),1,0,1<<16,1<<16);
    AHardwareBuffer_Desc desc{};AHardwareBuffer_describe(buffer->value,&desc);
    void* memory{};
    if(AHardwareBuffer_lock(buffer->value,AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,-1,nullptr,&memory))throw MediaError("Software video texture lock failed");
    std::uint8_t* output_planes[]={static_cast<std::uint8_t*>(memory),nullptr,nullptr,nullptr};
    int strides[]={int(desc.stride)*4,0,0,0};
    const int result=sws_scale(scale,pending->data,pending->linesize,0,height,output_planes,strides);
    if(deep)for(int y=0;y<height;++y){auto* row=reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(memory)+std::size_t(y)*strides[0]);for(int x=0;x<width;++x)row[x]|=0xc0000000u;}
    // Complete CPU writes before publishing or returning this bounded pool slot.
    int fence=-1;const int unlocked=AHardwareBuffer_unlock(buffer->value,nullptr);
    if(result!=height || unlocked){if(fence>=0)::close(fence);throw MediaError("Software video texture conversion failed");}
    auto pts=input.timestamp_us(stream,pending->best_effort_timestamp);
    if(!pts)pts=input.timestamp_us(stream,pending->pts);
    if(!pts)pts=next_pts;
    next_pts=*pts+period;
    auto frame_color=color;
    if(pending->color_trc==AVCOL_TRC_SMPTE2084)frame_color.transfer=6;
    if(pending->color_trc==AVCOL_TRC_ARIB_STD_B67)frame_color.transfer=7;
    if(pending->color_primaries==AVCOL_PRI_BT2020)frame_color.standard=6;
    pending.reset();
    return ImageLease::from_buffer(buffer->value,fence,buffer,*pts,epoch,frame_color);
  }
};
SoftwareVideo::SoftwareVideo(const Demuxer& input,int stream,VideoColor color,std::uint64_t epoch,std::int64_t start):impl_(std::make_unique<Impl>(input,stream,color,epoch,start)){}
SoftwareVideo::~SoftwareVideo()=default;
bool SoftwareVideo::send(const AVPacket* packet){return !impl_->pending && impl_->decoder.send(packet);}
ImageLease SoftwareVideo::receive(){return impl_->receive();}
bool SoftwareVideo::ended()const{return impl_->end;}
}
