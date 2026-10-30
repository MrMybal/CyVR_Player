#pragma once
#include "cyvr/player/demuxer.hpp"
namespace cyvr::player::android {
class AudioPlayback {
public:
  AudioPlayback(const Demuxer&,int stream,std::int64_t start_us);
  ~AudioPlayback();
  bool send(const AVPacket*);
  void pump();
  void start();
  void pause(bool);
  bool started() const;
  std::int64_t position_us() const;
private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}
