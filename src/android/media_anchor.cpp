#include "cyvr/player/demuxer.hpp"
// Force link-checking the demux/decoder implementation into the native library.
extern "C" void cyvr_player_check_media_link() {
    cyvr::player::Demuxer demuxer;
}
