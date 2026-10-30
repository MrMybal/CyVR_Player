#include "cyvr/player/application.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#ifdef __ANDROID__
#include "cyvr/player/android/video_format.hpp"
#include "cyvr/player/android/software_video.hpp"
#include "cyvr/player/android/audio_playback.hpp"
#include <android/log.h>
#include <unistd.h>
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}

namespace cyvr::player {
using Clock = std::chrono::steady_clock;
namespace {double performance_now(){return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();}}
struct PlayerSession::Impl {
  Platform platform;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool quit{}, open_pending{}, paused{}, stop_pending{};
  std::int64_t seek_pending{-1};
  std::int64_t requested_start{};
  bool requested_autoplay{true}, pause_after_first{};
  std::uint64_t epoch_counter{};
  std::string location, title;
  PlayerState state;
  TrackSelection requested_selection,active_selection;
  std::unique_ptr<SubtitleDecoder> subtitle_decoder;
  int subtitle_stream{-1};
  std::shared_ptr<SubtitleTimeline> subtitle_timeline;
  SplatSnapshot splat_frame;
  std::uint64_t splat_ack_epoch{};
  Clock::time_point splat_presented_at{};
  VideoMetrics counters;
  EventRate decoded_rate,submitted_rate;
  UniqueVideoFrames unique_submissions;
  std::shared_ptr<Cancellation> cancellation = std::make_shared<Cancellation>();
#ifdef __ANDROID__
  std::deque<android::ImageLease> frames;
  std::shared_ptr<android::AudioPlayback> audio;
  int audio_stream{-1};
  PacketPtr audio_pending;
  bool audio_eos{};
  unsigned presented{}, dropped{};
#endif
  explicit Impl(Platform p) : platform(std::move(p)) {
    worker = std::thread([this] { run(); });
  }
  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      quit = true;
      cancellation->cancel();
    }
    wake.notify_all();
    worker.join();
  }
  void publish(const std::function<void(PlayerState &)> &update) {
    std::lock_guard<std::mutex> lock(mutex);
    if (quit || open_pending || stop_pending || cancellation->cancelled())
      return;
    update(state);
    ++state.revision;
  }
  void run() {
    av_log_set_level(AV_LOG_QUIET);
    for (;;) {
      std::string target, name;
      std::int64_t start_us{};
      std::shared_ptr<Cancellation> cancel;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return quit || open_pending || stop_pending; });
        if (quit)
          return;
        if (stop_pending) {
          stop_pending = false;
          splat_frame={};subtitle_timeline.reset();
#ifdef __ANDROID__
          audio.reset();
#endif
          state = PlayerState{state.revision + 1};
        }
        if (!open_pending)
          continue;
        target = location;
        name = title;
        open_pending = false;
        paused = false;
        start_us = requested_start;
        active_selection=requested_selection;
        subtitle_decoder.reset();subtitle_timeline.reset();subtitle_stream=-1;
        splat_frame={};
        pause_after_first = !requested_autoplay;
        seek_pending = -1;
        cancellation = cancel = std::make_shared<Cancellation>();
#ifdef __ANDROID__
        frames.clear();
        audio.reset();audio_pending.reset();audio_stream=-1;
#endif
        state = PlayerState{state.revision + 1};
        state.status = "Opening media...";
        counters={};decoded_rate.reset();submitted_rate.reset();unique_submissions.reset();
        state.title = name;
      }
      try {
        if(classify_extension(std::filesystem::u8path(name))==FileKind::splat || classify_extension(std::filesystem::u8path(target))==FileKind::splat){play_splats(target,cancel,start_us);continue;}
        MediaSource source = target.rfind("content://", 0) == 0
                                 ? platform.open_document(target)
                             : target.find("://") != std::string::npos
                                 ? MediaSource::network(target)
                                 : MediaSource::file(target);
        Demuxer input;
        StreamOptions options;
        options.ca_file = platform.ca_file;
        try {
          input.open(source, options, cancel);
        } catch (...) {
#ifdef __ANDROID__
          if (source.kind == SourceKind::descriptor)
            ::close(source.fd);
#endif
          throw;
        }
#ifdef __ANDROID__
        if (source.kind == SourceKind::descriptor)
          ::close(source.fd);
#endif
        const auto valid_track=[&](int index,TrackKind kind){return std::any_of(input.tracks().begin(),input.tracks().end(),[&](const auto& t){return t.index==index && t.kind==kind && !t.attached_picture;});};
        const int video = valid_track(active_selection.video,TrackKind::video)?active_selection.video:input.best_video_stream();
        if (video < 0)
          throw MediaError("This media has no video track.");
        const auto &parameters = *input.codec_parameters(video);
        publish([&](PlayerState &s) {
          s.tracks = input.tracks();
          s.chapters=input.chapters();s.video_track=video;s.position_us=input.seekable()?start_us:0;
          s.audio_track=active_selection.audio==-1?-1:valid_track(active_selection.audio,TrackKind::audio)?active_selection.audio:input.best_audio_stream();
          s.subtitle_track=-1;
          if(active_selection.external_subtitle.empty() && active_selection.subtitle!=-1){
            if(valid_track(active_selection.subtitle,TrackKind::subtitle))s.subtitle_track=active_selection.subtitle;
            else for(const auto& t:s.tracks)if(t.kind==TrackKind::subtitle && (t.forced || (t.default_track && s.subtitle_track<0)))s.subtitle_track=t.index;
          }
          for(const auto& track:s.tracks)if(track.index==video)s.nominal_fps=track.nominal_fps;
          s.duration_us = input.duration_us().value_or(0);
          s.width = parameters.width;
          s.height = parameters.height;
          s.seekable = input.seekable();
          if (parameters.sample_aspect_ratio.num > 0 &&
              parameters.sample_aspect_ratio.den > 0)
            s.pixel_aspect = double(parameters.sample_aspect_ratio.num) /
                             parameters.sample_aspect_ratio.den;
        });
#ifdef __ANDROID__
        if (start_us > 0 && input.seekable())
          input.seek(start_us);
        else
          start_us = 0;
        play(input, video, cancel, start_us);
#else
        publish([](PlayerState &s) {
          s.ready = true;
          s.status = "Media inspected. Hardware playback runs on the headset.";
        });
#endif
      } catch (const std::exception &e) {
#ifdef __ANDROID__
        {std::lock_guard<std::mutex> lock(mutex);if(audio)audio->pause(true);}
        if (!cancel->cancelled())
          __android_log_print(ANDROID_LOG_ERROR, "CyVRPlayer",
                              "MEDIA_ERROR: %s", e.what());
#endif
        if (!cancel->cancelled())
          publish([&](PlayerState &s) {
            s.status = e.what();
            s.playing = false;
            s.ready = false;
          });
      }
    }
  }
  std::vector<std::uint8_t> splat_bytes(const std::string& path,const std::shared_ptr<Cancellation>& cancel){
    const auto source=path.rfind("content://",0)==0?platform.open_document(path):MediaSource::file(path);
    try{auto bytes=read_splat_source(source,*cancel);
#ifdef __ANDROID__
      if(source.kind==SourceKind::descriptor)::close(source.fd);
#endif
      return bytes;
    }catch(...){
#ifdef __ANDROID__
      if(source.kind==SourceKind::descriptor)::close(source.fd);
#endif
      throw;
    }
  }
  void play_splats(const std::string& path,const std::shared_ptr<Cancellation>& cancel,std::int64_t start){
    auto bytes=splat_bytes(path,cancel);SplatSequence sequence;
    const bool animated=bytes.size()>=21 && std::memcmp(bytes.data(),"CYVR_SPLAT_SEQUENCE 1",21)==0;
    if(animated)sequence=splat_sequence(path,bytes);else sequence.frames={path};
    SplatNormalization normalization;
    auto cloud=decode_splats(animated?splat_bytes(sequence.frames.front(),cancel):bytes,normalization,*cancel);
    bytes.clear();bytes.shrink_to_fit();
    const auto duration=animated?std::int64_t(double(sequence.frames.size())*1000000/sequence.fps):0;
    start=animated?std::clamp<std::int64_t>(start,0,duration-1):0;
    auto base=Clock::now();auto position=start;std::size_t loaded=0;std::uint64_t epoch=++epoch_counter;
    auto publish_frame=[&](std::int64_t pts){std::lock_guard<std::mutex> lock(mutex);if(cancel->cancelled())return;splat_frame={cloud,epoch,pts};state.position_us=pts;state.seeking=false;++counters.decoded;decoded_rate.add(performance_now());++state.revision;};
    publish([&](PlayerState& s){s.ready=true;s.volumetric=true;s.playing=animated&&!pause_after_first;s.seekable=animated;s.duration_us=duration;s.width=1920;s.height=1080;s.nominal_fps=animated?sequence.fps:0;s.codec="Gaussian PLY / SPLAT + Vulkan GPU sorting";s.status="Gaussian scene";});
    if(animated && start>0){loaded=std::min(sequence.frames.size()-1,std::size_t(double(start)*sequence.fps/1000000));cloud=decode_splats(splat_bytes(sequence.frames[loaded],cancel),normalization,*cancel);}
    bool was_paused;{std::lock_guard<std::mutex> lock(mutex);paused=!animated || pause_after_first;pause_after_first=false;was_paused=paused;}
    publish_frame(std::int64_t(double(loaded)*1000000/sequence.fps));
    bool awaiting_presentation=animated;
    while(!cancel->cancelled()){
      std::int64_t request=-1;bool hold;
      {std::unique_lock<std::mutex> lock(mutex);hold=paused;if(seek_pending>=0){request=seek_pending;seek_pending=-1;hold=false;}else if(hold){was_paused=true;wake.wait_for(lock,std::chrono::milliseconds(20));continue;}}
      if(request>=0){position=request;start=request;base=Clock::now();epoch=++epoch_counter;awaiting_presentation=true;}
      else if(was_paused){base=Clock::now();start=position;}
      was_paused=hold;
      if(!animated)continue;
      if(awaiting_presentation && request<0){std::unique_lock<std::mutex> lock(mutex);if(splat_ack_epoch<epoch){wake.wait_for(lock,std::chrono::milliseconds(3));continue;}base=std::max(base,splat_presented_at);start=position;awaiting_presentation=false;}
      position=std::min<std::int64_t>(duration-1,start+std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-base).count());
      const auto index=std::min(sequence.frames.size()-1,std::size_t(double(position)*sequence.fps/1000000));
      if(index!=loaded || request>=0){cloud=decode_splats(splat_bytes(sequence.frames[index],cancel),normalization,*cancel);loaded=index;publish_frame(std::int64_t(double(index)*1000000/sequence.fps));}
      {std::unique_lock<std::mutex> lock(mutex);if(pause_after_first){pause_after_first=false;paused=true;state.playing=false;was_paused=true;++state.revision;}if(position>=duration-1){paused=true;state.ended=true;state.playing=false;state.position_us=duration;was_paused=true;++state.revision;}if(paused)was_paused=true;wake.wait_for(lock,std::chrono::milliseconds(3));}
    }
  }
#ifdef __ANDROID__
  void begin_audio(Demuxer& input,std::int64_t start_us){
    int next;{std::lock_guard<std::mutex> lock(mutex);next=state.audio_track;}
    std::shared_ptr<android::AudioPlayback> output;
    if(next>=0){try{output=std::make_shared<android::AudioPlayback>(input,next,start_us);}catch(const std::exception& e){publish([&](PlayerState& s){s.audio_status=e.what();s.audio_track=-1;});next=-1;}}
    std::lock_guard<std::mutex> lock(mutex);
    audio=std::move(output);audio_stream=next;audio_pending.reset();audio_eos=false;
  }
  void begin_subtitles(Demuxer& input,const std::shared_ptr<Cancellation>& cancel){
    try{load_subtitles(input,cancel);}catch(const std::exception& e){subtitle_decoder.reset();subtitle_stream=-1;{std::lock_guard<std::mutex> lock(mutex);subtitle_timeline.reset();}publish([&](PlayerState& s){s.subtitle_status=e.what();s.subtitle_track=-1;});}
  }
  void load_subtitles(Demuxer& input,const std::shared_ptr<Cancellation>& cancel){
    subtitle_decoder.reset();
    int next;{std::lock_guard<std::mutex> lock(mutex);next=state.subtitle_track;}
    auto config=std::make_shared<SubtitleConfig>();config->fonts=input.fonts();
    std::shared_ptr<SubtitleTimeline> timeline;
    if(!active_selection.external_subtitle.empty()){
      const auto& path=active_selection.external_subtitle;
      const auto source=path.rfind("content://",0)==0?platform.open_document(path):MediaSource::file(path);
      Demuxer external;StreamOptions options;options.ca_file=platform.ca_file;
      try{external.open(source,options,cancel);}catch(...){if(source.kind==SourceKind::descriptor)::close(source.fd);throw;}
      if(source.kind==SourceKind::descriptor)::close(source.fd);
      int track=-1;for(const auto& t:external.tracks())if(t.kind==TrackKind::subtitle){track=t.index;break;}
      if(track<0)throw MediaError("This file has no subtitle track");
      SubtitleDecoder decoder(external,track);config->header=decoder.header();
      timeline=std::make_shared<SubtitleTimeline>(config);
      std::size_t bytes=0;
      for(;;){auto p=external.read();if(p.status==ReadStatus::end)break;if(p.status==ReadStatus::packet && p.packet->stream_index==track){bytes+=p.packet->size;if(bytes>16*1024*1024)throw MediaError("External subtitles exceed the loading budget");timeline->add(decoder.decode(p.packet.get()));}}
      next=-1;
    }else if(next>=0){subtitle_decoder=std::make_unique<SubtitleDecoder>(input,next);config->header=subtitle_decoder->header();timeline=std::make_shared<SubtitleTimeline>(config);}
    subtitle_stream=next;
    std::lock_guard<std::mutex> lock(mutex);subtitle_timeline=std::move(timeline);
  }
  PacketRead read_video_packet(Demuxer& input){
    if(audio){
      pump_audio();
      if(audio_pending){if(!send_audio(audio_pending.get())){
        // Long audio-leading interleaves must not wait for a video frame to
        // start the same clock that frees this bounded audio queue.
        {std::lock_guard<std::mutex> lock(mutex);if(audio && !paused && !pause_after_first)audio->start();}
        return {ReadStatus::again,{}};
      }audio_pending.reset();}
    }
    auto packet=input.read();
    if(packet.status==ReadStatus::packet && subtitle_decoder && packet.packet->stream_index==subtitle_stream){
      try{subtitle_timeline->add(subtitle_decoder->decode(packet.packet.get()));}
      catch(const std::exception& e){subtitle_decoder.reset();publish([&](PlayerState& s){s.subtitle_status=e.what();s.subtitle_track=-1;});{std::lock_guard<std::mutex> lock(mutex);subtitle_timeline.reset();}return {ReadStatus::again,{}};}
      std::int64_t position;{std::lock_guard<std::mutex> lock(mutex);position=state.position_us;}
      subtitle_timeline->prune(position-30000000);
      return {ReadStatus::again,{}};
    }
    if(packet.status==ReadStatus::end && audio && !audio_eos){
      if(!send_audio(nullptr))return {ReadStatus::again,{}};
      audio_eos=true;
    }
    if(packet.status==ReadStatus::packet && audio && packet.packet->stream_index==audio_stream){
      if(!send_audio(packet.packet.get()))audio_pending=std::move(packet.packet);
      return {ReadStatus::again,{}};
    }
    return packet;
  }
  void audio_failure(const std::exception& e){
    std::lock_guard<std::mutex> lock(mutex);if(audio)audio->pause(true);audio.reset();audio_stream=-1;clock_started=false;state.audio_track=-1;state.audio_status=e.what();++state.revision;
  }
  void pump_audio(){if(audio)try{audio->pump();}catch(const std::exception& e){audio_failure(e);}}
  bool send_audio(const AVPacket* packet){if(!audio)return true;try{return audio->send(packet);}catch(const std::exception& e){audio_failure(e);return true;}}
  void play_software(Demuxer& input,int video,const std::shared_ptr<Cancellation>& cancel,
                     std::int64_t start_us,VideoColor color,std::deque<PacketPtr> startup={}){
    bool first=true;
    while(!cancel->cancelled()){
      if(!first || !audio)begin_audio(input,start_us);
      if(!first)begin_subtitles(input,cancel);
      first=false;
      const auto epoch=++epoch_counter;
      android::SoftwareVideo decoder(input,video,color,epoch,start_us);
      publish([&](PlayerState& s){s.ready=true;s.playing=!paused&&!pause_after_first;s.ended=false;s.codec=std::string("FFmpeg / ")+avcodec_get_name(input.codec_parameters(video)->codec_id);s.status="Software decoding fallback";});
      __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","DECODER_READY software %s",avcodec_get_name(input.codec_parameters(video)->codec_id));
      PacketPtr pending;bool end=false,eos=false,restart=false;
      android::ImageLease last_preroll;
      {std::lock_guard<std::mutex> lock(mutex);presented=dropped=0;}
      while(!cancel->cancelled()){
        pump_audio();
        {
          std::unique_lock<std::mutex> lock(mutex);
          if(seek_pending>=0){start_us=seek_pending;seek_pending=-1;frames.clear();if(audio)audio->pause(true);lock.unlock();input.seek(start_us);startup.clear();restart=true;break;}
          if(paused || frames.size()>=3){wake.wait_for(lock,std::chrono::milliseconds(3));continue;}
        }
        bool progress=false;
        auto frame=decoder.receive();
        if(frame){
          progress=true;
          if(frame.pts_us<start_us)last_preroll=std::move(frame);
          else{
            AHardwareBuffer_Desc desc{};AHardwareBuffer_describe(frame.hardware_buffer(),&desc);
            std::lock_guard<std::mutex> lock(mutex);
            if(cancel->cancelled() || seek_pending>=0)continue;
            if(state.width!=int(desc.width) || state.height!=int(desc.height)){state.width=int(desc.width);state.height=int(desc.height);++state.revision;}
            ++counters.decoded;decoded_rate.add(performance_now());frames.push_back(std::move(frame));
          }
        }
        if(!pending && !end){
          auto packet=startup.empty()?read_video_packet(input):PacketRead{ReadStatus::packet,std::move(startup.front())};
          if(!startup.empty())startup.pop_front();
          end=packet.status==ReadStatus::end;
          if(packet.status==ReadStatus::packet && packet.packet->stream_index==video)pending=std::move(packet.packet);
          progress|=packet.status!=ReadStatus::again;
        }
        if(pending && decoder.send(pending.get())){pending.reset();progress=true;}
        if(end && !pending && !eos && decoder.send(nullptr)){eos=true;progress=true;}
        if(decoder.ended()){
          std::unique_lock<std::mutex> lock(mutex);
          if(frames.empty() && last_preroll){frames.push_back(std::move(last_preroll));++counters.decoded;}
          if(frames.empty() && (!audio || state.duration_us<=0 || audio->position_us()>=state.duration_us)){
            state.ended=true;state.seeking=false;state.playing=false;state.position_us=state.duration_us;state.status="End of video";++state.revision;
            if(audio)audio->pause(true);
            wake.wait(lock,[&]{return cancel->cancelled()||seek_pending>=0;});
          }
        }
        if(!progress)std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if(!restart)break;
    }
  }
  void play(Demuxer &input, int video,
            const std::shared_ptr<Cancellation> &cancel,
            std::int64_t start_us) {
    std::uint64_t epoch = ++epoch_counter;
    std::int64_t discard_before = start_us;
    while (!cancel->cancelled()) {
      begin_audio(input,discard_before);
      begin_subtitles(input,cancel);
      VideoBitstream bits(input, video);
      std::deque<PacketPtr> primed;
      if (!bits.configuration_ready()) {
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        std::size_t bytes = 0, packet_count = 0;
        PacketPtr incoming;
        while (!bits.configuration_ready()) {
          if (cancel->cancelled())
            return;
          if (Clock::now() > deadline || bytes > 8 * 1024 * 1024 ||
              packet_count >= 256)
            throw MediaError(
                "Video parameter sets exceed the stream startup budget.");
          auto filtered = bits.receive();
          if (filtered.status == ReadStatus::packet) {
            bytes += filtered.packet->size;
            primed.push_back(std::move(filtered.packet));
            ++packet_count;
            continue;
          }
          if (filtered.status == ReadStatus::end)
            throw MediaError("Stream ended before its video parameter sets.");
          if (!incoming) {
            auto packet = read_video_packet(input);
            if(packet.status==ReadStatus::again)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (packet.status == ReadStatus::end)
              throw MediaError("Stream ended before its video parameter sets.");
            if (packet.status == ReadStatus::packet &&
                packet.packet->stream_index == video)
              incoming = std::move(packet.packet);
          }
          if (incoming && bits.send(incoming.get()))
            incoming.reset();
        }
      }
      const auto &config = bits.configuration();
      const auto name = config.mime.empty() ? std::string{} :
          platform.hardware_codec(config.mime, config.width, config.height);
      if (name.empty()){
        play_software(input,video,cancel,discard_before,config.color,std::move(primed));return;
      }
      auto format = android::make_video_format(config);
      android::SurfaceDecoder decoder;
      try{decoder.open(name.c_str(), format.get(), epoch, 6);}
      catch(const std::exception&){play_software(input,video,cancel,discard_before,config.color,std::move(primed));return;}
      __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer",
                          "DECODER_READY %s %dx%d transfer=%d standard=%d peak_nits=%.0f", name.c_str(), config.width,
                          config.height,config.color.transfer,config.color.standard,config.color.peak_nits);
      publish([&](PlayerState &s) {
        s.ready = true;
        s.playing = !paused && !pause_after_first;
        s.ended = false;
        s.codec = name;
        s.status = "Hardware playback / no CPU pixel copies";
      });
      try {
      PacketPtr original, pending;
      bool input_end = false, filter_eos = false, filter_end = false,
           codec_eos = false, output_end = false;
      std::int64_t last_queued = 1'000'000;
      auto last_output = Clock::now();
      unsigned decoded = 0;
      android::ImageLease last_preroll;
      {
        std::lock_guard<std::mutex> lock(mutex);
        presented = dropped = 0;
      }
      bool restart = false;
      while (!cancel->cancelled()) {
        pump_audio();
        bool progress = false;
        {
          std::unique_lock<std::mutex> lock(mutex);
          if (seek_pending >= 0) {
            const auto position = seek_pending;
            seek_pending = -1;
            frames.clear();
            lock.unlock();
            input.seek(position);
            discard_before = position;
            epoch = ++epoch_counter;
            restart = true;
            break;
          }
          if (paused || frames.size() >= 3) {
            wake.wait_for(lock, std::chrono::milliseconds(3));
            continue;
          }
        }
        auto image = decoder.try_acquire_next();
        if (image) {
          image.pts_us -= 1'000'000;
          if (image.pts_us < discard_before) {
            last_preroll = std::move(image);
            continue;
          }
          last_preroll = {};
          std::lock_guard<std::mutex> lock(mutex);
          if (cancel->cancelled() || seek_pending >= 0)
            continue;
          ++counters.decoded;decoded_rate.add(performance_now());
          frames.push_back(std::move(image));
          progress = true;
          last_output = Clock::now();
          if (++decoded == 1 || decoded == 48)
            __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer",
                                "VIDEO_DECODED frames=%u", decoded);
        }
        const auto output = decoder.drain_one();
        if (output.kind != android::DrainKind::try_again)
          progress = true;
        output_end |= output.end_of_stream;
        if (!pending && !filter_end) {
          auto packet = primed.empty() ? bits.receive()
                                       : PacketRead{ReadStatus::packet,
                                                    std::move(primed.front())};
          if (!primed.empty())
            primed.pop_front();
          if (packet.status == ReadStatus::packet)
            pending = std::move(packet.packet);
          if (packet.status == ReadStatus::end)
            filter_end = true;
        }
        if (pending) {
          const auto pts = input.timestamp_us(video, pending->pts);
          if (!pts || *pts < -1'000'000)
            throw MediaError(
                "Missing video timestamps or more than one second of preroll.");
          if (decoder.queue_access_unit(pending->data, pending->size,
                                        *pts + 1'000'000,
                                        (pending->flags & AV_PKT_FLAG_KEY)
                                            ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
                                            : 0)) {
            last_queued = *pts + 1'000'000;
            pending.reset();
            progress = true;
          }
        }
        if (!original && !input_end && !pending) {
          auto packet = read_video_packet(input);
          input_end = packet.status == ReadStatus::end;
          if (packet.status == ReadStatus::packet &&
              packet.packet->stream_index == video)
            original = std::move(packet.packet);
          progress |= packet.status!=ReadStatus::again;
        }
        if (original && bits.send(original.get())) {
          original.reset();
          progress = true;
        }
        if (input_end && !original && !filter_eos && bits.send(nullptr)) {
          filter_eos = true;
          progress = true;
        }
        if (filter_end && !pending && !codec_eos &&
            decoder.queue_access_unit(nullptr, 0, last_queued + 1,
                                      AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
          codec_eos = true;
          progress = true;
        }
        if (output_end &&
            Clock::now() - last_output > std::chrono::milliseconds(150)) {
          // Keep the worker and decoder alive until queued leases have been
          // presented.
          std::unique_lock<std::mutex> lock(mutex);
          // A seek at the very end can be later than the final frame's PTS.
          // Present that final decoded frame rather than retaining an old image.
          if (frames.empty() && last_preroll && decoded == 0 &&
              !cancel->cancelled() && seek_pending < 0) {
            frames.push_back(std::move(last_preroll));
            ++counters.decoded;decoded_rate.add(performance_now());
            ++decoded;
          }
          if (frames.empty() && (!audio || state.duration_us<=0 || audio->position_us()>=state.duration_us)) {
            if (!state.ended) {
              __android_log_print(
                  ANDROID_LOG_INFO, "CyVRPlayer",
                  "VIDEO_COMPLETE decoded=%u scheduled=%u gpu_submitted=%llu late_drops=%u", decoded,
                  presented,static_cast<unsigned long long>(counters.submitted), dropped);
              state.ended = true;
              state.seeking = false;
              state.playing = false;
              if (state.duration_us > 0)
                state.position_us = state.duration_us;
              state.status = "End of video";
              if(audio)audio->pause(true);
              ++state.revision;
            }
            wake.wait(lock,
                      [&] { return cancel->cancelled() || seek_pending >= 0; });
          } else {
            wake.wait_for(lock, std::chrono::milliseconds(10));
          }
        }
        if (!progress)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!output_end && Clock::now()-last_output>std::chrono::seconds(10))throw MediaError("Hardware video decoder stalled");
      }
      if (!restart)
        break;
      }catch(const std::exception&){
        if(cancel->cancelled())return;
        decoder.close();
        if(!input.seekable())throw;
        std::int64_t position;
        {std::lock_guard<std::mutex> lock(mutex);position=std::max(discard_before,state.position_us);frames.clear();if(audio)audio->pause(true);audio.reset();clock_started=false;}
        input.seek(position);begin_subtitles(input,cancel);
        __android_log_print(ANDROID_LOG_WARN,"CyVRPlayer","HARDWARE_FALLBACK software_retry=1");
        play_software(input,video,cancel,position,config.color);return;
      }
    }
  }
  bool clock_started{};
  Clock::time_point clock_base{};
  std::int64_t pts_base{};
  std::uint64_t presented_epoch{};
#endif
};
PlayerSession::PlayerSession(Platform p)
    : impl_(std::make_unique<Impl>(std::move(p))) {}
PlayerSession::~PlayerSession() = default;
TrackSelection PlayerSession::selection()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->requested_selection;}
SplatSnapshot PlayerSession::splats()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->splat_frame;}
bool PlayerSession::select_track(TrackKind kind,int index){
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if(!impl_->state.ready || !impl_->state.seekable || impl_->open_pending)return false;
  if(index<0 && (index!=-1 || kind==TrackKind::video))return false;
  if(index>=0 && std::none_of(impl_->state.tracks.begin(),impl_->state.tracks.end(),[&](const auto& t){return t.index==index && t.kind==kind && !t.attached_picture;}))return false;
  auto& choice=impl_->requested_selection;
  if(kind==TrackKind::video)choice.video=index;
  else if(kind==TrackKind::audio)choice.audio=index;
  else if(kind==TrackKind::subtitle){choice.subtitle=index;choice.external_subtitle.clear();}
  else return false;
  impl_->requested_start=impl_->state.seeking?impl_->state.seek_target_us:impl_->state.position_us;
  impl_->requested_autoplay=impl_->state.playing;
  impl_->cancellation->cancel();impl_->open_pending=true;impl_->stop_pending=false;
#ifdef __ANDROID__
  if(impl_->audio)impl_->audio->pause(true);impl_->frames.clear();impl_->clock_started=false;
#endif
  impl_->wake.notify_all();return true;
}
bool PlayerSession::external_subtitle(std::string path){
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if(!impl_->state.ready || !impl_->state.seekable || impl_->open_pending)return false;
  impl_->requested_selection.external_subtitle=std::move(path);
  impl_->requested_selection.subtitle=-1;
  impl_->requested_start=impl_->state.position_us;impl_->requested_autoplay=impl_->state.playing;
  impl_->cancellation->cancel();impl_->open_pending=true;impl_->stop_pending=false;
#ifdef __ANDROID__
  if(impl_->audio)impl_->audio->pause(true);impl_->frames.clear();impl_->clock_started=false;
#endif
  impl_->wake.notify_all();return true;
}
SubtitleSnapshot PlayerSession::subtitles()const{
  std::shared_ptr<SubtitleTimeline> timeline;std::int64_t position;
  {std::lock_guard<std::mutex> lock(impl_->mutex);timeline=impl_->subtitle_timeline;position=impl_->state.position_us;
#ifdef __ANDROID__
    if(impl_->audio && impl_->audio->started() && !impl_->paused)position=impl_->audio->position_us();
#endif
  }
  return timeline?timeline->at(position):SubtitleSnapshot{};
}
void PlayerSession::open(std::string location, std::string title,
                         std::int64_t start_us, bool autoplay,TrackSelection selection) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->cancellation->cancel();
#ifdef __ANDROID__
  if(impl_->audio)impl_->audio->pause(true);
#endif
  impl_->location = std::move(location);
  impl_->title = std::move(title);
  impl_->requested_start = std::max<std::int64_t>(0, start_us);
  impl_->requested_autoplay = autoplay;
  impl_->requested_selection=std::move(selection);
  impl_->open_pending = true;
  impl_->stop_pending = false;
  impl_->wake.notify_all();
#ifdef __ANDROID__
  impl_->clock_started = false;
  impl_->frames.clear();
#endif
}
void PlayerSession::stop() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->cancellation->cancel();
#ifdef __ANDROID__
  if(impl_->audio)impl_->audio->pause(true);
#endif
  impl_->stop_pending = true;
  impl_->open_pending = false;
#ifdef __ANDROID__
  impl_->frames.clear();
  impl_->clock_started = false;
#endif
  impl_->wake.notify_all();
}
void PlayerSession::pause(bool paused) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if(impl_->state.volumetric && impl_->state.duration_us==0)return;
  impl_->paused = paused;
#ifdef __ANDROID__
  if(impl_->audio)impl_->audio->pause(paused);
#endif
    if (!paused)
        impl_->pause_after_first = false;
  impl_->state.playing = impl_->state.ready && !paused;
  impl_->decoded_rate.reset();impl_->submitted_rate.reset();
  ++impl_->state.revision;
#ifdef __ANDROID__
  impl_->clock_started = false;
#endif
  impl_->wake.notify_all();
}
void PlayerSession::seek(std::int64_t position) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->state.ready || !impl_->state.seekable)
    return;
  const auto duration = impl_->state.duration_us;
  const auto target = std::clamp<std::int64_t>(position, 0, duration > 0 ? duration - 1 : INT64_MAX);
  impl_->seek_pending = target;
#ifdef __ANDROID__
  if(impl_->audio)impl_->audio->pause(true);
#endif
  impl_->state.seek_target_us = target;
  impl_->state.seeking = true;
  impl_->state.ended = false;
  impl_->decoded_rate.reset();impl_->submitted_rate.reset();
  ++impl_->state.revision;
  if (impl_->paused || !impl_->state.playing) {
    impl_->pause_after_first = true;
    impl_->paused = false;
  }
#ifdef __ANDROID__
  impl_->clock_started = false;
  impl_->frames.clear();
  __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer", "SEEK_REQUEST position_us=%lld",static_cast<long long>(target));
#else
  // The Windows host inspects media; expose the selected position without claiming playback.
  if(!impl_->state.volumetric){impl_->state.position_us = target;impl_->state.seeking = false;impl_->seek_pending = -1;}
#endif
  impl_->wake.notify_all();
}
PlayerState PlayerSession::state() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state;
}
VideoMetrics PlayerSession::metrics()const{
  std::lock_guard<std::mutex> lock(impl_->mutex);auto result=impl_->counters;
  result.decode_rate=impl_->decoded_rate.sample(performance_now());result.submit_rate=impl_->submitted_rate.sample(performance_now());
#ifdef __ANDROID__
  result.queued=impl_->frames.size();
#endif
  return result;
}
void PlayerSession::frame_submitted(std::uint64_t epoch,std::int64_t pts){
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if(impl_->open_pending||impl_->stop_pending||!impl_->state.ready)return;
  if(impl_->state.volumetric && impl_->splat_frame.epoch==epoch && impl_->splat_ack_epoch<epoch){impl_->splat_ack_epoch=epoch;impl_->splat_presented_at=Clock::now();impl_->wake.notify_all();}
  if(impl_->unique_submissions.accept(epoch,pts)){++impl_->counters.submitted;impl_->submitted_rate.add(performance_now());}
}
#ifdef __ANDROID__
android::ImageLease PlayerSession::take_frame() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->paused || impl_->frames.empty() || impl_->seek_pending >= 0 ||
      impl_->open_pending || impl_->stop_pending)
    return {};
  const auto now = Clock::now();
  auto &first = impl_->frames.front();
  if (!impl_->clock_started || impl_->presented_epoch != first.epoch) {
    impl_->clock_base = now;
    impl_->pts_base = first.pts_us;
    impl_->clock_started = true;
    impl_->presented_epoch = first.epoch;
  }
  if(impl_->audio && !impl_->pause_after_first)impl_->audio->start();
  const auto due = impl_->audio && impl_->audio->started() ? impl_->audio->position_us() :
      impl_->pts_base + std::chrono::duration_cast<std::chrono::microseconds>(
                            now - impl_->clock_base)
                            .count();
  if (first.pts_us > due + 2000)
    return {};
  android::ImageLease image = std::move(first);
  impl_->frames.pop_front();
  while (!impl_->frames.empty() && impl_->frames.front().pts_us <= due) {
    ++impl_->dropped;
    ++impl_->counters.dropped;
    image = std::move(impl_->frames.front());
    impl_->frames.pop_front();
  }
  if (impl_->state.seeking || std::abs(image.pts_us - impl_->state.position_us) > 200000) {
    ++impl_->state.revision;
  }
  impl_->state.position_us = image.pts_us;
  impl_->state.seeking = false;
  ++impl_->presented;
  ++impl_->counters.scheduled;
  if (impl_->pause_after_first) {
    impl_->pause_after_first = false;
    impl_->paused = true;
    if(impl_->audio)impl_->audio->pause(true);
    impl_->state.playing = false;
    ++impl_->state.revision;
  }
  impl_->wake.notify_all();
  return image;
}
#endif
} // namespace cyvr::player
