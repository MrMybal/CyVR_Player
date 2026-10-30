#pragma once
#include "cyvr/player/demuxer.hpp"
#include <deque>

namespace cyvr::player {
// Produces full-resolution SBS frames. Eye boundaries remain independent in
// the existing projection, correction and reconstruction shaders.
FramePtr pack_stereo_frames(const AVFrame& left, const AVFrame& right);
class StereoFrameAssembler {
public:
    FramePtr push(FramePtr frame);
    void clear() { pending_.clear(); }
    bool empty() const { return pending_.empty(); }
private:
    std::deque<FramePtr> pending_;
};
class MvcDecoder {
public:
    MvcDecoder(const Demuxer&, int stream, int threads, std::int64_t max_pixels);
    ~MvcDecoder();
    bool send(const AVPacket*);
    FrameRead receive();
    void flush();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool packed_stereo_frame(const AVFrame&) noexcept;
}
