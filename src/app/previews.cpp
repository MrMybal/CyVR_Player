#include "previews.hpp"
extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
namespace cyvr::player {
namespace {
constexpr int thumb_w = 160, thumb_h = 90;
std::vector<std::uint8_t>
decode_preview(const Platform &platform, const std::string &location,
               const std::shared_ptr<Cancellation> &cancel) {
  // Never auto-fetch a stream URL. SAF/local files are opened on this worker
  // only.
  if (location.find("://") != std::string::npos &&
      location.rfind("content://", 0) != 0)
    return {};
  if (platform.thumbnail) {
    try {
      auto pixels = platform.thumbnail(location);
      if (pixels.size() == thumb_w * thumb_h * 4 && !cancel->cancelled())
        return pixels;
    } catch (const std::exception &) {
    }
  }
  if (cancel->cancelled())
    return {};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  Demuxer input;
  StreamOptions options;
  options.open_timeout_ms = 1500;
  options.read_timeout_ms = 1500;
  options.probe_bytes = 2 * 1024 * 1024;
  options.analyze_us = 1000000;
  if (location.rfind("content://", 0) == 0) {
    if (!platform.open_document)
      return {};
    auto source = platform.open_document(location);
    struct Close {
      int fd;
      ~Close() {
        if (fd >= 0) {
#ifdef _WIN32
          _close(fd);
#else
          ::close(fd);
#endif
        }
      }
    } close{source.fd};
    input.open(source, options, cancel);
  } else {
    if (location.find("://") != std::string::npos)
      return {};
    input.open(MediaSource::file(location), options, cancel);
  }
  const int stream = input.best_video_stream();
  if (stream < 0)
    return {};
  const auto &info =
      *std::find_if(input.tracks().begin(), input.tracks().end(),
                    [&](const auto &t) { return t.index == stream; });
  if (info.width <= 0 || info.height <= 0 ||
      std::int64_t(info.width) * info.height > 64 * 1024 * 1024)
    return {};
  // A single decoder, one thread, first decodable frame. No continuous preview
  // decode.
  SoftwareDecoder decoder(input, stream, 1, 64 * 1024 * 1024);
  const auto duration = input.duration_us().value_or(0);
  if (input.seekable() && duration > 10000000) {
    input.seek(std::min<std::int64_t>(duration / 10, 30000000));
    decoder.flush();
  }
  PacketPtr pending;
  bool end = false;
  for (int n = 0; n < 4096 && !cancel->cancelled() &&
                  std::chrono::steady_clock::now() < deadline;
       ++n) {
    if (!pending && !end) {
      auto packet = input.read();
      if (packet.status == ReadStatus::again)
        continue;
      if (packet.status == ReadStatus::packet) {
        if (packet.packet->stream_index != stream)
          continue;
        pending = std::move(packet.packet);
      } else
        end = true;
    }
    if (pending) {
      if (decoder.send(pending.get()))
        pending.reset();
    } else if (end)
      decoder.send(nullptr);
    auto frame = decoder.receive();
    if (frame.status == DecodeStatus::frame) {
      auto *f = frame.frame.get();
      if (f->width <= 0 || f->height <= 0 ||
          std::int64_t(f->width) * f->height > 64 * 1024 * 1024)
        return {};
      const double par =
          f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0
              ? double(f->sample_aspect_ratio.num) / f->sample_aspect_ratio.den
              : 1.;
      const double aspect = double(f->width) * par / f->height;
      const int w = std::max(1, std::min(thumb_w, int(thumb_h * aspect))),
                h = std::max(1, std::min(thumb_h, int(thumb_w / aspect)));
      auto *scaler = sws_getContext(
          f->width, f->height, AVPixelFormat(f->format), w, h, AV_PIX_FMT_RGBA,
          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
      if (!scaler)
        return {};
      std::vector<std::uint8_t> result(thumb_w * thumb_h * 4, 0);
      for (std::size_t a = 3; a < result.size(); a += 4)
        result[a] = 255;
      std::uint8_t *dst[]{result.data() +
                          ((thumb_h - h) / 2 * thumb_w + (thumb_w - w) / 2) *
                              4};
      int stride[]{thumb_w * 4};
      const int lines =
          sws_scale(scaler, f->data, f->linesize, 0, f->height, dst, stride);
      sws_freeContext(scaler);
      return lines == h ? result : std::vector<std::uint8_t>{};
    }
    if (frame.status == DecodeStatus::end)
      break;
  }
  return {};
}
} // namespace
PlayerApplication::PreviewWorker::PreviewWorker(Platform p)
    : platform(std::move(p)), worker([this] { run(); }) {}
PlayerApplication::PreviewWorker::~PreviewWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    quit = true;
    cancellation->cancel();
  }
  wake.notify_one();
  worker.join();
}
void PlayerApplication::PreviewWorker::request(std::vector<std::string> paths,
                                               bool refresh) {
  std::lock_guard<std::mutex> lock(mutex);
  cancellation->cancel();
  requests = std::move(paths);
  pending = true;
  reset_failures = true;
  invalidate_cache = invalidate_cache || refresh;
  ++generation;
  atlas.assign(thumb_w * thumb_h * 7 * 4, 0);
  present.fill(false);
  ++revision;
  wake.notify_one();
}
void PlayerApplication::PreviewWorker::run() {
  for (;;) {
    std::vector<std::string> paths;
    std::shared_ptr<Cancellation> cancel;
    std::uint64_t ticket;
    {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [&] { return pending || quit; });
      if (quit)
        return;
      paths = requests;
      ticket = generation;
      pending = false;
      if (reset_failures) {
        failures.clear();
        reset_failures = false;
      }
      if (invalidate_cache) {
        cache.clear();
        lru.clear();
        invalidate_cache = false;
      }
      cancellation = cancel = std::make_shared<Cancellation>();
    }
    for (std::size_t row = 0;
         row < paths.size() && row < 7 && !cancel->cancelled(); ++row) {
      const auto &path = paths[row];
      if (path.empty())
        continue;
      auto found = cache.find(path);
      if (found == cache.end() && failures[path] < 3) {
        std::vector<std::uint8_t> pixels;
        try {
          pixels = decode_preview(platform, path, cancel);
        } catch (const std::exception &) {
        }
        if (cancel->cancelled())
          break;
        if (pixels.empty()) {
          ++failures[path];
          continue;
        }
        failures.erase(path);
        if (cache.size() >= 32) {
          cache.erase(lru.front());
          lru.erase(lru.begin());
        }
        found = cache.emplace(path, std::move(pixels)).first;
        lru.push_back(path);
      } else if (found != cache.end()) {
        lru.erase(std::remove(lru.begin(), lru.end(), path), lru.end());
        lru.push_back(path);
      }
      std::lock_guard<std::mutex> lock(mutex);
      if (quit || ticket != generation)
        break;
      if (found != cache.end() && !found->second.empty()) {
        std::copy(found->second.begin(), found->second.end(),
                  atlas.begin() + row * thumb_w * thumb_h * 4);
        present[row] = true;
        ++revision;
      }
    }
    {
      bool retry = false;
      for (const auto &path : paths)
        if (!path.empty() && !cache.count(path) && failures[path] < 3)
          retry = true;
      std::unique_lock<std::mutex> lock(mutex);
      if (retry && !quit && ticket == generation) {
        wake.wait_for(lock, std::chrono::seconds(2),
                      [&] { return quit || pending; });
        if (!quit && ticket == generation)
          pending = true;
      }
    }
  }
}
} // namespace cyvr::player
