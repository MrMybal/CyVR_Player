#pragma once
#include <string>
namespace cyvr::player {
// -2: container default; -1: disabled (audio/subtitles only).
struct TrackSelection {int video{-2},audio{-2},subtitle{-2};std::string external_subtitle,clearkey_file;};
}
