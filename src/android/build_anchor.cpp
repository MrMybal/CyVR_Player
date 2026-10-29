#include "cyvr/player/android/surface_decoder.hpp"
// Pull the complete decoder object into the shared library for NDK link checks.
// This build artifact is a library; the Activity and OpenXR loop come later.
extern "C" void cyvr_player_check_native_link() {
    cyvr::player::android::SurfaceDecoder decoder;
}
