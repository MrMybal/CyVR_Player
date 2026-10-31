#pragma once
#include "cyvr/core.hpp"
#include "cyvr/player/image_settings.hpp"
#include "cyvr/player/video_color.hpp"
#include <algorithm>
#include <cmath>
namespace cyvr::player {
struct VideoView {
  // 0 packed source, 1 single eye, 2 flat cinema, 3 VR180, 4 VR360, 5 curved cinema, 6 averaged stereo capture.
  int kind{}, eye{};
  float fov[4]{-1, 1, 1, -1}; // tangent left/right/up/down
  cyvr::Quat orientation{};   // eye ray -> video-local coordinates
  cyvr::Vec3 origin{};        // eye position relative to the cinema screen
  float screen_width{4}, screen_height{2.25f};
  float screen_radius{};
};
// Exact shared layout consumed by video.frag on Android and in the GPU probe.
struct alignas(16) VideoPush {
  float crop[4]{0, 0, 1, 1}, controls[4]{0, 1, 1, 1}, view[4]{1, 0, 0, 0};
  int format[4]{};
  float fov[4]{-1, 1, 1, -1}, rotation[4]{0, 0, 0, 1}, origin[4]{},
      screen[4]{4, 2.25f, 0, 0};
};
static_assert(sizeof(VideoPush) == 128);
inline VideoPush video_push(cyvr::Rect crop, const ImageSettings &settings,
                            int stereo, const VideoView &mapping, VideoColor color = {}) {
  VideoPush p;
  p.crop[0] = crop.x;
  p.crop[1] = crop.y;
  p.crop[2] = crop.w;
  p.crop[3] = crop.h;
  p.controls[0] = settings.exposure;
  p.controls[1] = settings.contrast;
  p.controls[2] = settings.saturation;
  p.controls[3] = settings.gamma;
  p.view[0] = settings.zoom;
  p.view[1] = settings.pan_x;
  p.view[2] = settings.pan_y;
  // Spatial source controls fit the Vulkan-guaranteed 128-byte push budget.
  // CAS/FSR/GSR strength is passed separately to ImageProcessor.
  p.view[3] = settings.detail;
  p.format[0] = stereo;
  p.format[1] = mapping.kind;
  p.format[2] = mapping.eye;
  p.format[3] = color.transfer | (color.standard << 8) | (settings.sampling << 16);
  std::copy(mapping.fov, mapping.fov + 4, p.fov);
  const auto q = mapping.orientation;
  p.rotation[0] = q.x;
  p.rotation[1] = q.y;
  p.rotation[2] = q.z;
  p.rotation[3] = q.w;
  p.origin[0] = mapping.origin.x;
  p.origin[1] = mapping.origin.y;
  p.origin[2] = mapping.origin.z;
  p.origin[3] = settings.denoise;
  p.screen[0] = mapping.screen_width;
  p.screen[1] = mapping.screen_height;
  p.screen[2] = color.peak_nits;
  p.screen[3] = mapping.screen_radius;
  return p;
}
inline cyvr::Quat inverse_rotation(cyvr::Quat q) {
  return {-q.x, -q.y, -q.z, q.w};
}
inline cyvr::Quat multiply_rotation(cyvr::Quat a, cyvr::Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
// Fractions of one eye's image. A panorama shows a third of its pixels at most,
// so converting only those leaves the budget of a 60 fps 8K frame.
struct SourceRegion {
  float left{}, top{}, right{1}, bottom{1};
  bool contains(const SourceRegion &o) const {
    return left <= o.left && top <= o.top && right >= o.right &&
           bottom >= o.bottom;
  }
  SourceRegion joined(const SourceRegion &o) const {
    return {std::min(left, o.left), std::min(top, o.top),
            std::max(right, o.right), std::max(bottom, o.bottom)};
  }
};
// What a view reaches when widened by margin radians. projection: 1 VR180, 2 VR360.
inline SourceRegion visible_panorama(const float (&fov)[4], cyvr::Quat view,
                                     int projection, float margin) {
  const auto turn = [](cyvr::Quat q, float x, float y, float z) {
    const float cx = q.y * z - q.z * y + q.w * x,
                cy = q.z * x - q.x * z + q.w * y,
                cz = q.x * y - q.y * x + q.w * z;
    return cyvr::Vec3{x + 2 * (q.y * cz - q.z * cy),
                      y + 2 * (q.z * cx - q.x * cz),
                      z + 2 * (q.x * cy - q.y * cx)};
  };
  const auto widen = [&](float tangent) {
    return std::tan(std::atan(tangent) + (tangent < 0 ? -margin : margin));
  };
  const float left = widen(fov[0]), right = widen(fov[1]), up = widen(fov[2]),
              down = widen(fov[3]);
  const float span = projection == 1 ? cyvr::pi : 2 * cyvr::pi;
  const auto forward = turn(view, 0, 0, -1);
  const float centre = std::atan2(forward.x, -forward.z);
  float low = 0, high = 0, top = 1, bottom = 0;
  constexpr int steps = 8;
  for (int y = 0; y <= steps; ++y)
    for (int x = 0; x <= steps; ++x) {
      if (x && x != steps && y && y != steps)
        continue;
      const auto ray = turn(view, left + (right - left) * x / steps,
                            up + (down - up) * y / steps, -1);
      const float length =
          std::sqrt(ray.x * ray.x + ray.y * ray.y + ray.z * ray.z);
      const float side = std::remainder(
          std::atan2(ray.x, -ray.z) - centre, 2 * cyvr::pi);
      const float v =
          .5f - std::asin(std::clamp(ray.y / length, -1.f, 1.f)) / cyvr::pi;
      low = std::min(low, side);
      high = std::max(high, side);
      top = std::min(top, v);
      bottom = std::max(bottom, v);
    }
  // Every longitude meets at a pole inside the view.
  bool pole = false;
  const auto inverse = inverse_rotation(view);
  for (const float sign : {1.f, -1.f}) {
    const auto axis = turn(inverse, 0, sign, 0);
    if (axis.z >= 0)
      continue;
    const float x = axis.x / -axis.z, y = axis.y / -axis.z;
    if (x > left && x < right && y > down && y < up) {
      pole = true;
      (sign > 0 ? top : bottom) = sign > 0 ? 0.f : 1.f;
    }
  }
  float first = (centre + low) / span + .5f, last = (centre + high) / span + .5f;
  // The 360 seam behind the viewer would need two rectangles.
  if (pole || (projection != 1 && (first < 0 || last > 1))) {
    first = 0;
    last = 1;
  }
  return {std::clamp(first, 0.f, 1.f), std::clamp(top, 0.f, 1.f),
          std::clamp(last, 0.f, 1.f), std::clamp(bottom, 0.f, 1.f)};
}
} // namespace cyvr::player
