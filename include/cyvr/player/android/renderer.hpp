#pragma once
#include "cyvr/android_render.hpp"
#include "cyvr/player/application.hpp"
namespace cyvr::player {
std::shared_ptr<cyvr::AndroidRenderExtension>
make_android_renderer(std::function<PlayerApplication *()> application,
                      bool immersive);
}
