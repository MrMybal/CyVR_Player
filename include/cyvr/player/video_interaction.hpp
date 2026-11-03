#pragma once
#include "cyvr/player/video_placement.hpp"
#include <optional>
namespace cyvr::player {
class VideoGrab {
  bool down_{},active_{},blocked_{};
  cyvr::Pose relative_{};
public:
  bool active() const{return active_;}
  bool held() const{return down_;}
  void cancel(){active_=false;blocked_=true;down_=true;}
  std::optional<cyvr::Pose> update(cyvr::Pose controller,bool valid,bool down,
                                  bool target,cyvr::Pose screen){
    if(!down){active_=blocked_=down_=false;return {};}
    if(!valid){cancel();return {};}
    if(!down_ && target && !blocked_){relative_=compose_pose(inverse_pose(controller),screen);active_=true;}
    down_=true;
    if(active_)return compose_pose(controller,relative_);
    return {};
  }
};
inline bool hits_cinema(cyvr::Pose hand,cyvr::Pose screen,float width,float height,float radius=0){
  const auto local=compose_pose(inverse_pose(screen),hand);
  auto o=local.position;const auto d=cyvr::rotate(local.orientation,{0,0,-1});
  if(radius<=0){
    if(std::abs(d.z)<1e-6f)return false;
    const float t=-o.z/d.z;const auto p=o+d*t;
    return t>0 && std::abs(p.x)<=width*.5f && std::abs(p.y)<=height*.5f;
  }
  o.z-=radius;
  const float a=d.x*d.x+d.z*d.z,b=2*(o.x*d.x+o.z*d.z),c=o.x*o.x+o.z*o.z-radius*radius;
  const float discriminant=b*b-4*a*c;
  if(a<1e-6f || discriminant<0)return false;
  for(const float t:{(-b-std::sqrt(discriminant))/(2*a),(-b+std::sqrt(discriminant))/(2*a)}){
    if(t<=0)continue;const auto p=o+d*t;
    if(std::abs(std::atan2(p.x,-p.z))<=width/(2*radius) && std::abs(p.y)<=height*.5f)return true;
  }
  return false;
}
inline cyvr::Pose rotate_video_view(cyvr::Pose screen,cyvr::Pose head,float radians,bool panorama){
  const auto turn=cyvr::yawRotation(radians);
  if(!panorama)screen.position=head.position+cyvr::rotate(turn,screen.position-head.position);
  screen.orientation=multiply_rotation(turn,screen.orientation);
  return screen;
}
} // namespace cyvr::player
