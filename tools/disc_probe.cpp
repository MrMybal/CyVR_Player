#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/subtitles.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}
using namespace cyvr::player;
int main(int argc, char **argv) {
  av_log_set_level(AV_LOG_ERROR);
  try {
    if (argc < 2)
      throw std::runtime_error("DISC_ISO [title] [seek_us]");
    StreamOptions options;
    if (argc > 2)
      options.disc_title = std::stoi(argv[2]);
    Demuxer input;
    input.open(MediaSource::file(argv[1]), options);
    if (argc > 3)
      input.seek(std::stoll(argv[3]));
    std::map<int, std::unique_ptr<SoftwareDecoder>> decoders;
    std::map<int, std::unique_ptr<SubtitleDecoder>> subs;
    std::map<int, std::uint64_t> frames;
    std::map<int, std::int64_t> first, last;
    std::uint64_t bitmap_pixels = 0;
    for (const auto &t : input.tracks()) {
      if (t.kind == TrackKind::video || t.kind == TrackKind::audio)
        decoders[t.index] = std::make_unique<SoftwareDecoder>(input, t.index);
      if (t.kind == TrackKind::subtitle)
        subs[t.index] = std::make_unique<SubtitleDecoder>(input, t.index);
    }
    auto drain = [&](int id) {
      for (;;) {
        auto f = decoders[id]->receive();
        if (!f.frame)
          break;
        const auto ts =
            input.timestamp_us(id, f.frame->best_effort_timestamp).value_or(0);
        if (!frames[id])
          first[id] = ts;
        else if (ts < last[id])
          throw std::runtime_error("Title timestamp moved backwards");
        last[id] = ts;
        ++frames[id];
      }
    };
    for (int n = 0; n < 100000; ++n) {
      auto p = input.read();
      if (p.status == ReadStatus::end)
        break;
      if (!p.packet)
        continue;
      const int id = p.packet->stream_index;
      if (decoders.count(id)) {
        try {
          if (!decoders[id]->send(p.packet.get())) {
            drain(id);
            if (!decoders[id]->send(p.packet.get()))
              throw std::runtime_error("stalled");
          }
          drain(id);
        } catch (const std::exception &e) {
          throw std::runtime_error("stream=" + std::to_string(id) +
                                   " pos=" + std::to_string(p.packet->pos) +
                                   " size=" + std::to_string(p.packet->size) +
                                   " pts=" + std::to_string(p.packet->pts) +
                                   ": " + e.what());
        }
      }
      if (subs.count(id)) {
        auto cue = subs[id]->decode(p.packet.get());
        if (cue)
          for (const auto &b : cue->bitmaps)
            for (std::size_t a = 3; a < b.rgba.size(); a += 4)
              if (b.rgba[a])
                ++bitmap_pixels;
      }
    }
    for (auto &entry : decoders) {
      if (!entry.second->send(nullptr)) {
        drain(entry.first);
        entry.second->send(nullptr);
      }
      drain(entry.first);
    }
    std::cout << "{\"type\":\"" << input.disc().type
              << "\",\"title\":" << input.disc().selected
              << ",\"bitmap_pixels\":" << bitmap_pixels << ",\"streams\":[";
    bool comma = false;
    for (const auto &entry : frames) {
      if (comma)
        std::cout << ',';
      comma = true;
      std::cout << "{\"index\":" << entry.first
                << ",\"frames\":" << entry.second
                << ",\"first\":" << first[entry.first]
                << ",\"last\":" << last[entry.first] << '}';
    }
    std::cout << "]}\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
