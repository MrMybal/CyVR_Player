#include "cyvr/player/application.hpp"
#include "cyvr/player/locale.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace cyvr;
using namespace cyvr::player;
int main() {
  try {
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
               thumbnails.size() == 160 * 630 * 4 &&
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
      state.display_tab = 2;
      state.tracks={3,5,8,"content://test/document/captions"};
      state.tracks_open=true;state.track_tab=2;state.track_offset=7;
      if (!save_application_state(platform.state_file, state))
        throw std::runtime_error("State update failed");
      auto loaded = load_application_state(platform.state_file);
      if (!loaded || loaded->source != state.source ||
          loaded->position_us != 1234567 || loaded->playing ||
          loaded->parents != state.parents || loaded->image != state.image ||
          loaded->display_tab != 2 || loaded->tracks.video!=3 || loaded->tracks.audio!=5 || loaded->tracks.subtitle!=8 || loaded->tracks.external_subtitle!=state.tracks.external_subtitle || !loaded->tracks_open || loaded->track_tab!=2 || loaded->track_offset!=7)
        throw std::runtime_error(
            "Document or paused position lost during state transfer");
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
      state.url = std::string(300000, 'x');
      if (save_application_state(platform.state_file, state) ||
          !load_application_state(platform.state_file))
        throw std::runtime_error("Oversized state replaced the valid state");
      app.open((root / "timeline.gif").u8string());
      wait([&] { return app.session().state().ready; });
      const auto duration = app.session().state().duration_us;
      if (duration != 2000000 || !app.session().state().seekable)
        throw std::runtime_error("Timeline fixture metadata is invalid");
      model.setInterfaceHidden(true);
      if (!model.applicationController(1, {}, .8f, .8f, {{0, .5f, 0}, {}},
                                       true))
        throw std::runtime_error("Grip+trigger was not consumed");
      model.applicationController(1, {}, .8f, .8f, {{0, .75f, 0}, {}}, true);
      if (std::abs(app.image.zoom - 2) > 1e-5 || !model.interfaceHidden)
        throw std::runtime_error("Controller zoom failed or exposed UI");
      model.applicationController(1, {}, 0, 0, {}, false);
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
