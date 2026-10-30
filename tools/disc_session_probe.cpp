#include "cyvr/gpu.hpp"
#include "cyvr/player/application.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace cyvr::player;
int main(int argc, char **argv) {
  try {
    if (argc != 3 && argc != 4)
      throw std::runtime_error("DISC_ISO STATE_DIRECTORY [ASSETS]");
    cyvr::UiModel ui;
    Platform platform;
    platform.root = argv[2];
    platform.hardware_codec = [](const std::string &mime, int, int) {
      return mime == "video/avc"     ? std::string("c2.qti.avc.decoder")
             : mime == "video/mpeg2" ? std::string("c2.qti.mpeg2.decoder")
                                     : std::string{};
    };
    platform.state_file =
        (std::filesystem::u8path(argv[2]) / "disc-probe-state.bin").u8string();
    PlayerApplication app(ui, platform);
    auto wait = [&](auto predicate) {
      for (int n = 0; n < 2000; ++n) {
        app.tick();
        if (predicate())
          return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      throw std::runtime_error("Disc command timed out: " +
                               app.session().state().status);
    };
    app.open(argv[1], "Original disc fixture");
    wait([&] { return app.session().state().ready; });
    auto state = app.session().state();
    if (state.disc.titles.size() < 2)
      throw std::runtime_error("Fixture must have multiple titles");
    auto longest =
        std::max_element(state.disc.titles.begin(), state.disc.titles.end(),
                         [](const auto &a, const auto &b) {
                           return a.duration_us < b.duration_us;
                         });
    if (state.disc.selected != longest->index)
      throw std::runtime_error("Main title was not selected automatically");
    const auto main = state.disc.selected,
               other = state.disc.titles.front().index == main
                           ? state.disc.titles[1].index
                           : state.disc.titles.front().index;
    app.session().pause(true);
    app.session().seek(750000);
    if (!app.session().select_disc_title(other))
      throw std::runtime_error("Title switch rejected");
    wait([&] {
      const auto s = app.session().state();
      return s.ready && s.disc.selected == other;
    });
    state = app.session().state();
    if (state.playing || state.position_us)
      throw std::runtime_error(
          "Title switch lost pause or did not reset position");
    int audio = -1;
    for (const auto &t : state.tracks)
      if (t.kind == TrackKind::audio)
        audio = t.index;
    auto revision = state.revision;
    if (audio < 0 || !app.session().select_track(TrackKind::audio, audio))
      throw std::runtime_error("Disc audio switch rejected");
    wait([&] {
      const auto s = app.session().state();
      return s.revision > revision + 1 && s.ready && s.audio_track == audio &&
             s.disc.selected == other;
    });
    app.action(1001);
    app.action(1400);
    app.action(1414);
    cyvr::DrawList draw;
    ui.drawMain(draw);
    if (argc == 4) {
      cyvr::VulkanContext gpu;
      gpu.createStandalone();
      cyvr::VulkanRenderer renderer(gpu);
      renderer.initialize(
          VK_FORMAT_R8G8B8A8_SRGB, [&](const std::string &name) {
            std::ifstream f(std::filesystem::u8path(argv[3]) / name,
                            std::ios::binary);
            if (!f)
              throw std::runtime_error("Missing diagnostic asset");
            return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                             {});
          });
      auto target = renderer.createTarget(1440, 990);
      ui.viewportSize = {1440, 990};
      for (const auto *language : {"en", "fr"}) {
        ui.setLanguage(language);
        ui.drawMain(draw);
        renderer.draw(target, draw);
        auto pixels = renderer.readback(target);
        std::ofstream image(
            std::filesystem::u8path(argv[2]) /
                (std::string("disc-tracks-") + language + ".ppm"),
            std::ios::binary);
        image << "P6\n1440 990\n255\n";
        for (std::size_t n = 0; n < pixels.size(); n += 4)
          image.write(reinterpret_cast<const char *>(pixels.data() + n), 3);
      }
      renderer.destroyTarget(target);
      ui.setLanguage("en");
      ui.drawMain(draw);
    }
    if (std::none_of(ui.widgets().begin(), ui.widgets().end(),
                     [](const auto &w) { return w.id == 1420 && w.enabled; }))
      throw std::runtime_error("Disc title buttons unavailable");
    app.save();
    const auto saved = load_application_state(platform.state_file);
    if (!saved || saved->tracks.disc_title != other ||
        saved->tracks.audio != audio || saved->track_tab != 4 ||
        !saved->tracks_open)
      throw std::runtime_error("Disc checkpoint lost selection or navigation");
    const auto media_id = app.session().state().media_id;
    if (!app.restore())
      throw std::runtime_error("Disc host restore failed");
    wait([&] {
      const auto s = app.session().state();
      return s.media_id > media_id && s.ready && s.disc.selected == other &&
             s.audio_track == audio;
    });
    if (app.session().select_disc_title(9999) ||
        app.session().select_disc_title(other, 99))
      throw std::runtime_error("Invalid title/angle accepted");
    if (!app.session().select_disc_title(main))
      throw std::runtime_error("Main title switch rejected");
    wait([&] {
      const auto s = app.session().state();
      return s.ready && s.disc.selected == main;
    });
    std::cout << "Automatic main title, title/audio switches, pause, "
                 "selectable UI, checkpoint and host restoration passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
