// Original fixture diagnostic. No application-private state is read or written.
#include "cyvr/player/application.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
using namespace cyvr;
using namespace cyvr::player;
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("SYNTHETIC_MEDIA_FILE");
    const auto file = std::filesystem::absolute(argv[1]);
    Platform p;
    p.root = "content://test/preview";
    p.documents = [&](const std::string &) {
      return std::vector<BrowserItem>{
          {file.u8string(), file.filename().u8string(), false, 0}};
    };
    UiModel ui;
    PlayerApplication app(ui, p);
    std::vector<std::uint8_t> rgba;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < until) {
      app.tick();
      if (app.take_thumbnails(rgba) && !rgba.empty() && rgba[3] == 255) {
        unsigned nonblack = 0;
        for (size_t n = 0; n < 160 * 90 * 4; n += 4)
          if (rgba[n] > 16 || rgba[n + 1] > 16 || rgba[n + 2] > 16)
            ++nonblack;
        if (nonblack < 160 * 90 / 4)
          throw std::runtime_error("Preview is unexpectedly blank");
        std::cout << "{\"preview_width\":160,\"preview_height\":90,\"visible_"
                     "pixels\":"
                  << nonblack << ",\"software_fallback\":true}\n";
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("No preview produced within fixture deadline");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
