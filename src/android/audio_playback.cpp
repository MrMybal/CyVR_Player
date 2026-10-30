#include "cyvr/player/android/audio_playback.hpp"
#include <aaudio/AAudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <time.h>
#include <android/log.h>
extern "C" {
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
}
namespace cyvr::player::android {
struct AudioPlayback::Impl {
  static constexpr unsigned slots=128, block_frames=2048;
  struct Block {std::int64_t start{};unsigned count{};std::array<float,block_frames*2> samples{};};
  std::array<Block,slots> blocks;
  std::atomic<std::uint64_t> read{},write{};
  std::atomic<std::int64_t> cursor{};
  const Demuxer& input;int track;
  SoftwareDecoder decoder;
  AAudioStream* stream{};
  SwrContext* resampler{};
  AVChannelLayout input_layout{};
  int rate{48000},input_rate{},input_format{-1};
  std::int64_t base_us{},next_frame{},pending_start{};
  std::vector<float> pending;
  unsigned pending_offset{};
  std::atomic<bool> running{},begun{},failed{};
  bool flushed{};
  static aaudio_data_callback_result_t callback(AAudioStream*,void* owner,void* output,int32_t count){
    auto& self=*static_cast<Impl*>(owner);
    auto* destination=static_cast<float*>(output);
    auto position=self.cursor.load(std::memory_order_relaxed);
    int remaining=count;
    while(remaining>0){
      auto index=self.read.load(std::memory_order_relaxed);
      if(index==self.write.load(std::memory_order_acquire)){
        std::memset(destination,0,std::size_t(remaining)*2*sizeof(float));position+=remaining;break;
      }
      const auto& block=self.blocks[index%slots];
      if(block.start+block.count<=position){self.read.store(index+1,std::memory_order_release);continue;}
      if(block.start>position){
        const auto silence=int(std::min<std::int64_t>(remaining,block.start-position));
        std::memset(destination,0,std::size_t(silence)*2*sizeof(float));
        position+=silence;destination+=silence*2;remaining-=silence;continue;
      }
      const auto offset=unsigned(position-block.start);
      const auto take=std::min<unsigned>(remaining,block.count-offset);
      std::memcpy(destination,block.samples.data()+offset*2,std::size_t(take)*2*sizeof(float));
      position+=take;remaining-=int(take);destination+=take*2;
      if(offset+take==block.count)self.read.store(index+1,std::memory_order_release);
    }
    self.cursor.store(position,std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  static void error(AAudioStream*,void* owner,aaudio_result_t){static_cast<Impl*>(owner)->failed.store(true);}
  Impl(const Demuxer& source,int stream_index,std::int64_t start):input(source),track(stream_index),decoder(source,stream_index,1),base_us(start){
    AAudioStreamBuilder* builder{};
    if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)throw MediaError("Cannot create native audio output");
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder,2);
    AAudioStreamBuilder_setSampleRate(builder,48000);
    AAudioStreamBuilder_setDataCallback(builder,callback,this);
    AAudioStreamBuilder_setErrorCallback(builder,error,this);
    const auto result=AAudioStreamBuilder_openStream(builder,&stream);
    AAudioStreamBuilder_delete(builder);
    if(result!=AAUDIO_OK)throw MediaError("Cannot open native audio output: "+std::to_string(result));
    rate=AAudioStream_getSampleRate(stream);
    if(AAudioStream_getChannelCount(stream)!=2 || AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_FLOAT){AAudioStream_close(stream);stream=nullptr;throw MediaError("Unexpected native audio output format");}
    __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","AUDIO_READY track=%d output_rate=%d channels=2",track,rate);
  }
  ~Impl(){if(stream){AAudioStream_requestStop(stream);AAudioStream_close(stream);}swr_free(&resampler);av_channel_layout_uninit(&input_layout);}
  bool queue_pending(){
    const unsigned frames=unsigned(pending.size()/2);
    while(pending_offset<frames){
      const auto index=write.load(std::memory_order_relaxed);
      if(index-read.load(std::memory_order_acquire)>=slots)return false;
      auto& block=blocks[index%slots];
      block.start=pending_start+pending_offset;block.count=std::min(block_frames,frames-pending_offset);
      std::memcpy(block.samples.data(),pending.data()+pending_offset*2,std::size_t(block.count)*2*sizeof(float));
      pending_offset+=block.count;write.store(index+1,std::memory_order_release);
    }
    pending.clear();pending_offset=0;return true;
  }
  void pump(){
    if(failed.load())throw MediaError("Native audio device disconnected");
    if(!queue_pending())return;
    for(;;){
      auto result=decoder.receive();
      if(!result.frame){
        if(result.status==DecodeStatus::end && resampler && !flushed){
          flushed=true;const auto capacity=av_rescale_rnd(swr_get_delay(resampler,input_rate),rate,input_rate,AV_ROUND_UP);
          if(capacity>0 && capacity<1048576){pending.resize(std::size_t(capacity)*2);std::uint8_t* output[]={reinterpret_cast<std::uint8_t*>(pending.data())};const int count=swr_convert(resampler,output,int(capacity),nullptr,0);if(count<0)throw MediaError("Audio flush failed");pending.resize(std::size_t(count)*2);pending_start=next_frame;next_frame+=count;queue_pending();}
        }
        return;
      }
      const auto& frame=*result.frame;
      if(frame.sample_rate<=0 || frame.ch_layout.nb_channels<=0)throw MediaError("Audio track has incomplete channel metadata");
      if(!resampler || frame.sample_rate!=input_rate || frame.format!=input_format || av_channel_layout_compare(&input_layout,&frame.ch_layout)){
        swr_free(&resampler);av_channel_layout_uninit(&input_layout);
        av_channel_layout_copy(&input_layout,&frame.ch_layout);
        AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
        if(swr_alloc_set_opts2(&resampler,&stereo,AV_SAMPLE_FMT_FLT,rate,&input_layout,AVSampleFormat(frame.format),frame.sample_rate,0,nullptr)<0 || swr_init(resampler)<0)throw MediaError("Audio resampling configuration failed");
        input_rate=frame.sample_rate;input_format=frame.format;
      }
      const auto delay=swr_get_delay(resampler,input_rate);
      const auto capacity=av_rescale_rnd(delay+frame.nb_samples,rate,input_rate,AV_ROUND_UP);
      if(capacity<=0 || capacity>1048576)throw MediaError("Audio frame exceeds the conversion budget");
      pending.resize(std::size_t(capacity)*2);
      std::uint8_t* planes[]={reinterpret_cast<std::uint8_t*>(pending.data())};
      const int count=swr_convert(resampler,planes,int(capacity),const_cast<const std::uint8_t**>(frame.extended_data),frame.nb_samples);
      if(count<0)throw MediaError("Audio sample conversion failed");
      auto pts=input.timestamp_us(track,frame.best_effort_timestamp);
      if(!pts)pts=input.timestamp_us(track,frame.pts);
      pending_start=pts ? av_rescale_q(*pts-base_us,AVRational{1,1000000},AVRational{1,rate})-av_rescale_rnd(delay,rate,input_rate,AV_ROUND_NEAR_INF) : next_frame;
      pending.resize(std::size_t(count)*2);next_frame=pending_start+count;
      if(!queue_pending())return;
    }
  }
};
AudioPlayback::AudioPlayback(const Demuxer& source,int stream,std::int64_t start):impl_(std::make_unique<Impl>(source,stream,start)){}
AudioPlayback::~AudioPlayback()=default;
void AudioPlayback::pump(){impl_->pump();}
bool AudioPlayback::send(const AVPacket* packet){impl_->pump();return impl_->pending.empty() && impl_->decoder.send(packet);}
void AudioPlayback::start(){if(!impl_->begun.exchange(true)){impl_->running=true;if(AAudioStream_requestStart(impl_->stream)!=AAUDIO_OK)impl_->failed=true;__android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","AUDIO_STARTED");}}
void AudioPlayback::pause(bool paused){if(!impl_->begun.load())return;impl_->running=!paused;if(paused)AAudioStream_requestPause(impl_->stream);else AAudioStream_requestStart(impl_->stream);}
bool AudioPlayback::started()const{return impl_->begun.load();}
std::int64_t AudioPlayback::position_us()const{
  const auto submitted=impl_->cursor.load(std::memory_order_acquire);
  std::int64_t position{},timestamp{};
  if(AAudioStream_getTimestamp(impl_->stream,CLOCK_MONOTONIC,&position,&timestamp)==AAUDIO_OK){
    timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
    const auto ns=std::int64_t(now.tv_sec)*1000000000+now.tv_nsec;
    position+=std::max<std::int64_t>(0,ns-timestamp)*impl_->rate/1000000000;
  }else position=submitted-AAudioStream_getBufferSizeInFrames(impl_->stream);
  return impl_->base_us+std::clamp<std::int64_t>(position,0,submitted)*1000000/impl_->rate;
}
}
