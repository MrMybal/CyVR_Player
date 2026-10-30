#include "cyvr/player/android/video_renderer.hpp"
#include "cyvr/player/application.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>
using namespace cyvr;
using namespace cyvr::player;
int main(int argc, char **argv) {
  try {
    if (argc < 5 || argc > 8)
      throw std::invalid_argument(
          "FILE HARDWARE_CODEC ASSETS_DIR OUTPUT_PPM [controls]");
    const std::string mode = argc >= 6 ? argv[5] : "native";
    const unsigned minimum=argc>=7?unsigned(std::stoul(argv[6])):24;
    const bool controls = mode == "controls";
    ImageSettings settings;
    settings.quality = mode == "cas" || mode == "spatial-cas" ? 1
                       : mode == "fsr" || mode == "spatial-fsr" ? 2
                       : mode == "gsr" || mode == "spatial-gsr" ? 3
                                       : 0;
    if(mode.rfind("spatial",0)==0){settings.sampling=2;settings.denoise=.5f;settings.detail=.6f;}
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
      std::ifstream input(std::string(argv[3]) + "/" + name, std::ios::binary);
      if (!input)
        throw std::runtime_error("Asset missing: " + name);
      return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    };
    VulkanRenderer targets(gpu);
    targets.initialize(VK_FORMAT_R8G8B8A8_SRGB, assets);
    Platform platform;
    platform.hardware_codec = [&](const std::string &, int, int) {
      return std::string(argv[2])=="software"?std::string{}:std::string(argv[2]);
    };
    PlayerSession session(platform);
    TrackSelection selection;if(argc==8)selection.clearkey_file=argv[7];
    session.open(argv[1], "GPU probe",0,true,selection);
    Target target;
    std::uint64_t presented{};
    bool multiview=false,audio_started=false;
    std::vector<std::int64_t> av_offset;
    VideoMetrics metrics;
    std::string decoder_name;
    std::optional<android::AudioPlaybackStats> audio_metrics;
    {
      VideoRenderer video(gpu, VK_FORMAT_R8G8B8A8_SRGB, assets);
      auto start = std::chrono::steady_clock::now();
      bool controls_done = false, check_seek = false;
      std::uint64_t old_epoch{};
      while (std::chrono::steady_clock::now() - start <
             std::chrono::seconds(15)) {
        const auto state = session.state();
        if(!state.codec.empty())decoder_name=state.codec;
        multiview|=state.multiview;
        if (!state.ready && state.status != "Choose a video." &&
            state.status != "Opening media...")
          throw std::runtime_error(state.status);
        auto image = session.take_frame();
        if (image) {
          if(const auto clock=session.audio_position_us()){audio_started=true;av_offset.push_back(image.pts_us-*clock);}
          if (check_seek) {
            if (image.epoch == old_epoch || image.pts_us < 1'000'000)
              throw std::runtime_error("Stale frame after seek");
            check_seek = false;
          }
          old_epoch = image.epoch;
          if (!target.image) {
            const auto extent =
                processed_extent(state.width, state.height, settings, state.multiview?1:0);
            target = targets.createTarget(extent.width, extent.height);
          }
          video.draw_retained(
              target.image, {uint32_t(state.width), uint32_t(state.height)},
              {target.width, target.height}, image, settings, state.multiview?1:0, false);
          if (controls && !controls_done && video.presented() >= 12) {
            session.pause(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            if (session.take_frame() || session.state().playing)
              throw std::runtime_error("Pause did not suspend presentation");
            session.seek(1'000'000);
            session.pause(false);
            controls_done = true;
            check_seek = true;
          }
        }
        if (state.ended)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      presented = video.presented();
      metrics=session.metrics();
      audio_metrics=session.audio_metrics();
      if (controls && (!controls_done || check_seek))
        throw std::runtime_error("Control validation incomplete");
    }
    session.stop();
    if (presented < minimum || !target.image)
      throw std::runtime_error("Too few video frames presented");
    auto pixels = targets.readback(target);
    std::uint64_t nonblack{};
    for (size_t i = 0; i < pixels.size(); i += 4)
      if (pixels[i] > 16 || pixels[i + 1] > 16 || pixels[i + 2] > 16)
        ++nonblack;
    std::ofstream image(argv[4], std::ios::binary);
    image << "P6\n" << target.width << ' ' << target.height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
      image.write(reinterpret_cast<const char *>(pixels.data() + i), 3);
    if (!image)
      throw std::runtime_error("Cannot save diagnostic image");
    const auto area = std::uint64_t(target.width) * target.height;
    targets.destroyTarget(target);
    if (nonblack < area / 4)
      throw std::runtime_error("GPU output is unexpectedly black");
    std::sort(av_offset.begin(),av_offset.end());
    const auto low=av_offset.empty()?0:av_offset[av_offset.size()/20];
    const auto high=av_offset.empty()?0:av_offset[std::min(av_offset.size()-1,av_offset.size()*19/20)];
    std::cout << "{\"presented_frames\":" << presented
              << ",\"nonblack_pixels\":" << nonblack
              << ",\"decoder\":\"" << decoder_name << "\""
              << ",\"stereo\":" << (multiview?"true":"false")
              << ",\"audio_started\":" << (audio_started?"true":"false")
              << ",\"decoded_frames\":" << metrics.decoded << ",\"late_drops\":" << metrics.dropped
              << ",\"audio_relative_pts_p05_us\":" << low << ",\"audio_relative_pts_p95_us\":" << high
              << ",\"audio_underrun_frames\":" << (audio_metrics?audio_metrics->queue.underrun:0)
              << ",\"audio_gap_frames\":" << (audio_metrics?audio_metrics->queue.gap:0)
              << ",\"audio_expired_frames\":" << (audio_metrics?audio_metrics->queue.expired:0)
              << ",\"audio_peak\":" << (audio_metrics?audio_metrics->peak:0)
              << ",\"audio_limited_samples\":" << (audio_metrics?audio_metrics->limited:0)
              << ",\"audio_invalid_samples\":" << (audio_metrics?audio_metrics->invalid:0)
              << ",\"audio_xruns\":" << (audio_metrics?audio_metrics->xruns:0)
              << ",\"audio_buffer_frames\":" << (audio_metrics?audio_metrics->buffer_frames:0)
              << ",\"render_path\":\"decoded-AHB-Vulkan\",\"cpu_readback\":"
                 "\"diagnostic-only-after-"
                 "playback\"}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "GPU probe failed: " << e.what() << '\n';
    return 2;
  }
}
