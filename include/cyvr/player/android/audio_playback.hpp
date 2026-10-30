#pragma once
#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/audio_queue.hpp"
namespace cyvr::player::android {
struct AudioPlaybackStats {
  AudioQueueStats queue;float peak{};std::uint64_t limited{},invalid{};int xruns{},buffer_frames{},rate{};
};
class AudioPlayback {
public:
  AudioPlayback(const Demuxer&,int stream,std::int64_t start_us);
  ~AudioPlayback();
  bool send(const AVPacket*);
  void pump();
  void start();
  void pause(bool);
  bool started() const;
  bool ready() const;
  std::int64_t buffered_us() const;
  AudioPlaybackStats stats() const;
  std::int64_t position_us() const;
private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}
