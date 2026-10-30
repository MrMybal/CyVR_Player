#pragma once
#include "cyvr/gpu.hpp"
#include "cyvr/player/image_settings.hpp"
#include "cyvr/player/splats.hpp"
namespace cyvr::player {
class SplatRenderer {
public:
    SplatRenderer(cyvr::VulkanContext&,VkFormat,const cyvr::AssetReader&);
    ~SplatRenderer();
    void draw(VkImage,VideoExtent, const SplatSnapshot&,cyvr::Pose camera,
              const std::array<float,4>& tangents,const ImageSettings&,bool sampled_output);
    std::uint32_t drawn() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
