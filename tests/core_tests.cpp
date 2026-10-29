#include "cyvr/player/image_settings.hpp"
#include "cyvr/player/media.hpp"
#include "cyvr/player/performance.hpp"
#include "cyvr/player/postprocess.hpp"
#include "cyvr/player/presentation_queue.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace cyvr::player;
namespace {
int checks = 0;
void require(bool ok, const char *message) {
  ++checks;
  if (!ok)
    throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-5; }
template <class F> void rejects(F fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "Invalid media settings must be rejected");
}
float as_float(std::uint32_t bits) {
  float value = 0;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}
void projection_tests() {
  const auto front = equirect_uv({0, 0, -1}, Projection::equirect360);
  require(front && near(front->u, 0.5) && near(front->v, 0.5),
          "Forward must map to panorama center");
  const auto right360 = equirect_uv({1, 0, 0}, Projection::equirect360);
  const auto right180 = equirect_uv({1, 0, 0}, Projection::equirect180);
  require(right360 && near(right360->u, 0.75), "VR360 azimuth");
  require(right180 && near(right180->u, 1), "VR180 azimuth");
  require(!equirect_uv({0, 0, 1}, Projection::equirect180),
          "VR180 must reject the rear hemisphere");
  require(!equirect_uv({0, 0, 0}, Projection::equirect360),
          "Zero ray is invalid");
  require(!equirect_uv({0, 0, -1}, Projection::flat),
          "Flat is not an equirect projection");
  require(!equirect_uv({INFINITY, 0, 0}, Projection::equirect360),
          "Nonfinite ray is invalid");
  const auto top = equirect_uv({0, 2, 0}, Projection::equirect360);
  require(top && near(top->v, 0), "Up must map to top of texture");
  const auto top180 = equirect_uv({0, 1, 0}, Projection::equirect180);
  const auto bottom180 = equirect_uv({0, -1, 0}, Projection::equirect180);
  require(top180 && near(top180->v, 0), "VR180 must retain the upper pole");
  require(bottom180 && near(bottom180->v, 1),
          "VR180 must retain the lower pole");
  const auto huge = equirect_uv({std::numeric_limits<float>::max(), 0, 0},
                                Projection::equirect360);
  require(huge && near(huge->u, 0.75),
          "Finite rays must not overflow normalization");
  const auto left =
      apply_rect({0.5f, 0.5f}, eye_rect(StereoLayout::side_by_side, Eye::left));
  const auto right = apply_rect(
      {0.5f, 0.5f}, eye_rect(StereoLayout::side_by_side, Eye::right));
  require(near(left.u, 0.25) && near(right.u, 0.75),
          "SBS eyes must sample separate halves");
  const auto swapped = apply_rect(
      {0.5f, 0.5f}, eye_rect(StereoLayout::top_bottom, Eye::left, true));
  require(near(swapped.v, 0.75), "Eye swapping must apply before TB crop");
  require(near(eye_aspect(3840, 1080, StereoLayout::side_by_side), 16.0 / 9),
          "Full SBS aspect");
  require(near(eye_aspect(1920, 2160, StereoLayout::top_bottom), 16.0 / 9),
          "Full TB aspect");
  require(
      near(eye_aspect(1920, 1080, StereoLayout::side_by_side, 2.0), 16.0 / 9),
      "Half SBS pixel aspect override");
  rejects([] { eye_aspect(0, 1080, StereoLayout::mono); });
  rejects([] { eye_aspect(1920, 1080, StereoLayout::mono, -1); });
}
struct CountDelete {
  int *retired;
  void operator()(int *value) const {
    ++*retired;
    delete value;
  }
};
struct Frame {
  std::int64_t pts_us;
  std::uint64_t epoch;
  std::unique_ptr<int, CountDelete> resource;
  Frame(std::int64_t pts, std::uint64_t generation, int id, int &retired)
      : pts_us(pts), epoch(generation),
        resource(new int(id), CountDelete{&retired}) {}
  Frame(Frame &&) = default;
  Frame &operator=(Frame &&) = default;
};
void queue_tests() {
  int retired = 0;
  {
    PresentationQueue<Frame, 3> q(1);
    Frame a(0, 1, 10, retired), b(40000, 1, 11, retired),
        c(80000, 1, 12, retired), d(120000, 1, 13, retired);
    require(q.try_push(std::move(a)) && q.try_push(std::move(b)) &&
                q.try_push(std::move(c)),
            "Queue accepts capacity");
    require(!q.try_push(std::move(d)) && bool(d.resource),
            "Backpressure must not consume the pending frame");
    require(!q.take_due(-1) && q.size() == 3, "Must never take a future PTS");
    auto selected = q.take_due(45000);
    require(selected && *selected->resource == 11 && q.size() == 1,
            "Select newest due PTS, keep future frame");
    require(retired == 1, "Skipped old frame resources must be retired");
    require(q.try_push(std::move(d)), "Ring must accept after wrap");
    Frame backwards(70000, 1, 14, retired);
    require(!q.try_push(std::move(backwards)),
            "Out-of-order presentation timestamps must be rejected");
    q.reset(2);
    require(q.size() == 0 && retired == 3,
            "Seek reset must retire queued resources");
    Frame stale(0, 1, 15, retired), fresh(0, 2, 16, retired);
    require(!q.try_push(std::move(stale)) && bool(stale.resource),
            "Old decoder generation must be rejected");
    require(q.try_push(std::move(fresh)),
            "New generation may restart at PTS zero");
    selected.reset();
  }
  require(retired == 7, "All accepted, skipped and rejected frame owners must "
                        "release exactly once");
}
void effect_tests() {
  require(sizeof(CasConstants) == 32 && sizeof(EasuConstants) == 64 &&
              sizeof(RcasConstants) == 16,
          "Push constant layouts must match SPIR-V uvec4 blocks");
  const auto cas0 = cas_constants({1920, 1080}, 0);
  const auto cas1 = cas_constants({1920, 1080}, 1);
  require(near(as_float(cas0.con0[0]), 1) && near(as_float(cas0.con0[1]), 1),
          "CAS is native size");
  require(near(as_float(cas0.con1[0]), -1.0 / 8) &&
              near(as_float(cas1.con1[0]), -1.0 / 5),
          "CAS sharpness endpoints");
  const auto easu = easu_constants({960, 540}, {1920, 1080});
  require(near(as_float(easu.con0[0]), 0.5) &&
              near(as_float(easu.con0[2]), -0.25),
          "EASU pixel-center mapping");
  require(near(as_float(rcas_constants(0).con[0]), 1) &&
              near(as_float(rcas_constants(2).con[0]), 0.25),
          "RCAS stops direction");
  rejects([] { cas_constants({0, 1}, 0.5f); });
  rejects(
      [] { cas_constants({1, 1}, std::numeric_limits<float>::quiet_NaN()); });
  rejects([] { cas_constants({1, 1}, 1.01f); });
  rejects([] { easu_constants({1920, 1080}, {960, 540}); });
  rejects([] { easu_constants({0, 1080}, {1920, 1080}); });
  rejects([] { rcas_constants(-1); });
  rejects([] { rcas_constants(INFINITY); });
}
void performance_tests() {
  ImageSettings settings;
  settings.quality = 2;
  auto plan = view_processing_plan({1680, 1760}, {512, 512}, settings);
  require(plan.output.width == 1680 && plan.output.height == 1760,
          "Enhancement must default to runtime recommended view dimensions");
  require(plan.input.width <= 512 && plan.input.height <= 512,
          "Low-resolution visible source detail must be reconstructed to the "
          "display");
  plan = view_processing_plan({1920, 1080}, {960, 1080}, settings);
  require(plan.input.width == 960 && plan.input.height == 1080,
          "Horizontal upscaling must preserve available vertical detail in "
          "half-SBS");
  plan = view_processing_plan({1080, 1920}, {1080, 960}, settings);
  require(plan.input.width == 1080 && plan.input.height == 960,
          "Vertical upscaling must preserve available horizontal detail in "
          "half-TB");
  settings.upscale = 3;
  plan = view_processing_plan({2560, 2560}, {7680, 3840}, settings);
  require(plan.output.width == 5120 && plan.output.height == 5120,
          "Large display targets must not silently lose their selected scale");
  require(std::uint64_t(plan.output.width) * plan.output.height > 16777216,
          "The former panorama pixel cap must not reject a display target");
  require(plan.input.width <= plan.output.width &&
              plan.input.height <= plan.output.height,
          "Reconstruction input cannot exceed the display output");
  settings.quality = 1;
  settings.upscale = 0;
  plan = view_processing_plan({1680, 1760}, {512, 512}, settings);
  require(plan.input.width == 1680 && plan.input.height == 1760,
          "CAS must sharpen at the displayed resolution");
  EventRate video, render;
  UniqueVideoFrames distinct;
  require(!video.sample(0), "No FPS may be invented before events arrive");
  for (int n = 0; n <= 144; ++n) {
    const double now = n / 72.;
    render.add(now);
    const auto pts = std::int64_t(n / 3) * 41667;
    if (distinct.accept(1, pts))
      video.add(now);
  }
  require(video.sample(2) && near(*video.sample(2), 24),
          "24 fps video must not be counted as its 72 Hz VR redraws");
  require(render.sample(2) && near(*render.sample(2), 72),
          "Submission cadence must be measured independently");
  require(video.sample(2.6) && *video.sample(2.6) == 0,
          "Stalled video must not retain a good-looking FPS");
  require(!video.sample(1.9), "Backwards query time cannot yield a rate");
  video.reset();
  require(!video.sample(3), "Pause/seek reset must invalidate old FPS");
  require(!distinct.accept(1, 2000016),
          "Repeated retained frame must not count as a new video frame");
  require(distinct.accept(2, 2000016),
          "A new decoder generation may reuse PTS");
  video.add(5);
  video.add(4);
  require(!video.sample(4), "Backwards event time must restart sampling");
  video.add(INFINITY);
  require(!video.sample(4), "Nonfinite events must not enter cadence history");
  for (int n = 0; n <= 1200; ++n)
    video.add(10 + n / 120.);
  require(video.sample(20) && std::abs(*video.sample(20) - 120) < 1e-4,
          "Cadence window must survive ring wrap");
  TimeSamples time;
  require(!time.mean() && !time.p95(),
          "Absent GPU timings must remain unmeasured");
  time.add(-1);
  time.add(INFINITY);
  time.add(NAN);
  require(!time.mean(), "Invalid GPU timings cannot be reported");
  for (int n = 1; n <= 120; ++n)
    time.add(n);
  require(near(*time.mean(), 60.5) && near(*time.p95(), 114),
          "GPU mean/p95 must retain long stalls");
  time.add(1000);
  require(*time.mean() > 60.5, "A one-second stall must not be discarded");
  time.reset();
  require(!time.mean(), "Changing filter must clear timing samples");
}
} // namespace
int main() {
  try {
    projection_tests();
    queue_tests();
    effect_tests();
    performance_tests();
    std::cout << checks << " checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Failed after " << checks << " checks: " << error.what()
              << '\n';
    return 1;
  }
}
