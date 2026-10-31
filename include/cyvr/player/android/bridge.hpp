#pragma once
#include "cyvr/player/application.hpp"
#include <jni.h>
struct android_app;
namespace cyvr::player {
class AndroidBridge {
  public:
    explicit AndroidBridge(android_app *app);
    ~AndroidBridge();
    Platform platform();
    std::vector<std::string> poll();
    std::string saved_tree();
    int presentation_mode();

  private:
    JavaVM *vm_{};
    jobject activity_{};
    jclass type_{};
    std::string root_, ca_, state_file_;
    jmethodID tree_{}, poll_{}, saved_{}, certs_{}, codec_{}, list_{}, open_{}, close_{}, cancel_{};
    jmethodID locations_{},thumbnail_{},browser_{},web_request_{};
};
} // namespace cyvr::player
