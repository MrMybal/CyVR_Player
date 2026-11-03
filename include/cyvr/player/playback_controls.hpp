#pragma once
namespace cyvr::player {
class PlaybackControls {
public:
  enum class Mode { full, compact, hidden };
  Mode mode() const { return mode_; }
  void activity(double now) {
    last_activity_ = now;
    if (active_ && mode_ == Mode::hidden) mode_ = Mode::compact;
  }
  void expand(double now) { full_requested_ = true; mode_ = Mode::full; activity(now); }
  void compact(double now) { full_requested_ = false; mode_ = Mode::compact; activity(now); }
  void hide() { if (active_) { full_requested_ = false; mode_ = Mode::hidden; } }
  void update(bool immersive_video, double now, bool interacting) {
    if (!immersive_video) { active_ = false; mode_ = Mode::full; return; }
    if (!active_) { active_ = true; mode_ = full_requested_ ? Mode::full : Mode::compact; last_activity_ = now; }
    if (interacting) last_activity_ = now;
    if (mode_ == Mode::compact && now - last_activity_ >= 3.) mode_ = Mode::hidden;
  }
private:
  bool active_{};
  bool full_requested_{};
  Mode mode_{Mode::full};
  double last_activity_{};
};
}
