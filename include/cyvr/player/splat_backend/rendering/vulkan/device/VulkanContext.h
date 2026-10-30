// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <stdexcept>
#include "splat/core/Result.h"
namespace splatkit {
// Adapter for the pinned MIT backend. Borrows the host's device/queue; it creates
// neither a Vulkan instance nor a window and never destroys host-owned handles.
class VulkanContext {
public:
    VulkanContext(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family):instance_(instance),physical_(physical),device_(device),queue_(queue),family_(family){
        vkGetPhysicalDeviceProperties(physical,&info_.physical_device.properties);
        VmaAllocatorCreateInfo info{};info.instance=instance;info.physicalDevice=physical;info.device=device;info.vulkanApiVersion=VK_API_VERSION_1_1;
        if(vmaCreateAllocator(&info,&allocator_)!=VK_SUCCESS)throw std::runtime_error("Gaussian GPU allocator unavailable");
    }
    ~VulkanContext(){if(allocator_)vmaDestroyAllocator(allocator_);}
    VkInstance instance()const{return instance_;}VkPhysicalDevice physicalDevice()const{return physical_;}VkDevice device()const{return device_;}VkQueue queue()const{return queue_;}uint32_t queueFamily()const{return family_;}VmaAllocator allocator()const{return allocator_;}VkPipelineCache pipelineCache()const{return VK_NULL_HANDLE;}
    struct DeviceInfo{struct Physical{VkPhysicalDeviceProperties properties;}physical_device;};
    const DeviceInfo& vkbDevice()const{return info_;}
private:
    VkInstance instance_;VkPhysicalDevice physical_;VkDevice device_;VkQueue queue_;uint32_t family_;VmaAllocator allocator_{};DeviceInfo info_{};
};
}
