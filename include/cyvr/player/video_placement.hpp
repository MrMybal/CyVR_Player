#pragma once
#include "cyvr/player/video_view.hpp"

namespace cyvr::player {
inline cyvr::Pose compose_pose(cyvr::Pose destination_from_source,
                               cyvr::Pose source_pose) {
  return {destination_from_source.position +
              cyvr::rotate(destination_from_source.orientation,
                           source_pose.position),
          multiply_rotation(destination_from_source.orientation,
                            source_pose.orientation)};
}
inline cyvr::Pose inverse_pose(cyvr::Pose pose) {
  const auto rotation = inverse_rotation(pose.orientation);
  return {cyvr::rotate(rotation, pose.position * -1), rotation};
}

// A video has its own scene pose. No UI panel pose is read after placement,
// including when a panel drag ends and the host faces its controls to the user.
class VideoPlacement {
  cyvr::Pose base_{};
  bool placed_{};

public:
  bool placed() const { return placed_; }
  void reset() { placed_ = false; }
  void place(cyvr::Pose head, float distance) {
    base_ = cyvr::placePanel(head, distance, 1).pose;
    placed_ = true;
  }
  void transform(cyvr::Pose destination_from_source) {
    if (placed_)
      base_ = compose_pose(destination_from_source, base_);
  }
  cyvr::Pose screen(const ImageSettings &image) const {
    return compose_pose(base_,
                        {{image.pan_x * 2, .1f - image.pan_y * 2, -2.15f}, {}});
  }
};
} // namespace cyvr::player
