#include "cyvr/player/android/audio_playback.hpp"
#include "cyvr/player/audio_convert.hpp"
#include <aaudio/AAudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <time.h>
#include <android/log.h>
extern "C" {
#include <libavutil/frame.h>
}
namespace cyvr::player::android {
struct AudioPlayback::Impl {
  AudioQueue queue;
  const Demuxer& input;int track;
  SoftwareDecoder decoder;
  AAudioStream* stream{};
  int rate{48000};
  std::int64_t base_us{};
  std::unique_ptr<AudioConverter> converter;
  StereoSamples pending;unsigned pending_offset{};
  std::atomic<bool> running{},begun{},failed{},finished{};
  std::atomic<float> peak{};
  std::atomic<std::uint64_t> limited{},invalid{};
  bool decoder_end{};
  int last_xruns{};
  const std::chrono::steady_clock::time_point opened=std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point reported=opened;
  std::chrono::steady_clock::time_point checked=reported;
  static aaudio_data_callback_result_t callback(AAudioStream*,void* owner,void* output,int32_t count){
    auto& self=*static_cast<Impl*>(owner);
    self.queue.render(static_cast<float*>(output),unsigned(std::max(0,count)),self.running.load(std::memory_order_acquire));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  static void error(AAudioStream*,void* owner,aaudio_result_t){static_cast<Impl*>(owner)->failed.store(true);}
  Impl(const Demuxer& source,int stream_index,std::int64_t start,float volume):queue(volume),input(source),track(stream_index),decoder(source,stream_index,1),base_us(start){
    AAudioStreamBuilder* builder{};
    if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)throw MediaError("Cannot create native audio output");
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setUsage(builder,AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder,AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder,2);
    AAudioStreamBuilder_setSampleRate(builder,48000);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder,4800);
    AAudioStreamBuilder_setDataCallback(builder,callback,this);
    AAudioStreamBuilder_setErrorCallback(builder,error,this);
    const auto result=AAudioStreamBuilder_openStream(builder,&stream);
    AAudioStreamBuilder_delete(builder);
    if(result!=AAUDIO_OK)throw MediaError("Cannot open native audio output: "+std::to_string(result));
    rate=AAudioStream_getSampleRate(stream);
    if(rate<=0 || AAudioStream_getChannelCount(stream)!=2 || AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_FLOAT){AAudioStream_close(stream);stream=nullptr;throw MediaError("Unexpected native audio output format");}
    const auto burst=AAudioStream_getFramesPerBurst(stream);
    if(burst>0)AAudioStream_setBufferSizeInFrames(stream,burst*std::max(2,(rate/20+burst-1)/burst));
    converter=std::make_unique<AudioConverter>(rate);
    __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","AUDIO_READY track=%d output_rate=%d channels=2 buffer=%d burst=%d",track,rate,AAudioStream_getBufferSizeInFrames(stream),burst);
  }
  ~Impl(){if(stream){running=false;AAudioStream_requestStop(stream);AAudioStream_close(stream);}}
  bool queue_pending(){
    const unsigned frames=unsigned(pending.samples.size()/2);
    while(pending_offset<frames){
      const auto start=pending.start+pending_offset;
      if(start<0){pending_offset+=unsigned(std::min<std::int64_t>(frames-pending_offset,-start));continue;}
      const auto count=std::min(AudioQueue::block_frames,frames-pending_offset);
      if(!queue.push(pending.samples.data()+pending_offset*2,count,pending.start+pending_offset))return false;
      pending_offset+=count;
    }
    pending.samples.clear();pending_offset=0;return true;
  }
  void diagnostics(){
    const auto c=converter->stats();peak.store(c.peak);limited.store(c.limited);invalid.store(c.invalid);
    const auto now=std::chrono::steady_clock::now();
    if(now-checked<std::chrono::milliseconds(250))return;checked=now;
    const auto xruns=AAudioStream_getXRunCount(stream),burst=AAudioStream_getFramesPerBurst(stream);
    // Adapt outside the real-time callback; never block it to log or allocate.
    if(xruns>last_xruns && burst>0){
      const auto size=AAudioStream_getBufferSizeInFrames(stream);
      AAudioStream_setBufferSizeInFrames(stream,std::min(size+burst,std::max(burst*2,rate/10)));last_xruns=xruns;
      __android_log_print(ANDROID_LOG_WARN,"CyVRPlayer","AUDIO_XRUN count=%d elapsed_ms=%lld buffer=%d",xruns,(long long)std::chrono::duration_cast<std::chrono::milliseconds>(now-opened).count(),AAudioStream_getBufferSizeInFrames(stream));
    }
    if(now-reported<std::chrono::seconds(5))return;reported=now;
    const auto s=queue.stats();
    __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","AUDIO_STATS rendered=%llu underrun=%llu gap=%llu expired=%llu buffered=%lld peak=%.5f limited=%llu invalid=%llu xruns=%d",
      (unsigned long long)s.rendered,(unsigned long long)s.underrun,(unsigned long long)s.gap,(unsigned long long)s.expired,(long long)s.buffered,c.peak,(unsigned long long)c.limited,(unsigned long long)c.invalid,xruns);
  }
  void pump(){
    if(failed.load())throw MediaError("Native audio device disconnected");
    diagnostics();
    if(!queue_pending())return;
    if(decoder_end){queue.finish();finished=true;return;}
    for(;;){
      auto result=decoder.receive();
      if(!result.frame){
        if(result.status==DecodeStatus::end){
          converter->drain(pending);decoder_end=true;
          if(queue_pending()){queue.finish();finished=true;}
        }
        diagnostics();return;
      }
      const auto& frame=*result.frame;
      auto pts=input.timestamp_us(track,frame.best_effort_timestamp);
      if(!pts)pts=input.timestamp_us(track,frame.pts);
      if(pts)*pts-=base_us;
      converter->convert(frame,pts,pending);
      if(!queue_pending()){diagnostics();return;}
    }
  }
};
AudioPlayback::AudioPlayback(const Demuxer& source,int stream,std::int64_t start,float volume):impl_(std::make_unique<Impl>(source,stream,start,volume)){}
void AudioPlayback::volume(float value){impl_->queue.set_gain(value);}
AudioPlayback::~AudioPlayback()=default;
void AudioPlayback::pump(){impl_->pump();}
bool AudioPlayback::send(const AVPacket* packet){impl_->pump();return impl_->pending.samples.empty() && impl_->decoder.send(packet);}
std::int64_t AudioPlayback::buffered_us()const{return impl_->queue.stats().buffered*1000000/impl_->rate;}
bool AudioPlayback::ready()const{return impl_->finished.load() || buffered_us()>=120000;}
void AudioPlayback::start(){if(!ready())return;if(!impl_->begun.exchange(true)){impl_->running=true;if(AAudioStream_requestStart(impl_->stream)!=AAUDIO_OK)impl_->failed=true;__android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","AUDIO_STARTED buffered_us=%lld",(long long)buffered_us());}}
void AudioPlayback::pause(bool paused){if(!impl_->begun.load())return;impl_->running=!paused;if(paused)AAudioStream_requestPause(impl_->stream);else AAudioStream_requestStart(impl_->stream);}
bool AudioPlayback::started()const{return impl_->begun.load();}
AudioPlaybackStats AudioPlayback::stats()const{return {impl_->queue.stats(),impl_->peak.load(),impl_->limited.load(),impl_->invalid.load(),AAudioStream_getXRunCount(impl_->stream),AAudioStream_getBufferSizeInFrames(impl_->stream),impl_->rate};}
std::int64_t AudioPlayback::position_us()const{
  const auto submitted=impl_->queue.submitted();
  std::int64_t position{},timestamp{};
  if(AAudioStream_getTimestamp(impl_->stream,CLOCK_MONOTONIC,&position,&timestamp)==AAUDIO_OK){
    timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
    const auto ns=std::int64_t(now.tv_sec)*1000000000+now.tv_nsec;
    if(impl_->running.load())position+=std::max<std::int64_t>(0,ns-timestamp)*impl_->rate/1000000000;
  }else position=submitted-AAudioStream_getBufferSizeInFrames(impl_->stream);
  return impl_->base_us+impl_->queue.media_at(std::clamp<std::int64_t>(position,0,submitted))*1000000/impl_->rate;
}
}
