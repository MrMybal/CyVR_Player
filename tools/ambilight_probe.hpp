#pragma once
#include "cyvr/player/ambilight.hpp"
inline int check_ambilight(cyvr::VulkanContext &gpu,
                           cyvr::VulkanRenderer &renderer,
                           const cyvr::AssetReader &assets) {
  using namespace cyvr;
  using namespace cyvr::player;
  AmbilightRenderer glow(gpu, VK_FORMAT_R8G8B8A8_SRGB, assets);
  VkImage output{};
  VkDeviceMemory memory{};
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8B8A8_SRGB;
  info.extent = {128, 128, 1};
  info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  vkCheck(vkCreateImage(gpu.device, &info, nullptr, &output),
          "glow probe image");
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(gpu.device, output, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkCheck(vkAllocateMemory(gpu.device, &alloc, nullptr, &memory),
          "glow probe memory");
  vkCheck(vkBindImageMemory(gpu.device, output, memory, 0), "glow probe bind");
  auto source = renderer.createTarget(64, 36, glow.source_image()),
       target = renderer.createTarget(128, 128, output);
  AmbilightSettings settings;
  settings.enabled = true;
  settings.auto_bars = false;
  settings.smoothing = 0;
  settings.intensity = 1;
  auto paint = [&](Color c, bool bars = false) {
    DrawList d;
    d.rect({0, 0, 64, 36}, bars ? Color{0, 0, 0, 1} : c);
    if (bars)
      d.rect({0, 8, 64, 20}, c);
    renderer.draw(source, d);
    readable(gpu, source.image);
  };
  auto run = [&](float aspect = 16.f / 9, float dt = 1.f / 30) {
    glow.process(target.image, settings, aspect, dt);
    return renderer.readback(target);
  };
  auto at = [](int x, int y, int c) { return (y * 128 + x) * 4 + c; };
  int checks = 0;
  for (float aspect : {.5f, 1.f, 16.f / 9, 2.4f, 4.f}) {
    DrawList d;
    d.rect({0, 0, 64, 36}, {0, 1, 0, 1});
    d.rect({0, 0, 12, 36}, {1, 0, 0, 1});
    d.rect({52, 0, 12, 36}, {0, 0, 1, 1});
    renderer.draw(source, d);
    readable(gpu, source.image);
    glow.reset();
    auto pixels = run(aspect);
    if (pixels[at(64, 64, 3)] || pixels[at(0, 0, 3)] > 5)
      throw std::runtime_error(
          "Ambilight obscures movie or has a hard outer corner");
    if (pixels[at(14, 64, 0)] < 100 || pixels[at(14, 64, 2)] > 5 ||
        pixels[at(113, 64, 2)] < 100 || pixels[at(113, 64, 0)] > 5)
      throw std::runtime_error(
          "Ambilight did not preserve left/right edge colours");
    ++checks;
  }
  paint({0, 0, 0, 1});
  glow.reset();
  auto black = run();
  for (auto b : black)
    if (b)
      throw std::runtime_error(
          "Black movie emitted a visible Ambilight rectangle");
  ++checks;
  paint({1, 0, 0, 1});
  settings.intensity = 0;
  glow.reset();
  auto zero = run();
  for (auto b : zero)
    if (b)
      throw std::runtime_error("Zero Ambilight intensity was not transparent");
  ++checks;
  settings.intensity = 1;
  settings.saturation = 0;
  glow.reset();
  auto gray = run();
  if (std::abs(int(gray[at(14, 64, 0)]) - int(gray[at(14, 64, 1)])) > 1 ||
      std::abs(int(gray[at(14, 64, 1)]) - int(gray[at(14, 64, 2)])) > 1)
    throw std::runtime_error("Ambilight saturation control failed");
  ++checks;
  settings.saturation = 1;
  paint({1, 0, 0, 1}, true);
  glow.reset();
  auto bars = run();
  settings.auto_bars = true;
  glow.reset();
  auto cropped = run();
  if (cropped[at(64, 20, 0)] < 80 ||
      cropped[at(64, 20, 3)] <= bars[at(64, 20, 3)] + 20)
    throw std::runtime_error(
        "Ambilight letterbox detection did not recover active image colour");
  ++checks;
  paint({1, 0, 0, 1});
  glow.reset();
  settings.smoothing = 1;
  run();
  paint({0, 0, 1, 1});
  auto smooth = run();
  if (smooth[at(14, 64, 0)] < 100 || smooth[at(14, 64, 2)] > 100)
    throw std::runtime_error(
        "Ambilight temporal smoothing did not retain history");
  for (int i = 0; i < 150; ++i)
    smooth = run();
  if (smooth[at(14, 64, 2)] < 100 || smooth[at(14, 64, 0)] > 20)
    throw std::runtime_error("Ambilight temporal smoothing did not converge");
  ++checks;
  glow.reset();
  auto reset = run();
  if (reset[at(14, 64, 0)] > 2 || reset[at(14, 64, 2)] < 100)
    throw std::runtime_error("Ambilight retained colours from previous movie");
  ++checks;
  // Capture samples both eyes, in linear light, using the real video shader.
  VulkanRenderer encoded(gpu);
  encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM, assets);
  for (int stereo : {0, 1, 2}) {
    auto packed =
        encoded.createTarget(stereo == 1 ? 128 : 64, stereo == 2 ? 72 : 36);
    DrawList d;
    d.rect({0, 0, 128, 72}, {1, 0, 0, 1});
    if (stereo)
      d.rect({stereo == 1 ? 64.f : 0, stereo == 2 ? 36.f : 0, 64, 36},
             {0, 0, 1, 1});
    encoded.draw(packed, d);
    readable(gpu, packed.image);
    {
      ProjectionProbe capture(gpu, assets, packed.view);
      VideoView mapping;
      mapping.kind = 6;
      VideoColor color;
      color.transfer = 1;
      capture.run(
          source.image, {64, 36},
          video_push({0, 0, 1, 1}, ImageSettings{}, stereo, mapping, color),
          false);
      auto pixels = renderer.readback(source);
      const auto n = (18 * 64 + 32) * 4;
      if (std::abs(int(pixels[n]) - (stereo ? 188 : 255)) > 1 ||
          pixels[n + 1] > 1 ||
          std::abs(int(pixels[n + 2]) - (stereo ? 188 : 0)) > 1)
        throw std::runtime_error(
            "Ambilight stereo capture was not a linear-light average");
    }
    encoded.destroyTarget(packed);
    ++checks;
  }
  renderer.destroyTarget(source);
  renderer.destroyTarget(target);
  vkDestroyImage(gpu.device, output, nullptr);
  vkFreeMemory(gpu.device, memory, nullptr);
  std::cout << "{\"ambilight_checks\":" << checks
            << ",\"last_glow_gpu_ms\":" << glow.gpu_ms().value_or(-1) << "}\n";
  return checks;
}
