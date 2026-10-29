#pragma once
#include "cyvr/core.hpp"
#include "cyvr/player/image_settings.hpp"
namespace cyvr::player {
struct VideoView {
  // 0 packed source, 1 single eye in a 2D panel, 2 cinema, 3 VR180, 4 VR360.
  int kind{}, eye{};
  float fov[4]{-1, 1, 1, -1}; // tangent left/right/up/down
  cyvr::Quat orientation{};   // eye ray -> video-local coordinates
  cyvr::Vec3 origin{};        // eye position relative to the cinema screen
  float screen_width{4}, screen_height{2.25f};
};
// Exact shared layout consumed by video.frag on Android and in the GPU probe.
struct alignas(16) VideoPush {
  float crop[4]{0, 0, 1, 1}, controls[4]{0, 1, 1, 1}, view[4]{1, 0, 0, .5f};
  int format[4]{};
  float fov[4]{-1, 1, 1, -1}, rotation[4]{0, 0, 0, 1}, origin[4]{},
      screen[4]{4, 2.25f, 0, 0};
};
static_assert(sizeof(VideoPush) == 128);
inline VideoPush video_push(cyvr::Rect crop, const ImageSettings &settings,
                            int stereo, const VideoView &mapping) {
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
  p.view[3] = settings.sharpness;
  p.format[0] = stereo;
  p.format[1] = mapping.kind;
  p.format[2] = mapping.eye;
  std::copy(mapping.fov, mapping.fov + 4, p.fov);
  const auto q = mapping.orientation;
  p.rotation[0] = q.x;
  p.rotation[1] = q.y;
  p.rotation[2] = q.z;
  p.rotation[3] = q.w;
  p.origin[0] = mapping.origin.x;
  p.origin[1] = mapping.origin.y;
  p.origin[2] = mapping.origin.z;
  p.screen[0] = mapping.screen_width;
  p.screen[1] = mapping.screen_height;
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
} // namespace cyvr::player
