#include "cyvr/player/android/video_format.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}
#include <chrono>
#include <iostream>
#include <thread>

using namespace cyvr::player;
using namespace cyvr::player::android;
int main(int argc,char** argv){
    av_log_set_level(AV_LOG_QUIET);
    try {
        if(argc!=4) throw std::invalid_argument("Usage: cyvr_hardware_probe FILE HARDWARE_CODEC EXPECTED_FRAMES");
        const int expected=std::stoi(argv[3]);
        if(expected<1 || expected>1000000) throw std::invalid_argument("Invalid frame count");
        Demuxer input;
        const auto source=std::string(argv[1]).find("://")!=std::string::npos ? MediaSource::network(argv[1]) : MediaSource::file(argv[1]);
        input.open(source);
        const int video=input.best_video_stream();
        if(video<0) throw MediaError("No video stream");
        VideoBitstream bitstream(input,video);
        auto format=make_video_format(bitstream.configuration());
        SurfaceDecoder decoder;decoder.open(argv[2],format.get(),1);
        PacketPtr original,pending;
        bool input_end=false,filter_eos=false,filter_end=false,codec_eos=false,output_end=false;
        int frames=0;
        std::int64_t first_pts=-1,last_pts=-1,last_queued=1000000;
        std::uint32_t buffer_width=0,buffer_height=0;
        const auto start=std::chrono::steady_clock::now();
        auto last_output=start;
        while(std::chrono::steady_clock::now()-start<std::chrono::seconds(15)){
            bool progress=false;
            for(;;){
                auto image=decoder.try_acquire_next();
                if(!image) break;
                AHardwareBuffer_Desc description{};
                AHardwareBuffer_describe(image.hardware_buffer(),&description);
                if(description.width<unsigned(bitstream.configuration().width) || description.height<unsigned(bitstream.configuration().height))
                    throw MediaError("Decoded hardware buffer dimensions are too small");
                buffer_width=description.width;buffer_height=description.height;
                if(first_pts<0) first_pts=image.pts_us-1000000;
                if(last_pts>=0 && image.pts_us-1000000<last_pts) throw MediaError("Decoded presentation timestamps are out of order");
                last_pts=image.pts_us-1000000;
                ++frames;progress=true;last_output=std::chrono::steady_clock::now();
                // No GPU reads in this smoke test: the lease returns the acquire
                // fence to the reader without waiting or copying image pixels.
            }
            const auto output=decoder.drain_one();
            if(output.kind!=DrainKind::try_again) progress=true;
            output_end=output_end || output.end_of_stream;
            if(!pending && !filter_end){
                auto filtered=bitstream.receive();
                if(filtered.status==ReadStatus::packet) pending=std::move(filtered.packet);
                if(filtered.status==ReadStatus::end) filter_end=true;
            }
            if(pending){
                auto pts=input.timestamp_us(video,pending->pts);
                if(!pts || *pts < -1000000) throw MediaError("Hardware smoke test needs finite timestamps within its one-second preroll");
                if(decoder.queue_access_unit(pending->data,std::size_t(pending->size),*pts+1000000,
                    (pending->flags&AV_PKT_FLAG_KEY) ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0)){
                    last_queued=*pts+1000000;pending.reset();progress=true;
                }
            }
            if(!original && !input_end && !pending){
                auto packet=input.read();
                if(packet.status==ReadStatus::end) input_end=true;
                if(packet.status==ReadStatus::packet && packet.packet->stream_index==video) original=std::move(packet.packet);
                progress=true;
            }
            if(original && bitstream.send(original.get())){original.reset();progress=true;}
            if(input_end && !original && !filter_eos && bitstream.send(nullptr)){filter_eos=true;progress=true;}
            if(filter_end && !pending && !codec_eos && decoder.queue_access_unit(nullptr,0,last_queued+1,AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)){
                codec_eos=true;progress=true;
            }
            if(output_end && std::chrono::steady_clock::now()-last_output>std::chrono::milliseconds(100)) break;
            if(!progress) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!output_end || frames!=expected) throw MediaError("Hardware decoder did not produce the expected frame count before its deadline");
        // Codec name is supplied only from the Java hardware capability query.
        // The test harness records that name separately in its JSON report.
        std::cout<<"{\"hardware_frames\":"<<frames<<",\"first_pts_us\":"<<first_pts<<",\"last_pts_us\":"<<last_pts
            <<",\"buffer_width\":"<<buffer_width<<",\"buffer_height\":"<<buffer_height<<",\"pixels_copied_to_cpu\":false}\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr<<"Hardware probe failed: "<<error.what()<<'\n';return 2;
    }
}
