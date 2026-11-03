// Original fixture diagnostic. No application-private state is read or written.
#include "cyvr/player/application.hpp"
#include <filesystem>
#include <iostream>
#include <thread>
using namespace cyvr;
using namespace cyvr::player;
int main(int argc, char **argv) {
  try {
    if (argc != 2 && !(argc==3 && std::string(argv[2])=="--scroll"))
      throw std::runtime_error("SYNTHETIC_MEDIA_FILE [--scroll]");
    const bool scroll=argc==3;
    const auto file = std::filesystem::absolute(argv[1]);
    Platform p;
    p.root = "content://test/preview";
    p.documents = [&](const std::string &) {
      std::vector<BrowserItem> items;
      for(int n=0;n<(scroll?31:1);++n)
        items.push_back({file.u8string(),"Fixture "+std::to_string(n)+file.extension().u8string(),false,0});
      return items;
    };
    UiModel ui;
    PlayerApplication app(ui, p);
    std::vector<std::uint8_t> rgba;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool checked_scroll=false;
    DrawList draw;
    while (std::chrono::steady_clock::now() < until) {
      app.tick();
      ui.drawMain(draw);
      if (app.take_thumbnails(rgba) && !rgba.empty() && rgba[3] == 255) {
        unsigned nonblack = 0;
        for (size_t n = 0; n < 160 * 90 * 4; n += 4)
          if (rgba[n] > 16 || rgba[n + 1] > 16 || rgba[n + 2] > 16)
            ++nonblack;
        if (nonblack < 160 * 90 / 4)
          throw std::runtime_error("Preview is unexpectedly blank");
        if(scroll && !checked_scroll){
          const auto zoom=app.image.zoom;
          for(int hand=0;hand<2;++hand)for(int n=0;n<5;++n){
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ControllerInput input;input.hand=hand;input.stick={0,-1};input.trigger=.2f;input.poseValid=true;input.rayValid=true;
            PanelGeometry panel;input.rayOrigin={0,1.45f,0};const Vec3 target=panel.pose.position+Vec3{(600.f/1440-.5f)*panel.width,(.5f-500.f/990)*panel.height,0};
            input.rayDirection=normalized(target-input.rayOrigin);input.pointer=intersectPanel(input.rayOrigin,input.rayDirection,panel);input.deltaSeconds=.01f;
            const bool consumed=ui.controllerInput(input);if(consumed)throw std::runtime_error("Browser scrolling cleared its cursor");
            ui.pointer(hand,input.pointer.pixels,false);app.tick();ui.drawMain(draw);
          }
          if(app.snapshot().browser_scroll<=0 || app.snapshot().offset!=0 || app.image.zoom!=zoom)
            throw std::runtime_error("Continuous joystick scrolling changed a page or movie zoom");
          int visible=0;
          for(const auto& w:ui.widgets()){
            if(w.id==1013 || w.id==1014)throw std::runtime_error("File pagination is still visible");
            if(w.id>=1100 && w.id<1100+int(preview_slots)){
              ++visible;if(w.rect.y<304 || w.rect.y+w.rect.h>804)throw std::runtime_error("Offscreen tile is selectable");
            }
          }
          if(visible!=9)throw std::runtime_error("Clipped tile row is missing");
          checked_scroll=true;
          continue;
        }
        if(scroll && rgba[(8*160*90)*4+3]!=255)continue;
        std::cout << "{\"continuous_scroll\":"<<(scroll?"true":"false")
                  <<",\"nine_preview_slots\":"<<(scroll?"true":"false")
                  <<",\"preview_width\":160,\"preview_height\":90,\"visible_"
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
