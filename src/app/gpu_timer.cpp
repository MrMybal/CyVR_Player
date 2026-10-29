#include "cyvr/player/gpu_timer.hpp"
namespace cyvr::player {
using namespace cyvr;
GpuTimer::GpuTimer(VulkanContext &gpu) : gpu_(gpu) {
  std::uint32_t count{};
  vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &count, queues.data());
  if (gpu.queueFamily >= count || !queues[gpu.queueFamily].timestampValidBits)
    return;
  bits_ = queues[gpu.queueFamily].timestampValidBits;
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(gpu.physical, &properties);
  period_ = properties.limits.timestampPeriod;
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = 2;
  if (vkCreateQueryPool(gpu.device, &info, nullptr, &queries_) != VK_SUCCESS)
    queries_ = VK_NULL_HANDLE;
}
GpuTimer::~GpuTimer() {
  if (queries_)
    vkDestroyQueryPool(gpu_.device, queries_, nullptr);
}
void GpuTimer::begin(VkCommandBuffer command) {
  if (queries_) {
    vkCmdResetQueryPool(command, queries_, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_,
                        0);
  }
}
void GpuTimer::end(VkCommandBuffer command) {
  if (queries_)
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                        1);
}
std::optional<double> GpuTimer::result() const {
  if (!queries_)
    return {};
  struct Query {
    std::uint64_t value, available;
  };
  Query values[2]{};
  if (vkGetQueryPoolResults(
          gpu_.device, queries_, 0, 2, sizeof(values), values, sizeof(Query),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) !=
          VK_SUCCESS ||
      !values[0].available || !values[1].available)
    return {};
  const std::uint64_t mask =
      bits_ >= 64 ? UINT64_MAX : ((std::uint64_t(1) << bits_) - 1);
  return double((values[1].value - values[0].value) & mask) * period_ /
         1000000.;
}
} // namespace cyvr::player
