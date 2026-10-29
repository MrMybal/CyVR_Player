#pragma once
#include "cyvr/player/android/surface_decoder.hpp"
#include "cyvr/player/video_bitstream.hpp"
namespace cyvr::player::android {
// CSD and geometry only. The renderer must separately retain crop/color/HDR
// metadata from the stream and the MediaCodec output format.
MediaFormatPtr make_video_format(const VideoConfiguration& config);
}
