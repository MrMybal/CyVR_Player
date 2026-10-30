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
  }
  return {input, output};
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
