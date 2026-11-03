#include "cyvr/player/state.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
namespace cyvr::player {
namespace {
std::mutex writes;
constexpr std::size_t max_bytes = 256 * 1024;
struct Codec {
  std::string bytes{"CYVRP04\0", 8};
  std::size_t cursor{8};
  void put(std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
      bytes += char((value >> (i * 8)) & 255);
  }
  std::uint64_t get() {
    if (cursor + 8 > bytes.size())
      throw std::runtime_error("Truncated state");
    std::uint64_t n{};
    for (int i = 0; i < 8; ++i)
      n |= std::uint64_t(static_cast<unsigned char>(bytes[cursor++]))
           << (i * 8);
    return n;
  }
  void put(const std::string &s) {
    if (s.size() > 16384 || s.find('\0') != std::string::npos)
      throw std::runtime_error("State string budget exceeded");
    put(s.size());
    bytes += s;
  }
  std::string string() {
    auto n = get();
    if (n > 16384 || n > bytes.size() - cursor)
      throw std::runtime_error("Invalid state string");
    auto s = bytes.substr(cursor, std::size_t(n));
    cursor += std::size_t(n);
    if (s.find('\0') != std::string::npos)
      throw std::runtime_error("Invalid state string");
    return s;
  }
};
// Reaches storage before the rename, so a power loss cannot leave an empty file.
bool write_durable(const std::filesystem::path &path, const std::string &bytes) {
#ifdef _WIN32
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), std::streamsize(bytes.size()));
  file.flush();
  return bool(file);
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count <= 0) {
      ::close(fd);
      return false;
    }
    written += std::size_t(count);
  }
  const bool synced = ::fsync(fd) == 0;
  return ::close(fd) == 0 && synced;
#endif
}
// One overlong value, such as a huge VIEW URL, must not block every later save.
const std::string &storable(const std::string &value) {
  static const std::string empty;
  return value.size() > 16384 ? empty : value;
}
} // namespace
bool save_application_state(const std::string &path,
                            const ApplicationState &s) noexcept {
  if (path.empty())
    return false;
  try {
    Codec c;
    c.put(s.directory);
    c.put(s.url);
    c.put(storable(s.source));
    c.put(storable(s.title));
    if (s.parents.size() > 128)
      throw std::runtime_error("State navigation budget exceeded");
    c.put(s.parents.size());
    for (const auto &p : s.parents)
      c.put(p);
    c.put(std::uint64_t(std::max<std::int64_t>(0, s.position_us)));
    c.put(s.page);
    c.put(s.offset);
    c.put(s.stereo);
    c.put(s.projection);
    c.put((s.half_stereo ? 1 : 0) | (s.swap_eyes ? 2 : 0) | (s.shift ? 4 : 0) |
          (s.symbols ? 8 : 0) | (s.playing ? 16 : 0));
    // Optional tagged extension: old 0.4 host state remains readable.
    c.put(std::uint64_t(0x494d4732));
    auto image = s.image;
    image.sanitize();
    for (float value :
         {image.exposure, image.contrast, image.saturation, image.gamma,
          image.zoom, image.pan_x, image.pan_y, image.sharpness}) {
      std::uint32_t bits;
      std::memcpy(&bits, &value, 4);
      c.put(std::uint64_t(bits));
    }
    c.put(image.quality);
    c.put(image.upscale);
    c.put(std::clamp(s.display_tab, 0, 4));
    c.put(std::uint64_t(0x54524b31));
    for(int stream:{s.tracks.video,s.tracks.audio,s.tracks.subtitle})c.put(std::uint64_t(std::clamp(stream,-2,255)+2));
    c.put(s.tracks.external_subtitle);
    c.put(std::uint64_t(0x55494d31));c.put(s.tracks_open?1:0);c.put(std::clamp(s.track_tab,0,4));c.put(std::clamp(s.track_offset,0,10000));
    c.put(std::uint64_t(0x4b455931));c.put(s.tracks.clearkey_file);
    c.put(std::uint64_t(0x43494e31));
    auto cinema=s.cinema;cinema.sanitize();c.put(cinema.environment);
    for(float value:{cinema.ambient,cinema.panel_opacity,cinema.width,cinema.distance,cinema.curvature,cinema.elevation,cinema.pitch,cinema.roll}){
      std::uint32_t bits;std::memcpy(&bits,&value,4);c.put(bits);
    }
    c.put(std::clamp(s.cinema_tab,0,2));
    c.put(std::uint64_t(0x414d4231));auto ambilight=s.ambilight;ambilight.sanitize();
    c.put((ambilight.enabled?1:0)|(ambilight.auto_bars?2:0));
    for(float value:{ambilight.intensity,ambilight.spread,ambilight.smoothing,ambilight.saturation,ambilight.inset}){std::uint32_t bits;std::memcpy(&bits,&value,4);c.put(bits);}
    c.put(std::uint64_t(0x464d5431));c.put(s.format_manual?1:0);
    c.put(std::uint64_t(0x44534331));c.put(std::clamp(s.tracks.disc_title,-1,4095)+1);c.put(std::clamp(s.tracks.disc_angle,0,8));
    c.put(std::uint64_t(0x41555431));c.put(s.auto_format?1:0);
    c.put(std::uint64_t(0x464c5431));c.put(image.sampling);
    for(float value:{image.denoise,image.detail}){std::uint32_t bits;std::memcpy(&bits,&value,4);c.put(bits);}
    c.put(std::uint64_t(0x42525331));c.put(std::clamp(s.browser_view,0,1));
    auto scene=s.scene;scene.sanitize();c.put(scene.stable?1:0);
    for(float value:{scene.x,scene.y,scene.z,scene.yaw,scene.pitch,scene.roll,std::isfinite(s.volume)?std::clamp(s.volume,0.f,1.f):1.f}){std::uint32_t bits;std::memcpy(&bits,&value,4);c.put(bits);}
    if(s.favorites.size()>16)throw std::runtime_error("Folder bookmark limit exceeded");
    c.put(s.favorites.size());
    for(const auto& favorite:s.favorites){c.put(favorite.location);c.put(favorite.name);if(favorite.parents.size()>32)throw std::runtime_error("Bookmark navigation limit exceeded");c.put(favorite.parents.size());for(const auto& parent:favorite.parents)c.put(parent);}
    c.put(std::uint64_t(0x485a3132));c.put(s.smooth_video?1:0);
    c.put(std::uint64_t(0x53554231));auto sub=s.subtitle_settings;sub.sanitize();
    for(float value:{sub.scale,sub.x,sub.y,sub.opacity,sub.outline,sub.shadow,sub.distance}){std::uint32_t bits;std::memcpy(&bits,&value,4);c.put(bits);}
    c.put(sub.color);c.put((sub.custom?1:0)|(sub.bold?2:0)|(sub.italic?4:0)|(sub.box?8:0)|(sub.follow_depth?16:0));
    c.put(s.subtitle_settings_open?1:0);c.put(std::clamp(s.subtitle_settings_tab,0,1));
    c.put(std::uint64_t(0x53434c31));
    const float scroll=std::isfinite(s.browser_scroll)?std::clamp(s.browser_scroll,-1.f,2600000.f):-1.f;
    std::uint32_t scroll_bits;std::memcpy(&scroll_bits,&scroll,4);c.put(scroll_bits);
    c.put(std::uint64_t(0x574e4431)); // WND1: immersive interface window preferences.
    c.put((s.window_resizable?1:0)|(s.window_face_viewer?2:0)|(s.window_settings_open?4:0));
    if (c.bytes.size() > max_bytes)
      throw std::runtime_error("State budget exceeded");
    std::lock_guard<std::mutex> lock(writes);
    const auto destination = std::filesystem::u8path(path),
               temporary = std::filesystem::u8path(path + ".tmp");
    std::filesystem::create_directories(destination.parent_path());
    if (!write_durable(temporary, c.bytes))
      return false;
    // Keep the previous checkpoint: it is the fallback for a damaged file.
    std::error_code absent;
    std::filesystem::rename(destination, std::filesystem::u8path(path + ".bak"), absent);
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0)
      return false;
    if (const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_CLOEXEC); directory >= 0) {
      ::fsync(directory);
      ::close(directory);
    }
    return true;
#endif
  } catch (...) {
    return false;
  }
}
static std::optional<ApplicationState> read_state(const std::string &path) noexcept {
  try {
    std::ifstream file(std::filesystem::u8path(path),
                       std::ios::binary | std::ios::ate);
    if (!file)
      return {};
    const auto size = file.tellg();
    if (size < 8 || size > std::streamoff(max_bytes))
      return {};
    Codec c;
    c.bytes.resize(std::size_t(size));
    file.seekg(0);
    file.read(c.bytes.data(), size);
    if (!file || c.bytes.compare(0, 8, std::string("CYVRP04\0", 8)) != 0)
      return {};
    ApplicationState s;
    s.directory = c.string();
    s.url = c.string();
    s.source = c.string();
    s.title = c.string();
    auto count = c.get();
    if (count > 128)
      return {};
    for (std::size_t i = 0; i < count; ++i)
      s.parents.push_back(c.string());
    auto position = c.get();
    if (position > std::uint64_t(INT64_MAX))
      return {};
    s.position_us = std::int64_t(position);
    s.page = int(std::min<std::uint64_t>(4, c.get()));
    s.offset = int(std::min<std::uint64_t>(9999, c.get()));
    s.stereo = int(std::min<std::uint64_t>(2, c.get()));
    s.projection = int(std::min<std::uint64_t>(2, c.get()));
    auto flags = c.get();
    s.half_stereo = flags & 1;
    s.swap_eyes = flags & 2;
    s.shift = flags & 4;
    s.symbols = flags & 8;
    s.playing = flags & 16;
    if (c.cursor < c.bytes.size()) {
      const auto imageTag=c.get();
      if(imageTag!=0x494d4731&&imageTag!=0x494d4732)return {};
      for (float *value :
           {&s.image.exposure, &s.image.contrast, &s.image.saturation,
            &s.image.gamma, &s.image.zoom, &s.image.pan_x, &s.image.pan_y,
            &s.image.sharpness}) {
        const auto raw = c.get();
        if (raw > UINT32_MAX)
          return {};
        const auto bits = std::uint32_t(raw);
        std::memcpy(value, &bits, 4);
      }
      s.image.quality = int(std::min<std::uint64_t>(3, c.get()));
      const auto savedScale=c.get();s.image.upscale=imageTag==0x494d4731?0:int(std::min<std::uint64_t>(3,savedScale));
      s.image.sanitize();
      if (c.cursor < c.bytes.size())
        s.display_tab = int(std::min<std::uint64_t>(4, c.get()));
      if(c.cursor<c.bytes.size()){
        if(c.get()!=0x54524b31)return {};
        for(int* stream:{&s.tracks.video,&s.tracks.audio,&s.tracks.subtitle}){const auto value=c.get();if(value>257)return {};*stream=int(value)-2;}
        s.tracks.external_subtitle=c.string();
        if(c.cursor<c.bytes.size()){if(c.get()!=0x55494d31)return {};s.tracks_open=c.get()!=0;s.track_tab=int(std::min<std::uint64_t>(4,c.get()));s.track_offset=int(std::min<std::uint64_t>(10000,c.get()));}
        if(c.cursor<c.bytes.size()){if(c.get()!=0x4b455931)return {};s.tracks.clearkey_file=c.string();}
        if(c.cursor<c.bytes.size()){
          if(c.get()!=0x43494e31)return {};s.cinema.environment=int(std::min<std::uint64_t>(7,c.get()));
          for(float* value:{&s.cinema.ambient,&s.cinema.panel_opacity,&s.cinema.width,&s.cinema.distance,&s.cinema.curvature,&s.cinema.elevation,&s.cinema.pitch,&s.cinema.roll}){
            const auto raw=c.get();if(raw>UINT32_MAX)return {};const auto bits=std::uint32_t(raw);std::memcpy(value,&bits,4);
          }
          s.cinema.sanitize();s.cinema_tab=int(std::min<std::uint64_t>(2,c.get()));
          if(c.cursor<c.bytes.size()){
            if(c.get()!=0x414d4231)return {};const auto flags=c.get();s.ambilight.enabled=flags&1;s.ambilight.auto_bars=flags&2;
            for(float* value:{&s.ambilight.intensity,&s.ambilight.spread,&s.ambilight.smoothing,&s.ambilight.saturation,&s.ambilight.inset}){const auto raw=c.get();if(raw>UINT32_MAX)return {};const auto bits=std::uint32_t(raw);std::memcpy(value,&bits,4);}
            s.ambilight.sanitize();
            if(c.cursor<c.bytes.size()){if(c.get()!=0x464d5431)return {};s.format_manual=c.get()!=0;}
            if(c.cursor<c.bytes.size()){if(c.get()!=0x44534331)return {};const auto title=c.get(),angle=c.get();if(title>4096 || angle>8)return {};s.tracks.disc_title=int(title)-1;s.tracks.disc_angle=int(angle);}
            if(c.cursor<c.bytes.size()){if(c.get()!=0x41555431)return {};const auto enabled=c.get();if(enabled>1)return {};s.auto_format=enabled!=0;}
            if(c.cursor<c.bytes.size()){
              if(c.get()!=0x464c5431)return {};const auto sampling=c.get();if(sampling>2)return {};s.image.sampling=int(sampling);
              for(float* value:{&s.image.denoise,&s.image.detail}){const auto raw=c.get();if(raw>UINT32_MAX)return {};const auto bits=std::uint32_t(raw);std::memcpy(value,&bits,4);}
              s.image.sanitize();
            }
            if(c.cursor<c.bytes.size()){
              if(c.get()!=0x42525331)return {};auto view=c.get(),stable=c.get();if(view>1 || stable>1)return {};s.browser_view=int(view);s.scene.stable=stable!=0;
              for(float* value:{&s.scene.x,&s.scene.y,&s.scene.z,&s.scene.yaw,&s.scene.pitch,&s.scene.roll,&s.volume}){const auto raw=c.get();if(raw>UINT32_MAX)return {};const auto bits=std::uint32_t(raw);std::memcpy(value,&bits,4);}
              s.scene.sanitize();s.volume=std::isfinite(s.volume)?std::clamp(s.volume,0.f,1.f):1.f;
              const auto count=c.get();if(count>16)return {};
              for(unsigned i=0;i<count;++i){FolderBookmark favorite;favorite.location=c.string();favorite.name=c.string();const auto parents=c.get();if(parents>32)return {};for(unsigned n=0;n<parents;++n)favorite.parents.push_back(c.string());s.favorites.push_back(std::move(favorite));}
            }
            if(c.cursor<c.bytes.size()){if(c.get()!=0x485a3132)return {};const auto enabled=c.get();if(enabled>1)return {};s.smooth_video=enabled!=0;}
          }
        }
      }
    }
    if(c.cursor<c.bytes.size()){
      if(c.get()!=0x53554231)return {};
      auto& sub=s.subtitle_settings;
      for(float* value:{&sub.scale,&sub.x,&sub.y,&sub.opacity,&sub.outline,&sub.shadow,&sub.distance}){const auto raw=c.get();if(raw>UINT32_MAX)return {};const auto bits=std::uint32_t(raw);std::memcpy(value,&bits,4);}
      const auto color=c.get(),flags=c.get();if(color>5 || flags>31)return {};sub.color=int(color);
      sub.custom=flags&1;sub.bold=flags&2;sub.italic=flags&4;sub.box=flags&8;sub.follow_depth=flags&16;sub.sanitize();
      const auto opened=c.get(),tab=c.get();if(opened>1 || tab>1)return {};s.subtitle_settings_open=opened!=0;s.subtitle_settings_tab=int(tab);
    }
    if(c.cursor<c.bytes.size()){
      if(c.get()!=0x53434c31)return {};const auto raw=c.get();if(raw>UINT32_MAX)return {};
      const auto bits=std::uint32_t(raw);std::memcpy(&s.browser_scroll,&bits,4);
      s.browser_scroll=std::isfinite(s.browser_scroll)?std::clamp(s.browser_scroll,-1.f,2600000.f):-1.f;
    }
    if(c.cursor<c.bytes.size()){
      if(c.get()!=0x574e4431)return {};const auto flags=c.get();if(flags>7)return {};
      s.window_resizable=flags&1;s.window_face_viewer=flags&2;s.window_settings_open=flags&4;
    }
    if (c.cursor != c.bytes.size() || s.url.size() > 2048)
      return {};
    return s;
  } catch (...) {
    return {};
  }
}
std::optional<ApplicationState>
load_application_state(const std::string &path) noexcept {
  if (path.empty())
    return {};
  if (auto state = read_state(path))
    return state;
  return read_state(path + ".bak");
}
struct StateWriter::Impl {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  std::optional<std::pair<std::string, ApplicationState>> waiting;
  bool writing{}, quit{}, failed{};
  void run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      wake.wait(lock, [&] { return quit || waiting; });
      if (!waiting)
        return;
      auto next = std::move(*waiting);
      waiting.reset();
      writing = true;
      lock.unlock();
      const bool saved = save_application_state(next.first, next.second);
      lock.lock();
      writing = false;
      failed = failed || !saved;
      wake.notify_all();
    }
  }
};
StateWriter::StateWriter() : impl_(std::make_unique<Impl>()) {}
StateWriter::~StateWriter() {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->quit = true;
  }
  impl_->wake.notify_all();
  if (impl_->worker.joinable())
    impl_->worker.join();
}
void StateWriter::post(std::string path, ApplicationState state) {
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->waiting.emplace(std::move(path), std::move(state));
    if (!impl_->worker.joinable())
      impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
  }
  impl_->wake.notify_all();
}
void StateWriter::drain() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->waiting.reset();
  impl_->wake.wait(lock, [&] { return !impl_->writing; });
}
bool StateWriter::take_failure() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return std::exchange(impl_->failed, false);
}
} // namespace cyvr::player
