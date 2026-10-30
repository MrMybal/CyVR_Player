#pragma once
#include "cyvr/player/video_view.hpp"
#include "cyvr/player/cinema_settings.hpp"
#include "cyvr/player/scene_settings.hpp"

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
inline cyvr::Pose cylinder_center(cyvr::Pose screen,float radius){
  screen.position=screen.position+cyvr::rotate(screen.orientation,{0,0,radius});return screen;
}

// A video has its own scene pose. No UI panel pose is read after placement,
// including when a panel drag ends and the host faces its controls to the user.
class VideoPlacement {
  cyvr::Pose base_{};
  bool placed_{};
  float anchor_distance_{1.35f};

public:
  bool placed() const { return placed_; }
  void reset() { placed_ = false; }
  void place(cyvr::Pose head, float distance,bool align_to_view=false) {
    base_ = cyvr::placePanel(head, distance, 1).pose;
    anchor_distance_=std::clamp(distance,1.f,2.f);
    if(align_to_view)base_={head.position+cyvr::rotate(head.orientation,{0,-.1f,-anchor_distance_}),head.orientation};
    placed_ = true;
  }
  void transform(cyvr::Pose destination_from_source) {
    if (placed_)
      base_ = compose_pose(destination_from_source, base_);
  }
  cyvr::Pose screen(const ImageSettings &image,const CinemaSettings& cinema={},const SceneSettings& scene={}) const {
    const float pitch=(cinema.pitch+scene.pitch)*cyvr::pi/360,roll=(cinema.roll+scene.roll)*cyvr::pi/360;
    const auto rotation=multiply_rotation(cyvr::yawRotation(scene.yaw*cyvr::pi/180),multiply_rotation({std::sin(pitch),0,0,std::cos(pitch)}, {0,0,std::sin(roll),std::cos(roll)}));
    return compose_pose(base_,
                        {{image.pan_x * 2+scene.x, .1f + cinema.elevation - image.pan_y * 2+scene.y, anchor_distance_-cinema.distance+scene.z}, rotation});
  }
};
} // namespace cyvr::player
