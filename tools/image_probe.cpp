// Runs the same processing shaders as the player and checks every output pixel.
#include "cyvr/player/gpu_timer.hpp"
#include "cyvr/player/image_processor.hpp"
#include "projection_probe.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace cyvr;
using namespace cyvr::player;
std::optional<double> combined(VulkanContext &gpu, ImageProcessor &first,
                               ImageProcessor &second, VkImage target,
                               const ImageSettings &settings) {
  VkCommandPool pool{};
  VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  p.queueFamilyIndex = gpu.queueFamily;
  vkCheck(vkCreateCommandPool(gpu.device, &p, nullptr, &pool), "combined pool");
  VkCommandBuffer cmd{};
  VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  a.commandPool = pool;
  a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  a.commandBufferCount = 1;
  vkCheck(vkAllocateCommandBuffers(gpu.device, &a, &cmd), "combined command");
  VkFence fence{};
  VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCheck(vkCreateFence(gpu.device, &f, nullptr, &fence), "combined fence");
  GpuTimer timer(gpu);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkCheck(vkBeginCommandBuffer(cmd, &begin), "combined begin");
  timer.begin(cmd);
  first.record(cmd, second.source_image(), ImageSettings{}, 0, true);
  second.record(cmd, target, settings, 0, false);
  timer.end(cmd);
  vkCheck(vkEndCommandBuffer(cmd), "combined end");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkCheck(vkQueueSubmit(gpu.queue, 1, &submit, fence), "combined submit");
  vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
          "combined completion");
  auto result = timer.result();
  vkDestroyFence(gpu.device, fence, nullptr);
  vkDestroyCommandPool(gpu.device, pool, nullptr);
  return result;
}
void readable(VulkanContext &gpu, VkImage image) {
  VkCommandPool pool{};
  VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  p.queueFamilyIndex = gpu.queueFamily;
  vkCheck(vkCreateCommandPool(gpu.device, &p, nullptr, &pool), "probe pool");
  VkCommandBuffer cmd{};
  VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  a.commandPool = pool;
  a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  a.commandBufferCount = 1;
  vkCheck(vkAllocateCommandBuffers(gpu.device, &a, &cmd), "probe command");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkCheck(vkBeginCommandBuffer(cmd, &begin), "probe begin");
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  vkCheck(vkEndCommandBuffer(cmd), "probe end");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkCheck(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE), "probe submit");
  vkCheck(vkDeviceWaitIdle(gpu.device), "probe wait");
  vkDestroyCommandPool(gpu.device, pool, nullptr);
}
#include "ambilight_probe.hpp"
#include "vr180_zoom_probe.hpp"
int main(int argc, char **argv) {
  try {
    if (argc != 2 && argc != 3)
      throw std::runtime_error("ASSETS_DIRECTORY");
    VulkanContext gpu;
    gpu.createStandalone();
    AssetReader assets = [&](const std::string &name) {
      std::ifstream f(std::filesystem::path(argv[1]) / name, std::ios::binary);
      if (!f)
        throw std::runtime_error(name);
      return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
    };
    VulkanRenderer renderer(gpu);
    renderer.initialize(VK_FORMAT_R8G8B8A8_SRGB, assets);
    int checks = check_ambilight(gpu,renderer,assets)+check_vr180_zoom(gpu,renderer,assets);
    for (int stereo = 0; stereo < 3; ++stereo)
      for (int quality = 0; quality < 4; ++quality) {
        ImageSettings settings;
        settings.quality = quality;
        const VideoExtent input{stereo == 1 ? 128u : 64u,
                                stereo == 2 ? 128u : 64u};
        const auto output =
            processed_extent(input.width, input.height, settings, stereo);
        auto processor = std::make_unique<ImageProcessor>(
            gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, output, quality);
        auto source = renderer.createTarget(input.width, input.height,
                                            processor->source_image());
        auto target = renderer.createTarget(output.width, output.height);
        DrawList d;
        d.rect({0, 0, float(input.width), float(input.height)},
               Color::hex(0xff0000));
        if (stereo)
          d.rect({stereo == 1 ? 64.f : 0.f, stereo == 2 ? 64.f : 0.f, 64, 64},
                 Color::hex(0x0000ff));
        renderer.draw(source, d);
        readable(gpu, source.image);
        for (float zoom : {1.f, 2.5f}) {
          settings.zoom = zoom;
          settings.pan_x = .9f;
          settings.pan_y = -.9f;
          processor->process(target.image, settings, stereo, false);
          auto pixels = renderer.readback(target);
          for (uint32_t y = 0; y < output.height; ++y)
            for (uint32_t x = 0; x < output.width; ++x) {
              const bool second = stereo == 1
                                      ? x >= output.width / 2
                                      : stereo == 2 && y >= output.height / 2;
              const auto n = (std::size_t(y) * output.width + x) * 4;
              if (pixels[n + (second ? 2 : 0)] < 250 || pixels[n + 1] > 2 ||
                  pixels[n + (second ? 0 : 2)] > 2)
                throw std::runtime_error(
                    "Eye isolation / constant preservation failed: quality=" +
                    std::to_string(quality) +
                    " stereo=" + std::to_string(stereo) +
                    " x=" + std::to_string(x) + " y=" + std::to_string(y));
            }
          ++checks;
        }
        renderer.destroyTarget(source);
        processor.reset();
        renderer.destroyTarget(target);
      }
    {
      ImageSettings settings;
      auto processor = std::make_unique<ImageProcessor>(
          gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, VideoExtent{64, 64},
          VideoExtent{64, 64});
      auto source = renderer.createTarget(64, 64, processor->source_image()),
           target = renderer.createTarget(64, 64);
      DrawList d;
      d.rect({0, 0, 64, 64}, {.21586f, .21586f, .21586f, 1});
      renderer.draw(source, d);
      readable(gpu, source.image);
      processor->process(target.image, settings, 0, false);
      auto native = renderer.readback(target);
      settings.exposure = 1;
      processor->process(target.image, settings, 0, false);
      auto exposed = renderer.readback(target);
      if (std::abs(int(native[0]) - 128) > 1 || exposed[0] < 174 ||
          exposed[0] > 177)
        throw std::runtime_error("Neutral/exposure transfer failed");
      ++checks;
      settings = ImageSettings{};
      settings.saturation = 0;
      d.clear();
      d.rect({0, 0, 64, 64}, {1, 0, 0, 1});
      renderer.draw(source, d);
      readable(gpu, source.image);
      processor->process(target.image, settings, 0, false);
      auto gray = renderer.readback(target);
      if (std::abs(int(gray[0]) - int(gray[1])) > 1 ||
          std::abs(int(gray[1]) - int(gray[2])) > 1)
        throw std::runtime_error("Saturation correction failed");
      ++checks;
      settings = ImageSettings{};
      settings.zoom = 4;
      d.clear();
      d.rect({0, 0, 64, 64}, {1, 0, 0, 1});
      d.rect({16, 16, 32, 32}, {0, 1, 0, 1});
      renderer.draw(source, d);
      readable(gpu, source.image);
      processor->process(target.image, settings, 0, false);
      auto zoom = renderer.readback(target);
      for (std::size_t n = 0; n < zoom.size(); n += 4)
        if (zoom[n] > 1 || zoom[n + 1] < 254 || zoom[n + 2] > 1)
          throw std::runtime_error("Digital zoom did not crop the center");
      ++checks;
      renderer.destroyTarget(source);
      processor.reset();
      renderer.destroyTarget(target);
    }
    for (int quality = 1; quality <= 3; ++quality) {
      ImageSettings settings;
      settings.quality = quality;
      const VideoExtent input{64, 64};
      const auto output = processed_extent(64, 64, settings, 0);
      auto processor = std::make_unique<ImageProcessor>(
          gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, output, quality);
      auto source = renderer.createTarget(64, 64, processor->source_image()),
           target = renderer.createTarget(output.width, output.height);
      DrawList d;
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
          const float c = .4f + .1f * ((x * 13 + y * 37 + x * y) % 23) / 22.f;
          d.rect({float(x), float(y), 1, 1}, {c, c, c, 1});
        }
      renderer.draw(source, d);
      readable(gpu, source.image);
      settings.sharpness = 0;
      processor->process(target.image, settings, 0, false);
      const auto soft = renderer.readback(target);
      settings.sharpness = 1;
      processor->process(target.image, settings, 0, false);
      const auto sharp = renderer.readback(target);
      unsigned different = 0;
      for (std::size_t n = 0; n < soft.size(); n += 4)
        if (soft[n] != sharp[n])
          ++different;
      if (!different)
        throw std::runtime_error("Sharpness control had no effect: quality=" +
                                 std::to_string(quality));
      ++checks;
      renderer.destroyTarget(source);
      processor.reset();
      renderer.destroyTarget(target);
    }
    for (int quality = 1; quality <= 3; ++quality) {
      ImageSettings settings;
      settings.quality = quality;
      settings.exposure = .2f;
      settings.sharpness = .8f;
      const VideoExtent input{64, 64},
          output{quality == 1 ? 64u : 96u, quality == 1 ? 64u : 96u};
      auto first = std::make_unique<ImageProcessor>(
          gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, input);
      auto second = std::make_unique<ImageProcessor>(
          gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, output, quality);
      auto source = renderer.createTarget(64, 64, first->source_image()),
           target = renderer.createTarget(output.width, output.height);
      DrawList pattern;
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
          const float c = .1f + .7f * ((x * 13 + y * 7) % 29) / 28.f;
          pattern.rect({float(x), float(y), 1, 1}, {c, 1 - c, .3f, 1});
        }
      renderer.draw(source, pattern);
      readable(gpu, source.image);
      first->process(second->source_image(), ImageSettings{}, 0, true);
      second->process(target.image, settings, 0, false);
      const auto reference = renderer.readback(target);
      const auto ms = combined(gpu, *first, *second, target.image, settings);
      if (renderer.readback(target) != reference)
        throw std::runtime_error("Combining GPU passes changed filter pixels");
      if (ms && (!std::isfinite(*ms) || *ms <= 0))
        throw std::runtime_error("Invalid GPU timestamp result");
      std::cout << "{\"combined_quality\":" << quality
                << ",\"matches_separate_submissions\":true,\"gpu_ms\":";
      if (ms)
        std::cout << *ms;
      else
        std::cout << "null";
      std::cout << "}\n";
      renderer.destroyTarget(source);
      first.reset();
      second.reset();
      renderer.destroyTarget(target);
      ++checks;
    }
    {
      // A flat cinema view introduces transparent pixels before filtering.
      // A constant video must retain its color right up to its alpha edge.
      VulkanRenderer encoded(gpu);
      encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM, assets);
      auto source = encoded.createTarget(128, 128);
      DrawList gray;
      gray.rect({0, 0, 128, 128}, {.5f, .5f, .5f, 1});
      encoded.draw(source, gray);
      readable(gpu, source.image);
      VideoView view;
      view.kind = 2;
      view.origin = {.037f, -.083f, 3.5f};
      view.screen_width = 4;
      view.screen_height = 2.1f;
      const auto push = video_push({0, 0, 1, 1}, ImageSettings{}, 0, view);
      for (int quality = 1; quality <= 3; ++quality) {
        const VideoExtent input{64, 64},
            output{quality == 1 ? 64u : 128u, quality == 1 ? 64u : 128u};
        ImageSettings settings;
        settings.quality = quality;
        auto processor = std::make_unique<ImageProcessor>(
            gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, output, quality);
        auto target = renderer.createTarget(output.width, output.height);
        int bad = 0, visible = 0, worst = 0;
        {
          ProjectionProbe projection(gpu, assets, source.view);
          projection.run(processor->source_image(), input, push, true,
                         [&](VkCommandBuffer cmd) {
                           processor->record(cmd, target.image, settings, 0,
                                             false);
                         });
          const auto pixels = renderer.readback(target);
          const auto center = (std::size_t(output.height / 2) * output.width +
                               output.width / 2) *
                              4;
          const int reference = pixels[center];
          for (std::size_t n = 0; n < pixels.size(); n += 4)
            if (pixels[n + 3] > 200) {
              ++visible;
              const int difference = std::abs(int(pixels[n]) - reference);
              worst = std::max(worst, difference);
              if (difference > 3)
                ++bad;
            }
          if (!visible || pixels[3] != 0)
            throw std::runtime_error("Cinema projection coverage is invalid");
        }
        std::cout << "{\"projection_quality\":" << quality
                  << ",\"edge_color_errors\":" << bad
                  << ",\"worst_edge_error\":" << worst << "}\n";
        processor.reset();
        renderer.destroyTarget(target);
        if (bad)
          throw std::runtime_error(
              "Projected cinema has dark/bright filter fringes: quality=" +
              std::to_string(quality));
        ++checks;
      }
      encoded.destroyTarget(source);
    }
    for (int packing : {1, 2}) {
      VulkanRenderer encoded(gpu);
      encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM, assets);
      auto source = encoded.createTarget(packing == 1 ? 256 : 128,
                                         packing == 2 ? 256 : 128);
      constexpr unsigned colors[2][4]{{0xff0000, 0x00ff00, 0x0000ff, 0xffff00},
                                      {0x00ffff, 0xff00ff, 0xffffff, 0x404040}};
      DrawList pattern;
      for (int eye = 0; eye < 2; ++eye)
        for (int cell = 0; cell < 4; ++cell) {
          const auto rgb = colors[eye][cell];
          pattern.rect(
              {float((packing == 1 ? eye * 128 : 0) + (cell % 2) * 64),
               float((packing == 2 ? eye * 128 : 0) + (cell / 2) * 64), 64, 64},
              {float((rgb >> 16) & 255) / 255, float((rgb >> 8) & 255) / 255,
               float(rgb & 255) / 255, 1});
        }
      encoded.draw(source, pattern);
      readable(gpu, source.image);
      for (int quality = 0; quality <= 3; ++quality) {
        const VideoExtent input{quality < 2 ? 128u : 64u,
                                quality < 2 ? 128u : 64u},
            output{128, 128};
        ImageSettings settings;
        settings.quality = quality;
        auto processor = quality ? std::make_unique<ImageProcessor>(
                                       gpu, VK_FORMAT_R8G8B8A8_SRGB, assets,
                                       input, output, quality)
                                 : nullptr;
        auto target = renderer.createTarget(128, 128);
        {
          ProjectionProbe projection(gpu, assets, source.view);
          for (int kind = 1; kind <= 4; ++kind)
            for (int eye = 0; eye < 2; ++eye)
              for (bool back : {false, true}) {
                if (back && kind < 3)
                  continue;
                VideoView view;
                view.kind = kind;
                view.eye = eye;
                view.origin = {0, 0, 3.5f};
                view.screen_height = 2.1f;
                if (back)
                  view.orientation = {0, 1, 0, 0};
                const auto push =
                    video_push({0, 0, 1, 1}, ImageSettings{}, packing, view);
                if (processor)
                  projection.run(processor->source_image(), input, push, true,
                                 [&](VkCommandBuffer cmd) {
                                   processor->record(cmd, target.image,
                                                     settings, 0, false);
                                 });
                else
                  projection.run(target.image, output, push);
                const auto pixels = renderer.readback(target);
                if (back && kind == 3) {
                  for (std::size_t n = 3; n < pixels.size(); n += 4)
                    if (pixels[n])
                      throw std::runtime_error(
                          "VR180 rear hemisphere became visible");
                } else
                  for (int cell = 0; cell < 4; ++cell) {
                    const int x = kind == 2 ? (cell % 2 ? 86 : 42)
                                            : (cell % 2 ? 96 : 32),
                              y = kind == 2 ? (cell / 2 ? 73 : 55)
                                            : (cell / 2 ? 96 : 32);
                    const auto n = (std::size_t(y) * 128 + x) * 4;
                    const int expectedCell = back ? (cell ^ 1) : cell;
                    const unsigned expected = colors[eye][expectedCell];
                    for (int channel = 0; channel < 3; ++channel) {
                      const int encodedByte =
                          (expected >> (16 - channel * 8)) & 255;
                      const double v = encodedByte / 255.;
                      const double linear =
                          v < .081 ? v / 4.5
                                   : std::pow((v + .099) / 1.099, 1 / .45);
                      const int expectedByte = int(std::round(
                          255 *
                          (linear <= .0031308
                               ? 12.92 * linear
                               : 1.055 * std::pow(linear, 1 / 2.4) - .055)));
                      if (std::abs(int(pixels[n + channel]) - expectedByte) >
                              3 ||
                          pixels[n + 3] < 250)
                        throw std::runtime_error(
                            "Projection geometry/eye/color failed: quality=" +
                            std::to_string(quality) +
                            " packing=" + std::to_string(packing) +
                            " kind=" + std::to_string(kind) +
                            " eye=" + std::to_string(eye) +
                            " back=" + std::to_string(back) +
                            " cell=" + std::to_string(cell));
                    }
                  }
                ++checks;
              }
        }
        processor.reset();
        renderer.destroyTarget(target);
      }
      encoded.destroyTarget(source);
    }
    {
      // Half-width stereo/non-square source pixels: expanding horizontally
      // must not first throw away half of the available vertical detail.
      VulkanRenderer encoded(gpu);
      encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM, assets);
      auto source = encoded.createTarget(128, 128);
      DrawList stripes;
      for (int y = 0; y < 128; ++y) {
        const float c = y % 2 ? 1.f : 0.f;
        stripes.rect({0, float(y), 128, 1}, {c, c, c, 1});
      }
      encoded.draw(source, stripes);
      readable(gpu, source.image);
      for (int quality : {2, 3}) {
        ImageSettings settings;
        settings.quality = quality;
        const auto plan =
            view_processing_plan({256, 128}, {128, 128}, settings);
        auto processor = std::make_unique<ImageProcessor>(
            gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, plan.input, plan.output,
            quality);
        auto target =
            renderer.createTarget(plan.output.width, plan.output.height);
        int low = 255, high = 0;
        {
          ProjectionProbe projection(gpu, assets, source.view);
          VideoView view;
          view.kind = 1;
          projection.run(processor->source_image(), plan.input,
                         video_push({0, 0, 1, 1}, ImageSettings{}, 0, view),
                         true, [&](VkCommandBuffer cmd) {
                           processor->record(cmd, target.image, settings, 0,
                                             false);
                         });
          const auto pixels = renderer.readback(target);
          for (int y = 8; y < 120; ++y) {
            const int c = pixels[(std::size_t(y) * 256 + 128) * 4];
            low = std::min(low, c);
            high = std::max(high, c);
          }
        }
        std::cout << "{\"anisotropic_quality\":" << quality
                  << ",\"input_width\":" << plan.input.width
                  << ",\"input_height\":" << plan.input.height
                  << ",\"vertical_contrast\":" << high - low << "}\n";
        processor.reset();
        renderer.destroyTarget(target);
        if (high - low < 128)
          throw std::runtime_error(
              "Upscaling discarded available vertical source detail: quality=" +
              std::to_string(quality));
        ++checks;
      }
      encoded.destroyTarget(source);
    }
    // Aim rays at analytically known points on a cylinder, independently of the
    // shader's quadratic intersection. Check UV registration, parallax, packing,
    // source eye boundaries and transparent coverage through every filter.
    for(int packing=0;packing<3;++packing){
      VulkanRenderer encoded(gpu);encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM,assets);
      auto source=encoded.createTarget(packing==1?256:128,packing==2?256:128);
      constexpr unsigned colors[2][4]{{0xff0000,0x00ff00,0x0000ff,0xffff00},{0x00ffff,0xff00ff,0xffffff,0x404040}};
      DrawList pattern;
      for(int eye=0;eye<(packing?2:1);++eye)for(int cell=0;cell<4;++cell){
        const auto rgb=colors[eye][cell];pattern.rect({float((packing==1?eye*128:0)+(cell%2)*64),float((packing==2?eye*128:0)+(cell/2)*64),64,64},{float((rgb>>16)&255)/255,float((rgb>>8)&255)/255,float(rgb&255)/255,1});
      }
      encoded.draw(source,pattern);readable(gpu,source.image);
      for(float degrees:{0.f,.1f,30.f,90.f,120.f})for(int quality=0;quality<4;++quality){
        ImageSettings settings;settings.quality=quality;
        const VideoExtent output{64,64},input=quality>=2?VideoExtent{32,32}:output;
        auto target=renderer.createTarget(output.width,output.height);
        auto processor=quality?std::make_unique<ImageProcessor>(gpu,VK_FORMAT_R8G8B8A8_SRGB,assets,input,output,quality):nullptr;
        {
          ProjectionProbe projection(gpu,assets,source.view);
          for(int eye=0;eye<(packing?2:1);++eye)for(int cell=0;cell<5;++cell){
            const float u=cell==4?1.2f:(cell%2?.75f:.25f),v=cell/2?.75f:.25f;
            const float angle=degrees*pi/180,radius=degrees?4/angle:0;
            const Vec3 point{degrees?radius*std::sin((u-.5f)*angle):4*(u-.5f),2.25f*(.5f-v),degrees?radius*(1-std::cos((u-.5f)*angle)):0};
            VideoView view;view.kind=5;view.eye=eye;view.screen_width=4;view.screen_height=2.25f;view.screen_radius=radius;view.origin={eye?.032f:-.032f,.1f,3.5f};
            const auto ray=point-view.origin;const auto x=ray.x/-ray.z,y=ray.y/-ray.z;
            view.fov[0]=x-.005f;view.fov[1]=x+.005f;view.fov[2]=y+.005f;view.fov[3]=y-.005f;
            auto push=video_push({0,0,1,1},ImageSettings{},packing,view);
            if(processor)projection.run(processor->source_image(),input,push,true,[&](VkCommandBuffer cmd){processor->record(cmd,target.image,settings,0,false);});
            else projection.run(target.image,output,push);
            const auto pixels=renderer.readback(target);const auto at=(32*64+32)*4;
            if(cell==4){if(pixels[at+3]>2)throw std::runtime_error("Curved screen leaked beyond its arc");}
            else{
              for(int channel=0;channel<3;++channel){
                const auto byte=(colors[eye][cell]>>(16-channel*8))&255;const double e=byte/255.;
                const auto linear=e<.081?e/4.5:std::pow((e+.099)/1.099,1/.45);
                const auto expected=std::lround(255*(linear<=.0031308?12.92*linear:1.055*std::pow(linear,1/2.4)-.055));
                if(std::abs(int(pixels[at+channel])-expected)>3 || pixels[at+3]<250)throw std::runtime_error("Curved screen UV/eye mismatch: degrees="+std::to_string(degrees)+" filter="+std::to_string(quality)+" packing="+std::to_string(packing)+" cell="+std::to_string(cell));
              }
            }
            ++checks;
          }
        }
        processor.reset();renderer.destroyTarget(target);
      }
      encoded.destroyTarget(source);
    }
    // Environment dimming must reach full black without changing video controls.
    {
      auto target=renderer.createTarget(64,64);DrawList empty;Settings settings;
      for(auto background:{Background::Black,Background::Dark,Background::Studio,Background::Night}){
        settings.background=background;settings.environmentDim=0;
        auto env=environmentView({{0,1.6f,0},{}},-.7f,.7f,.7f,-.7f,settings);
        renderer.draw(target,empty,&env);const auto pixels=renderer.readback(target);
        for(std::size_t i=0;i<pixels.size();i+=4)if(pixels[i]||pixels[i+1]||pixels[i+2])throw std::runtime_error("Environment dimmer did not reach black");
        ++checks;
      }
      settings.background=Background::Black;settings.environmentDim=1;
      auto env=environmentView({{0,1.6f,0},{}},-.7f,.7f,.7f,-.7f,settings);renderer.draw(target,empty,&env);
      const auto pixels=renderer.readback(target);for(std::size_t i=0;i<pixels.size();i+=4)if(pixels[i]||pixels[i+1]||pixels[i+2])throw std::runtime_error("Black cinema emitted background light");
      ++checks;renderer.destroyTarget(target);
    }
    // PQ/HLG reference gray steps through the actual video shader and all filters.
    for(int transfer:{6,7}){
      VulkanRenderer encoded(gpu);encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM,assets);
      auto source=encoded.createTarget(160,32);
      DrawList steps;
      const int levels[]{0,64,128,192,255};
      for(int i=0;i<5;++i){const float v=levels[i]/255.f;steps.rect({float(i*32),0,32,32},{v,v,v,1});}
      encoded.draw(source,steps);readable(gpu,source.image);
      for(int quality=0;quality<4;++quality){
        ImageSettings settings;settings.quality=quality;
        VideoColor color;color.standard=6;color.transfer=transfer;color.peak_nits=1000;
        VideoView view;view.kind=1;
        const auto push=video_push({0,0,1,1},ImageSettings{},0,view,color);
        auto target=renderer.createTarget(160,32);
        std::unique_ptr<ImageProcessor> processor;
        {
          ProjectionProbe projection(gpu,assets,source.view);
          if(quality){
            const VideoExtent input=quality==1?VideoExtent{160,32}:VideoExtent{80,16};
            processor=std::make_unique<ImageProcessor>(gpu,VK_FORMAT_R8G8B8A8_SRGB,assets,input,VideoExtent{160,32},quality);
            projection.run(processor->source_image(),input,push,true,[&](VkCommandBuffer cmd){processor->record(cmd,target.image,settings,0,false);});
          }else projection.run(target.image,{160,32},push);
          const auto pixels=renderer.readback(target);
          int previous=-1;
          for(int i=0;i<5;++i){
            const double e=levels[i]/255.;double nits;
            if(transfer==6){const auto p=std::pow(e,32./2523.);nits=10000*std::pow(std::max(p-3424./4096.,0.)/(2413./128.-2392./128.*p),16384./2610.);}
            else{const auto scene=e<=.5?e*e/3.:(std::exp((e-.55991073)/.17883277)+.28466892)/12.;nits=1000*std::pow(scene,1.2);}
            const auto x=nits/203.,white=1000./203.;
            const auto linear=std::clamp(x*(1+x/(white*white))/(1+x),0.,1.);
            const int expected=int(std::round(255*(linear<=.0031308?12.92*linear:1.055*std::pow(linear,1./2.4)-.055)));
            const auto at=(16*160+i*32+16)*4;
            for(int channel=0;channel<3;++channel)
              if(std::abs(int(pixels[at+channel])-expected)>3)throw std::runtime_error("HDR PQ/HLG reference mismatch: transfer="+std::to_string(transfer)+" filter="+std::to_string(quality)+" expected="+std::to_string(expected)+" got="+std::to_string(pixels[at+channel]));
            if(int(pixels[at])<previous || pixels[at+3]!=255)throw std::runtime_error("HDR gray steps lost ordering or coverage");
            previous=pixels[at];++checks;
          }
        }
        processor.reset();renderer.destroyTarget(target);
      }
      encoded.destroyTarget(source);
    }
    if (argc == 3 && std::string(argv[2]) == "--benchmark") {
      for (int quality = 1; quality <= 3; ++quality) {
        ImageSettings settings;
        settings.quality = quality;
        settings.upscale = 2;
        const VideoExtent input{1920, 1080};
        const auto output = processed_extent(1920, 1080, settings, 0);
        auto processor = std::make_unique<ImageProcessor>(
            gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, output, quality);
        auto source =
                 renderer.createTarget(1920, 1080, processor->source_image()),
             target = renderer.createTarget(output.width, output.height);
        DrawList d;
        for (int x = 0; x < 1920; x += 8) {
          const float c = .1f + .8f * ((x / 8) % 7) / 6.f;
          d.rect({float(x), 0, 8, 1080}, {c, 1 - c, .2f, 1});
        }
        renderer.draw(source, d);
        readable(gpu, source.image);
        std::vector<double> ms, gpuTimes;
        for (int n = 0; n < 15; ++n) {
          const auto start = std::chrono::steady_clock::now();
          processor->process(target.image, settings, 0, false);
          if (n >= 3) {
            ms.push_back(std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count());
            if (auto gpuMs = processor->gpu_ms())
              gpuTimes.push_back(*gpuMs);
          }
        }
        std::sort(ms.begin(), ms.end());
        std::cout << "{\"quality\":" << quality
                  << ",\"input\":\"1920x1080\",\"output\":\"" << output.width
                  << "x" << output.height
                  << "\",\"filter_wall_median_ms\":" << ms[ms.size() / 2]
                  << ",\"filter_wall_max_ms\":" << ms.back()
                  << ",\"filter_gpu_median_ms\":";
        std::sort(gpuTimes.begin(), gpuTimes.end());
        if (gpuTimes.empty())
          std::cout << "null";
        else
          std::cout << gpuTimes[gpuTimes.size() / 2];
        std::cout << "}\n";
        renderer.destroyTarget(source);
        processor.reset();
        renderer.destroyTarget(target);
      }
    }
    std::cout << "{\"gpu_image_checks\":" << checks
              << ",\"eye_isolation\":true,\"neutral_transfer\":true,"
                 "\"corrections\":true,\"zoom\":true}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
