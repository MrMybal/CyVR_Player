#pragma once
#include "cyvr/core.hpp"
#include "cyvr/player/demuxer.hpp"
#include "cyvr/player/file_browser.hpp"
#include "cyvr/player/performance.hpp"
#include "cyvr/player/state.hpp"
#include <functional>
#include <memory>
#ifdef __ANDROID__
#include "cyvr/player/android/surface_decoder.hpp"
#endif

namespace cyvr::player {
struct BrowserItem {
  std::string location, name;
  bool directory{};
  std::uint64_t size{};
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
};
struct PlayerState {
  std::uint64_t revision{};
  std::string status{"Choose a video."}, title, codec;
  std::vector<TrackInfo> tracks;
  std::int64_t duration_us{}, position_us{}, seek_target_us{};
  bool seeking{};
  int width{}, height{};
  double pixel_aspect{1};
  double nominal_fps{};
  bool ready{}, playing{}, seekable{}, ended{};
};
// One serialized media worker. UI, filesystem and network never share a lock
// during blocking I/O. Commands cancel the previous open/read before replacing
// it.
class PlayerSession {
public:
  explicit PlayerSession(Platform platform);
  ~PlayerSession();
  void open(std::string location, std::string title, std::int64_t start_us = 0,
            bool autoplay = true);
  void stop();
  void pause(bool paused);
  void seek(std::int64_t position_us);
  PlayerState state() const;
  VideoMetrics metrics() const;
  void frame_submitted(std::uint64_t epoch, std::int64_t pts_us);
#ifdef __ANDROID__
  android::ImageLease take_frame(); // only images due on the presentation clock
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
  bool processing_active{};
  std::string processing_status;
  std::uint64_t processing_request{};
  std::uint64_t video_placement_request{};
  RenderMonitor render_monitor;
  PlayerSession &session() { return session_; }
  int stereo{};     // mono, SBS, top/bottom
  int projection{}; // flat, 180, 360
  bool half_stereo{}, swap_eyes{};
  ImageSettings image;
  void video_input(float seek, float zoom, float pan_x = 0, float pan_y = 0);
  // Visible-page thumbnails, tightly packed in a 7 x 160x90 atlas.
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
  std::array<bool, 7> preview_present_{};
  std::vector<std::uint8_t> thumbnail_atlas_;
  std::uint64_t preview_revision_{};
  bool thumbnail_dirty_{};
  std::vector<BrowserItem> entries_;
  std::vector<std::string> parents_;
  std::string directory_, browse_status_, url_;
  PlayerState state_;
  int page_{}, offset_{};
  int display_tab_{};
  bool zoom_gesture_[2]{}, stick_latched_[2]{};
  float zoom_start_[2]{}, zoom_y_[2]{};
  std::chrono::steady_clock::time_point controller_time_[2]{};
  bool shift_{}, symbols_{};
  std::uint64_t browser_revision_{};
  std::string source_, title_;
  bool suspended_{}, resume_playing_{};
  double last_save_{-1};
  mutable bool state_dirty_{true};
  std::string_view tr(std::string_view key) const;
  std::int64_t requested_position_{};
  bool scrubbing_{};
  std::int64_t scrub_position_us_{}, scrub_duration_us_{};
  bool slide(int id, float fraction, cyvr::SliderPhase phase);
  bool draw_performance(cyvr::DrawList &);
  double last_performance_update_{};
  void cancel_scrub();
  std::int64_t timeline_position() const;
  void button(cyvr::DrawList &, int, cyvr::Rect, std::string_view,
              bool selected = false, bool enabled = true);
};
} // namespace cyvr::player
