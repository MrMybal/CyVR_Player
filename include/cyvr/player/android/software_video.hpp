#pragma once
#include "cyvr/player/android/surface_decoder.hpp"
#include "cyvr/player/demuxer.hpp"
namespace cyvr::player::android {
class SoftwareVideo {
public:
  SoftwareVideo(const Demuxer&,int stream,VideoColor color,std::uint64_t epoch,std::int64_t start_us=0,bool multiview=false);
  ~SoftwareVideo();
  bool send(const AVPacket*);
  ImageLease receive();
  bool ended() const;
  bool stereo() const;
private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}
