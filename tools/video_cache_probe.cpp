#include "cyvr/player/android/video_renderer.hpp"
#include <algorithm>
#include <android/hardware_buffer.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace cyvr;
using namespace cyvr::player;
int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 4)
      throw std::runtime_error("ASSETS [BUFFERS] [QUALITY]");
    VulkanContext gpu;
    loadVulkan();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "CyVR GPU probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &app;
    vkCheck(vkCreateInstance(&instance, nullptr, &gpu.instance),
            "create instance");
    uint32_t count{};
    vkCheck(vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr),
            "physical device count");
    if (!count)
      throw std::runtime_error("No GPU");
    std::vector<VkPhysicalDevice> physical(count);
    vkCheck(vkEnumeratePhysicalDevices(gpu.instance, &count, physical.data()),
            "physical devices");
    gpu.physical = physical[0];
    gpu.selectQueue();
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = gpu.queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    ycbcr.samplerYcbcrConversion = VK_TRUE;
    const char *extensions[]{
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.pNext = &ycbcr;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = 3;
    device.ppEnabledExtensionNames = extensions;
    vkCheck(vkCreateDevice(gpu.physical, &device, nullptr, &gpu.device),
            "create device");
    vkGetDeviceQueue(gpu.device, gpu.queueFamily, 0, &gpu.queue);
    AssetReader assets = [&](const std::string &name) {
      std::ifstream input(std::string(argv[1]) + "/" + name, std::ios::binary);
      if (!input)
        throw std::runtime_error("Asset missing: " + name);
      return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    };
    VulkanRenderer targets(gpu);
    targets.initialize(VK_FORMAT_R8G8B8A8_SRGB, assets);

    std::vector<android::ImageLease> leases;
    const int buffers = argc >= 3 ? std::stoi(argv[2]) : 16,
              frames = 3 * buffers;
    const int quality = argc >= 4 ? std::stoi(argv[3]) : 0;
    if (buffers < 1 || buffers > 80 || quality < 0 || quality > 3)
      throw std::runtime_error("Probe bounds");
    ImageSettings settings;
    settings.quality = quality;
    for (int n = 0; n < buffers; ++n) {
      AHardwareBuffer_Desc desc{};
      desc.width = 128;
      desc.height = 64;
      desc.layers = 1;
      desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
      desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                   AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
      AHardwareBuffer *buffer{};
      if (AHardwareBuffer_allocate(&desc, &buffer))
        throw std::runtime_error("Cache fixture allocation");
      std::shared_ptr<AHardwareBuffer> owner(buffer, AHardwareBuffer_release);
      AHardwareBuffer_describe(buffer, &desc);
      void *memory{};
      if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                               -1, nullptr, &memory))
        throw std::runtime_error("Cache fixture lock");
      auto *pixels = static_cast<unsigned char *>(memory);
      for (unsigned y = 0; y < desc.height; ++y)
        for (unsigned x = 0; x < desc.width; ++x) {
          auto at = (std::size_t(y) * desc.stride + x) * 4;
          pixels[at] = n % 3 == 0 ? 255 : 0;
          pixels[at + 1] = n % 3 == 1 ? 255 : 0;
          pixels[at + 2] = n % 3 == 2 ? 255 : 0;
          pixels[at + 3] = 255;
        }
      if (AHardwareBuffer_unlock(buffer, nullptr))
        throw std::runtime_error("Cache fixture unlock");
      VideoColor color;
      color.transfer = 1;
      leases.push_back(android::ImageLease::from_buffer(buffer, -1, owner,
                                                        n * 16667, 1, color));
    }
    auto target = targets.createTarget(128, 64);
    std::vector<double> wall, imports, gpuTimes;
    VideoRenderTimings timing;
    {
      VideoRenderer video(gpu, VK_FORMAT_R8G8B8A8_SRGB, assets);
      for (int n = 0; n < frames; ++n) {
        auto &lease = leases[n % buffers];
        const auto start = std::chrono::steady_clock::now();
        video.draw_retained(target.image, {128, 64}, {128, 64}, lease, settings,
                            0, false);
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        timing = video.timings();
        if (n >= buffers) {
          wall.push_back(ms);
          imports.push_back(timing.import_ms);
          if (auto t = video.gpu_ms())
            gpuTimes.push_back(*t);
        }
        const auto pixels = targets.readback(target);
        for (int c = 0; c < 3; ++c)
          if (std::abs(int(pixels[c]) - ((n % buffers) % 3 == c ? 255 : 0)) > 1)
            throw std::runtime_error("Cached AHB returned stale image pixels");
      }
      if (buffers <= 64 && (timing.imports != std::uint64_t(buffers) ||
                            timing.cache_evictions || timing.cache_resets))
        throw std::runtime_error(
            "AHB working-set cache was flushed or reimported");
      if (timing.cached_images != std::size_t(std::min(buffers, 64)))
        throw std::runtime_error("AHB cache exceeded its bound");
      const auto old = timing;
      leases[0].epoch = 2;
      video.draw_retained(target.image, {128, 64}, {128, 64}, leases[0],
                          settings, 0, false);
      const auto reset = video.timings();
      if (reset.cached_images != 1 ||
          reset.cache_resets != old.cache_resets + 1 ||
          reset.imports != old.imports + 1)
        throw std::runtime_error(
            "AHB generation change did not discard old imports");
    }
    auto median = [](std::vector<double> values) {
      std::sort(values.begin(), values.end());
      return values.empty() ? -1 : values[values.size() / 2];
    };
    std::cout << "{\"buffers\":" << buffers << ",\"frames\":" << frames
              << ",\"imports\":" << timing.imports << ",\"quality\":" << quality
              << ",\"cache_resets\":" << timing.cache_resets
              << ",\"cache_evictions\":" << timing.cache_evictions
              << ",\"warm_wall_median_ms\":" << median(wall)
              << ",\"warm_import_median_ms\":" << median(imports)
              << ",\"gpu_median_ms\":" << median(gpuTimes)
              << ",\"pixels_correct\":true}\n";
    targets.destroyTarget(target);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
