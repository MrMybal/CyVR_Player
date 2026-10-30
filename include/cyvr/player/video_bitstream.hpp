#pragma once
#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/video_color.hpp"

namespace cyvr::player {
struct VideoConfiguration {
    std::string mime;
    int width=0,height=0;
    VideoColor color;
    // Android csd-0, csd-1... AVC: SPS/PPS separate; HEVC: VPS/SPS/PPS combined.
    std::vector<std::vector<std::uint8_t>> codec_data;
};
// Converts AVC/HEVC access units into Annex B with the official FFmpeg BSFs.
// This only prepares compressed data; it does not copy decoded image pixels.
class VideoBitstream {
public:
    VideoBitstream(const Demuxer& source,int stream);
    ~VideoBitstream();
    VideoBitstream(const VideoBitstream&)=delete;
    VideoBitstream& operator=(const VideoBitstream&)=delete;
    const VideoConfiguration& configuration() const noexcept;
    // MPEG-TS/HLS can carry parameter sets only in-band. Feed/drain compressed
    // packets until ready, retaining them under a bounded startup budget.
    bool configuration_ready() const noexcept;
    bool send(const AVPacket* packet); // nullptr = end of stream
    PacketRead receive();
    void flush();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
