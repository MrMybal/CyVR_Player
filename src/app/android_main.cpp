#include "cyvr/android.hpp"
#include "cyvr/player/android/bridge.hpp"
#include "cyvr/player/android/renderer.hpp"
#include <android/log.h>
#include <android_native_app_glue.h>
#include <mutex>
namespace {
std::mutex activityHost;
}
extern "C" void android_main(android_app *android) {
  JNIEnv *env{};
  android->activity->vm->AttachCurrentThread(&env, nullptr);
  try {
    // An external panel launch may overlap the outgoing activity's teardown.
    // Serialize hosts so its durable state and decoder shutdown finish first.
    std::lock_guard<std::mutex> exclusive(activityHost);
    cyvr::player::AndroidBridge bridge(android);
    auto platform = bridge.platform();
    platform.panorama = CYVR_ENABLE_IMMERSIVE;
    std::filesystem::create_directories(std::filesystem::u8path(platform.root));
    std::unique_ptr<cyvr::player::PlayerApplication> player;
    cyvr::player::PlayerApplication *current{};
    auto graphics = cyvr::player::make_android_renderer(
        [&] { return current; }, bridge.presentation_mode() == 1);
    cyvr::AndroidAppConfig config;
    config.name = "CyVR Player";
    config.version = 17;
    config.immersiveEnabled = CYVR_ENABLE_IMMERSIVE;
    config.nativeResolution = true;
    config.viewFallback = true;
    config.graphics = graphics;
    config.configure = [&](cyvr::UiModel &ui) {
      player = std::make_unique<cyvr::player::PlayerApplication>(ui, platform);
      current = player.get();
      auto initial = bridge.poll();
      if (!player->restore(initial.size() != 3 || initial[0] != "open")) {
        auto tree = bridge.saved_tree();
        if (!tree.empty())
          player->browse(tree);
      }
      if (initial.size() == 3) {
        if (initial[0] == "open")
          player->open(initial[1], initial[2]);
        else if (initial[0] == "tree")
          player->browse(initial[1]);
        else if (initial[0] == "error")
          ui.setToast(initial[1]);
      }
      ui.applicationTick = [&](double) {
        for (int i = 0; i < 4; ++i) {
          auto event = bridge.poll();
          if (event.size() != 3)
            break;
          if (event[0] == "tree")
            player->browse(event[1]);
          else if (event[0] == "open")
            player->open(event[1], event[2]);
          else if (event[0] == "error")
            ui.setToast(event[1]);
          else if (event[0] == "back" && !player->back())
            ui.exitRequested = true;
          else if (event[0] == "show_interface")
            player->show_interface();
          else if (event[0] == "video_key") {
            if (!player->show_interface()) {
              if (event[1] == "21")
                player->video_input(-10, 0);
              else if (event[1] == "22")
                player->video_input(10, 0);
              else if (event[1] == "19")
                player->video_input(0, .15f);
              else if (event[1] == "20")
                player->video_input(0, -.15f);
            }
          } else if (event[0] == "toggle_interface" &&
                     !player->show_interface())
            player->action(1024);
        }
        player->tick();
      };
      __android_log_print(
          ANDROID_LOG_INFO, "CyVRPlayer",
          "APP_READY host=%s immersive_enabled=%d native_quality=1",
          ui.presentation == cyvr::Presentation::Immersive ? "immersive"
                                                           : "panel",
          int(config.immersiveEnabled));
    };
    config.activityState = [&](bool resumed) {
      if (player)
        player->active(resumed);
    };
    config.releaseApplication = [&] {
      current = nullptr;
      player.reset();
    };
    cyvr::runAndroidApp(android, config);
  } catch (const std::exception &error) {
    __android_log_print(ANDROID_LOG_ERROR, "CyVRPlayer", "FATAL: %s",
                        error.what());
  }
  ANativeActivity_finish(android->activity);
  // NativeActivity's Java callbacks synchronously wait for native glue to
  // acknowledge pause/stop/destroy. A presentation host may return before
  // those callbacks arrive; keep servicing them until destruction completes.
  while (!android->destroyRequested) {
    android_poll_source *source{};
    int events{};
    if (ALooper_pollOnce(-1, nullptr, &events,
                         reinterpret_cast<void **>(&source)) >= 0 &&
        source)
      source->process(android, source);
  }
  android->activity->vm->DetachCurrentThread();
}
