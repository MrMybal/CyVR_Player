#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/file_browser.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
using namespace cyvr::player;
namespace {
std::string json(const std::string& text){
    std::string result="\"";
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:text){
        if(c=='"' || c=='\\'){ result+='\\'; result+=char(c); }
        else if(c<32){ result+="\\u00"; result+=hex[c>>4]; result+=hex[c&15]; }
        else result+=char(c);
    }
    return result+'"';
}
const char* track_name(TrackKind kind){
    switch(kind){ case TrackKind::video:return "video"; case TrackKind::audio:return "audio";
        case TrackKind::subtitle:return "subtitle"; default:return "other"; }
}
struct CancelAfter {
    std::mutex mutex;
    std::condition_variable event;
    bool finished=false;
    std::thread worker;
    CancelAfter(std::shared_ptr<Cancellation> token,int ms){
        if(ms>0) worker=std::thread([this,token,ms]{
            std::unique_lock<std::mutex> lock(mutex);
            if(!event.wait_for(lock,std::chrono::milliseconds(ms),[this]{return finished;})) token->cancel();
        });
    }
    ~CancelAfter(){
        {std::lock_guard<std::mutex> lock(mutex);finished=true;}
        event.notify_all();
        if(worker.joinable()) worker.join();
    }
};
struct FdOwner {
    int value=-1;
    ~FdOwner(){ if(value>=0){
#ifdef _WIN32
        _close(value);
#else
        ::close(value);
#endif
    }}
};
}
int main(int argc,char** argv){
    // Library clients control their own logging. This CLI emits sanitized JSON;
    // FFmpeg diagnostics may include URL secrets, so suppress its stderr logger.
    av_log_set_level(AV_LOG_QUIET);
    try {
        std::string input,listing;
        bool decode=false,use_fd=false;
        std::int64_t seek=-1,slice_offset=0,slice_length=-1;
        int packets_limit=4096,cancel_ms=0;
        StreamOptions options;
        for(int i=1;i<argc;++i){
            const std::string arg=argv[i];
            auto value=[&](){ if(i+1>=argc) throw std::invalid_argument("Missing argument value"); return std::string(argv[++i]); };
            if(arg=="--decode") decode=true;
            else if(arg=="--fd") use_fd=true;
            else if(arg=="--slice-offset") slice_offset=std::stoll(value());
            else if(arg=="--slice-length") slice_length=std::stoll(value());
            else if(arg=="--packets") packets_limit=std::stoi(value());
            else if(arg=="--seek-us") seek=std::stoll(value());
            else if(arg=="--disc-title") options.disc_title=std::stoi(value());
            else if(arg=="--disc-angle") options.disc_angle=std::stoi(value());
            else if(arg=="--timeout-ms"){options.open_timeout_ms=options.read_timeout_ms=std::stoi(value());}
            else if(arg=="--cancel-after-ms") cancel_ms=std::stoi(value());
            else if(arg=="--ca-file") options.ca_file=value();
            else if(arg=="--http-header") options.http_headers+=value()+"\r\n";
            else if(arg=="--http-cookie") options.http_cookies+=value()+"\n";
            else if(arg=="--list") listing=value();
            else if(arg.rfind("--",0)==0) throw std::invalid_argument("Unknown argument");
            else if(input.empty()) input=arg;
            else throw std::invalid_argument("Only one media source is accepted");
        }
        if(!listing.empty()){
            const auto result=FileBrowser(std::filesystem::u8path(listing)).list();
            std::cout<<"{\"entries\":[";
            bool comma=false;
            for(const auto& entry:result.entries){
                if(comma) std::cout<<',';
                comma=true;
                std::cout<<"{\"name\":"<<json(entry.name)<<",\"directory\":"<<(entry.kind==FileKind::directory?"true":"false")<<'}';
            }
            std::cout<<"],\"truncated\":"<<(result.truncated?"true":"false")<<"}\n";
            return 0;
        }
        if(input.empty() || packets_limit<1 || packets_limit>1000000)
            throw std::invalid_argument("Usage: cyvr_media_probe FILE_OR_URL [--decode] [--packets N] [--seek-us N] [--fd]");
        auto token=std::make_shared<Cancellation>();
        CancelAfter cancel_after(token,cancel_ms);
        auto source=input.find("://")!=std::string::npos ? MediaSource::network(input) : MediaSource::file(input);
        Demuxer demuxer;
        if(use_fd){
            FdOwner fd;
#ifdef _WIN32
            _wsopen_s(&fd.value,std::filesystem::u8path(input).c_str(),_O_RDONLY|_O_BINARY,_SH_DENYNO,0);
#else
            fd.value=::open(input.c_str(),O_RDONLY);
#endif
            if(fd.value<0) throw MediaError("Cannot open source descriptor");
            source=MediaSource::descriptor(fd.value,slice_offset,slice_length);
            demuxer.open(source,options,token);
            // Close the original now; AVIO must own an independent duplicate.
        } else demuxer.open(source,options,token);
        if(seek>=0) demuxer.seek(seek);
        const int video=demuxer.best_video_stream(),audio=demuxer.best_audio_stream();
        std::unique_ptr<SoftwareDecoder> video_decoder,audio_decoder;
        if(decode && video>=0) video_decoder=std::make_unique<SoftwareDecoder>(demuxer,video);
        if(decode && audio>=0) audio_decoder=std::make_unique<SoftwareDecoder>(demuxer,audio);
        std::int64_t video_frames=0,audio_frames=0,audio_samples=0;
        std::optional<std::int64_t> first_video_pts,last_video_pts;
        auto drain=[&](SoftwareDecoder& decoder,int stream){
            for(;;){
                auto output=decoder.receive();
                if(output.status!=DecodeStatus::frame) break;
                if(stream==video){
                    ++video_frames;
                    const auto pts=demuxer.timestamp_us(stream,output.frame->best_effort_timestamp);
                    if(pts){ if(!first_video_pts) first_video_pts=pts; last_video_pts=pts; }
                } else {++audio_frames;audio_samples+=output.frame->nb_samples;}
            }
        };
        int packets=0;
        bool eof=false;
        while(packets<packets_limit){
            auto input_packet=demuxer.read();
            if(input_packet.status==ReadStatus::end){eof=true;break;}
            if(input_packet.status==ReadStatus::again){ std::this_thread::yield(); continue; }
            ++packets;
            auto* decoder=input_packet.packet->stream_index==video ? video_decoder.get() :
                (input_packet.packet->stream_index==audio ? audio_decoder.get() : nullptr);
            if(decoder){
                if(!decoder->send(input_packet.packet.get())){
                    drain(*decoder,input_packet.packet->stream_index);
                    if(!decoder->send(input_packet.packet.get())) throw MediaError("Decoder made no progress");
                }
                drain(*decoder,input_packet.packet->stream_index);
            }
        }
        if(eof){
            for(auto item:{std::pair<SoftwareDecoder*,int>{video_decoder.get(),video},{audio_decoder.get(),audio}})
                if(item.first){
                    if(!item.first->send(nullptr)){drain(*item.first,item.second);item.first->send(nullptr);}
                    drain(*item.first,item.second);
                }
        }
        std::cout<<"{\"source\":"<<json(source.display_name())<<",\"seekable\":"<<(demuxer.seekable()?"true":"false")<<",\"duration_us\":";
        if(auto duration=demuxer.duration_us()) std::cout<<*duration; else std::cout<<"null";
        std::cout<<",\"tracks\":[";
        bool comma=false;
        for(const auto& track:demuxer.tracks()){
            if(comma) std::cout<<',';
            comma=true;
            std::cout<<"{\"index\":"<<track.index<<",\"type\":"<<json(track_name(track.kind))<<",\"codec\":"<<json(track.codec)
                <<",\"language\":"<<json(track.language)<<",\"width\":"<<track.width<<",\"height\":"<<track.height<<",\"nominal_fps\":"<<track.nominal_fps<<",\"sample_rate\":"<<track.sample_rate<<",\"channels\":"<<track.channels<<'}';
        }
        std::cout<<"],\"packets\":"<<packets<<",\"eof\":"<<(eof?"true":"false")<<",\"decoded_video_frames\":"<<video_frames
            <<",\"decoded_audio_frames\":"<<audio_frames<<",\"decoded_audio_samples\":"<<audio_samples<<",\"first_video_pts_us\":";
        if(first_video_pts) std::cout<<*first_video_pts;else std::cout<<"null";
        std::cout<<",\"last_video_pts_us\":";
        if(last_video_pts) std::cout<<*last_video_pts;else std::cout<<"null";
        const auto& disc=demuxer.disc();
        std::cout<<",\"disc_type\":"<<json(disc.type)<<",\"disc_title\":"<<disc.selected<<",\"disc_angle\":"<<disc.angle<<",\"disc_titles\":[";
        comma=false;for(const auto& t:disc.titles){if(comma)std::cout<<',';comma=true;std::cout<<"{\"index\":"<<t.index<<",\"duration_us\":"<<t.duration_us<<",\"chapters\":"<<t.chapters<<",\"angles\":"<<t.angles<<'}';}
        std::cout<<"],\"chapters\":[";comma=false;for(const auto& c:demuxer.chapters()){if(comma)std::cout<<',';comma=true;std::cout<<"{\"start_us\":"<<c.start_us<<",\"end_us\":"<<c.end_us<<'}';}
        std::cout<<"]}\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr<<"{\"error\":"<<json(error.what())<<"}\n";
        return 2;
    }
}
