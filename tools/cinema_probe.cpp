// Offscreen diagnostic: verifies the actual UI shader and video-only geometry.
// The media supplies dimensions; an owned green texture stands in for decoded
// pixels.
#include "cyvr/gpu.hpp"
#include "cyvr/player/application.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
using namespace cyvr;
using namespace cyvr::player;
int main(int argc, char **argv) {
  try {
    if (argc != 4)
      throw std::runtime_error("320x192_MEDIA ASSETS_DIRECTORY OUTPUT_PPM");
    VulkanContext gpu;
    gpu.createStandalone();
    VulkanRenderer renderer(gpu);
    renderer.initialize(VK_FORMAT_R8G8B8A8_SRGB, [&](const std::string &name) {
      std::ifstream file(std::filesystem::path(argv[2]) / name,
                         std::ios::binary);
      if (!file)
        throw std::runtime_error("Missing asset: " + name);
      return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
    });
    UiModel ui;
    Platform platform;
    platform.root = std::filesystem::absolute(argv[1]).parent_path().u8string();
    PlayerApplication app(ui, platform);
    app.open(std::filesystem::absolute(argv[1]).u8string());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!app.session().state().ready &&
           std::chrono::steady_clock::now() < deadline) {
      app.tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    app.tick();
    const auto state = app.session().state();
    if (!state.ready || state.width != 320 || state.height != 192)
      throw std::runtime_error("Requires a valid 320x192 fixture");
    std::vector<uint8_t> green(320 * 192 * 4);
    for (size_t i = 0; i < green.size(); i += 4) {
      green[i + 1] = 255;
      green[i + 3] = 255;
    }
    renderer.uploadImages(320, 192, green);
    // Owned test texture is rebound to the video slot by the probe helper
    // below.
    renderer.useImageAtlasForVideo();
    app.preview_ready = true;
    for (const auto size : {Vec2{1920, 1080}, Vec2{1600, 1100}}) {
      ui.viewportSize = size;
      app.action(1024);
      if (!ui.interfaceHidden)
        throw std::runtime_error("Hide interface action failed");
      DrawList draw;
      ui.drawMain(draw);
      DrawList dock;
      ui.drawDock(dock);
      if (!ui.widgets().empty() || !dock.vertices.empty())
        throw std::runtime_error("Interactive chrome survived hiding");
      auto target = renderer.createTarget(uint32_t(size.x), uint32_t(size.y));
      renderer.draw(target, draw, nullptr, false, {1440, 990});
      auto pixels = renderer.readback(target);
      const int left = size.x == 1920 ? 60 : 0, top = size.x == 1920 ? 0 : 70;
      const int right = size.x == 1920 ? 1860 : 1600,
                bottom = size.x == 1920 ? 1080 : 1030;
      for (int y = 0; y < int(size.y); ++y)
        for (int x = 0; x < int(size.x); ++x) {
          auto i = (size_t(y) * size_t(size.x) + x) * 4;
          const bool video = x >= left && x < right && y >= top && y < bottom;
          if (pixels[i] > 2 || pixels[i + 2] > 2 ||
              (video ? pixels[i + 1] < 253 : pixels[i + 1] > 2))
            throw std::runtime_error("Video-only output contains UI, wrong "
                                     "aspect or a reduced viewport");
        }
      if (size.x == 1920) {
        std::ofstream out(argv[3], std::ios::binary);
        out << "P6\n1920 1080\n255\n";
        for (size_t i = 0; i < pixels.size(); i += 4)
          out.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
      }
      renderer.destroyTarget(target);
      ui.pointer(0, {196, 125}, true);
      ui.drawMain(draw);
      ui.pointer(0, {196, 125}, false);
      if (ui.interfaceHidden || app.page() != 1)
        throw std::runtime_error(
            "Wake gesture navigated instead of only revealing controls");
    }
    for (const auto language : {"en", "fr"}) {
      ui.setLanguage(language);
      DrawList hud;
      ui.drawDebug(hud);
      auto target = renderer.createTarget(620, 760);
      renderer.draw(target, hud);
      const auto pixels = renderer.readback(target);
      std::ofstream out(std::string(argv[3]) + ".hud-" + language + ".ppm",
                        std::ios::binary);
      out << "P6\n620 760\n255\n";
      for (size_t i = 0; i < pixels.size(); i += 4)
        out.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
      renderer.destroyTarget(target);
    }
    app.show_interface();ui.presentation=Presentation::Immersive;app.tick();app.action(1662);
    for(const auto language:{"en","fr"})for(int tab=0;tab<3;++tab){
      ui.setLanguage(language);app.action(tab==2?1663:1660+tab);app.tick();DrawList settings;ui.drawMain(settings);
      auto target=renderer.createTarget(1440,990);renderer.draw(target,settings);const auto pixels=renderer.readback(target);
      std::ofstream out(std::string(argv[3])+".cinema-"+language+"-"+std::to_string(tab)+".ppm",std::ios::binary);
      out<<"P6\n1440 990\n255\n";for(std::size_t i=0;i<pixels.size();i+=4)out.write(reinterpret_cast<const char*>(pixels.data()+i),3);
      renderer.destroyTarget(target);
    }
    app.action(1300);
    for(const auto language:{"en","fr"}){
      ui.setLanguage(language);DrawList format;ui.drawMain(format);auto target=renderer.createTarget(1440,990);renderer.draw(target,format);const auto pixels=renderer.readback(target);
      std::ofstream out(std::string(argv[3])+".format-"+language+".ppm",std::ios::binary);out<<"P6\n1440 990\n255\n";for(std::size_t i=0;i<pixels.size();i+=4)out.write(reinterpret_cast<const char*>(pixels.data()+i),3);renderer.destroyTarget(target);
    }
    std::cout << "Cinema Vulkan: 1920x1080 and 1600x1100, source aspect "
                 "preserved, video only, "
                 "wake consumed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
