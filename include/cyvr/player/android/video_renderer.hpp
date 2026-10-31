#pragma once
#include "cyvr/gpu.hpp"
#include "cyvr/player/android/surface_decoder.hpp"
#include "cyvr/player/image_settings.hpp"
#include "cyvr/player/video_view.hpp"
#include <memory>
#include <optional>
namespace cyvr::player {
struct VideoRenderTimings {
  double total_ms{}, import_ms{}, completion_ms{};
  std::uint64_t imports{}, cache_resets{}, cache_evictions{};
  std::size_t cached_images{};
};
class VideoRenderer {
public:
  // A shared cache spares recompiling shaders each time a renderer is rebuilt.
  VideoRenderer(cyvr::VulkanContext &, VkFormat, const cyvr::AssetReader &,
                VkPipelineCache shared_cache = VK_NULL_HANDLE);
  ~VideoRenderer();
  void draw(VkImage output, std::uint32_t width, std::uint32_t height,
            android::ImageLease image, bool sampled_output = false);
  // Conversion takes most of a 72 Hz frame, so the frame loop must not wait for
  // it. Whoever defers calls settle before releasing the image it passed in.
  void defer_completion();
  void settle();
  std::uint64_t presented() const;
  std::optional<double> gpu_ms() const;
  VideoRenderTimings timings() const;
  // Pixels outside visible keep their previous content; see SourceRegion.
  void draw_retained(VkImage output, VideoExtent source, VideoExtent target,
                     android::ImageLease &, const ImageSettings &, int stereo,
                     bool sampled_output, SourceRegion visible = {});
  void draw_view(VkImage output, VideoExtent reconstruction, VideoExtent target,
                 android::ImageLease &, const ImageSettings &, int stereo,
                 const VideoView &, bool sampled_output);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
