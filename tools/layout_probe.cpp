#include "cyvr/player/demuxer.hpp"
#include <iostream>
#include <cstring>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
}
using namespace cyvr::player;
// Create original metadata fixtures by remuxing generated test media.
void write_metadata(const char *source, const char *destination, int packing,
                    int projection) {
  AVFormatContext *in{}, *out{};
  AVPacket *packet{};
  auto check = [](int r) {
    if (r < 0)
      throw std::runtime_error("Metadata fixture remux failed");
  };
  try {
    check(avformat_open_input(&in, source, nullptr, nullptr));
    check(avformat_find_stream_info(in, nullptr));
    check(avformat_alloc_output_context2(&out, nullptr, "mp4", destination));
    out->strict_std_compliance=FF_COMPLIANCE_UNOFFICIAL;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      auto *stream = avformat_new_stream(out, nullptr);
      if (!stream)
        throw std::bad_alloc();
      check(
          avcodec_parameters_copy(stream->codecpar, in->streams[i]->codecpar));
      stream->time_base = in->streams[i]->time_base;
      stream->codecpar->codec_tag = 0;
      if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        auto *stereo = av_packet_side_data_new(
            &stream->codecpar->coded_side_data,
            &stream->codecpar->nb_coded_side_data, AV_PKT_DATA_STEREO3D,
            sizeof(AVStereo3D), 0);
        if (!stereo)
          throw std::bad_alloc();
        std::memset(stereo->data,0,stereo->size);
        auto *value = reinterpret_cast<AVStereo3D *>(stereo->data);
        value->type = packing == 1   ? AV_STEREO3D_SIDEBYSIDE
                      : packing == 2 ? AV_STEREO3D_TOPBOTTOM
                                     : AV_STEREO3D_2D;
        value->view = AV_STEREO3D_VIEW_UNSPEC;
        auto *spherical = av_packet_side_data_new(
            &stream->codecpar->coded_side_data,
            &stream->codecpar->nb_coded_side_data, AV_PKT_DATA_SPHERICAL,
            sizeof(AVSphericalMapping), 0);
        if (!spherical)
          throw std::bad_alloc();
        std::memset(spherical->data,0,spherical->size);
        auto *mapping = reinterpret_cast<AVSphericalMapping *>(spherical->data);
        mapping->projection = projection == 1
                                  ? AV_SPHERICAL_EQUIRECTANGULAR_TILE
                              : projection == 2 ? AV_SPHERICAL_EQUIRECTANGULAR
                              : projection == 3 ? AV_SPHERICAL_CUBEMAP
                                                : AV_SPHERICAL_RECTILINEAR;
        if (projection == 1) {
          mapping->bound_left = 0x40000000u;
          mapping->bound_right = 0x40000000u;
        }
      }
    }
    check(avio_open(&out->pb, destination, AVIO_FLAG_WRITE));
    check(avformat_write_header(out, nullptr));
    packet = av_packet_alloc();
    if (!packet)
      throw std::bad_alloc();
    while (av_read_frame(in, packet) >= 0) {
      const auto n = packet->stream_index;
      av_packet_rescale_ts(packet, in->streams[n]->time_base,
                           out->streams[n]->time_base);
      packet->pos = -1;
      check(av_interleaved_write_frame(out, packet));
      av_packet_unref(packet);
    }
    check(av_write_trailer(out));
  } catch (...) {
    av_packet_free(&packet);
    if (out) {
      if (out->pb)
        avio_closep(&out->pb);
      avformat_free_context(out);
    }
    avformat_close_input(&in);
    throw;
  }
  av_packet_free(&packet);
  avio_closep(&out->pb);
  avformat_free_context(out);
  avformat_close_input(&in);
}
int main(int argc, char **argv) {
  try {
    if (argc == 6 && std::string(argv[1]) == "--write-metadata") {
      write_metadata(argv[2], argv[3], std::stoi(argv[4]), std::stoi(argv[5]));
      return 0;
    }
    if (argc != 2)
      throw std::runtime_error("MEDIA");
    Demuxer input;
    input.open(MediaSource::file(argv[1]));
    for (const auto &track : input.tracks())
      if (track.kind == TrackKind::video) {
        auto f = detect_video_layout(argv[1], "", track.layout, track.width,
                                     track.height);
        std::cout << "{\"stereo\":" << f.stereo
                  << ",\"projection\":" << f.projection
                  << ",\"half\":" << (f.half ? "true" : "false")
                  << ",\"swap\":" << (f.swap ? "true" : "false")
                  << ",\"evidence\":" << f.evidence
                  << ",\"ambiguous\":" << (f.ambiguous ? "true" : "false")
                  << "}\n";
        return 0;
      }
    throw std::runtime_error("No video");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
