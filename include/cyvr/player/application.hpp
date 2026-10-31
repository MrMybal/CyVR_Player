#pragma once
#include "cyvr/core.hpp"
#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/file_browser.hpp"
#include "cyvr/player/performance.hpp"
#include "cyvr/player/playback_controls.hpp"
#include "cyvr/player/state.hpp"
#include "cyvr/player/subtitles.hpp"
#include "cyvr/player/track_selection.hpp"
#include "cyvr/player/splats.hpp"
#include "cyvr/player/icons.hpp"
#include "cyvr/player/branding.hpp"
#include <functional>
#include <memory>
#include <tuple>
#ifdef __ANDROID__
#include "cyvr/player/android/surface_decoder.hpp"
#include "cyvr/player/android/audio_playback.hpp"
#endif

namespace cyvr::player {
struct BrowserItem {
  std::string location, name;
  bool directory{};
  std::uint64_t size{};
};
struct WebRequest {
  std::string headers, user_agent, cookies;
};
struct Platform {
  std::string root, ca_file, state_file;
  bool panorama{true};
  std::function<std::vector<BrowserItem>(const std::string &)> documents;
  // The returned descriptor belongs to the caller, which closes it after open.
  std::function<MediaSource(const std::string &)> open_document;
  std::function<std::string(const std::string &, int, int)> hardware_codec;
  std::function<void()> request_tree;
  std::function<void()> cancel_documents;
  std::function<std::vector<BrowserItem>()> locations;
  std::function<std::vector<std::uint8_t>(const std::string &)> thumbnail;
  // Opens the web browser window, in the interface language.
  std::function<void(const std::string &)> open_browser;
  // Request settings for a video opened from a web page. These live only in
  // memory: they can hold session cookies, so they never reach the state file.
  std::function<WebRequest(const std::string &)> web_request;
};
struct PlayerState {
  std::uint64_t revision{},media_id{};
  std::string status{"Choose a video."}, title, codec;
  std::vector<TrackInfo> tracks;
  std::vector<ChapterInfo> chapters;
  DiscInfo disc;
  int video_track{-1},audio_track{-1},subtitle_track{-1};
  std::int64_t duration_us{}, position_us{}, seek_target_us{};
  bool seeking{};
  int width{}, height{};
  double pixel_aspect{1};
  double nominal_fps{};
  bool ready{}, playing{}, seekable{}, ended{};
  bool volumetric{};
  bool multiview{};
  VideoLayoutHints layout;
  std::string audio_status,subtitle_status;
};
// One serialized media worker. UI, filesystem and network never share a lock
// during blocking I/O. Commands cancel the previous open/read before replacing
// it.
// The fields read every display frame, without copying track and chapter lists.
struct PlayerVideoState {
  bool ready{}, volumetric{};
  int width{}, height{};
  double pixel_aspect{1};
  double nominal_fps{};
};
// Longest whole-character prefix that fits, without measuring every length.
inline std::string fit_text(cyvr::DrawList &d, std::string_view value, float width,
                            float size) {
  if (d.textWidth(size, value) <= width)
    return std::string(value);
  std::vector<std::size_t> ends;
  for (std::size_t n = 1; n < value.size(); ++n)
    if ((static_cast<unsigned char>(value[n]) & 0xc0) != 0x80)
      ends.push_back(n);
  std::size_t low = 0, high = ends.size(), fitted = 0;
  while (low < high) {
    const auto middle = (low + high) / 2;
    if (d.textWidth(size, value.substr(0, ends[middle])) <= width) {
      fitted = ends[middle];
      low = middle + 1;
    } else
      high = middle;
  }
  return std::string(value.substr(0, fitted));
}
class PlayerSession {
public:
  explicit PlayerSession(Platform platform);
  ~PlayerSession();
  std::uint64_t open(std::string location, std::string title, std::int64_t start_us = 0,
            bool autoplay = true, TrackSelection selection = {});
  bool select_track(TrackKind,int index);
  bool select_disc_title(int index, int angle = 0);
  bool external_subtitle(std::string location);
  bool clearkey_file(std::string location);
  TrackSelection selection() const;
  SubtitleSnapshot subtitles() const;
  SplatSnapshot splats() const;
  void stop();
  void pause(bool paused);
  void volume(float);
  void seek(std::int64_t position_us);
  PlayerState state() const;
  std::uint64_t revision() const;
  PlayerVideoState video_state() const;
  VideoMetrics metrics() const;
  void frame_submitted(std::uint64_t epoch, std::int64_t pts_us);
#ifdef __ANDROID__
  android::ImageLease take_frame(); // only images due on the presentation clock
  std::optional<std::int64_t> audio_position_us() const;
  std::optional<android::AudioPlaybackStats> audio_metrics() const;
#endif
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class PlayerApplication {
public:
  PlayerApplication(cyvr::UiModel &ui, Platform platform);
  ~PlayerApplication();
  void tick();
  void open(std::string location, std::string title = {});
  void browse(std::string location);
  void text(char character);
  bool draw(cyvr::DrawList &draw);
  bool action(int id);
  bool back();
  bool show_interface();
  void active(bool resumed);
  bool restore(bool resume_media = true);
  void save() const;
  ApplicationState snapshot() const;
  int page() const { return page_; }
  bool preview_ready{};
  bool preview_is_view{};
  bool caption_ready{};
  cyvr::Rect caption_rect{};
  bool processing_active{};
  std::string processing_status;
  std::uint64_t processing_request{};
  std::uint64_t video_placement_request{};
  CinemaSettings cinema;
  SceneSettings scene;
  float volume{1};
  bool smooth_video{};
  AmbilightSettings ambilight;
  RenderMonitor render_monitor;
  PlayerSession &session() { return session_; }
  int stereo{};     // mono, SBS, top/bottom
  int projection{}; // flat, 180, 360
  bool half_stereo{}, swap_eyes{};
  ImageSettings image;
  void video_input(float seek, float zoom, float pan_x = 0, float pan_y = 0);
  void set_logo(const std::vector<std::uint8_t>& rgba);
  // Shared UI atlas: visible-page thumbnails and a static branding tile.
  bool take_thumbnails(std::vector<std::uint8_t> &rgba, bool force = false);
  const std::string &location() const { return directory_; }

private:
  cyvr::UiModel &ui_;
  Platform platform_;
  PlayerSession session_;
  struct BrowserWorker;
  std::unique_ptr<BrowserWorker> browser_;
  struct PreviewWorker;
  std::unique_ptr<PreviewWorker> previews_;
  std::vector<std::string> preview_requests_;
  std::tuple<bool, int, int, std::uint64_t> preview_key_{};
  std::size_t playlist_position_{};
  std::array<bool, 7> preview_present_{};
  std::vector<std::uint8_t> thumbnail_atlas_;
  std::uint64_t preview_revision_{};
  bool thumbnail_dirty_{};
  bool logo_ready_{};
  std::vector<BrowserItem> entries_;
  std::vector<std::string> parents_;
  std::string directory_, browse_status_, url_;
  PlayerState state_;
  int page_{}, offset_{};
  int display_tab_{};
  int cinema_tab_{};
  bool format_manual_{};
  bool auto_format_{};
  int browser_view_{},sidebar_tab_{},sidebar_offset_{};
  std::vector<FolderBookmark> favorites_;
  std::vector<BrowserItem> locations_;
  float unmuted_volume_{1};
  std::string icon_hint_;
  std::string folder_name_;
  int browser_page_size()const{return browser_view_?6:7;}
  void refresh_locations();
  void toggle_favorite();
  void draw_library(cyvr::DrawList&);
  void draw_settings(cyvr::DrawList&);
  void draw_placement(cyvr::DrawList&);
  bool draw_dock(cyvr::DrawList&);
  std::uint64_t format_media_id_{};
  int format_track_{-1};
  DetectedVideoLayout detected_layout_;
  void refresh_video_format(bool force = false);
  float minimum_zoom() const { return projection==1 ? .25f : 1.f; }
public:
  const std::string &state_file() const { return platform_.state_file; }
private:
  bool zoom_gesture_[2]{}, stick_latched_[2]{};
  PlaybackControls controls_;
  std::vector<BrowserItem> playlist_;
  cyvr::Pose last_controller_pose_[2]{};
  bool controller_seen_[2]{}, wake_gesture_[2]{};
  float last_trigger_[2]{}, last_grip_[2]{};
  void control_activity();
  bool adjacent_video(int direction, bool play);
  float zoom_start_[2]{}, zoom_y_[2]{};
  std::chrono::steady_clock::time_point controller_time_[2]{};
  bool shift_{}, symbols_{};
  std::uint64_t browser_revision_{};
  std::string source_, title_;
  bool suspended_{}, resume_playing_{};
  double last_save_{-1};
  mutable bool state_dirty_{true};
  mutable StateWriter state_writer_;
  std::string_view tr(std::string_view key) const;
  std::int64_t requested_position_{};
  bool scrubbing_{};
  std::int64_t scrub_position_us_{}, scrub_duration_us_{};
  bool slide(int id, float fraction, cyvr::SliderPhase phase);
  bool draw_performance(cyvr::DrawList &);
  double last_performance_update_{};
  void cancel_scrub();
  bool tracks_open_{};
  int track_tab_{},track_offset_{};
  void draw_tracks(cyvr::DrawList&);
  bool track_action(int);
  std::int64_t timeline_position() const;
  void button(cyvr::DrawList &, int, cyvr::Rect, std::string_view,
              bool selected = false, bool enabled = true);
  void icon_button(cyvr::DrawList&,int,cyvr::Rect,Icon,std::string_view,bool selected=false,bool enabled=true,bool label=false);
};
} // namespace cyvr::player
