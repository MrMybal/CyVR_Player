#include "cyvr/player/video_layout.hpp"
#include <iostream>
#include <stdexcept>
using namespace cyvr::player;
int main() {
  try {
    int checks = 0;
    auto expect = [&](std::string_view name, int stereo, int projection,
                      bool half = false, bool swap = false, int width = 0,
                      int height = 0) {
      const auto value = detect_video_layout(name, "", {}, width, height);
      if (value.stereo != stereo || value.projection != projection ||
          value.half != half || value.swap != swap)
        throw std::runtime_error("Incorrect layout: " + std::string(name));
      ++checks;
    };
    expect("movie.mp4", 0, 0);
    expect("movie_180_LR.mp4", 1, 1);
    expect("clip_VR360_TB.mkv", 2, 2);
    expect("clip-180-RL.mov", 1, 1, false, true);
    expect("clip.360.BT.mp4", 2, 2, false, true);
    expect("film_HSBS.mkv", 1, 0, true);
    expect("film.FSBS.mkv", 1, 0, false, false, 1920, 1080);
    expect("film Half-SBS.mkv", 1, 0, true);
    expect("film Full-SBS.mkv", 1, 0, false, false, 1920, 1080);
    expect("film.HOU.mkv", 2, 0, true);
    expect("film.FOU.mkv", 2, 0, false);
    expect("video_3DH_180x180.mp4", 1, 1);
    expect("video_3DV_360.mp4", 2, 2);
    expect("video_side_by_side_180.mp4", 1, 1);
    expect("video_bottom_top.mp4", 2, 0, false, true);
    expect("video.SBS.mp4", 1, 0, true, false, 1920, 1080);
    expect("video.SBS.mp4", 1, 0, false, false, 3840, 1080);
    expect("video.TB.mp4", 2, 0, true, false, 1920, 1080);
    expect("video.TB.mp4", 2, 0, false, false, 1920, 2160);
    expect("https://example.test/video_180_LR.mp4?token=360_TB", 1, 1);
    expect("content://test/document/primary%3AVideos%2Fvideo_360_TB.mp4", 2, 2);
    expect("D:/VR180/TB/movie.mp4", 0, 0);
    expect("movie360p.mp4", 0, 0);
    expect("Episode180.mp4", 0, 0);
    expect("normal-2to1.mp4", 0, 0, false, false, 3840, 1920);
    expect("film.3D.mp4", 0, 0);
    expect("ambiguous.SBS.TB.180.360.mp4", 0, 0);
    VideoLayoutHints metadata;
    metadata.stereo = 2;
    metadata.projection = 2;
    metadata.swap = true;
    auto detected =
        detect_video_layout("wrong_180_LR.mp4", "", metadata, 3840, 2160);
    if (detected.stereo != 2 || detected.projection != 2 || !detected.swap ||
        detected.evidence != 2)
      throw std::runtime_error("Metadata did not override contradictory name");
    ++checks;
    metadata.stereo = 0;
    metadata.projection = 0;
    detected = detect_video_layout("wrong_180_LR.mp4", "", metadata);
    if (detected.stereo || detected.projection || detected.half ||
        detected.swap)
      throw std::runtime_error("Explicit mono/flat metadata was ignored");
    ++checks;
    metadata = {};
    metadata.stereo = 1;
    detected = detect_video_layout("title_180.mp4", "", metadata, 3840, 1920);
    if (detected.stereo != 1 || detected.projection != 1 || detected.half)
      throw std::runtime_error("Independent projection hint was lost");
    ++checks;
    metadata = {};
    metadata.unsupported_projection = true;
    detected = detect_video_layout("cube_360.mp4", "", metadata);
    if (detected.projection || !detected.ambiguous)
      throw std::runtime_error("Unsupported projection was silently guessed");
    ++checks;
    metadata = {};
    detected =
        detect_video_layout("film_HSBS.mkv", "", metadata, 1920, 1080, 1, true);
    if (detected.stereo != 1 || detected.half || detected.swap ||
        detected.evidence != 3)
      throw std::runtime_error(
          "Decoded stereo packing did not override file hint");
    ++checks;
    detected = detect_video_layout("film_SBS.mkv", "", {}, 1920, 1080, 2);
    if (detected.half)
      throw std::runtime_error("Non-square SAR was applied twice");
    ++checks;
    std::cout << checks << " video layout cases passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
