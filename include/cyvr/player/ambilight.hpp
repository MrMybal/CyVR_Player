#pragma once
#include "cyvr/gpu.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
namespace cyvr::player {
struct AmbilightSettings {
  bool enabled{}, auto_bars{true};
  float intensity{.7f}, spread{.2f}, smoothing{.25f}, saturation{1},
      inset{.02f};
  void sanitize() {
    auto bound = [](float v, float lo, float hi, float fallback) {
      return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    intensity = bound(intensity, 0, 2, .7f);
    spread = bound(spread, .05f, .6f, .2f);
    smoothing = bound(smoothing, 0, 1.5f, .25f);
    saturation = bound(saturation, 0, 2, 1);
    inset = bound(inset, 0, .2f, .02f);
  }
  bool operator==(const AmbilightSettings &b) const {
    return enabled == b.enabled && auto_bars == b.auto_bars &&
           intensity == b.intensity && spread == b.spread &&
           smoothing == b.smoothing && saturation == b.saturation &&
           inset == b.inset;
  }
  bool operator!=(const AmbilightSettings &b) const { return !(*this == b); }
};
// Small, GPU-only edge-colour field. The movie keeps its own full-size layer.
// Source: 64x36 sRGB, shader-read layout. Output: 128x128 sRGB, colour layout,
// with COLOR_ATTACHMENT | TRANSFER_DST usage and premultiplied alpha.
class AmbilightRenderer {
public:
  AmbilightRenderer(cyvr::VulkanContext &, VkFormat, const cyvr::AssetReader &);
  ~AmbilightRenderer();
  VkImage source_image() const;
  VkImageView source_view() const;
  void reset();
  void process(VkImage, const AmbilightSettings &, float aspect, float elapsed);
  std::optional<double> gpu_ms() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
