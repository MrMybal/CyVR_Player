#pragma once
#include "cyvr/gpu.hpp"
#include <optional>
namespace cyvr::player {
// Optional Vulkan timestamp measurement. Read only after the owner's fence.
class GpuTimer {
  cyvr::VulkanContext &gpu_;
  VkQueryPool queries_{};
  float period_{};
  std::uint32_t bits_{};

public:
  explicit GpuTimer(cyvr::VulkanContext &);
  ~GpuTimer();
  GpuTimer(const GpuTimer &) = delete;
  GpuTimer &operator=(const GpuTimer &) = delete;
  void begin(VkCommandBuffer);
  void end(VkCommandBuffer);
  std::optional<double> result() const;
};
} // namespace cyvr::player
