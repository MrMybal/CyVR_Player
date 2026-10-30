#pragma once
#include <algorithm>
#include <cmath>
namespace cyvr::player {
struct SceneSettings {
  float x{}, y{}, z{}, yaw{}, pitch{}, roll{};
  bool stable{true};
  void sanitize() {
    auto bound = [](float v, float lo, float hi) {
      return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.f;
    };
    x = bound(x, -10, 10);
    y = bound(y, -10, 10);
    z = bound(z, -10, 10);
    yaw = bound(yaw, -180, 180);
    pitch = bound(pitch, -180, 180);
    roll = bound(roll, -180, 180);
  }
  void reset() {
    const auto keep = stable;
    *this = {};
    stable = keep;
  }
  SceneSettings rotation_only() const {
    auto result = *this;
    result.x = result.y = result.z = 0;
    return result;
  }
  bool operator==(const SceneSettings &b) const {
    return x == b.x && y == b.y && z == b.z && yaw == b.yaw &&
           pitch == b.pitch && roll == b.roll && stable == b.stable;
  }
  bool operator!=(const SceneSettings &b) const { return !(*this == b); }
};
} // namespace cyvr::player
