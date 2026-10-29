#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
namespace cyvr::player {
// Event timestamps, not a display refresh target. Empty/stale samples never
// inherit the last apparently-good FPS value. Time is injected for tests.
class EventRate {
  std::array<double, 512> times_{};
  std::size_t next_{}, count_{};

public:
  void reset() { next_ = count_ = 0; }
  void add(double now) {
    if (!std::isfinite(now))
      return;
    if (count_ && now < times_[(next_ + times_.size() - 1) % times_.size()])
      reset();
    times_[next_] = now;
    next_ = (next_ + 1) % times_.size();
    count_ = std::min(count_ + 1, times_.size());
  }
  std::optional<double> sample(double now) const {
    if (!count_ || !std::isfinite(now))
      return {};
    const auto last = times_[(next_ + times_.size() - 1) % times_.size()];
    if (now < last)
      return {};
    if (now - last > .5)
      return 0.;
    double first = last;
    std::size_t samples{};
    for (std::size_t n = 0; n < count_; ++n) {
      const auto time = times_[(next_ + times_.size() - 1 - n) % times_.size()];
      if (now - time > 2.)
        break;
      first = time;
      ++samples;
    }
    const auto duration = now - first;
    if (samples < 2 || duration < .25)
      return {};
    return double(samples - 1) / duration;
  }
};
class TimeSamples {
  std::array<double, 120> values_{};
  std::size_t count_{}, next_{};

public:
  void reset() { count_ = next_ = 0; }
  void add(double ms) {
    if (!std::isfinite(ms) || ms < 0)
      return;
    values_[next_] = ms;
    next_ = (next_ + 1) % values_.size();
    count_ = std::min(count_ + 1, values_.size());
  }
  std::optional<double> mean() const {
    if (!count_)
      return {};
    double sum = 0;
    for (std::size_t i = 0; i < count_; ++i)
      sum += values_[i];
    return sum / count_;
  }
  std::optional<double> p95() const {
    if (!count_)
      return {};
    auto copy = values_;
    std::sort(copy.begin(), copy.begin() + count_);
    return copy[std::size_t(std::ceil(count_ * .95)) - 1];
  }
};
struct VideoMetrics {
  std::uint64_t decoded{}, scheduled{}, submitted{}, dropped{}, queued{};
  std::optional<double> decode_rate, submit_rate;
};
class UniqueVideoFrames {
  bool valid_{};
  std::uint64_t epoch_{};
  std::int64_t pts_{};

public:
  void reset() { valid_ = false; }
  bool accept(std::uint64_t epoch, std::int64_t pts) {
    if (valid_ && epoch == epoch_ && pts == pts_)
      return false;
    valid_ = true;
    epoch_ = epoch;
    pts_ = pts;
    return true;
  }
};
struct RenderMonitor {
  EventRate submissions;
  TimeSamples work, gpu;
  std::uint64_t frames{}, errors{};
  void reset() {
    submissions.reset();
    work.reset();
    gpu.reset();
    frames = errors = 0;
  }
};
} // namespace cyvr::player
