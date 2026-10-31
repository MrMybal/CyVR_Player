#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace cyvr::player {
// SDR controls. Neutral defaults retain the native-resolution, single-pass
// path.
struct ImageSettings {
  float exposure{}, contrast{1}, saturation{1}, gamma{1};
  float zoom{1}, pan_x{}, pan_y{}, sharpness{.5f};
  int quality{}; // 0 native, 1 AMD CAS, 2 AMD FSR 1, 3 Snapdragon GSR 1
  int upscale{}; // Display target: 0 runtime/native, 1 1.25x, 2 1.5x, 3 2x.
  int sampling{}; // 0 original point sampling, 1 bilinear, 2 bicubic.
  float denoise{}, detail{}; // Optional spatial noise reduction/detail, 0..1.
  bool reconstruct_view() const {
    return quality != 0 || sampling != 0 || denoise > 0 || detail > 0;
  }
  void sanitize() {
    auto bound = [](float v, float lo, float hi, float fallback) {
      return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    exposure = bound(exposure, -2, 2, 0);
    contrast = bound(contrast, .5f, 1.5f, 1);
    saturation = bound(saturation, 0, 2, 1);
    gamma = bound(gamma, .5f, 2, 1);
    zoom = bound(zoom, .25f, 4, 1);
    pan_x = bound(pan_x, -1, 1, 0);
    pan_y = bound(pan_y, -1, 1, 0);
    sharpness = bound(sharpness, 0, 1, .5f);
    quality = std::clamp(quality, 0, 3);
    upscale = std::clamp(upscale, 0, 3);
    sampling = std::clamp(sampling, 0, 2);
    denoise = bound(denoise, 0, 1, 0);
    detail = bound(detail, 0, 1, 0);
  }
  bool operator==(const ImageSettings &b) const {
    return exposure == b.exposure && contrast == b.contrast &&
           saturation == b.saturation && gamma == b.gamma && zoom == b.zoom &&
           pan_x == b.pan_x && pan_y == b.pan_y && sharpness == b.sharpness &&
           quality == b.quality && upscale == b.upscale &&
           sampling == b.sampling && denoise == b.denoise && detail == b.detail;
  }
  bool operator!=(const ImageSettings &b) const { return !(*this == b); }
};
struct VideoExtent {
  std::uint32_t width{}, height{};
};
inline float display_scale(const ImageSettings &image) {
  return image.upscale == 1   ? 1.25f
         : image.upscale == 2 ? 1.5f
         : image.upscale == 3 ? 2.f
                              : 1.f;
}
// Reconstruct only the view that will be displayed, never source-size x factor.
struct ViewProcessingPlan {
  VideoExtent input, output;
};
inline ViewProcessingPlan view_processing_plan(VideoExtent display,
                                               VideoExtent available,
                                               const ImageSettings &image) {
  const auto scale = display_scale(image);
  VideoExtent output{
      std::max(1u, std::uint32_t(std::ceil(display.width * scale))),
      std::max(1u, std::uint32_t(std::ceil(display.height * scale)))};
  auto input = output;
  if (image.quality >= 2) {
    // A projected texture may have non-square pixels. Limiting both axes by
    // the smaller ratio needlessly discards detail on the other axis (e.g.
    // half-SBS). The FOV/UV mapping, not the intermediate texture aspect,
    // determines the geometry. FSR/GSR receive the independent input sizes.
    input = {std::min(output.width, std::max(1u, available.width)),
             std::min(output.height, std::max(1u, available.height))};
    // EASU costs a full pass, which a slight enlargement does not repay.
    if (input.width * 20 >= output.width * 17 &&
        input.height * 20 >= output.height * 17)
      input = output;
  }
  return {input, output};
}
// A world-locked cinema screen is filtered once per decoded frame and handed to
// the compositor, so its size follows the screen's density rather than the head.
struct ScreenView {
  VideoExtent source; // Packed decoded frame, both eyes for stereo.
  float aspect{};     // Displayed aspect ratio of one eye.
  int stereo{};       // 0 mono, 1 side by side, 2 top and bottom.
  float pixels_per_radian{}, angular_width{}, frame_rate{};
};
inline ViewProcessingPlan screen_processing_plan(const ScreenView &view,
                                                 const ImageSettings &image) {
  const std::uint32_t across = view.stereo == 1 ? 2 : 1,
                      down = view.stereo == 2 ? 2 : 1;
  const float display_w =
      std::max(16.f, view.pixels_per_radian * view.angular_width);
  const float display_h = display_w / std::max(.1f, view.aspect);
  // A sharper source keeps its detail, unless high frame rates leave no budget.
  const float headroom = view.frame_rate > 40 ? 1.25f : 2.f;
  const auto density = [&](float display, float source) {
    return std::max(display, std::min(source, display * headroom));
  };
  // Multiples of eight stay even for stereo and absorb small aspect changes.
  const auto stable = [](float size) {
    return (std::uint32_t(std::ceil(size)) + 7u) & ~7u;
  };
  const VideoExtent display{
      stable(density(display_w, float(view.source.width) / across)) * across,
      stable(density(display_h, float(view.source.height) / down)) * down};
  auto plan = view_processing_plan(display, view.source, image);
  plan.input.width &= ~1u;
  plan.input.height &= ~1u;
  return plan;
}
// Output pixels that get the full filter, centred on the lens axis. Radius 0
// filters the whole image.
struct FilterRegion {
  std::uint32_t x{}, y{}, radius_x{}, radius_y{};
};
// A view rendered per eye has no budget for filtering what the lens periphery
// cannot resolve. fov holds tangents left, right, up, down.
inline FilterRegion view_filter_region(const float (&fov)[4],
                                       VideoExtent output) {
  constexpr float reach = .7f; // tan(35 degrees)
  const float width = fov[1] - fov[0], height = fov[2] - fov[3];
  return {std::uint32_t(output.width * -fov[0] / width),
          std::uint32_t(output.height * fov[2] / height),
          std::uint32_t(output.width * reach / width),
          std::uint32_t(output.height * reach / height)};
}
// A window shows a fraction of an 8K frame, and converting all of it misses the
// frame rate. Twice its size keeps the picture sharp through the final scaling.
inline VideoExtent window_extent(VideoExtent source, float width, float height) {
  const float scale = std::min({1.f, 2 * width / std::max(1u, source.width),
                                2 * height / std::max(1u, source.height)});
  return {std::max(2u, std::uint32_t(source.width * scale) & ~1u),
          std::max(2u, std::uint32_t(source.height * scale) & ~1u)};
}
// 72 Hz repeats every sixth frame of a 60 fps video; 120 Hz shows each one twice.
// Per-eye filters have no budget left at that rate.
inline bool wants_double_rate(bool enabled, double frame_rate,
                              const ImageSettings &image) {
  return enabled && std::abs(frame_rate - 60) < 1.5 && !image.reconstruct_view();
}
// Standalone shader diagnostics: explicit reconstruction of a small fixture.
inline VideoExtent processed_extent(std::uint32_t w, std::uint32_t h,
                                    const ImageSettings &image, int stereo) {
  if (image.quality < 2)
    return {w, h};
  const float scale = display_scale(image);
  auto x = std::uint32_t(std::ceil(w * scale)),
       y = std::uint32_t(std::ceil(h * scale));
  if (stereo == 1)
    x = (x + 1) & ~1u;
  if (stereo == 2)
    y = (y + 1) & ~1u;
  return {x, y};
}
} // namespace cyvr::player
