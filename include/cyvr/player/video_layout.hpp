#pragma once
#include <optional>
#include <string_view>
namespace cyvr::player {
// Independent hints: metadata can specify packing without specifying
// projection.
struct VideoLayoutHints {
  std::optional<int> stereo, projection; // 0 mono/flat, 1 SBS/180, 2 TB/360
  std::optional<bool> half, swap;
  bool unsupported_projection{};
};
struct DetectedVideoLayout {
  int stereo{}, projection{};
  bool half{}, swap{}, ambiguous{};
  int evidence{}; // 0 neutral default, 1 filename, 2 metadata, 3 decoded
                  // multiview
};
VideoLayoutHints named_video_layout(std::string_view);
DetectedVideoLayout detect_video_layout(std::string_view location,
                                        std::string_view title,
                                        const VideoLayoutHints &metadata = {},
                                        int width = 0, int height = 0,
                                        double pixel_aspect = 1,
                                        bool multiview = false);
} // namespace cyvr::player
