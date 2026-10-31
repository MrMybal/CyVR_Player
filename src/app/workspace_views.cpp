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
} // namespace
void PlayerApplication::icon_button(cyvr::DrawList &d, int id, cyvr::Rect r,
                                    Icon icon, std::string_view hint,
                                    bool selected, bool enabled, bool label) {
  button(d, id, r, "", selected, enabled);
  const bool light = ui_.settings.theme == cyvr::Theme::Light;
  const auto color = cyvr::Color::hex(selected  ? (light ? 0xffffff : 0x102d28)
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
  icon_button(d, 1016, {1326, 180, 68, 54}, Icon::Cards, "Card view",
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
  const int count = browser_page_size();
  for (int row = 0; row < count && offset_ + row < int(entries_.size());
       ++row) {
    const auto &item = entries_[offset_ + row];
    cyvr::Rect r = browser_view_ ? cyvr::Rect{280.f + (row % 3) * 378,
                                              304.f + (row / 3) * 259, 366, 241}
                                 : cyvr::Rect{280, 304.f + row * 75, 1128, 65};
    const bool hover = ui_.hovered() == 1100 + row;
    d.rect(r, hover ? Color::hex(light ? 0xacd6ce : 0x344b56) : surface, 12);
    cyvr::Rect preview = browser_view_
                             ? cyvr::Rect{r.x + 8, r.y + 8, r.w - 16, 166}
                             : cyvr::Rect{r.x + 8, r.y + 1, 112, 63};
    const auto kind = classify_extension(item.name);
    if (preview_present_[row] && !item.directory)
      d.image(preview,
              {.5f / 160, (row * 90 + .5f) / ui_atlas_height, 159.f / 160, 89.f / ui_atlas_height});
    else {
      d.rect(preview, Color::hex(light ? 0xb4cbd3 : 0x14242e), 8);
      draw_icon(d,
                item.directory               ? Icon::Folder
                : kind == FileKind::subtitle ? Icon::Tracks
                                             : Icon::Play,
                {preview.x + preview.w * .5f, preview.y + preview.h * .5f},
                browser_view_ ? 46 : 30, accent);
    }
    if (browser_view_) {
      d.text(r.x + 14, r.y + 183, 21, clipped(d, item.name, r.w - 28, 21),
             white);
      d.text(r.x + 14, r.y + 214, 16,
             tr(item.directory               ? "Folder"
                : kind == FileKind::disc     ? "Disc image"
                : kind == FileKind::subtitle ? "Subtitles"
                                             : "Video"),
             muted);
    } else {
      d.text(r.x + 136, r.y + 17, 23, clipped(d, item.name, r.w - 270, 23),
             white);
      if (!item.directory) {
        char size[40];
        std::snprintf(size, sizeof(size),
                      item.size >= 1073741824 ? "%.1f GiB" : "%.1f MiB",
                      double(item.size) /
                          (item.size >= 1073741824 ? 1073741824 : 1048576));
        d.text(r.x + r.w - 121, r.y + 21, 18, size, muted);
      }
    }
    ui_.applicationWidget(1100 + row, r);
  }
  d.text(284, 858, 18, clipped(d, tr(browse_status_), 660, 18), muted);
  icon_button(d, 1013, {1036, 846, 176, 54}, Icon::Previous, "Previous", false,
              offset_ > 0, true);
  icon_button(d, 1014, {1224, 846, 184, 54}, Icon::Next, "Next", false,
              offset_ + count < int(entries_.size()), true);
}
void PlayerApplication::draw_settings(cyvr::DrawList &d) {
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
         tr("Cinema: translation and rotation. VR180/360: rotation and Zoom."),
         white);
  d.text(36, 880, 18,
         tr("Stable room space is used when available; tracking recovery may "
            "still need adjustment."),
         white);
}
bool PlayerApplication::draw_dock(cyvr::DrawList &d) {
  using cyvr::Color;
  d.rect(
      {0, 0, 480, 90},
      Color::hex(ui_.settings.theme == cyvr::Theme::Light ? 0xe4eef1 : 0x17272f,
                 .97f),
      28);
  auto control = [&](int id, float x, float w, Icon icon,
                     std::string_view label,
                     cyvr::Control type = cyvr::Control::Button) {
    const cyvr::Rect local{x, 9, w, 72};
    const bool hover = ui_.hovered() == id;
    if (hover)
      d.rect(local, Color::hex(0x344b56), 18);
    draw_icon(d, icon, {x + w * .5f, 45}, 30, Color::hex(0xf0f5f6));
    ui_.applicationWidget(
        id, {x + cyvr::dockOriginX, local.y + cyvr::dockOriginY, w, local.h},
        type);
    if (hover) {
      auto text = tr(label);
      d.text((480 - d.textWidth(14, text)) * .5f, 2, 14, text,
             Color::hex(0x86e7cf));
    }
  };
  control(600, 12, 108, Icon::Move, "Move controls", cyvr::Control::Drag);
  control(610, 128, 108,
          ui_.settings.follow == cyvr::FollowMode::World        ? Icon::Pin
          : ui_.settings.follow == cyvr::FollowMode::FaceViewer ? Icon::Info
                                                                : Icon::Headset,
          "Placement mode");
  if (ui_.windowState == cyvr::WindowState::Open) {
    control(611, 244, 108, Icon::Minimize, "Minimize");
    control(612, 360, 108, Icon::Close, "Close");
  } else
    control(613, 244, 224, Icon::Window, "Restore interface");
  return true;
}
} // namespace cyvr::player
