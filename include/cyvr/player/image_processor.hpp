#pragma once
#include "cyvr/gpu.hpp"
#include "cyvr/player/image_settings.hpp"
#include <memory>
#include <optional>
namespace cyvr::player {
// GPU-only spatial processing, shared by Quest and the offscreen Vulkan probe.
// Input and output attachments use an sRGB format; shaders operate in linear
// RGB.
class ImageProcessor {
public:
  ImageProcessor(cyvr::VulkanContext &, VkFormat, const cyvr::AssetReader &,
                 VideoExtent input, VideoExtent output, int quality = 0,
                 VkPipelineCache cache = VK_NULL_HANDLE);
  ~ImageProcessor();
  VkImage source_image() const;
  VkImageView source_view() const;
  void process(VkImage output, const ImageSettings &, int stereo,
               bool sampled_output);
  // Caller owns command-buffer lifetime and completion; no extra submit/wait.
  void record(VkCommandBuffer, VkImage output, const ImageSettings &,
              int stereo, bool sampled_output, FilterRegion region = {});
  std::optional<double> gpu_ms() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
