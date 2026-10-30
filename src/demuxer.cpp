#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/multiview.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/stereo3d.h>
#include <libavutil/spherical.h>
}
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cctype>
#include <limits>
#include <mutex>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cyvr::player {
namespace {
constexpr AVRational microseconds{1,1000000};
std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
[[noreturn]] void fail(const char* operation,int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code,message,sizeof message);
    throw MediaError(std::string(operation)+": "+message);
}
std::string tag(AVDictionary* metadata,const char* key) {
    auto* value=av_dict_get(metadata,key,nullptr,0);
    if(!value || !value->value)return {};
    std::string result=value->value;
    if(result.size()>1024){std::size_t end=1024;while(end>0 && (static_cast<unsigned char>(result[end])&0xc0)==0x80)--end;result.resize(end);}
    return result;
}
TrackKind track_kind(AVMediaType value) {
    if(value==AVMEDIA_TYPE_VIDEO) return TrackKind::video;
    if(value==AVMEDIA_TYPE_AUDIO) return TrackKind::audio;
    if(value==AVMEDIA_TYPE_SUBTITLE) return TrackKind::subtitle;
    return TrackKind::other;
}
struct Dictionary {
    AVDictionary* value=nullptr;
    ~Dictionary(){ av_dict_free(&value); }
    void set(const char* key,const char* text){ if(av_dict_set(&value,key,text,0)<0) throw std::bad_alloc(); }
    void integer(const char* key,std::int64_t n){ if(av_dict_set_int(&value,key,n,0)<0) throw std::bad_alloc(); }
};
}
void PacketDeleter::operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
void FrameDeleter::operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }

struct Demuxer::Impl {
    AVFormatContext* format=nullptr;
    AVIOContext* io=nullptr;
    std::shared_ptr<Cancellation> cancel;
    std::int64_t deadline=0;
    StreamOptions options;
    std::vector<TrackInfo> tracks;
    std::vector<ChapterInfo> chapters;
    PacketPtr prefetched;
    std::int64_t transport_origin=AV_NOPTS_VALUE;
    int fd=-1;
    std::int64_t offset=0,length=-1,position=0;
    bool fd_seekable=false;
    ~Impl(){
        avformat_close_input(&format);
        if(io){ av_freep(&io->buffer); avio_context_free(&io); }
        if(fd>=0){
#ifdef _WIN32
            _close(fd);
#else
            ::close(fd);
#endif
        }
    }
    static int interrupt(void* opaque){
        auto& self=*static_cast<Impl*>(opaque);
        return (self.cancel && self.cancel->cancelled()) || (self.deadline && now_ns()>=self.deadline);
    }
    void arm(int milliseconds){ deadline=now_ns()+std::int64_t(milliseconds)*1000000; }
    void check(int result,const char* operation){
        if(result>=0) return;
        if(cancel->cancelled()) throw MediaError(std::string(operation)+": cancelled");
        if(deadline && now_ns()>=deadline) throw MediaError(std::string(operation)+": timed out");
        fail(operation,result);
    }
    AVStream* stream(int index) const {
        if(!format || index<0 || unsigned(index)>=format->nb_streams) throw std::invalid_argument("Invalid stream index");
        return format->streams[index];
    }
    static int read_fd(void* opaque,std::uint8_t* data,int count){
        auto& self=*static_cast<Impl*>(opaque);
        if(interrupt(opaque)) return AVERROR_EXIT;
        if(self.length>=0){
            if(self.position>=self.length) return AVERROR_EOF;
            count=int(std::min<std::int64_t>(count,self.length-self.position));
        }
#ifdef _WIN32
        if(self.fd_seekable && _lseeki64(self.fd,self.offset+self.position,SEEK_SET)<0) return AVERROR(errno);
        const int read=_read(self.fd,data,unsigned(count));
#else
        ssize_t read=0;
        if(self.fd_seekable) read=pread(self.fd,data,std::size_t(count),self.offset+self.position);
        else {
            pollfd event{self.fd,POLLIN,0};
            for(;;){
                if(interrupt(opaque)) return AVERROR_EXIT;
                const int ready=poll(&event,1,20);
                if(ready>0) break;
                if(ready<0 && errno!=EINTR) return AVERROR(errno);
            }
            read=::read(self.fd,data,std::size_t(count));
        }
#endif
        if(read<0) return (errno==EINTR) ? AVERROR(EAGAIN) : AVERROR(errno);
        self.position+=read;
        return read ? int(read) : AVERROR_EOF;
    }
    static std::int64_t seek_fd(void* opaque,std::int64_t offset,int whence){
        auto& self=*static_cast<Impl*>(opaque);
        if(whence==AVSEEK_SIZE) return self.length>=0 ? self.length : AVERROR(ENOSYS);
        whence &= ~AVSEEK_FORCE;
        if(!self.fd_seekable) return AVERROR(ENOSYS);
        std::int64_t base=0;
        if(whence==SEEK_CUR) base=self.position;
        else if(whence==SEEK_END && self.length>=0) base=self.length;
        else if(whence!=SEEK_SET) return AVERROR(EINVAL);
        if((offset>0 && base>INT64_MAX-offset) || (offset<0 && offset < -base)) return AVERROR(EINVAL);
        const auto next=base+offset;
        if(next<0 || next>INT64_MAX-self.offset) return AVERROR(EINVAL);
        self.position=next;
        return next;
    }
    void attach_fd(const MediaSource& source){
        offset=source.offset; length=source.length;
#ifdef _WIN32
        fd=_dup(source.fd);
        if(fd>=0){
            fd_seekable=_lseeki64(fd,0,SEEK_CUR)>=0;
            struct _stat64 info{};
            if(length<0 && _fstat64(fd,&info)==0 && (info.st_mode&_S_IFREG)) length=std::max<std::int64_t>(0,info.st_size-offset);
        }
#else
        fd=dup(source.fd);
        if(fd>=0){
            fd_seekable=lseek(fd,0,SEEK_CUR)>=0;
            struct stat info{};
            if(length<0 && fstat(fd,&info)==0 && S_ISREG(info.st_mode)) length=std::max<std::int64_t>(0,info.st_size-offset);
        }
#endif
        if(fd<0) fail("Duplicate document descriptor",AVERROR(errno));
        if(!fd_seekable && offset) throw MediaError("A pipe cannot have a document slice offset");
        auto* buffer=static_cast<unsigned char*>(av_malloc(65536));
        if(!buffer) throw std::bad_alloc();
        io=avio_alloc_context(buffer,65536,0,this,read_fd,nullptr,seek_fd);
        if(!io){ av_free(buffer); throw std::bad_alloc(); }
        io->seekable=fd_seekable ? AVIO_SEEKABLE_NORMAL : 0;
        format->pb=io;
        format->flags|=AVFMT_FLAG_CUSTOM_IO;
    }
};

Demuxer::Demuxer()=default;
Demuxer::~Demuxer()=default;
void Demuxer::close() noexcept { impl_.reset(); }
void Demuxer::open(const MediaSource& source,const StreamOptions& options,std::shared_ptr<Cancellation> cancel){
    options.validate();
    if(!cancel) throw std::invalid_argument("Cancellation token is null");
    if(cancel->cancelled()) throw MediaError("Open media: cancelled");
    static std::once_flag initialized;
    std::call_once(initialized,[]{ if(avformat_network_init()<0) throw MediaError("Network initialization failed"); });
    auto next=std::make_unique<Impl>();
    next->cancel=std::move(cancel); next->options=options;
    next->format=avformat_alloc_context();
    if(!next->format) throw std::bad_alloc();
    next->format->interrupt_callback={Impl::interrupt,next.get()};
    next->format->probesize=options.probe_bytes;
    next->format->max_analyze_duration=options.analyze_us;
    next->format->max_streams=256;
    next->format->flags|=AVFMT_FLAG_GENPTS;
    if(options.low_latency) next->format->flags|=AVFMT_FLAG_NOBUFFER;
    Dictionary settings;
    settings.integer("rw_timeout",std::int64_t(options.read_timeout_ms)*1000);
    settings.set("tls_verify","1");
    if(!options.ca_file.empty()) settings.set("ca_file",options.ca_file.c_str());
    if(options.rtsp_over_tcp) settings.set("rtsp_transport","tcp");
    if(!options.clear_keys.empty()) {
        std::string keys;
        const auto hex=[](const auto& bytes){std::string out;for(auto b:bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;};
        for(const auto& key:options.clear_keys){if(!keys.empty())keys+=':';keys+=hex(key.id)+'='+hex(key.key);}
        settings.set("decryption_keys",keys.c_str());
    }
    if(source.kind==SourceKind::network){
        // Nested HLS/DASH requests cannot escape into local files or devices.
        settings.set("protocol_whitelist","http,https,tcp,tls,rtp,udp,crypto,srt");
        settings.set("protocol_blacklist","file,subfile,concat,concatf,pipe,fd");
    }
    if(source.kind==SourceKind::descriptor) next->attach_fd(source);
    next->arm(options.open_timeout_ms);
    const auto location_text=source.kind==SourceKind::file
        ? "file:"+std::filesystem::absolute(std::filesystem::u8path(source.location)).generic_u8string()
        : source.location;
    const char* location=source.kind==SourceKind::descriptor ? nullptr : location_text.c_str();
    const AVInputFormat* hint=nullptr;
    if(source.kind==SourceKind::file){
        auto extension=std::filesystem::u8path(source.location).extension().u8string();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(extension==".264" || extension==".h264")hint=av_find_input_format("h264");
        if(extension==".265" || extension==".h265" || extension==".hevc")hint=av_find_input_format("hevc");
        if(extension==".ts" || extension==".mts" || extension==".m2ts" || extension==".ssif")hint=av_find_input_format("mpegts");
    }
    next->check(avformat_open_input(&next->format,location,hint,&settings.value),"Open media");
    // The AVC parser cannot delimit MVC extension slices. MPEG-TS PES packets
    // already carry access units; retaining them keeps the dependent PID in
    // demux order instead of collecting it into one late packet at EOF.
    bool transport_mvc=false;
    if(next->format->iformat && std::string(next->format->iformat->name)=="mpegts"){
        bool mvc=false;for(unsigned i=0;i<next->format->nb_streams;++i)mvc|=next->format->streams[i]->codecpar->codec_tag==0x20;
        if(mvc){next->format->flags|=AVFMT_FLAG_NOPARSE|AVFMT_FLAG_NOFILLIN;transport_mvc=true;}
    }
    next->check(avformat_find_stream_info(next->format,nullptr),"Probe media");
    if(transport_mvc && next->format->start_time==AV_NOPTS_VALUE){
        next->prefetched.reset(av_packet_alloc());if(!next->prefetched)throw std::bad_alloc();
        const int result=av_read_frame(next->format,next->prefetched.get());
        if(result>=0){const auto* p=next->prefetched.get();const auto ts=p->dts!=AV_NOPTS_VALUE?p->dts:p->pts;if(ts!=AV_NOPTS_VALUE)next->transport_origin=av_rescale_q(ts,next->stream(p->stream_index)->time_base,microseconds);}
        else if(result==AVERROR_EOF || result==AVERROR(EAGAIN))next->prefetched.reset();
        else next->check(result,"Read transport timestamp");
    }
    for(unsigned i=0;i<next->format->nb_streams;++i){
        auto* stream=next->format->streams[i];
        const auto* p=stream->codecpar;
        TrackInfo track;
        track.index=int(i); track.kind=track_kind(p->codec_type); track.codec=avcodec_get_name(p->codec_id);
        track.language=tag(stream->metadata,"language"); track.title=tag(stream->metadata,"title");
        track.default_track=stream->disposition&AV_DISPOSITION_DEFAULT;
        track.forced=stream->disposition&AV_DISPOSITION_FORCED;
        track.attached_picture=stream->disposition&AV_DISPOSITION_ATTACHED_PIC;
        track.hearing_impaired=stream->disposition&AV_DISPOSITION_HEARING_IMPAIRED;
        track.visual_impaired=stream->disposition&AV_DISPOSITION_VISUAL_IMPAIRED;
        track.width=p->width; track.height=p->height; track.sample_rate=p->sample_rate; track.channels=p->ch_layout.nb_channels;
        if(p->codec_type==AVMEDIA_TYPE_VIDEO){auto rate=av_guess_frame_rate(next->format,stream,nullptr);if(rate.num>0&&rate.den>0 && av_q2d(rate)>240)rate=stream->avg_frame_rate;if(rate.num>0&&rate.den>0 && av_q2d(rate)<=240)track.nominal_fps=av_q2d(rate);}
        if(stream->duration!=AV_NOPTS_VALUE) track.duration_us=av_rescale_q(stream->duration,stream->time_base,microseconds);
        if(track.kind==TrackKind::video){
            auto& hints=track.layout;
            // Container StereoMode / st3d and spherical sv3d metadata are
            // exposed by FFmpeg as codec-parameter side data after probing.
            if(const auto* data=av_packet_side_data_get(p->coded_side_data,p->nb_coded_side_data,AV_PKT_DATA_STEREO3D);data && data->size>=sizeof(AVStereo3D)){
                const auto* stereo=reinterpret_cast<const AVStereo3D*>(data->data);
                if(stereo->type==AV_STEREO3D_2D || stereo->view==AV_STEREO3D_VIEW_LEFT || stereo->view==AV_STEREO3D_VIEW_RIGHT)hints.stereo=0;
                else if(stereo->type==AV_STEREO3D_SIDEBYSIDE)hints.stereo=1;
                else if(stereo->type==AV_STEREO3D_TOPBOTTOM)hints.stereo=2;
                if(hints.stereo)hints.swap=bool(stereo->flags&AV_STEREO3D_FLAG_INVERT);
            }
            if(const auto* data=av_packet_side_data_get(p->coded_side_data,p->nb_coded_side_data,AV_PKT_DATA_SPHERICAL);data && data->size>=sizeof(AVSphericalMapping)){
                const auto* sphere=reinterpret_cast<const AVSphericalMapping*>(data->data);
                if(sphere->projection==AV_SPHERICAL_EQUIRECTANGULAR)hints.projection=2;
                else if(sphere->projection==AV_SPHERICAL_HALF_EQUIRECTANGULAR)hints.projection=1;
                else if(sphere->projection==AV_SPHERICAL_RECTILINEAR)hints.projection=0;
                else if(sphere->projection==AV_SPHERICAL_EQUIRECTANGULAR_TILE && !sphere->bound_top && !sphere->bound_bottom && std::abs(double(sphere->bound_left)+sphere->bound_right-2147483648.)<2)hints.projection=1;
                else hints.unsupported_projection=true;
            }
            auto normal=[](std::string value){std::string result;for(unsigned char c:value)if(c<128 && std::isalnum(c))result+=char(std::tolower(c));return result;};
            auto stereo=normal(tag(stream->metadata,"stereo_mode"));
            if(!hints.stereo){
                if(stereo=="mono" || stereo=="2d")hints.stereo=0;
                else if(stereo=="leftright" || stereo=="sidebyside" || stereo=="rightleft"){hints.stereo=1;hints.swap=stereo=="rightleft";}
                else if(stereo=="topbottom" || stereo=="bottomtop"){hints.stereo=2;hints.swap=stereo=="bottomtop";}
            }
            auto projection=normal(tag(stream->metadata,"projection"));if(projection.empty())projection=normal(tag(stream->metadata,"projection_type"));
            if(!hints.projection && !hints.unsupported_projection){
                if(projection=="equirectangular" || projection=="360" || projection=="vr360")hints.projection=2;
                else if(projection=="180" || projection=="vr180" || projection=="halfequirectangular")hints.projection=1;
                else if(projection=="rectilinear" || projection=="flat")hints.projection=0;
                else if(!projection.empty())hints.unsupported_projection=true;
            }
        }
        next->tracks.push_back(std::move(track));
    }
    for(unsigned i=0;i<next->format->nb_chapters && i<10000;++i){
        const auto* c=next->format->chapters[i];
        next->chapters.push_back({av_rescale_q(c->start,c->time_base,microseconds),av_rescale_q(c->end,c->time_base,microseconds),tag(c->metadata,"title")});
    }
    impl_=std::move(next);
    for(auto& track:impl_->tracks)if(track.kind==TrackKind::video){const int dependent=mvc_dependent_stream(track.index);if(dependent>=0)impl_->tracks[std::size_t(dependent)].companion_video=track.index;}
}
const std::vector<ChapterInfo>& Demuxer::chapters() const {if(!impl_)throw std::logic_error("Media is closed");return impl_->chapters;}
std::vector<FontAttachment> Demuxer::fonts() const {
    if(!impl_)throw std::logic_error("Media is closed");
    std::vector<FontAttachment> result;std::size_t total=0;
    for(unsigned i=0;i<impl_->format->nb_streams;++i){
        const auto* s=impl_->format->streams[i];const auto* p=s->codecpar;
        if(p->codec_type!=AVMEDIA_TYPE_ATTACHMENT || (p->codec_id!=AV_CODEC_ID_TTF && p->codec_id!=AV_CODEC_ID_OTF))continue;
        if(p->extradata_size<=0 || std::size_t(p->extradata_size)>32*1024*1024-total)continue;
        total+=std::size_t(p->extradata_size);
        result.push_back({tag(s->metadata,"filename"),{p->extradata,p->extradata+p->extradata_size}});
    }
    return result;
}
const std::vector<TrackInfo>& Demuxer::tracks() const {
    if(!impl_) throw std::logic_error("Media is closed");
    return impl_->tracks;
}
std::optional<std::int64_t> Demuxer::duration_us() const {
    if(!impl_) throw std::logic_error("Media is closed");
    if(impl_->format->duration==AV_NOPTS_VALUE) return {};
    return impl_->format->duration;
}
bool Demuxer::seekable() const {
    if(!impl_) throw std::logic_error("Media is closed");
    const auto* format=impl_->format;
    if(format->ctx_flags&AVFMTCTX_UNSEEKABLE) return false;
    if(format->pb && (format->pb->seekable&AVIO_SEEKABLE_NORMAL)) return true;
    // HLS/DASH implement temporal seeking by opening segments, independently
    // of whether the manifest HTTP response supports byte-range seeking.
    const std::string name=format->iformat ? format->iformat->name : "";
    return (name=="hls" || name=="dash") && format->duration!=AV_NOPTS_VALUE && format->duration>0;
}
PacketRead Demuxer::read(){
    if(!impl_) throw std::logic_error("Media is closed");
    if(impl_->cancel->cancelled()) throw MediaError("Read media: cancelled");
    if(impl_->prefetched)return {ReadStatus::packet,std::move(impl_->prefetched)};
    PacketPtr packet(av_packet_alloc());
    if(!packet) throw std::bad_alloc();
    impl_->arm(impl_->options.read_timeout_ms);
    const int result=av_read_frame(impl_->format,packet.get());
    if(result==AVERROR_EOF) return {ReadStatus::end,{}};
    if(result==AVERROR(EAGAIN)) return {ReadStatus::again,{}};
    impl_->check(result,"Read media");
    return {ReadStatus::packet,std::move(packet)};
}
std::int64_t Demuxer::origin_us() const {
    if(!impl_) throw std::logic_error("Media is closed");
    return impl_->format->start_time!=AV_NOPTS_VALUE ? impl_->format->start_time : impl_->transport_origin!=AV_NOPTS_VALUE?impl_->transport_origin:0;
}
void Demuxer::seek(std::int64_t position){
    if(!impl_) throw std::logic_error("Media is closed");
    if(impl_->cancel->cancelled()) throw MediaError("Seek media: cancelled");
    const auto origin=origin_us();
    if(position<0 || (origin>0 && position>INT64_MAX-origin)) throw std::invalid_argument("Invalid seek timestamp");
    if(!seekable()) throw MediaError("This source does not support seeking");
    impl_->arm(impl_->options.open_timeout_ms);
    impl_->check(avformat_seek_file(impl_->format,-1,INT64_MIN,origin+position,origin+position,0),"Seek media");
    impl_->prefetched.reset();
}
const AVCodecParameters* Demuxer::codec_parameters(int stream) const {
    if(!impl_) throw std::logic_error("Media is closed");
    return impl_->stream(stream)->codecpar;
}
int Demuxer::time_base_numerator(int stream) const {
    if(!impl_) throw std::logic_error("Media is closed");
    return impl_->stream(stream)->time_base.num;
}
int Demuxer::time_base_denominator(int stream) const {
    if(!impl_) throw std::logic_error("Media is closed");
    return impl_->stream(stream)->time_base.den;
}
std::optional<std::int64_t> Demuxer::timestamp_us(int stream,std::int64_t timestamp) const {
    if(timestamp==AV_NOPTS_VALUE) return {};
    if(!impl_) throw std::logic_error("Media is closed");
    return av_rescale_q(timestamp,impl_->stream(stream)->time_base,microseconds)-origin_us();
}
int Demuxer::best_video_stream() const {
    if(!impl_) throw std::logic_error("Media is closed");
    return av_find_best_stream(impl_->format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
}
int Demuxer::best_audio_stream() const {
    if(!impl_) throw std::logic_error("Media is closed");
    return av_find_best_stream(impl_->format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
}
int Demuxer::mvc_dependent_stream(int base)const{
    if(!impl_)throw std::logic_error("Media is closed");
    const auto* parameters=impl_->stream(base)->codecpar;
    if(parameters->codec_id!=AV_CODEC_ID_H264 || parameters->codec_tag!=0x1b)return -1;
    const auto* format=impl_->format;
    for(unsigned p=0;p<format->nb_programs;++p){
        const auto* program=format->programs[p];
        if(std::find(program->stream_index,program->stream_index+program->nb_stream_indexes,unsigned(base))==program->stream_index+program->nb_stream_indexes)continue;
        int candidate=-1;
        unsigned bases=0;
        for(unsigned n=0;n<program->nb_stream_indexes;++n)if(impl_->stream(int(program->stream_index[n]))->codecpar->codec_tag==0x1b)++bases;
        if(bases!=1)return -1;
        for(unsigned n=0;n<program->nb_stream_indexes;++n){const int i=int(program->stream_index[n]);const auto* other=impl_->stream(i)->codecpar;if(other->codec_id==AV_CODEC_ID_H264 && other->codec_tag==0x20){if(candidate>=0)return -1;candidate=i;}}
        return candidate;
    }
    return -1;
}

struct SoftwareDecoder::Impl {
    AVCodecContext* codec=nullptr;
    std::unique_ptr<MvcDecoder> mvc;
    StereoFrameAssembler views;
    bool multiview{};
    ~Impl(){ avcodec_free_context(&codec); }
};
SoftwareDecoder::SoftwareDecoder(const Demuxer& demuxer,int stream,int threads,std::int64_t max_pixels,bool decode_views) : impl_(std::make_unique<Impl>()) {
    if(threads<1 || threads>8) throw std::invalid_argument("Decoder thread budget must be 1..8");
    const auto* parameters=demuxer.codec_parameters(stream);
    if(max_pixels<1||max_pixels>64*1024*1024)throw std::invalid_argument("Invalid decoder pixel budget");
    if(decode_views && parameters->codec_id==AV_CODEC_ID_H264) {
        impl_->mvc=std::make_unique<MvcDecoder>(demuxer,stream,threads,max_pixels);return;
    }
    const auto* decoder=parameters->codec_id==AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d") : nullptr;
    if(!decoder)decoder=avcodec_find_decoder(parameters->codec_id);
    if(!decoder) throw MediaError("No software decoder in this FFmpeg build");
    impl_->codec=avcodec_alloc_context3(decoder);
    if(!impl_->codec) throw std::bad_alloc();
    int result=avcodec_parameters_to_context(impl_->codec,parameters);
    if(result<0) fail("Configure software decoder",result);
    impl_->codec->pkt_timebase={demuxer.time_base_numerator(stream),demuxer.time_base_denominator(stream)};
    impl_->codec->thread_count=threads;
    impl_->codec->max_pixels=max_pixels;
    impl_->multiview=decode_views && parameters->codec_id==AV_CODEC_ID_HEVC;
    if(impl_->multiview && av_opt_set(impl_->codec->priv_data,"view_ids","-1",0)<0)
        throw MediaError("This HEVC decoder cannot output stereoscopic views");
    result=avcodec_open2(impl_->codec,decoder,nullptr);
    if(result<0) fail("Open software decoder",result);
}
SoftwareDecoder::~SoftwareDecoder()=default;
bool SoftwareDecoder::send(const AVPacket* packet){
    if(impl_->mvc)return impl_->mvc->send(packet);
    const auto result=avcodec_send_packet(impl_->codec,packet);
    if(result==AVERROR(EAGAIN)) return false;
    if(result<0) fail("Send decoder packet",result);
    return true;
}
FrameRead SoftwareDecoder::receive(){
    if(impl_->mvc)return impl_->mvc->receive();
    for(int attempt=0;attempt<16;++attempt){
    FramePtr frame(av_frame_alloc());
    if(!frame) throw std::bad_alloc();
    const auto result=avcodec_receive_frame(impl_->codec,frame.get());
    if(result==AVERROR(EAGAIN)) return {DecodeStatus::again,{}};
    if(result==AVERROR_EOF) {
        if(!impl_->views.empty())throw MediaError("Multiview video ended with unmatched eyes");
        return {DecodeStatus::end,{}};
    }
    if(result<0) fail("Receive decoded frame",result);
    if(impl_->multiview){frame=impl_->views.push(std::move(frame));if(!frame)continue;}
    return {DecodeStatus::frame,std::move(frame)};
    }
    return {DecodeStatus::again,{}};
}
void SoftwareDecoder::flush(){ if(impl_->mvc)impl_->mvc->flush();else avcodec_flush_buffers(impl_->codec);impl_->views.clear(); }
} // namespace cyvr::player

