#pragma once
#include <algorithm>
#include <cmath>
namespace cyvr::player {
struct CinemaSettings {
  // Black, dark, sunlit lounge photo, Venice photo, passthrough, three 3D rooms.
  int environment{};
  float ambient{.25f}, panel_opacity{.9f};
  float width{4}, distance{3.5f}, curvature{}, elevation{}, pitch{}, roll{};
  void sanitize(){
    auto bound=[](float v,float lo,float hi,float fallback){return std::isfinite(v)?std::clamp(v,lo,hi):fallback;};
    environment=std::clamp(environment,0,7);
    ambient=bound(ambient,0,1,.25f);panel_opacity=bound(panel_opacity,.25f,1,.9f);
    width=bound(width,1,12,4);distance=bound(distance,1,12,3.5f);
    curvature=bound(curvature,0,120,0);if(curvature<.1f)curvature=0;
    elevation=bound(elevation,-3,3,0);pitch=bound(pitch,-90,90,0);roll=bound(roll,-180,180,0);
  }
  float angle()const{return curvature*.017453292519943295f;}
  float radius(float zoom=1)const{return curvature>0?width*zoom/angle():0;}
  void reset_screen(){auto e=environment;auto a=ambient,o=panel_opacity;*this={};environment=e;ambient=a;panel_opacity=o;}
  bool operator==(const CinemaSettings& b)const{return environment==b.environment && ambient==b.ambient && panel_opacity==b.panel_opacity && width==b.width && distance==b.distance && curvature==b.curvature && elevation==b.elevation && pitch==b.pitch && roll==b.roll;}
  bool operator!=(const CinemaSettings& b)const{return !(*this==b);}
};
}
