#include "cyvr/player/application.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#ifdef __ANDROID__
#include "cyvr/player/android/video_format.hpp"
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
  VideoMetrics counters;
  EventRate decoded_rate,submitted_rate;
  UniqueVideoFrames unique_submissions;
  std::shared_ptr<Cancellation> cancellation = std::make_shared<Cancellation>();
#ifdef __ANDROID__
  std::deque<android::ImageLease> frames;
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
          state = PlayerState{state.revision + 1};
        }
        if (!open_pending)
          continue;
        target = location;
        name = title;
        open_pending = false;
        paused = false;
        start_us = requested_start;
        pause_after_first = !requested_autoplay;
        seek_pending = -1;
        cancellation = cancel = std::make_shared<Cancellation>();
#ifdef __ANDROID__
        frames.clear();
#endif
        state = PlayerState{state.revision + 1};
        state.status = "Opening media...";
        counters={};decoded_rate.reset();submitted_rate.reset();unique_submissions.reset();
        state.title = name;
      }
      try {
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
        const int video = input.best_video_stream();
        if (video < 0)
          throw MediaError("This media has no video track.");
        const auto &parameters = *input.codec_parameters(video);
#ifdef __ANDROID__
        if (parameters.color_trc == AVCOL_TRC_SMPTE2084 ||
            parameters.color_trc == AVCOL_TRC_ARIB_STD_B67)
          throw MediaError(
              "HDR tone mapping is not supported by this SDR profile yet.");
#endif
        publish([&](PlayerState &s) {
          s.tracks = input.tracks();
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
#ifdef __ANDROID__
  void play(Demuxer &input, int video,
            const std::shared_ptr<Cancellation> &cancel,
            std::int64_t start_us) {
    std::uint64_t epoch = ++epoch_counter;
    std::int64_t discard_before = start_us;
    while (!cancel->cancelled()) {
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
            auto packet = input.read();
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
      const auto name =
          platform.hardware_codec(config.mime, config.width, config.height);
      if (name.empty())
        throw MediaError("No compatible hardware decoder. This profile "
                         "supports AVC and HEVC.");
      auto format = android::make_video_format(config);
      android::SurfaceDecoder decoder;
      decoder.open(name.c_str(), format.get(), epoch, 6);
      __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer",
                          "DECODER_READY %s %dx%d", name.c_str(), config.width,
                          config.height);
      publish([&](PlayerState &s) {
        s.ready = true;
        s.playing = !paused && !pause_after_first;
        s.ended = false;
        s.codec = name;
        s.status = "Hardware playback / no CPU pixel copies";
      });
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
          auto packet = input.read();
          input_end = packet.status == ReadStatus::end;
          if (packet.status == ReadStatus::packet &&
              packet.packet->stream_index == video)
            original = std::move(packet.packet);
          progress = true;
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
          if (frames.empty()) {
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
      }
      if (!restart)
        break;
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
void PlayerSession::open(std::string location, std::string title,
                         std::int64_t start_us, bool autoplay) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->cancellation->cancel();
  impl_->location = std::move(location);
  impl_->title = std::move(title);
  impl_->requested_start = std::max<std::int64_t>(0, start_us);
  impl_->requested_autoplay = autoplay;
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
  impl_->paused = paused;
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
  impl_->state.position_us = target;
  impl_->state.seeking = false;
  impl_->seek_pending = -1;
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
  const auto due =
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
    impl_->state.playing = false;
    ++impl_->state.revision;
  }
  impl_->wake.notify_all();
  return image;
}
#endif
} // namespace cyvr::player
