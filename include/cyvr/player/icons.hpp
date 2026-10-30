#pragma once
#include "cyvr/core.hpp"
#include <cmath>
namespace cyvr::player {
enum class Icon {
  Play,
  Pause,
  Stop,
  Previous,
  Next,
  Back10,
  Forward10,
  Volume,
  Mute,
  Library,
  Info,
  Settings,
  Headset,
  Window,
  Image,
  Move,
  Rotate,
  Recenter,
  ZoomIn,
  ZoomOut,
  Fit,
  Mono,
  Sbs,
  Tb,
  Cinema,
  HalfSphere,
  Sphere,
  Swap,
  Half,
  Detect,
  Geometry,
  Environment,
  Glow,
  List,
  Cards,
  Folder,
  Star,
  Up,
  Refresh,
  Close,
  Pin,
  Minimize,
  Tracks
};
inline void draw_icon(cyvr::DrawList &d, Icon icon, cyvr::Vec2 center,
                      float size, cyvr::Color c) {
  const float s = size / 24;
  auto p = [&](float x, float y) {
    return cyvr::Vec2{center.x + (x - 12) * s, center.y + (y - 12) * s};
  };
  auto line = [&](float x, float y, float a, float b) {
    d.line(p(x, y), p(a, b), c, 1.8f * s);
  };
  auto box = [&](float x, float y, float w, float h) {
    auto q = p(x, y);
    d.border({q.x, q.y, w * s, h * s}, c, 2 * s, 1.7f * s);
  };
  auto tri = [&](float x) { d.triangle(p(x, 5), p(x, 19), p(x + 10, 12), c); };
  switch (icon) {
  case Icon::Play:
    tri(8);
    break;
  case Icon::Pause:
    for (int x : {7, 14}) {
      auto q = p(float(x), 5);
      d.rect({q.x, q.y, 3 * s, 14 * s}, c, 1 * s);
    }
    break;
  case Icon::Stop: {
    auto q = p(6, 6);
    d.rect({q.x, q.y, 12 * s, 12 * s}, c, 2 * s);
    break;
  }
  case Icon::Previous:
    line(5, 5, 5, 19);
    d.triangle(p(18, 5), p(18, 19), p(7, 12), c);
    break;
  case Icon::Next:
    tri(5);
    line(18, 5, 18, 19);
    break;
  case Icon::Back10:
  case Icon::Forward10: {
    bool back = icon == Icon::Back10;
    line(5, 6, 19, 6);
    line(back ? 5 : 19, 6, back ? 9 : 15, 2);
    line(back ? 5 : 19, 6, back ? 9 : 15, 10);
    d.text(center.x - d.textWidth(10 * s, "10") * .5f, center.y + 0 * s, 10 * s,
           "10", c);
    break;
  }
  case Icon::Volume:
  case Icon::Mute:
    line(3, 9, 7, 9);
    line(7, 9, 12, 5);
    line(12, 5, 12, 19);
    line(12, 19, 7, 15);
    line(7, 15, 3, 15);
    line(3, 15, 3, 9);
    if (icon == Icon::Mute) {
      line(16, 8, 21, 16);
      line(21, 8, 16, 16);
    } else {
      line(16, 8, 18, 10);
      line(18, 10, 18, 14);
      line(18, 14, 16, 16);
      line(20, 5, 22, 9);
      line(22, 9, 22, 15);
      line(22, 15, 20, 19);
    }
    break;
  case Icon::Library:
  case Icon::Folder:
    line(3, 8, 3, 20);
    line(3, 20, 21, 20);
    line(21, 20, 21, 7);
    line(21, 7, 12, 7);
    line(12, 7, 9, 4);
    line(9, 4, 3, 4);
    line(3, 4, 3, 8);
    break;
  case Icon::Info:
    d.ring(center, 9 * s, 1.8f * s, c);
    line(12, 11, 12, 17);
    d.circle(p(12, 7), 1.1f * s, c);
    break;
  case Icon::Settings:
    for (int i = 0; i < 8; ++i) {
      float a = i * cyvr::pi / 4;
      line(12 + 8 * std::cos(a), 12 + 8 * std::sin(a), 12 + 11 * std::cos(a),
           12 + 11 * std::sin(a));
    }
    d.ring(center, 7.5f * s, 1.8f * s, c);
    d.ring(center, 2.6f * s, 1.8f * s, c);
    break;
  case Icon::Headset:
    box(2, 6, 20, 12);
    line(4, 18, 9, 18);
    line(9, 18, 12, 14);
    line(12, 14, 15, 18);
    line(15, 18, 20, 18);
    break;
  case Icon::Window:
    box(3, 8, 15, 12);
    box(7, 4, 14, 11);
    break;
  case Icon::Image:
    box(3, 4, 18, 16);
    line(4, 17, 10, 11);
    line(10, 11, 14, 16);
    line(14, 16, 17, 13);
    line(17, 13, 20, 17);
    d.circle(p(16, 8), 1.5f * s, c);
    break;
  case Icon::Move:
  case Icon::Recenter:
    line(12, 2, 12, 22);
    line(2, 12, 22, 12);
    for (int k = 0; k < 4; ++k) {
      const float a = k * cyvr::pi / 2;
      auto q = p(12 + 10 * std::cos(a), 12 + 10 * std::sin(a));
      auto q1 = p(12 + 6 * std::cos(a) - 3 * std::sin(a),
                  12 + 6 * std::sin(a) + 3 * std::cos(a));
      auto q2 = p(12 + 6 * std::cos(a) + 3 * std::sin(a),
                  12 + 6 * std::sin(a) - 3 * std::cos(a));
      d.line(q, q1, c, 1.8f * s);
      d.line(q, q2, c, 1.8f * s);
    }
    break;
  case Icon::Rotate:
    d.ring(center, 8 * s, 1.8f * s, c);
    line(17, 4, 22, 4);
    line(22, 4, 22, 9);
    break;
  case Icon::ZoomIn:
  case Icon::ZoomOut:
    d.ring(p(10, 10), 6 * s, 1.8f * s, c);
    line(15, 15, 21, 21);
    line(7, 10, 13, 10);
    if (icon == Icon::ZoomIn)
      line(10, 7, 10, 13);
    break;
  case Icon::Fit:
  case Icon::Geometry:
    box(3, 5, 18, 14);
    line(3, 3, 21, 3);
    line(1, 5, 1, 19);
    break;
  case Icon::Mono:
  case Icon::Cinema:
    box(2, 5, 20, 13);
    if (icon == Icon::Cinema) {
      line(12, 18, 12, 22);
      line(7, 22, 17, 22);
    }
    break;
  case Icon::Sbs:
    box(2, 5, 9, 14);
    box(13, 5, 9, 14);
    break;
  case Icon::Tb:
    box(3, 3, 18, 8);
    box(3, 13, 18, 8);
    break;
  case Icon::Sphere:
  case Icon::HalfSphere:
  case Icon::Environment:
    d.ring(center, 9 * s, 1.8f * s, c);
    line(3, 12, 21, 12);
    line(12, 3, 12, 21);
    if (icon == Icon::HalfSphere)
      line(6, 5, 6, 19);
    else {
      line(8, 4, 6, 12);
      line(6, 12, 8, 20);
      line(16, 4, 18, 12);
      line(18, 12, 16, 20);
    }
    break;
  case Icon::Swap:
    line(3, 8, 21, 8);
    line(17, 4, 21, 8);
    line(17, 12, 21, 8);
    line(21, 17, 3, 17);
    line(7, 13, 3, 17);
    line(7, 21, 3, 17);
    break;
  case Icon::Half:
    box(3, 4, 18, 16);
    line(12, 4, 12, 20);
    for (int y = 7; y < 20; y += 4)
      line(4, float(y), 11, float(y));
    break;
  case Icon::Detect:
    box(4, 4, 16, 16);
    d.ring(center, 4 * s, 1.8f * s, c);
    line(12, 1, 12, 6);
    line(12, 18, 12, 23);
    line(1, 12, 6, 12);
    line(18, 12, 23, 12);
    break;
  case Icon::Glow:
    d.circle(center, 4 * s, c);
    for (int k = 0; k < 8; ++k) {
      float a = k * cyvr::pi / 4;
      line(12 + 7 * std::cos(a), 12 + 7 * std::sin(a), 12 + 10 * std::cos(a),
           12 + 10 * std::sin(a));
    }
    break;
  case Icon::List:
  case Icon::Tracks:
    for (int y : {5, 12, 19}) {
      d.circle(p(3, float(y)), 1 * s, c);
      line(7, float(y), 21, float(y));
    }
    break;
  case Icon::Cards:
    for (int y : {3, 14})
      for (int x : {3, 14})
        box(float(x), float(y), 7, 7);
    break;
  case Icon::Star: {
    cyvr::Vec2 points[10];
    for (int n = 0; n < 10; ++n) {
      float a = -cyvr::pi / 2 + n * cyvr::pi / 5, r = n % 2 ? 4.f : 10.f;
      points[n] = p(12 + r * std::cos(a), 12 + r * std::sin(a));
    }
    d.polyline(points, 10, c, 1.8f * s, true);
    break;
  }
  case Icon::Pin:
    line(8, 3, 16, 3);
    line(9, 3, 9, 10);
    line(15, 3, 15, 10);
    line(9, 10, 6, 15);
    line(15, 10, 18, 15);
    line(6, 15, 18, 15);
    line(12, 15, 12, 22);
    break;
  case Icon::Up:
    line(12, 21, 12, 3);
    line(5, 10, 12, 3);
    line(19, 10, 12, 3);
    break;
  case Icon::Refresh:
    d.ring(center, 8 * s, 1.8f * s, c);
    line(18, 3, 22, 3);
    line(22, 3, 22, 8);
    break;
  case Icon::Close:
    line(5, 5, 19, 19);
    line(19, 5, 5, 19);
    break;
  case Icon::Minimize:
    line(4, 12, 20, 12);
    break;
  }
}
} // namespace cyvr::player
