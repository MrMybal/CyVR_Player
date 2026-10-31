#pragma once
#include "cyvr/player/image_settings.hpp"
#include "cyvr/player/cinema_settings.hpp"
#include "cyvr/player/ambilight.hpp"
#include "cyvr/player/track_selection.hpp"
#include "cyvr/player/scene_settings.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace cyvr::player {
struct FolderBookmark { std::string location,name;std::vector<std::string> parents; };
struct ApplicationState {
  std::string directory, url, source, title;
  std::vector<std::string> parents;
  std::int64_t position_us{};
  int page{}, offset{}, stereo{}, projection{}, display_tab{};
  bool half_stereo{}, swap_eyes{}, shift{}, symbols{}, playing{};
  ImageSettings image;
  CinemaSettings cinema;
  AmbilightSettings ambilight;
  int cinema_tab{};
  bool format_manual{true}; // Legacy checkpoints preserve their selected format.
  bool auto_format{}; // Opt in; retain manual formats when upgrading old state.
  bool smooth_video{}; // Opt in; see wants_double_rate.
  TrackSelection tracks;
  bool tracks_open{};int track_tab{},track_offset{};
  SceneSettings scene;
  int browser_view{};
  std::vector<FolderBookmark> favorites;
  float volume{1};
};
std::optional<ApplicationState>
load_application_state(const std::string &path) noexcept;
bool save_application_state(const std::string &path,
                            const ApplicationState &) noexcept;
// Durable writes wait for storage, so periodic checkpoints leave the render
// thread. Only the newest waiting state is written.
class StateWriter {
public:
  StateWriter();
  ~StateWriter();
  void post(std::string path, ApplicationState state);
  // Drops the waiting state and finishes any write already in progress.
  void drain();
  // Reports a failed background write once, so the caller can retry.
  bool take_failure();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
