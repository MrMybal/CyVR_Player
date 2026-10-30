#pragma once
#include "cyvr/player/image_settings.hpp"
#include "cyvr/player/track_selection.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace cyvr::player {
struct ApplicationState {
  std::string directory, url, source, title;
  std::vector<std::string> parents;
  std::int64_t position_us{};
  int page{}, offset{}, stereo{}, projection{}, display_tab{};
  bool half_stereo{}, swap_eyes{}, shift{}, symbols{}, playing{};
  ImageSettings image;
  TrackSelection tracks;
  bool tracks_open{};int track_tab{},track_offset{};
};
std::optional<ApplicationState>
load_application_state(const std::string &path) noexcept;
bool save_application_state(const std::string &path,
                            const ApplicationState &) noexcept;
} // namespace cyvr::player
