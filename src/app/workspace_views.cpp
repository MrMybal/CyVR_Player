#include "cyvr/locale.hpp"
#include "cyvr/player/application.hpp"
#include <cstdio>
#include <filesystem>
namespace cyvr::player {
namespace {
std::string clipped(cyvr::DrawList &d, std::string_view value, float width,
                    float size = 22) {
  return fit_text(d, value, width, size);
}
constexpr float browser_top = 304, browser_height = 500;
constexpr cyvr::Rect browser_rail{1382, browser_top, 22, browser_height};
// Clip only the bounded visible rows, retaining interpolated glyph/image UVs.
// The foundation's renderer can therefore keep using one retained UI draw.
void append_browser(cyvr::DrawList& dst, const cyvr::DrawList& src) {
  const float bottom=browser_top+browser_height;
  auto blend=[](const cyvr::Vertex& a,const cyvr::Vertex& b,float t){
    return cyvr::Vertex{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,
      a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t,a.r+(b.r-a.r)*t,
      a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t};
  };
  for(size_t n=0;n+2<src.vertices.size();n+=3){
    const auto* tri=src.vertices.data()+n;
    const float lo=std::min({tri[0].y,tri[1].y,tri[2].y}),hi=std::max({tri[0].y,tri[1].y,tri[2].y});
    if(hi<=browser_top || lo>=bottom)continue;
    if(lo>=browser_top && hi<=bottom){dst.vertices.insert(dst.vertices.end(),tri,tri+3);continue;}
    std::array<cyvr::Vertex,8> a{},b{};std::copy(tri,tri+3,a.begin());int count=3;
    for(int edge=0;edge<2;++edge){
      const float y=edge?bottom:browser_top;int out=0;
      for(int i=0;i<count;++i){
        const auto& p=a[i];const auto& q=a[(i+1)%count];
        const bool pin=edge?p.y<=y:p.y>=y,qin=edge?q.y<=y:q.y>=y;
        if(pin)b[out++]=p;
        if(pin!=qin)b[out++]=blend(p,q,(y-p.y)/(q.y-p.y));
      }
      a=b;count=out;
    }
    for(int i=1;i+1<count;++i){dst.vertices.push_back(a[0]);dst.vertices.push_back(a[i]);dst.vertices.push_back(a[i+1]);}
  }
}
} // namespace
int PlayerApplication::browser_visible_count() const {
  const float fraction=browser_scroll_-std::floor(browser_scroll_/browser_row_height())*browser_row_height();
  return std::min(int(preview_slots),int(std::ceil((browser_height+fraction)/browser_row_height()))*browser_columns());
}
float PlayerApplication::browser_scroll_limit() const {
  const int rows=(int(entries_.size())+browser_columns()-1)/browser_columns();
  return std::max(0.f,rows*browser_row_height()-(browser_view_?18.f:10.f)-browser_height);
}
void PlayerApplication::set_browser_scroll(float value) {
  if(!std::isfinite(value))return;
  const float next=std::clamp(value,0.f,browser_scroll_limit());
  const int first=int(std::floor(next/browser_row_height()))*browser_columns();
  if(next==browser_scroll_ && first==offset_)return;
  if(first!=offset_)preview_present_.fill(false);
  browser_scroll_=next;offset_=first;state_dirty_=true;ui_.invalidate();
}
bool PlayerApplication::browser_pointer(size_t hand,cyvr::Vec2 pos,bool down) {
  if(hand>=2)return false;
  const bool began=down && !browser_pointer_down_[hand];browser_pointer_down_[hand]=down;
  if(page_!=0 || ui_.interfaceHidden || ui_.compactInterface || ui_.windowState!=cyvr::WindowState::Open){browser_drag_[hand]=false;return false;}
  const float limit=browser_scroll_limit();
  const float handle=std::max(36.f,browser_height*browser_height/(browser_height+limit));
  const float travel=browser_height-handle;
  if(began && browser_rail.contains(pos) && limit>0){
    const float top=browser_top+travel*browser_scroll_/limit;
    browser_grab_[hand]=pos.y>=top && pos.y<top+handle?pos.y-top:handle*.5f;
    browser_drag_[hand]=true;
  }
  if(!browser_drag_[hand])return false;
  if(down && !(pos.x==-100 && pos.y==-100) && travel>0)
    set_browser_scroll((pos.y-browser_top-browser_grab_[hand])/travel*limit);
  if(!down)browser_drag_[hand]=false;
  return true;
}
bool PlayerApplication::scroll_library(cyvr::Vec2 point,float delta){
  if(page_!=0 || ui_.interfaceHidden || ui_.compactInterface || ui_.modalOpen || ui_.windowState!=cyvr::WindowState::Open || !cyvr::Rect{280,304,1124,500}.contains(point))return false;
  bool capture=browser_drag_[0] || browser_drag_[1];
  for(size_t hand=0;hand<2;++hand){const int id=ui_.captured(hand);capture=capture || (id>=1100 && id<1100+int(preview_slots));}
  if(!capture)set_browser_scroll(browser_scroll_+delta);
  return true;
}
void PlayerApplication::icon_button(cyvr::DrawList &d, int id, cyvr::Rect r,
                                    Icon icon, std::string_view hint,
                                    bool selected, bool enabled, bool label) {
  button(d, id, r, "", selected, enabled);
  const bool light = ui_.settings.theme == cyvr::Theme::Light;
  const bool mode_switch=id==1062 && enabled;
  if(mode_switch)d.rect(r,cyvr::Color::hex(light?0x4054c5:0x5262ce),12);
  const auto color = cyvr::Color::hex(mode_switch?0xffffff:selected  ? (light ? 0xffffff : 0x102d28)
                                      : enabled ? (light ? 0x142b35 : 0xf0f5f6)
                                                : 0x778b94);
  const auto text = tr(hint);
  float size = 21;
  if (label)
    while (size > 14 && d.textWidth(size, text) + 54 > r.w)
      size -= 1;
  const auto width = label ? d.textWidth(size, text) + 42 : 30;
  const float start = r.x + (r.w - width) * .5f;
  draw_icon(d, icon, {label ? start + 14 : r.x + r.w * .5f, r.y + r.h * .5f},
            28, color);
  if (label)
    d.text(start + 40, r.y + (r.h - 28) * .5f, size, text, color);
  if (ui_.hovered() == id)
    icon_hint_ = std::string(text);
}
void PlayerApplication::refresh_locations() {
  try {
    locations_ =
        platform_.locations
            ? platform_.locations()
            : std::vector<BrowserItem>{{platform_.root, "App files", true, 0}};
  } catch (const std::exception &) {
    locations_ = {{platform_.root, "App files", true, 0}};
  }
}
void PlayerApplication::toggle_favorite() {
  const auto found =
      std::find_if(favorites_.begin(), favorites_.end(),
                   [&](const auto &f) { return f.location == directory_; });
  if (found != favorites_.end()) {
    favorites_.erase(found);
    return;
  }
  if (favorites_.size() >= 16) {
    ui_.setToast(std::string(tr("Up to 16 favorite folders.")));
    return;
  }
  std::string name =
      directory_.rfind("content://", 0) == 0
          ? std::string(tr("Folder"))
          : std::filesystem::u8path(directory_).filename().u8string();
  for (const auto &entry : locations_)
    if (entry.location == directory_)
      name = std::string(tr(entry.name));
  // A previously visited child retains its provider display name.
  if (!folder_name_.empty())
    name = folder_name_;
  auto parents = parents_;
  if (parents.size() > 32)
    parents.erase(parents.begin() + 1, parents.end() - 31);
  favorites_.push_back({directory_, name, std::move(parents)});
}
void PlayerApplication::draw_library(cyvr::DrawList &d) {
  using cyvr::Color;
  const bool light = ui_.settings.theme == cyvr::Theme::Light;
  const auto white = Color::hex(light ? 0x142b35 : 0xf0f5f6),
             muted = Color::hex(light ? 0x48616b : 0xa7b7bf),
             accent = Color::hex(light ? 0x126854 : 0x86e7cf),
             surface = Color::hex(light ? 0xc1d3db : 0x22323b);
  d.text(36, 187, 32, tr("Your videos"), white);
  icon_button(d, 1010, {676, 180, 58, 54}, Icon::Up, "Parent folder", false,
              !parents_.empty());
  icon_button(d, 1011, {746, 180, 58, 54}, Icon::Refresh, "Refresh");
  const bool favorite =
      std::any_of(favorites_.begin(), favorites_.end(),
                  [&](const auto &f) { return f.location == directory_; });
  icon_button(d, 1017, {816, 180, 58, 54}, Icon::Star,
              favorite ? "Remove favorite folder" : "Add favorite folder",
              favorite);
  icon_button(d, 1012, {888, 180, 294, 54}, Icon::Folder, "Open a folder",
              false, bool(platform_.request_tree), true);
  icon_button(d, 1015, {1246, 180, 68, 54}, Icon::List, "List view",
              browser_view_ == 0);
  icon_button(d, 1016, {1326, 180, 68, 54}, Icon::Cards, "Tile view",
              browser_view_ == 1);
  d.text(286, 260, 19,
         clipped(d,
                 directory_.rfind("content://", 0) == 0
                     ? tr("Authorized Android folder")
                     : std::string_view(directory_),
                 1100, 19),
         muted);
  d.rect({24, 248, 236, 574}, Color::hex(light ? 0xd1dfe4 : 0x101c23), 14);
  icon_button(d, 1018, {36, 264, 101, 54}, Icon::Folder, "Locations",
              sidebar_tab_ == 0);
  icon_button(d, 1019, {145, 264, 101, 54}, Icon::Star, "Favorites",
              sidebar_tab_ == 1);
  d.text(38, 332, 19, tr(sidebar_tab_ ? "Favorites" : "Locations"), accent);
  const int total =
      sidebar_tab_ ? int(favorites_.size()) : int(locations_.size());
  sidebar_offset_ =
      std::min(sidebar_offset_, total ? ((total - 1) / 7) * 7 : 0);
  for (int row = 0; row < 7 && sidebar_offset_ + row < total; ++row) {
    const int i = sidebar_offset_ + row;
    const auto &location =
        sidebar_tab_ ? favorites_[i].location : locations_[i].location;
    const auto label =
        sidebar_tab_ ? favorites_[i].name : std::string(tr(locations_[i].name));
    button(d, 1800 + row, {36, 370.f + row * 54, 212, 46},
           clipped(d, label, 182, 23), location == directory_);
  }
  if (!total)
    d.text(38, 381, 17,
           tr(sidebar_tab_ ? "No favorite folders yet."
                           : "No locations available."),
           muted);
  icon_button(d, 1026, {36, 766, 98, 42}, Icon::Previous, "Previous", false,
              sidebar_offset_ > 0);
  icon_button(d, 1027, {146, 766, 98, 42}, Icon::Next, "Next", false,
              sidebar_offset_ + 7 < total);
  auto& content=browser_draw_;content.clear();
  const int count = browser_visible_count();
  const float fraction=browser_scroll_-(offset_/browser_columns())*browser_row_height();
  int shown=0;
  for (int row = 0; row < count && offset_ + row < int(entries_.size());
       ++row) {
    const auto &item = entries_[offset_ + row];
    cyvr::Rect r = browser_view_ ? cyvr::Rect{280.f + (row % 3) * 366,
                                              browser_top + (row / 3) * 259-fraction, 354, 241}
                                 : cyvr::Rect{280, browser_top + row * 72-fraction, 1086, 62};
    if(r.y+r.h<=browser_top || r.y>=browser_top+browser_height)continue;
    ++shown;
    const bool hover = ui_.hovered() == 1100 + row;
    content.rect(r, hover ? Color::hex(light ? 0xacd6ce : 0x344b56) : surface, 12);
    cyvr::Rect preview = browser_view_
                             ? cyvr::Rect{r.x + 8, r.y + 8, r.w - 16, 166}
                             : cyvr::Rect{r.x + 8, r.y + 1, 107, 60};
    const auto kind = classify_extension(item.name);
    if (preview_present_[row] && !item.directory)
      content.image(preview,
              {.5f / 160, (row * 90 + .5f) / ui_atlas_height, 159.f / 160, 89.f / ui_atlas_height});
    else {
      content.rect(preview, Color::hex(light ? 0xb4cbd3 : 0x14242e), 8);
      draw_icon(content,
                item.directory               ? Icon::Folder
                : kind == FileKind::subtitle ? Icon::Tracks
                                             : Icon::Play,
                {preview.x + preview.w * .5f, preview.y + preview.h * .5f},
                browser_view_ ? 46 : 30, accent);
    }
    if (browser_view_) {
      content.text(r.x + 14, r.y + 183, 21, clipped(content, item.name, r.w - 28, 21),
             white);
      content.text(r.x + 14, r.y + 214, 16,
             tr(item.directory               ? "Folder"
                : kind == FileKind::disc     ? "Disc image"
                : kind == FileKind::subtitle ? "Subtitles"
                                             : "Video"),
             muted);
    } else {
      content.text(r.x + 136, r.y + 17, 23, clipped(content, item.name, r.w - 270, 23),
             white);
      if (!item.directory) {
        char size[40];
        std::snprintf(size, sizeof(size),
                      item.size >= 1073741824 ? "%.1f GiB" : "%.1f MiB",
                      double(item.size) /
                          (item.size >= 1073741824 ? 1073741824 : 1048576));
        content.text(r.x + r.w - 121, r.y + 21, 18, size, muted);
      }
    }
    // Hit rectangles use the same clip as rendering; offscreen rows cannot open.
    const float bottom=std::min(r.y+r.h,browser_top+browser_height);
    r.y=std::max(r.y,browser_top);r.h=bottom-r.y;
    ui_.applicationWidget(1100 + row, r);
  }
  append_browser(d,content);
  const float limit=browser_scroll_limit();
  const float handle=std::max(36.f,browser_height*browser_height/(browser_height+limit));
  d.rect(browser_rail,surface,10);
  d.rect({browser_rail.x+4,browser_top+(limit>0?(browser_height-handle)*browser_scroll_/limit:0),14,handle},limit>0?accent:muted,7);
  d.text(284,842,19,tr("Joystick up/down: scroll"),accent);
  const auto range=entries_.empty()?"0 / 0":std::to_string(offset_+1)+"–"+std::to_string(std::min(offset_+shown,int(entries_.size())))+" / "+std::to_string(entries_.size());
  d.text(1170,842,19,range,muted);
  d.text(284,882,18,clipped(d,tr(browse_status_),1100,18),muted);
}
void PlayerApplication::draw_settings(cyvr::DrawList &d) {
  if(window_settings_open_){draw_window_settings(d);return;}
  const auto white = cyvr::Color::hex(
      ui_.settings.theme == cyvr::Theme::Light ? 0x142b35 : 0xf0f5f6);
  d.text(36, 190, 34, tr("Settings"), white);
  d.text(52, 304, 24, tr("Language"), white);
  button(d, 1060, {470, 288, 480, 64},
         ui_.settings.language == "fr" ? "Français" : "English");
  d.text(52, 402, 24, tr("Theme"), white);
  button(d, 1061, {470, 386, 480, 64},
         ui_.settings.theme == cyvr::Theme::Light   ? "Light"
         : ui_.settings.theme == cyvr::Theme::Ocean ? "Ocean"
                                                    : "Graphite");
  d.text(52, 506, 24, tr("Volume"), white);
  icon_button(d, 1370, {368, 488, 70, 58},
              volume > 0 ? Icon::Volume : Icon::Mute, "Mute", volume == 0);
  const cyvr::Rect track{470, 488, 820, 58};
  d.rect({track.x, track.y + 25, track.w, 8}, cyvr::Color::hex(0x344b56), 4);
  d.rect({track.x, track.y + 25, track.w * volume, 8},
         cyvr::Color::hex(0x86e7cf), 4);
  d.circle({track.x + track.w * volume, track.y + 29}, 10,
           cyvr::Color::hex(0x86e7cf));
  ui_.applicationWidget(1371, track, cyvr::Control::Slider);
  icon_button(d, 1680, {52, 614, 580, 72}, Icon::Move, "Video placement", false,
              true, true);
  icon_button(d, 1062, {674, 614, 680, 72},
              ui_.presentation == cyvr::Presentation::Immersive ? Icon::Window
                                                                : Icon::Headset,
              ui_.presentation == cyvr::Presentation::Immersive
                  ? "Return to 2D"
                  : "Enter immersive",
              false,
              ui_.presentation == cyvr::Presentation::Immersive ||
                  ui_.immersiveAvailable,
              true);
  icon_button(d,1440,{52,730,1302,66},Icon::Tracks,"Subtitle settings",false,true,true);
  icon_button(d,1900,{52,826,1302,66},Icon::Window,"Interface window",false,true,true);
}
void PlayerApplication::draw_window_settings(cyvr::DrawList &d) {
  const auto white=cyvr::Color::hex(ui_.settings.theme==cyvr::Theme::Light?0x142b35:0xf0f5f6);
  const auto accent=cyvr::Color::hex(ui_.settings.theme==cyvr::Theme::Light?0x126854:0x86e7cf);
  d.text(36,190,34,tr("Interface window"),white);
  button(d,1901,{1150,182,254,58},"Back");
  button(d,1902,{52,290,1302,64},"Resize with grip on edges",ui_.window.resizable);
  button(d,1903,{52,370,1302,64},"Face viewer while moving",ui_.window.faceViewerWhileMoving);
  button(d,1904,{52,450,1302,64},"Reveal window icons on hover",ui_.settings.minimalDock);
  d.text(52,552,24,tr("Placement mode"),white);
  const char* modes[]{"World anchored","Face viewer","Follow view"};
  for(int i=0;i<3;++i)button(d,1910+i,{52.f+i*440,604,422,64},modes[i],int(ui_.settings.follow)==i);
  button(d,1905,{52,710,1302,58},"Reset window controls");
  d.text(52,812,20,tr("Grip the window to move it; grip an edge or corner to resize."),white);
  d.text(52,850,20,tr("While holding: joystick up/down adjusts distance, left/right turns the window."),white);
  d.text(52,892,18,tr("These controls apply in immersive mode. The movie keeps its own placement."),accent);
}
void PlayerApplication::draw_placement(cyvr::DrawList &d) {
  const auto white = cyvr::Color::hex(ui_.settings.theme == cyvr::Theme::Light
                                          ? 0x142b35
                                          : 0xf0f5f6),
             accent = cyvr::Color::hex(0x86e7cf);
  float values[]{scene.x, scene.y, scene.z, scene.yaw, scene.pitch, scene.roll};
  const char *labels[]{"Left / right (m)",       "Up / down (m)",
                       "Forward / backward (m)", "Yaw (degrees)",
                       "Pitch (degrees)",        "Roll (degrees)"};
  for (int n = projection ? 3 : 0; n < 6; ++n) {
    const float y = 338 + (n - (projection ? 3 : 0)) * 64,
                limit = n < 3 ? 10.f : 180.f;
    d.text(44, y + 9, 21, tr(labels[n]), white);
    d.text(365, y + 9, 21,
           cyvr::formatNumber(ui_.settings.language, values[n], 2), accent);
    const auto f = (values[n] + limit) / (2 * limit);
    cyvr::Rect r{478, y, 914, 48};
    d.rect({r.x, y + 20, r.w, 8}, cyvr::Color::hex(0x344b56), 4);
    d.rect({r.x, y + 20, r.w * f, 8}, accent, 4);
    d.circle({r.x + r.w * f, y + 24}, 10, accent);
    ui_.applicationWidget(1690 + n, r, cyvr::Control::Slider);
  }
  if (projection)
    d.text(44, 570, 21,
           tr("Panoramas are viewed at infinity: use rotation and Zoom."),
           white);
  icon_button(d, 1681, {36, 746, 386, 58}, Icon::Rotate, "Reset placement",
              false, true, true);
  icon_button(d, 1683, {438, 746, 478, 58}, Icon::Recenter,
              "Place in front of me", false,
              ui_.presentation == cyvr::Presentation::Immersive, true);
  icon_button(d, 1682, {934, 746, 470, 58}, Icon::Pin,
              "Keep placement on recenter", scene.stable, true, true);
  d.text(36, 844, 20,
         tr("Point at the movie, hold trigger, then move or turn your hand."),
         white);
  d.text(36, 880, 18,
         tr("Joystick left/right: turn view. Grip + trigger: zoom."),
         white);
}
} // namespace cyvr::player
