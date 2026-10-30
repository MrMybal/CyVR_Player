#include "cyvr/player/state.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
} // namespace
bool save_application_state(const std::string &path,
                            const ApplicationState &s) noexcept {
  if (path.empty())
    return false;
  try {
    Codec c;
    c.put(s.directory);
    c.put(s.url);
    c.put(s.source);
    c.put(s.title);
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
    c.put(std::clamp(s.display_tab, 0, 3));
    c.put(std::uint64_t(0x54524b31));
    for(int stream:{s.tracks.video,s.tracks.audio,s.tracks.subtitle})c.put(std::uint64_t(std::clamp(stream,-2,255)+2));
    c.put(s.tracks.external_subtitle);
    c.put(std::uint64_t(0x55494d31));c.put(s.tracks_open?1:0);c.put(std::clamp(s.track_tab,0,3));c.put(std::clamp(s.track_offset,0,10000));
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
    if (c.bytes.size() > max_bytes)
      throw std::runtime_error("State budget exceeded");
    std::lock_guard<std::mutex> lock(writes);
    const auto destination = std::filesystem::u8path(path),
               temporary = std::filesystem::u8path(path + ".tmp");
    std::filesystem::create_directories(destination.parent_path());
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.write(c.bytes.data(), c.bytes.size());
      file.flush();
      if (!file)
        return false;
    }
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(temporary.c_str(), destination.c_str()) == 0;
#endif
  } catch (...) {
    return false;
  }
}
std::optional<ApplicationState>
load_application_state(const std::string &path) noexcept {
  if (path.empty())
    return {};
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
    s.page = int(std::min<std::uint64_t>(3, c.get()));
    s.offset = int(std::min<std::uint64_t>(511, c.get()));
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
        s.display_tab = int(std::min<std::uint64_t>(3, c.get()));
      if(c.cursor<c.bytes.size()){
        if(c.get()!=0x54524b31)return {};
        for(int* stream:{&s.tracks.video,&s.tracks.audio,&s.tracks.subtitle}){const auto value=c.get();if(value>257)return {};*stream=int(value)-2;}
        s.tracks.external_subtitle=c.string();
        if(c.cursor<c.bytes.size()){if(c.get()!=0x55494d31)return {};s.tracks_open=c.get()!=0;s.track_tab=int(std::min<std::uint64_t>(3,c.get()));s.track_offset=int(std::min<std::uint64_t>(10000,c.get()));}
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
          }
        }
      }
    }
    if (c.cursor != c.bytes.size() || s.url.size() > 2048)
      return {};
    return s;
  } catch (...) {
    return {};
  }
}
} // namespace cyvr::player
