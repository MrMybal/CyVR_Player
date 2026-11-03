#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace cyvr::player {
struct SubtitleSettings {
  float scale{1}, x{}, y{}, opacity{1}, outline{2}, shadow{1}, distance{3.5f};
  int color{};
  bool custom{}, bold{}, italic{}, box{}, follow_depth{true};
  void sanitize() {
    auto bound=[](float v,float lo,float hi,float fallback){return std::isfinite(v)?std::clamp(v,lo,hi):fallback;};
    scale=bound(scale,.5f,2.5f,1);x=bound(x,-.45f,.45f,0);y=bound(y,-.45f,.45f,0);
    opacity=bound(opacity,0,1,1);outline=bound(outline,0,6,2);shadow=bound(shadow,0,6,1);distance=bound(distance,.5f,12,3.5f);
    color=std::clamp(color,0,5);
  }
  std::uint32_t text_color()const {constexpr std::uint32_t colors[]{0xffffff,0xffe66d,0x71e5ff,0x9cf0ac,0xffbb78,0xffa9db};return colors[std::clamp(color,0,5)];}
  bool operator==(const SubtitleSettings& b)const{return scale==b.scale && x==b.x && y==b.y && opacity==b.opacity && outline==b.outline && shadow==b.shadow && distance==b.distance && color==b.color && custom==b.custom && bold==b.bold && italic==b.italic && box==b.box && follow_depth==b.follow_depth;}
  bool operator!=(const SubtitleSettings& b)const{return !(*this==b);}
};
}
