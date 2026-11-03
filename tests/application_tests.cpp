#include "cyvr/player/application.hpp"
#include "cyvr/player/locale.hpp"
#include "cyvr/player/video_view.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <optional>
#include "cyvr/player/subtitle_rasterizer.hpp"
#include <atomic>
using namespace cyvr;
using namespace cyvr::player;
bool controller(UiModel& ui,int hand,Vec2 stick,float trigger,float grip,Pose pose,bool valid,Vec2 pixels={600,500}){
  ControllerInput input;input.hand=hand;input.stick=stick;input.trigger=trigger;input.grip=grip;input.pose=pose;input.poseValid=valid;
  PanelGeometry panel;const Vec3 origin{0,1.45f,0};
  const auto target=panel.pose.position+Vec3{(pixels.x/1440-.5f)*panel.width,(.5f-pixels.y/990)*panel.height,0};
  input.rayOrigin=origin;input.rayDirection=normalized(target-origin);input.rayValid=valid;
  if(valid)input.pointer=intersectPanel(origin,input.rayDirection,panel);
  static int64_t time=1000000000;input.timeNs=time+=13888889;
  const bool consumed=ui.controllerInput(input);
  ui.pointer(hand,consumed?Vec2{-100,-100}:input.pointer.valid?input.pointer.pixels:Vec2{-100,-100},!consumed && trigger>.55f);
  return consumed;
}
static void require(bool passed, const char *message) {
  if (!passed)
    throw std::runtime_error(message);
}
void visible_region_tests() {
  const float fov[4]{-1.19f, .84f, .93f, -1.11f}; // A Quest 3 eye, tangents.
  const float pi = 3.14159265f;
  const auto yaw = [&](float angle) {
    return cyvr::Quat{0, std::sin(angle / 2), 0, std::cos(angle / 2)};
  };
  const auto pitch = [&](float angle) {
    return cyvr::Quat{std::sin(angle / 2), 0, 0, std::cos(angle / 2)};
  };
  const auto ahead = visible_panorama(fov, {0, 0, 0, 1}, 1, .3f);
  require(ahead.left > .05f && ahead.right < .95f && ahead.top > .05f && ahead.bottom < .95f,
          "A view straight ahead must leave most of a VR180 frame unconverted");
  require(ahead.left < .5f && ahead.right > .5f && ahead.top < .5f && ahead.bottom > .5f,
          "The view axis must stay inside the region");
  // Every edge ray of the view must land inside its region, for any head pose.
  for (const auto view : {cyvr::Quat{0, 0, 0, 1}, yaw(.7f), yaw(-.7f), pitch(.6f), pitch(-.6f),
                          multiply_rotation(yaw(.5f), pitch(.5f))}) {
    const auto region = visible_panorama(fov, view, 1, .05f);
    for (int y = 0; y <= 4; ++y)
      for (int x = 0; x <= 4; ++x) {
        const float dx = fov[0] + (fov[1] - fov[0]) * x / 4, dy = fov[2] + (fov[3] - fov[2]) * y / 4;
        const float cx = view.y * -1 - view.z * dy + view.w * dx, cy = view.z * dx + view.x + view.w * dy,
                    cz = view.x * dy - view.y * dx - view.w;
        const float rx = dx + 2 * (view.y * cz - view.z * cy), ry = dy + 2 * (view.z * cx - view.x * cz),
                    rz = -1 + 2 * (view.x * cy - view.y * cx);
        const float u = std::atan2(rx, -rz) / pi + .5f,
                    v = .5f - std::asin(ry / std::sqrt(rx * rx + ry * ry + rz * rz)) / pi;
        require(std::clamp(u, 0.f, 1.f) >= region.left && std::clamp(u, 0.f, 1.f) <= region.right &&
                    v >= region.top && v <= region.bottom,
                "Every visible ray must fall inside the converted region");
      }
  }
  const auto up = visible_panorama(fov, pitch(1.5f), 1, .3f);
  require(up.left == 0 && up.right == 1 && up.top == 0,
          "Looking at a pole sees every longitude");
  const auto behind = visible_panorama(fov, yaw(pi), 2, .3f);
  require(behind.left == 0 && behind.right == 1, "The 360 seam needs the full width");
  const auto side = visible_panorama(fov, yaw(pi / 2), 2, .3f);
  require(side.right - side.left < .6f, "A 360 view away from the seam stays partial");
  require(ahead.contains(visible_panorama(fov, {0, 0, 0, 1}, 1, .08f)),
          "The widened region must contain the tighter redraw check");
  const float tangents[4]{-1.19f, .84f, .93f, -1.11f};
  const auto region = view_filter_region(tangents, {1680, 1760});
  require(region.x > 840 && region.x < 1100 && region.y > 700 && region.y < 900,
          "The filter region must follow the asymmetric lens axis");
}
int main() {
  try {
    visible_region_tests();
    {
      ImageSettings native,filtered;filtered.quality=3;
      require(wants_double_rate(true,60,native) && wants_double_rate(true,59.94,native),"60 fps video must double the display rate");
      require(!wants_double_rate(false,60,native),"120 Hz must stay opt-in");
      require(!wants_double_rate(true,24,native) && !wants_double_rate(true,30,native),"24 and 30 fps must keep the saved rate");
      require(!wants_double_rate(true,60,filtered),"Per-eye filters must keep the saved rate");
      const auto window=window_extent({7680,3840},1600,1100);
      require(window.width==3200 && window.height==1600,"An 8K frame must convert at twice the window");
      const auto small=window_extent({1920,960},1600,1100);
      require(small.width==1920 && small.height==960,"A window must never enlarge the source");
    }
    const auto root =
        std::filesystem::temp_directory_path() /
        ("cyvr-application-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "Videos");
    std::ofstream(root / "Videos" / "broken.mp4") << "invalid media fixture";
    // Original 1x1 two-frame GIF: two one-second frames, no external encoder or
    // assets.
    const unsigned char gif[]{
        'G', 'I', 'F', '8', '9', 'a',  1,    0,    1,    0,    0x80,
        0,   0,   0,   0,   0,   255,  255,  255,  0x21, 0xf9, 4,
        0,   100, 0,   0,   0,   0x2c, 0,    0,    0,    0,    1,
        0,   1,   0,   0,   2,   2,    0x44, 1,    0,    0x21, 0xf9,
        4,   0,   100, 0,   0,   0,    0x2c, 0,    0,    0,    0,
        1,   0,   1,   0,   0,   2,    2,    0x4c, 1,    0,    0x3b};
    std::ofstream(root / "timeline.gif", std::ios::binary)
        .write(reinterpret_cast<const char *>(gif), sizeof(gif));
    std::ofstream(root / "Videos" / "preview.mp4", std::ios::binary)
        .write(reinterpret_cast<const char *>(gif), sizeof(gif));
    {
      UiModel model;
      Platform platform;
      platform.root = root.u8string();
      platform.state_file = (root / "state.bin").u8string();
      PlayerApplication app(model, platform);
      DrawList draw;
      auto click = [&](int id) {
        model.drawMain(draw);
        for (const auto &widget : model.widgets())
          if (widget.id == id) {
            const Vec2 point{widget.rect.x + widget.rect.w * .5f,
                             widget.rect.y + widget.rect.h * .5f};
            model.pointer(0, point, true);
            model.pointer(0, point, false);
            return;
          }
        throw std::runtime_error("Missing interactive widget: " +
                                 std::to_string(id));
      };
      auto wait = [&](auto condition) {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
          app.tick();
          model.drawMain(draw);
          if (condition())
            return;
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("Asynchronous application update timed out");
      };
      wait([&] {
        for (const auto &w : model.widgets())
          if (w.id == 1100)
            return true;
        return false;
      });
      click(1100);
      if (std::filesystem::u8path(app.location()).filename() != "Videos")
        throw std::runtime_error("Folder navigation failed");
      wait([&] {
        for (const auto &w : model.widgets())
          if (w.id == 1100)
            return true;
        return false;
      });
      std::vector<std::uint8_t> thumbnails;
      wait([&] {
        return app.take_thumbnails(thumbnails) &&
               thumbnails.size() == ui_atlas_width * ui_atlas_height * 4 &&
               thumbnails[(160 * 90 + 1) * 4 + 3] == 255;
      });
      click(1100);
      wait([&] {
        auto s = app.session().state();
        return !s.ready && s.status != "Choose a video." &&
               s.status != "Opening media...";
      });
      if (app.session().state().title != "broken.mp4")
        throw std::runtime_error("Row did not open the selected media");
      for (const auto &w : model.widgets())
        if (w.id == 1025 && w.enabled)
          throw std::runtime_error("Timeline enabled for unavailable media");
      click(1334);
      DrawList measured;
      model.drawDebug(measured);
      if (!model.debugVisible || measured.vertices.empty() ||
          !model.renderApplicationDebug)
        throw std::runtime_error("Player performance HUD was not connected");
      click(1334);
      if (model.debugVisible)
        throw std::runtime_error("Performance button did not close the HUD");
      click(1003);
      click(1302);
      model.drawMain(draw);
      int quality_controls=0;
      for (const auto &w : model.widgets())
        if ((w.id >= 1310 && w.id <= 1313) || (w.id >= 1320 && w.id <= 1323)) {
          ++quality_controls;
          if(!w.enabled)throw std::runtime_error("A quality/display-target control was disabled");
        }
      if(quality_controls!=8)throw std::runtime_error("A quality/display-target control is missing");
      for(const char* language:{"en","fr"}){
        model.setLanguage(language);model.drawMain(draw);
        for(int id:{1350,1351,1352,1353,1348,1349}){
          bool found=false;
          for(const auto& w:model.widgets())if(w.id==id){
            found=true;if(!w.enabled || w.rect.y<304 || w.rect.y+w.rect.h>929)
              throw std::runtime_error("Spatial filter control disabled or outside content bounds");
          }
          if(!found)throw std::runtime_error("Spatial filter control missing");
        }
        click(1352);
        model.handleApplicationSlider(1348,.65f,SliderPhase::Commit);
        model.handleApplicationSlider(1349,.8f,SliderPhase::Commit);
        if(app.image.sampling!=2 || std::abs(app.image.denoise-.65f)>1e-5 || std::abs(app.image.detail-.8f)>1e-5 || !app.image.reconstruct_view())
          throw std::runtime_error("Spatial filter interactions failed");
        click(1353);
        if(app.image.reconstruct_view())throw std::runtime_error("Disabling spatial filters did not restore native path");
      }
      model.setLanguage("en");
      click(1300);
      click(1031);
      click(1042);
      if (app.stereo != 1 || app.projection != 2)
        throw std::runtime_error("Stereo/projection selection failed");
      click(1000);
      click(1010);
      if (std::filesystem::u8path(app.location()) != root)
        throw std::runtime_error("Parent navigation failed");
      click(1002);
      click(1255);
      click(1210);
      click(1253);
      click(1200); // uppercase and symbol keyboard pages
      app.session().stop();
      wait([&] { return app.session().state().title.empty(); });
      click(1002);
      app.text('x');
      app.action(1024);
      if (model.interfaceHidden)
        throw std::runtime_error("Hide enabled without a playable video");
      for (auto presentation :
           {Presentation::Panel2D, Presentation::Immersive}) {
        model.presentation = presentation;
        model.setInterfaceHidden(true);
        model.drawMain(draw);
        DrawList dock;
        model.drawDock(dock);
        if (!model.widgets().empty() || !dock.vertices.empty())
          throw std::runtime_error("Cinema mode retained interactive chrome");
        model.pointer(0, {520, 125}, true);
        if (model.interfaceHidden)
          throw std::runtime_error("Pointer did not restore interface");
        model.drawMain(draw);
        model.pointer(0, {520, 125}, true);
        model.pointer(0, {520, 125}, false);
        if (app.page() != 2)
          throw std::runtime_error(
              "Wake gesture clicked through onto navigation");
        model.setInterfaceHidden(true);
        if (!app.back() || model.interfaceHidden || model.panelRequested ||
            app.page() != 2)
          throw std::runtime_error(
              "First Back did not exclusively reveal controls");
      }
      model.presentation = Presentation::Panel2D;
      app.save();
      const auto saved = load_application_state(platform.state_file);
      if (!saved || saved->page != 2 || saved->stereo != 1 ||
          saved->projection != 2 || saved->url.empty())
        throw std::runtime_error("Application navigation was not persisted");
      {
        UiModel second;
        PlayerApplication restored(second, platform);
        if (!restored.restore(false) || restored.snapshot().url != saved->url ||
            restored.page() != 2 || restored.stereo != 1)
          throw std::runtime_error(
              "Activity replacement lost application state");
        second.navigate(Page(3));
        restored.tick();
        if (restored.snapshot().page != 3)
          throw std::runtime_error("Shared dock navigation was not retained");
        second.presentation = Presentation::Immersive;
        if (!restored.back() || !second.panelRequested)
          throw std::runtime_error("Immersive Back did not request the panel");
      }
      if (player_translate("en", "Library") != "Library" ||
          player_translate("fr", "Library") == "Library")
        throw std::runtime_error(
            "English/French application translations failed");
      auto state = *saved;
      state.position_us = 1234567;
      state.playing = false;
      state.source = "content://test/document/video";
      state.parents = {root.u8string()};
      state.image.exposure = 1;
      state.image.quality = 2;
      state.image.zoom = 2.25f;
      state.image.sampling=2;state.image.denoise=.65f;state.image.detail=.8f;
      state.auto_format=true;state.smooth_video=true;
      state.display_tab = 2;
      state.tracks={3,5,8,"content://test/document/captions"};
      state.tracks.clearkey_file="content://test/document/keys.jwk";
      state.tracks.disc_title=7;state.tracks.disc_angle=1;
      state.tracks_open=true;state.track_tab=2;state.track_offset=7;
      state.cinema={};state.cinema.environment=6;state.cinema.width=7.5f;state.cinema.curvature=75;state.cinema.distance=5;state.cinema.pitch=20;state.cinema.ambient=.15f;state.cinema_tab=2;state.ambilight.enabled=true;state.ambilight.intensity=1.4f;state.ambilight.smoothing=.75f;
      if (!save_application_state(platform.state_file, state))
        throw std::runtime_error("State update failed");
      auto loaded = load_application_state(platform.state_file);
      if(!loaded || loaded->image!=state.image)throw std::runtime_error("Spatial image settings lost during checkpoint transfer");
      if(!loaded->smooth_video)throw std::runtime_error("120 Hz preference lost during checkpoint transfer");
      {
        std::ifstream f(platform.state_file,std::ios::binary);std::string bytes(std::istreambuf_iterator<char>(f),{});f.close();
        const auto tag=bytes.find(std::string("1TLF\0\0\0\0",8));if(tag==std::string::npos)throw std::runtime_error("Spatial image extension missing");bytes.resize(tag);
        std::ofstream old(platform.state_file,std::ios::binary|std::ios::trunc);old.write(bytes.data(),bytes.size());old.close();
        const auto migrated=load_application_state(platform.state_file);
        auto expected=state.image;expected.sampling=0;expected.denoise=expected.detail=0;
        if(!migrated || migrated->image!=expected || migrated->auto_format!=state.auto_format || migrated->tracks.disc_title!=state.tracks.disc_title)
          throw std::runtime_error("Previous checkpoint migration altered existing settings");
        if(!save_application_state(platform.state_file,state))throw std::runtime_error("Spatial checkpoint restore failed");
      }
      if(!loaded || !loaded->auto_format)throw std::runtime_error("Auto-detection preference lost during state transfer");
      {
        std::ifstream f(platform.state_file,std::ios::binary);std::string bytes(std::istreambuf_iterator<char>(f),{});f.close();
        const auto tag=bytes.find(std::string("1TUA\0\0\0\0",8));if(tag==std::string::npos)throw std::runtime_error("Detection preference extension missing");bytes.resize(tag);
        std::ofstream old(platform.state_file,std::ios::binary|std::ios::trunc);old.write(bytes.data(),bytes.size());old.close();
        const auto migrated=load_application_state(platform.state_file);
        if(!migrated || migrated->auto_format || migrated->stereo!=state.stereo || migrated->projection!=state.projection || migrated->tracks.disc_title!=state.tracks.disc_title)throw std::runtime_error("Legacy detection migration changed manual layout or disc selection");
        if(!save_application_state(platform.state_file,state))throw std::runtime_error("Detection checkpoint restore failed");
      }
      if(!loaded || loaded->tracks.disc_title!=7 || loaded->tracks.disc_angle!=1)throw std::runtime_error("Disc selection lost during activity transfer");
      if(!loaded || loaded->cinema!=state.cinema || loaded->cinema_tab!=2 || loaded->ambilight!=state.ambilight)throw std::runtime_error("Cinema settings were not retained across activity replacement");
      if (!loaded || loaded->source != state.source ||
          loaded->position_us != 1234567 || loaded->playing ||
          loaded->parents != state.parents || loaded->image != state.image ||
          loaded->display_tab != 2 || loaded->tracks.video!=3 || loaded->tracks.audio!=5 || loaded->tracks.subtitle!=8 || loaded->tracks.external_subtitle!=state.tracks.external_subtitle || loaded->tracks.clearkey_file!=state.tracks.clearkey_file || !loaded->tracks_open || loaded->track_tab!=2 || loaded->track_offset!=7)
        throw std::runtime_error(
            "Document or paused position lost during state transfer");
      // A checkpoint from the installed pre-Ambilight cinema build retains all
      // screen settings and defaults the new optional effect to off.
      {
        std::ifstream f(platform.state_file,std::ios::binary);std::string bytes(std::istreambuf_iterator<char>(f),{});f.close();
        const auto tag=bytes.find(std::string("1BMA\0\0\0\0",8));if(tag==std::string::npos)throw std::runtime_error("Ambilight extension missing");bytes.resize(tag);
        std::ofstream old(platform.state_file,std::ios::binary|std::ios::trunc);old.write(bytes.data(),bytes.size());old.close();
        const auto cinemaOnly=load_application_state(platform.state_file);if(!cinemaOnly || cinemaOnly->cinema!=state.cinema || cinemaOnly->ambilight!=AmbilightSettings{})throw std::runtime_error("Pre-Ambilight checkpoint migration lost cinema settings");
        if(!save_application_state(platform.state_file,state))throw std::runtime_error("Checkpoint restore failed");
      }
      // Previous app versions wrote the same base record without the optional
      // extension.
      {
        std::ifstream f(platform.state_file, std::ios::binary);
        std::string bytes(std::istreambuf_iterator<char>(f), {});
        const auto extension=bytes.find(std::string("2GMI\0\0\0\0",8));
        if(extension==std::string::npos)throw std::runtime_error("Image extension missing from state fixture");
        bytes.resize(extension);
        f.close();
        std::ofstream old(platform.state_file,
                          std::ios::binary | std::ios::trunc);
        old.write(bytes.data(), bytes.size());
      }
      loaded = load_application_state(platform.state_file);
      if (!loaded || loaded->source != state.source ||
          loaded->image != ImageSettings{})
        throw std::runtime_error(
            "Legacy state migration lost source or neutral defaults");
      if(loaded->cinema!=CinemaSettings{})throw std::runtime_error("Legacy checkpoints did not receive neutral screen/default cinema settings");
      state.url = std::string(300000, 'x');
      if (save_application_state(platform.state_file, state) ||
          !load_application_state(platform.state_file))
        throw std::runtime_error("Oversized state replaced the valid state");
      // The preference is opt-in, and opening a media cannot enable it.
      if(app.snapshot().auto_format)throw std::runtime_error("Auto-detection was enabled by default");
      app.action(1032);app.action(1042);
      app.open((root / "timeline.gif").u8string(),"Fixture_180_LR.gif");
      wait([&]{return app.session().state().ready && app.session().state().title=="Fixture_180_LR.gif";});app.tick();
      if(app.stereo!=2 || app.projection!=2 || app.snapshot().auto_format)throw std::runtime_error("Disabled detection changed manual format on open/probe");
      app.action(1053);
      if(app.stereo!=1 || app.projection!=1 || app.snapshot().auto_format)throw std::runtime_error("One-shot detection enabled the preference");
      app.action(1032);app.action(1042);app.save();
      if(!app.restore())throw std::runtime_error("Disabled detection restore failed");
      wait([&]{return app.session().state().ready;});app.tick();
      if(app.snapshot().auto_format || app.stereo!=2 || app.projection!=2)throw std::runtime_error("Disabled preference or manual format lost across hosts");
      app.action(1054);
      if(!app.snapshot().auto_format || app.stereo!=1 || app.projection!=1)throw std::runtime_error("Enabling detection did not apply the current format");
      // Opt-in auto mode still permits per-file manual choices.
      app.open((root / "timeline.gif").u8string(),"Fixture_180_LR.gif");
      wait([&]{return app.session().state().ready && app.session().state().title=="Fixture_180_LR.gif";});app.tick();
      if(app.stereo!=1 || app.projection!=1 || app.snapshot().format_manual)throw std::runtime_error("Filename layout was not applied at open");
      app.action(1032);app.action(1042);app.tick();app.save();
      if(!app.snapshot().format_manual || app.stereo!=2 || app.projection!=2)throw std::runtime_error("Manual format was not retained");
      if(!app.restore())throw std::runtime_error("Format checkpoint restore failed");
      wait([&]{return app.session().state().ready;});app.tick();
      if(app.stereo!=2 || app.projection!=2 || !app.snapshot().format_manual)throw std::runtime_error("Host replacement discarded manual format");
      app.action(1053);if(app.stereo!=1 || app.projection!=1 || app.snapshot().format_manual)throw std::runtime_error("Explicit format re-detection failed");
      app.open((root / "timeline.gif").u8string());
      wait([&] { return app.session().state().ready; });
      app.tick();if(app.stereo || app.projection || app.half_stereo || app.swap_eyes)throw std::runtime_error("New ordinary video retained the previous spherical/stereo format");
      app.action(1054);app.action(1032);app.action(1041);app.action(1051);app.action(1052);
      app.open((root / "timeline.gif").u8string(),"Other_360_LR.gif");
      wait([&]{return app.session().state().ready && app.session().state().title=="Other_360_LR.gif";});app.tick();
      if(app.snapshot().auto_format || app.stereo!=2 || app.projection!=1 || !app.half_stereo || !app.swap_eyes)throw std::runtime_error("Disabled detection did not retain the complete manual layout for the next file");
      app.action(1030);app.action(1040);
      const auto duration = app.session().state().duration_us;
      if (duration != 2000000 || !app.session().state().seekable)
        throw std::runtime_error("Timeline fixture metadata is invalid");
      model.drawMain(draw);
      for(const auto& a:model.widgets()){
        if(a.id==1024)throw std::runtime_error("Playback still exposes Hide interface");
        if(!a.enabled)continue;
        for(const auto& b:model.widgets())if(b.enabled && a.id<b.id &&
          a.rect.x<b.rect.x+b.rect.w && b.rect.x<a.rect.x+a.rect.w &&
          a.rect.y<b.rect.y+b.rect.h && b.rect.y<a.rect.y+a.rect.h)
          throw std::runtime_error("Playback controls overlap: "+std::to_string(a.id)+" / "+std::to_string(b.id));
        if(a.rect.y>=750 && a.rect.y+a.rect.h>929)throw std::runtime_error("Playback control crosses its footer");
      }
      model.presentation=Presentation::Immersive;app.tick();model.drawMain(draw);
      if(!model.compactInterface)throw std::runtime_error("Immersive video did not open compact controls");
      auto controllerClick=[&](int id,unsigned hand){
        model.drawMain(draw);
        auto widget=std::find_if(model.widgets().begin(),model.widgets().end(),[&](const auto& w){return w.id==id && w.enabled;});
        if(widget==model.widgets().end())throw std::runtime_error("Missing immersive control");
        const Vec2 pixels{widget->rect.x+widget->rect.w*.5f,widget->rect.y+widget->rect.h*.5f};
        PanelGeometry panel;
        const Vec3 origin{0,1.45f,0};
        const Vec3 target=panel.pose.position+Vec3{(pixels.x/panel.pixelsW-.5f)*panel.width,(.5f-pixels.y/panel.pixelsH)*panel.height,0};
        auto route=[&](bool down){
          // Use the XR host's surface classifier after capture, then the real
          // panel ray intersection and UiModel press/hold/release dispatcher.
          const auto hit=isDockWidget(model.captured(hand))?Hit{}:intersectPanel(origin,target-origin,panel);
          model.pointer(hand,hit.valid?hit.pixels:Vec2{-100,-100},down);
        };
        route(true);app.tick();model.drawMain(draw);route(true);
        if(model.captured(hand)!=id)throw std::runtime_error("Controller capture left its application control");
        route(false);app.tick();model.drawMain(draw);
      };
      for(unsigned hand:{0u,1u}){
        controllerClick(1360,hand);
        if(model.compactInterface || model.interfaceHidden || app.page()!=1)throw std::runtime_error("Full interface click was lost on trigger release");
        if(hand==0){
          DrawList dock;
          model.drawDock(dock);
          require(!model.renderApplicationDock,"Player still overrides the foundation window bar");
          require(std::any_of(model.widgets().begin(),model.widgets().end(),[](const auto& w){return w.id==600;}),"Minimal window handle missing");
          require(std::none_of(model.widgets().begin(),model.widgets().end(),[](const auto& w){return w.id==611 || w.id==612;}),"Hidden dock buttons are interactive before hover");
          const auto original=draw.vertices.size();
          model.setWindowFeedback(EdgeRight|EdgeBottom,true,true);model.drawMain(draw);
          require(draw.vertices.size()>original,"Custom player view did not draw window resize feedback");
          model.setWindowFeedback(0,false,false);
        }
        controllerClick(1361,hand);
        if(model.windowState!=WindowState::Minimized || model.compactInterface)throw std::runtime_error("Minimize must retain the full interface mode");
        auto dockClick=[&](int id){
          DrawList dock;
          model.settings.reducedMotion=true;
          model.updateDockPointer(hand,{720,1058});
          model.tick(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
          model.drawDock(dock);
          auto w=std::find_if(model.widgets().begin(),model.widgets().end(),[&](const auto& w){return w.id==id;});
          if(w==model.widgets().end())throw std::runtime_error("Dock control missing after hover reveal");
          Vec2 point{w->rect.x+w->rect.w/2,w->rect.y+w->rect.h/2};
          model.pointer(hand,point,true);model.pointer(hand,point,false);app.tick();model.drawMain(draw);
        };
        dockClick(613);if(model.windowState!=WindowState::Open)throw std::runtime_error("Restore did not reopen full controls");
        controllerClick(1064,hand);app.tick();model.drawMain(draw);
        if(!model.compactInterface || model.interfaceHidden || model.windowState!=WindowState::Open)throw std::runtime_error("Close must return to compact playback");
        auto handle=std::find_if(model.widgets().begin(),model.widgets().end(),[](const auto& w){return w.id==605 && w.type==Control::Drag;});
        if(handle==model.widgets().end() || isDockWidget(605))throw std::runtime_error("Compact drag handle must use the main surface");
        app.action(1024);auto before=app.image.zoom;
        for(int n=0;n<3;++n)controller(model,hand,{},0,0,{{float(n),2,0},{}},true);
        app.tick();if(!model.interfaceHidden)throw std::runtime_error("Controller movement woke hidden controls");
        controller(model,hand,{},.2f,0,{},true);app.tick();if(!model.interfaceHidden)throw std::runtime_error("A light trigger touch woke hidden controls");
        controller(model,hand,{},.8f,0,{},true);app.tick();if(model.interfaceHidden || !model.compactInterface || app.image.zoom!=before)throw std::runtime_error("A real trigger press did not restore compact controls safely");
        controller(model,hand,{},0,0,{},true);
      }
      controllerClick(1662,0);
      if(model.compactInterface || app.page()!=3 || app.snapshot().display_tab!=3)throw std::runtime_error("Compact cinema shortcut did not open its settings");
      click(1661);click(1611);app.tick();
      if(model.settings.background!=Background::Dark)throw std::runtime_error("Cinema background did not reach the shared immersive host");
      click(1660);model.handleApplicationSlider(1640,.5f,SliderPhase::Commit);model.handleApplicationSlider(1641,.25f,SliderPhase::Commit);model.handleApplicationSlider(1642,.5f,SliderPhase::Commit);
      if(app.cinema.width!=6.5f || app.cinema.distance!=3.75f || app.cinema.curvature!=60)throw std::runtime_error("Cinema geometry sliders did not apply");
      click(1663);click(1670);model.handleApplicationSlider(1648,.8f,SliderPhase::Commit);model.handleApplicationSlider(1649,.4f,SliderPhase::Commit);model.handleApplicationSlider(1650,.5f,SliderPhase::Commit);
      if(!app.ambilight.enabled || app.ambilight.intensity!=1.6f || app.ambilight.smoothing!=.75f || app.snapshot().cinema_tab!=2)throw std::runtime_error("Ambilight controls did not apply");
      click(1672);if(app.ambilight!=AmbilightSettings{})throw std::runtime_error("Ambilight reset failed");
      AmbilightSettings invalid;invalid.intensity=NAN;invalid.spread=100;invalid.smoothing=-1;invalid.sanitize();if(invalid.intensity!=.7f || invalid.spread!=.6f || invalid.smoothing!=0)throw std::runtime_error("Ambilight settings bounds failed");
      app.cinema={};app.action(1001);
      {
        bool target=false;app.video_manipulation=[&](const ControllerInput&,bool allowed){target=allowed;return false;};
        controller(model,1,{1,0},0,0,{},true,{600,500});
        if(target)throw std::runtime_error("Pointing at full UI enabled a movie grab or view rotation");
        controller(model,1,{1,0},0,0,{},true,{-30,-30});
        if(!target)throw std::runtime_error("Pointing outside UI did not reach spatial movie controls");
        app.video_manipulation=[&](const ControllerInput&,bool allowed){target=allowed;return true;};
        app.action(1024);target=false;
        controller(model,1,{1,0},0,0,{},true);
        if(!target || !model.interfaceHidden)throw std::runtime_error("Hidden movie view could not turn without waking the controls");
        app.show_interface();
        app.video_manipulation={};
      }
      if(!isDockWidget(600) || !isDockWidget(611) || isDockWidget(605))throw std::runtime_error("Dock routing broke the dock or main drag handle");
      model.presentation=Presentation::Panel2D;app.tick();
      model.setInterfaceHidden(true);
      const auto zoomBeforeWake=app.image.zoom;
      if (!controller(model,1, {}, .8f, .8f, {{0, .5f, 0}, {}},
                                       true))
        throw std::runtime_error("Grip+trigger was not consumed");
      controller(model,1, {}, .8f, .8f, {{0, .75f, 0}, {}}, true);
      if(app.image.zoom!=zoomBeforeWake || model.interfaceHidden)throw std::runtime_error("The waking gesture must only restore the interface");
      controller(model,1, {}, 0, 0, {}, true);
      controller(model,1, {}, .8f, .8f, {{0,.5f,0},{}},true);
      controller(model,1, {}, .8f, .8f, {{0,.75f,0},{}},true);
      if (std::abs(app.image.zoom-std::min(4.f,zoomBeforeWake+1)) > 1e-5)
        throw std::runtime_error("Controller zoom failed after the waking gesture");
      controller(model,1, {}, 0, 0, {}, false);
      app.show_interface();
      app.action(1003);
      app.action(1301);
      model.drawMain(draw);
      model.pointer(0, {470 + 934 * .75f, 354}, true);
      model.pointer(0, {470 + 934 * .75f, 354}, false);
      if (std::abs(app.image.exposure - 1) > 1e-4)
        throw std::runtime_error("Image correction slider failed");
      app.video_input(0, 100, 100, -100);
      if (app.image.zoom != 4 || app.image.pan_x != 1 || app.image.pan_y != -1)
        throw std::runtime_error("Image transform bounds failed");
      app.action(1041);app.action(1333);app.video_input(0,-100);
      if(app.image.zoom!=.25f)throw std::runtime_error("VR180 button/stick zoom-out limit failed");
      model.handleApplicationSlider(1344,0,SliderPhase::Commit);
      if(app.image.zoom!=.25f)throw std::runtime_error("VR180 slider zoom-out limit failed");
      app.action(1333);
      controller(model,1,{},.8f,.8f,{{0,.5f,0},{}},true);
      controller(model,1,{},.8f,.8f,{{0,.1f,0},{}},true);
      controller(model,1,{},0,0,{},false);
      if(app.image.zoom!=.25f)throw std::runtime_error("VR180 grip/trigger zoom-out limit failed");
      app.save();const auto zoomState=load_application_state(platform.state_file);
      if(!zoomState || zoomState->image.zoom!=.25f || zoomState->auto_format)throw std::runtime_error("VR180 zoom or disabled detection lost in checkpoint");
      app.action(1040);if(app.image.zoom!=1)throw std::runtime_error("Leaving VR180 retained an invalid cinema zoom");
      app.action(1330);
      app.action(1001);
      auto drag = [&](float from, float to, bool cancel = false) {
        model.drawMain(draw);
        auto it = std::find_if(model.widgets().begin(), model.widgets().end(),
                               [](const auto &w) { return w.id == 1025; });
        if (it == model.widgets().end() || !it->enabled)
          throw std::runtime_error("Playable timeline is not interactive");
        const auto rect = it->rect;
        const auto old = app.session().state().position_us;
        model.pointer(0, {rect.x + rect.w * from, rect.y + rect.h * .5f}, true);
        model.drawMain(draw);
        model.pointer(0, {rect.x + rect.w * to, rect.y + rect.h * .5f}, true);
        if (app.session().state().position_us != old)
          throw std::runtime_error(
              "Dragging restarted decoding before release");
        model.drawMain(draw);
        model.pointer(0,
                      cancel
                          ? Vec2{-100, -100}
                          : Vec2{rect.x + rect.w * to, rect.y + rect.h * .5f},
                      false);
      };
      app.session().pause(true);
      drag(.2f, .75f);
      if (std::abs(app.session().state().position_us - 1499999) > 1 ||
          app.session().state().playing)
        throw std::runtime_error("Release did not seek while preserving pause");
      drag(.3f, .8f, true);
      if (std::abs(app.session().state().position_us - 1499999) > 1)
        throw std::runtime_error("Cancelled drag sought the video");
      model.drawMain(draw);
      model.pointer(0, {700, 784}, true);
      app.action(1000);
      app.action(1001);
      model.drawMain(draw);
      model.pointer(0, {1200, 784}, true);
      model.pointer(0, {1200, 784}, false);
      if (std::abs(app.session().state().position_us - 1499999) > 1)
        throw std::runtime_error("Navigation reactivated a cancelled drag");
      drag(.5f, 1.2f);
      if (app.session().state().position_us != duration - 1)
        throw std::runtime_error(
            "Drag beyond the bar did not clamp to the end");
      drag(.5f, -.2f);
      if (app.session().state().position_us != 0)
        throw std::runtime_error(
            "Drag before the bar did not clamp to the beginning");
      app.session().pause(false);
      drag(.2f, .5f);
      if (!app.session().state().playing)
        throw std::runtime_error("Seek unexpectedly paused playback");
      model.drawMain(draw);
      model.pointer(0, {700, 784}, true);
      model.setInterfaceHidden(true);
      model.drawMain(draw);
      model.pointer(0, {900, 784}, true);
      model.drawMain(draw);
      model.pointer(0, {900, 784}, false);
      if (std::abs(app.session().state().position_us - 999999) > 1)
        throw std::runtime_error("Cancelled scrub or cinema wake gesture "
                                 "accidentally sought the video");
      app.action(1023);
    }
    {
      UiModel ui;Platform platform;platform.root="content://test/library";platform.state_file=(root/"workspace.bin").u8string();
      platform.locations=[] {return std::vector<BrowserItem>{{"content://test/library","Storage",true,0},{"content://test/usb","USB",true,0}};};
      platform.documents=[](const std::string& uri){std::vector<BrowserItem> items;for(int n=0;n<13;++n)items.push_back({uri+"/video"+std::to_string(n),"Video "+std::to_string(n)+".mp4",false,1000});return items;};
      std::atomic<int> calls{};
      platform.thumbnail=[&](const std::string&){if(calls.fetch_add(1)<7)return std::vector<std::uint8_t>{};std::vector<std::uint8_t> pixels(160*90*4,255);return pixels;};
      {
        PlayerApplication app(ui,platform);DrawList draw;app.action(1015);
        auto wait=[&](auto condition){const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(7);while(std::chrono::steady_clock::now()<until){app.tick();ui.drawMain(draw);if(condition())return;std::this_thread::sleep_for(std::chrono::milliseconds(5));}throw std::runtime_error("Workspace update timed out");};
        auto exists=[&](int id){for(const auto& w:ui.widgets())if(w.id==id)return true;return false;};
        wait([&]{return exists(1106);});
        std::vector<std::uint8_t> atlas;
        wait([&]{if(!app.take_thumbnails(atlas))return false;for(int n=0;n<7;++n)if(atlas[(n*160*90)*4+3]!=255)return false;return true;});
        if(calls.load()<8)throw std::runtime_error("Transient thumbnail failures were not retried");
        app.action(1017);app.action(1016);app.tick();ui.drawMain(draw);
        if(!app.take_thumbnails(atlas) || atlas[3]!=255 || atlas[(5*160*90)*4+3]!=255)throw std::runtime_error("Visible previews blinked away when the tile layout changed");
        if(!exists(1105) || exists(1106) || exists(1013) || exists(1014))throw std::runtime_error("Tiles must show visible rows without pagination buttons");
        auto scroll=[&](int hand,float axis,int frames){for(int n=0;n<frames;++n){std::this_thread::sleep_for(std::chrono::milliseconds(5));controller(ui,hand,{0,axis},0,0,{},true);app.tick();ui.drawMain(draw);}};
        const auto before_miss=app.snapshot().browser_scroll;
        controller(ui,1,{0,-1},0,0,{},true,{100,200});
        if(app.snapshot().browser_scroll!=before_miss)throw std::runtime_error("Joystick scrolled without pointing inside the library");
        if(controller(ui,1,{0,-1},.2f,0,{},true))throw std::runtime_error("Scroll consumed the pointer instead of retaining hover");
        if(app.snapshot().browser_scroll<=before_miss || ui.hovered()<1100)throw std::runtime_error("Light trigger contact or normal ray dispatch blocked scrolling");
        ControllerInput fallback;fallback.hand=1;fallback.stick={0,-1};fallback.pointer={true,{600,500},{},1};fallback.rayValid=true;
        const auto before_fallback=app.snapshot().browser_scroll;ui.controllerInput(fallback);
        if(app.snapshot().browser_scroll<=before_fallback)throw std::runtime_error("Hand-ray hover required an active controller aim pose to scroll");
        scroll(1,-1,3);
        if(app.snapshot().browser_scroll<=0 || app.snapshot().offset!=0 || !exists(1108))throw std::runtime_error("Tile scrolling did not expose a clipped third row continuously");
        const auto before_wheel=app.snapshot().browser_scroll;
        if(!ui.scroll({600,500},{0,18}) || app.snapshot().browser_scroll!=before_wheel+18)throw std::runtime_error("2D Quest/system scroll did not reach the library");
        if(ui.scroll({100,200},{0,18}) || app.snapshot().browser_scroll!=before_wheel+18)throw std::runtime_error("Scroll outside the file grid changed library position");
        ui.modalOpen=true;if(ui.scroll({600,500},{0,18}))throw std::runtime_error("Modal interaction leaked scroll to the files");ui.modalOpen=false;
        scroll(0,-1,65);
        if(app.snapshot().offset<3)throw std::runtime_error("Left joystick did not advance the tile rows");
        for(const auto& w:ui.widgets())if(w.id>=1100 && w.id<1100+int(preview_slots) && (w.rect.y<304 || w.rect.y+w.rect.h>804))throw std::runtime_error("Offscreen tile remains interactive");
        const auto held=app.snapshot().browser_scroll;
        ui.pointer(0,{400,600},true);scroll(1,-1,3);
        if(app.snapshot().browser_scroll!=held)throw std::runtime_error("Scrolling replaced a file under pointer capture");
        ui.pointer(0,{-100,-100},false);
        ui.pointer(0,{1390,780},true);ui.pointer(0,{1390,2000},true);ui.pointer(0,{1390,2000},false);ui.drawMain(draw);
        if(app.snapshot().offset!=9)throw std::runtime_error("Vertical scrollbar could not reach the final tile row");
        ui.pointer(0,{1390,310},true);ui.pointer(0,{1390,-1000},true);ui.pointer(0,{1390,-1000},false);
        if(app.snapshot().offset!=0 || app.snapshot().browser_scroll!=0)throw std::runtime_error("Scrollbar did not clamp to the beginning");
        app.action(1015);ui.drawMain(draw);if(!exists(1106))throw std::runtime_error("Scrolling list must expose seven initial items");
        app.open((root/"timeline.gif").u8string());wait([&]{return app.session().state().ready;});app.session().pause(false);
        app.action(1000);wait([&]{app.take_thumbnails(atlas);return !atlas.empty() && atlas[3]==255;});
        if(!app.session().state().playing)throw std::runtime_error("Browser preview test did not run during playback");
        app.session().pause(true);app.tick();const auto position=app.session().state().position_us;const auto zoom=app.image.zoom;
        scroll(1,-1,3);controller(ui,0,{1,0},0,0,{},true);
        if(app.image.zoom!=zoom || app.session().state().position_us!=position)throw std::runtime_error("Browser joystick altered movie playback or zoom");
        app.action(1004);ui.drawMain(draw);if(!exists(1060)||!exists(1061)||!exists(1371))throw std::runtime_error("Language/theme/volume settings missing");
        app.action(1900);ui.drawMain(draw);
        for(int id:{1901,1902,1903,1904,1905,1910,1911,1912})
          if(!exists(id))throw std::runtime_error("Immersive window setting is missing");
        for(const char* language:{"en","fr"}){
          ui.setLanguage(language);ui.drawMain(draw);
          for(const auto& a:ui.widgets())for(const auto& b:ui.widgets())
            if(a.enabled && b.enabled && a.id<b.id && a.rect.x<b.rect.x+b.rect.w &&
               b.rect.x<a.rect.x+a.rect.w && a.rect.y<b.rect.y+b.rect.h && b.rect.y<a.rect.y+a.rect.h)
              throw std::runtime_error("Window settings overlap");
        }
        app.action(1902);app.action(1903);app.action(1904);app.action(1910);app.save();
        if(ui.window.resizable || ui.window.faceViewerWhileMoving || ui.settings.minimalDock ||
           ui.settings.follow!=FollowMode::World || !ui.saveRequested)
          throw std::runtime_error("Window settings did not take effect");
        ui.window={};
        if(!app.restore(false) || ui.window.resizable || ui.window.faceViewerWhileMoving ||
           !app.snapshot().window_settings_open)
          throw std::runtime_error("Window preferences lost during host replacement");
        {
          std::ifstream f(platform.state_file,std::ios::binary);std::string bytes(std::istreambuf_iterator<char>(f),{});f.close();
          const auto tag=bytes.find(std::string("1DNW\0\0\0\0",8));require(tag!=std::string::npos,"Missing window checkpoint extension");
          bytes.resize(tag);std::ofstream old(platform.state_file,std::ios::binary|std::ios::trunc);old.write(bytes.data(),bytes.size());old.close();
          const auto migrated=load_application_state(platform.state_file);
          require(migrated && migrated->window_resizable && migrated->window_face_viewer && !migrated->window_settings_open,"Previous state did not receive usable window defaults");
          require(migrated->scene==app.scene && migrated->browser_scroll==app.snapshot().browser_scroll,"Window migration changed movie/browser state");
        }
        app.action(1905);app.action(1901);
        require(ui.window.resizable && ui.window.faceViewerWhileMoving && ui.settings.minimalDock && ui.settings.follow==FollowMode::FaceViewer,"Reset window controls failed");
        ui.drawMain(draw);require(exists(1060) && exists(1900),"Back did not restore settings");
        ui.handleApplicationSlider(1371,.4f,SliderPhase::Commit);app.action(1370);if(app.volume!=0)throw std::runtime_error("Mute control failed");app.action(1370);if(std::abs(app.volume-.4f)>1e-5)throw std::runtime_error("Unmute lost the selected volume");
        app.action(1680);ui.drawMain(draw);if(!exists(1695))throw std::runtime_error("Video placement controls missing");
        ui.handleApplicationSlider(1690,.6f,SliderPhase::Commit);ui.handleApplicationSlider(1693,.75f,SliderPhase::Commit);
        app.action(1440);ui.drawMain(draw);if(!exists(1780) || !exists(1771))throw std::runtime_error("Subtitle settings are missing");
        ui.handleApplicationSlider(1780,.75f,SliderPhase::Commit);app.action(1791);app.action(1771);app.action(1762);ui.handleApplicationSlider(1784,.6f,SliderPhase::Commit);
        app.action(1000);app.action(1016);scroll(1,-1,3);app.action(1440);app.save();const auto saved=load_application_state(platform.state_file);
        if(!saved || saved->browser_scroll<=0 || saved->browser_scroll!=app.snapshot().browser_scroll)throw std::runtime_error("Continuous browser position was not saved");
        if(!saved || !saved->subtitle_settings_open || saved->subtitle_settings_tab!=1 || saved->subtitle_settings.scale!=2 || saved->subtitle_settings.color!=1 || !saved->subtitle_settings.bold || !saved->subtitle_settings.custom || std::abs(saved->subtitle_settings.x-.09f)>1e-4)throw std::runtime_error("Subtitle preferences were not persisted");
        if(!saved || saved->favorites.size()!=1 || saved->browser_view!=1 || saved->display_tab!=4 || std::abs(saved->scene.x-2)>1e-4 || saved->scene.yaw!=90 || std::abs(saved->volume-.4f)>1e-5)throw std::runtime_error("Workspace preferences were not persisted");
        app.action(1681);if(app.scene.x!=0 || app.scene.yaw!=0)throw std::runtime_error("Reset placement failed");
        app.scene=saved->scene;app.save();
      }
      {
        const auto saved=load_application_state(platform.state_file);
        UiModel restored;PlayerApplication app(restored,platform);if(!app.restore(false) || app.snapshot().favorites.size()!=1 || app.scene.yaw!=90 || app.snapshot().browser_view!=1 || app.snapshot().browser_scroll!=saved->browser_scroll)throw std::runtime_error("Workspace preferences lost during host transfer");
      }
      {
        std::ifstream f(platform.state_file,std::ios::binary);std::string bytes(std::istreambuf_iterator<char>(f),{});f.close();
        const auto extension=bytes.find(std::string("1SRB\0\0\0\0",8));if(extension==std::string::npos)throw std::runtime_error("Workspace state extension missing");bytes.resize(extension);
        std::ofstream legacy(platform.state_file,std::ios::binary|std::ios::trunc);legacy.write(bytes.data(),bytes.size());legacy.close();
        auto migrated=load_application_state(platform.state_file);if(!migrated || migrated->scene!=SceneSettings{} || migrated->volume!=1 || !migrated->favorites.empty() || migrated->browser_view!=0)throw std::runtime_error("Previous player state was not migrated with neutral workspace defaults");
      }
      std::filesystem::remove(root/"workspace.bin");
      std::filesystem::remove(root/"workspace.bin.bak");
    }
    {
      // Indexed caption lookups must keep long and overlapping events.
      auto cue = [](std::int64_t start, std::int64_t end, const char *text) {
        auto value = std::make_shared<SubtitleCue>();
        value->start_us = start;value->end_us = end;value->ass = {text};
        return value;
      };
      for (bool ordered : {true, false}) {
        SubtitleTimeline timeline(std::make_shared<SubtitleConfig>());
        timeline.add(cue(0, 100000000, "long"));
        if (!ordered)
          timeline.add(cue(9000000, 9500000, "late"));
        for (int n = 0; n < 200; ++n)
          timeline.add(cue(n * 1000000ll, n * 1000000ll + 400000, "line"));
        if (timeline.at(5200000).cues.size() != 2 ||
            timeline.at(5500000).cues.size() != 1 ||
            timeline.at(9200000).cues.size() != (ordered ? 2u : 3u) ||
            timeline.at(150200000).cues.size() != 1 ||
            timeline.at(150500000).cues.size() != 0 || timeline.at(-1).cues.size() != 0)
          throw std::runtime_error("Subtitle lookup lost or invented an event");
      }
      // The VR caption worker delivers images asynchronously and joins cleanly.
      SubtitleWorker captions({});
      SubtitleSnapshot empty;
      captions.post(empty, 640, 360);
      std::optional<SubtitleImage> caption;
      for (int attempt = 0; attempt < 400 && !caption; ++attempt) {
        caption = captions.take();
        if (!caption)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!caption || caption->canvas_width != 640 || !caption->rgba.empty())
        throw std::runtime_error("Subtitle worker did not deliver its image");
      // A damaged checkpoint falls back to the previous one, and one overlong
      // value cannot stop every later save.
      const auto path = (root / "fallback.bin").u8string();
      ApplicationState first;first.title = "first";
      ApplicationState second;second.title = "second";second.source.assign(20000, 'x');
      if (!save_application_state(path, first) || !save_application_state(path, second))
        throw std::runtime_error("An overlong source blocked the state file");
      auto reloaded = load_application_state(path);
      if (!reloaded || reloaded->title != "second" || !reloaded->source.empty())
        throw std::runtime_error("Overlong source was not omitted from the state");
      std::ofstream(path, std::ios::binary | std::ios::trunc) << "broken";
      reloaded = load_application_state(path);
      if (!reloaded || reloaded->title != "first")
        throw std::runtime_error("Damaged state did not fall back to its backup");
      StateWriter writer;
      writer.post(path, second);writer.post(path, first);writer.drain();
      reloaded = load_application_state(path);
      if (writer.take_failure() || !reloaded)
        throw std::runtime_error("Background state write failed");
      std::filesystem::remove(path);std::filesystem::remove(path + ".bak");
      std::filesystem::remove(root / "state.bin.bak");
    }
    std::ofstream(root / "state.bin", std::ios::binary | std::ios::trunc)
        << "CYVRP04";
    if (load_application_state((root / "state.bin").u8string()))
      throw std::runtime_error("Truncated state accepted");
    // Only remove the exact directory created by this test and its two known
    // children.
    std::filesystem::remove(root / "Videos" / "broken.mp4");
    std::filesystem::remove(root / "Videos" / "preview.mp4");
    std::filesystem::remove(root / "Videos");
    std::filesystem::remove(root / "state.bin");
    std::filesystem::remove(root / "timeline.gif");
    std::filesystem::remove(root);
    std::cout << "Application: async browser, media error propagation, pointer "
                 "actions, stereo and "
                 "keyboard, hybrid state transfer, localization and timeline "
                 "gestures passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
