#include "cyvr/player/android/video_renderer.hpp"
#include "cyvr/player/gpu_timer.hpp"
#include "cyvr/player/image_processor.hpp"
#include <chrono>
#include <cstring>
#include <map>
#include <unistd.h>

namespace cyvr::player {
using namespace cyvr;
namespace {
struct AcquireSync {
  VkDevice device;
  PFN_vkDestroySemaphore destroy;
  android::ImageLease &lease;
  VkSemaphore semaphore{};
  int fd{-1}, backup{-1};
  ~AcquireSync() {
    if (fd >= 0)
      lease.release_after_gpu(fd);
    else if (backup >= 0) {
      lease.release_after_gpu(backup);
      backup = -1;
    }
    if (backup >= 0)
      ::close(backup);
    if (semaphore)
      destroy(device, semaphore, nullptr);
  }
};
struct PendingRead {
  VkDevice device;
  bool submitted{};
  ~PendingRead() {
    if (submitted)
      vkDeviceWaitIdle(device);
  }
};
} // namespace
struct VideoRenderer::Impl {
  VulkanContext &gpu;
  VkFormat format;
  GpuTimer timer;
  std::optional<double> last_gpu_ms;
  VideoRenderTimings timings;
  VkRenderPass pass{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkShaderModule vertex{}, fragment{};
  VkPipelineCache pipeline_cache{};
  PFN_vkDestroyPipelineCache destroy_pipeline_cache{};
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties{};
  PFN_vkCreateSamplerYcbcrConversion create_conversion{};
  PFN_vkDestroySamplerYcbcrConversion destroy_conversion{};
  PFN_vkCreateSemaphore create_semaphore{};
  PFN_vkDestroySemaphore destroy_semaphore{};
  PFN_vkImportSemaphoreFdKHR import_fence{};
  struct Imported {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkSamplerYcbcrConversion conversion{};
    VkSampler sampler{};
    VkDescriptorSetLayout layout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    std::uint32_t width{}, height{};
    std::uint64_t last_use{};
  };
  struct Output {
    VkImageView view{};
    VkFramebuffer framebuffer{};
  };
  std::map<AHardwareBuffer *, Imported> imported;
  std::map<VkImage, Output> outputs;
  std::uint64_t count{}, epoch{};
  std::uint64_t import_use{};
  std::unique_ptr<ImageProcessor> processor;
  VideoExtent processor_input{}, processor_output{};
  int processor_quality{-1};
  using Push = VideoPush;
  explicit Impl(VulkanContext &g, VkFormat f, const AssetReader &read)
      : gpu(g), format(f), timer(g), assets(read) {
    auto get = [&](const char *name) {
      auto p = vkGetInstanceProcAddr(gpu.instance, name);
      if (!p)
        throw std::runtime_error(std::string("Vulkan function missing: ") +
                                 name);
      return p;
    };
    properties =
        reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            get("vkGetAndroidHardwareBufferPropertiesANDROID"));
    create_conversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
        get("vkCreateSamplerYcbcrConversion"));
    destroy_conversion = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
        get("vkDestroySamplerYcbcrConversion"));
    create_semaphore =
        reinterpret_cast<PFN_vkCreateSemaphore>(get("vkCreateSemaphore"));
    destroy_semaphore =
        reinterpret_cast<PFN_vkDestroySemaphore>(get("vkDestroySemaphore"));
    import_fence = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        get("vkImportSemaphoreFdKHR"));
    const auto create_pipeline_cache =
        reinterpret_cast<PFN_vkCreatePipelineCache>(
            get("vkCreatePipelineCache"));
    destroy_pipeline_cache = reinterpret_cast<PFN_vkDestroyPipelineCache>(
        get("vkDestroyPipelineCache"));
    VkPipelineCacheCreateInfo cache_info{
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCheck(create_pipeline_cache(gpu.device, &cache_info, nullptr,
                                  &pipeline_cache),
            "video pipeline cache");
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    vkCheck(vkCreateRenderPass(gpu.device, &rp, nullptr, &pass),
            "video render pass");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = gpu.queueFamily;
    vkCheck(vkCreateCommandPool(gpu.device, &cp, nullptr, &pool),
            "video command pool");
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(gpu.device, &ca, &command),
            "video commands");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(gpu.device, &fc, nullptr, &fence), "video fence");
    auto shader = [&](const char *name, VkShaderModule &module) {
      auto bytes = read(name);
      std::vector<std::uint32_t> words((bytes.size() + 3) / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      VkShaderModuleCreateInfo info{
          VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = bytes.size();
      info.pCode = words.data();
      vkCheck(vkCreateShaderModule(gpu.device, &info, nullptr, &module), name);
    };
    shader("video.vert.spv", vertex);
    shader("video.frag.spv", fragment);
  }
  void destroy_import(Imported &i) {
    vkDestroyPipeline(gpu.device, i.pipeline, nullptr);
    vkDestroyPipelineLayout(gpu.device, i.pipeline_layout, nullptr);
    vkDestroyDescriptorPool(gpu.device, i.pool, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, i.layout, nullptr);
    vkDestroyImageView(gpu.device, i.view, nullptr);
    vkDestroySampler(gpu.device, i.sampler, nullptr);
    destroy_conversion(gpu.device, i.conversion, nullptr);
    vkDestroyImage(gpu.device, i.image, nullptr);
    vkFreeMemory(gpu.device, i.memory, nullptr);
  }
  void clear_imports() {
    for (auto &pair : imported)
      destroy_import(pair.second);
    imported.clear();
  }
  ~Impl() {
    vkDeviceWaitIdle(gpu.device);
    clear_imports();
    if (pipeline_cache)
      destroy_pipeline_cache(gpu.device, pipeline_cache, nullptr);
    for (auto &pair : outputs) {
      vkDestroyFramebuffer(gpu.device, pair.second.framebuffer, nullptr);
      vkDestroyImageView(gpu.device, pair.second.view, nullptr);
    }
    vkDestroyShaderModule(gpu.device, vertex, nullptr);
    vkDestroyShaderModule(gpu.device, fragment, nullptr);
    vkDestroyFence(gpu.device, fence, nullptr);
    vkDestroyCommandPool(gpu.device, pool, nullptr);
    vkDestroyRenderPass(gpu.device, pass, nullptr);
  }
  Imported &import(AHardwareBuffer *buffer) {
    auto found = imported.find(buffer);
    if (found != imported.end()) {
      found->second.last_use = ++import_use;
      return found->second;
    }
    ++timings.imports;
    // Decoder buffer pools can exceed twelve images. Flushing the complete
    // working set made every subsequent image compile its pipeline again.
    // Retain a bounded working set and evict only its least-recently-used
    // entry. convert() has completed the previous GPU read before reaching this
    // point.
    if (imported.size() >= 64) {
      const auto oldest = std::min_element(
          imported.begin(), imported.end(), [](const auto &a, const auto &b) {
            return a.second.last_use < b.second.last_use;
          });
      destroy_import(oldest->second);
      imported.erase(oldest);
      ++timings.cache_evictions;
    }
    auto &out = imported[buffer];
    out.last_use = ++import_use;
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    out.width = desc.width;
    out.height = desc.height;
    VkAndroidHardwareBufferFormatPropertiesANDROID fmt{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID props{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    props.pNext = &fmt;
    vkCheck(properties(gpu.device, buffer, &props), "AHB properties");
    if (!(fmt.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      throw std::runtime_error(
          "This AHardwareBuffer cannot be sampled by Vulkan");
    VkExternalFormatANDROID ext{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    ext.externalFormat =
        fmt.format == VK_FORMAT_UNDEFINED ? fmt.externalFormat : 0;
    VkExternalMemoryImageCreateInfo external{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.pNext = ext.externalFormat ? &ext : nullptr;
    external.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.pNext = &external;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = fmt.format;
    image.extent = {desc.width, desc.height, 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateImage(gpu.device, &image, nullptr, &out.image),
            "AHB image");
    VkImportAndroidHardwareBufferInfoANDROID handle{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    handle.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &handle;
    dedicated.image = out.image;
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.pNext = &dedicated;
    memory.allocationSize = props.allocationSize;
    memory.memoryTypeIndex = gpu.memoryType(props.memoryTypeBits, 0);
    vkCheck(vkAllocateMemory(gpu.device, &memory, nullptr, &out.memory),
            "AHB import memory");
    vkCheck(vkBindImageMemory(gpu.device, out.image, out.memory, 0),
            "AHB bind memory");
    VkSamplerYcbcrConversionCreateInfo conversion{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    conversion.pNext = ext.externalFormat ? &ext : nullptr;
    conversion.format = fmt.format;
    conversion.ycbcrModel = fmt.suggestedYcbcrModel;
    conversion.ycbcrRange = fmt.suggestedYcbcrRange;
    conversion.components = fmt.samplerYcbcrConversionComponents;
    conversion.xChromaOffset = fmt.suggestedXChromaOffset;
    conversion.yChromaOffset = fmt.suggestedYChromaOffset;
    conversion.chromaFilter = VK_FILTER_NEAREST;
    const bool rgb = fmt.format == VK_FORMAT_R8G8B8A8_UNORM ||
                     fmt.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                     fmt.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (!rgb)
      vkCheck(
          create_conversion(gpu.device, &conversion, nullptr, &out.conversion),
          "AHB YCbCr conversion");
    VkSamplerYcbcrConversionInfo converted{
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    converted.conversion = out.conversion;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.pNext = out.conversion ? &converted : nullptr;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(gpu.device, &sampler, nullptr, &out.sampler),
            "AHB sampler");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.pNext = out.conversion ? &converted : nullptr;
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = fmt.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCheck(vkCreateImageView(gpu.device, &view, nullptr, &out.view),
            "AHB view");
    VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, &out.sampler};
    VkDescriptorSetLayoutCreateInfo layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 1;
    layout.pBindings = &binding;
    vkCheck(
        vkCreateDescriptorSetLayout(gpu.device, &layout, nullptr, &out.layout),
        "AHB descriptor layout");
    // Android external YCbCr formats can consume multiple sampler descriptors.
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    vkCheck(vkCreateDescriptorPool(gpu.device, &dp, nullptr, &out.pool),
            "AHB descriptor pool");
    VkDescriptorSetAllocateInfo ds{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = out.pool;
    ds.descriptorSetCount = 1;
    ds.pSetLayouts = &out.layout;
    vkCheck(vkAllocateDescriptorSets(gpu.device, &ds, &out.set),
            "AHB descriptor set");
    VkDescriptorImageInfo texture{out.sampler, out.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = out.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &texture;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &out.layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    vkCheck(
        vkCreatePipelineLayout(gpu.device, &pl, nullptr, &out.pipeline_layout),
        "video pipeline layout");
    VkPipelineShaderStageCreateInfo stages[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    raster.cullMode = VK_CULL_MODE_NONE;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &ba;
    VkDynamicState dyn[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pipeline{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vi;
    pipeline.pInputAssemblyState = &ia;
    pipeline.pViewportState = &vp;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &ms;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = out.pipeline_layout;
    pipeline.renderPass = pass;
    vkCheck(vkCreateGraphicsPipelines(gpu.device, pipeline_cache, 1, &pipeline,
                                      nullptr, &out.pipeline),
            "video pipeline");
    return out;
  }
  void convert(VkImage target, std::uint32_t width, std::uint32_t height,
               android::ImageLease &lease, bool sampled_output,
               const ImageSettings &settings, int stereo,
               const VideoView &mapping = {},
               const std::function<void(VkCommandBuffer)> &finish = {}) {
    const auto started = std::chrono::steady_clock::now();
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5'000'000'000ull),
            "video completion");
    if (epoch != lease.epoch) {
      if (!imported.empty())
        ++timings.cache_resets;
      clear_imports();
      epoch = lease.epoch;
    }
    const auto import_start = std::chrono::steady_clock::now();
    auto &source = import(lease.hardware_buffer());
    timings.import_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - import_start)
                            .count();
    timings.cached_images = imported.size();
    auto &output = outputs[target];
    if (!output.view) {
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = target;
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCheck(vkCreateImageView(gpu.device, &view, nullptr, &output.view),
              "video target view");
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = pass;
      fb.attachmentCount = 1;
      fb.pAttachments = &output.view;
      fb.width = width;
      fb.height = height;
      fb.layers = 1;
      vkCheck(
          vkCreateFramebuffer(gpu.device, &fb, nullptr, &output.framebuffer),
          "video framebuffer");
    }
    AcquireSync sync{gpu.device, destroy_semaphore, lease};
    // Locals unwind before the lease parameter, also on error paths.
    PendingRead pending_read{gpu.device};
    sync.fd = lease.take_acquire_fence();
    if (sync.fd >= 0) {
      sync.backup = ::dup(sync.fd);
      if (sync.backup < 0)
        throw std::runtime_error(
            "Cannot retain the acquire fence for error recovery");
      VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      vkCheck(create_semaphore(gpu.device, &info, nullptr, &sync.semaphore),
              "video acquire semaphore");
      VkImportSemaphoreFdInfoKHR imported{
          VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
      imported.semaphore = sync.semaphore;
      imported.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
      imported.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      imported.fd = sync.fd;
      const auto result = import_fence(gpu.device, &imported);
      vkCheck(result, "video acquire sync FD");
      sync.fd = -1; // Vulkan consumes the descriptor only after success.
    }
    vkCheck(vkResetCommandBuffer(command, 0), "video reset commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &begin), "video begin");
    timer.begin(command);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = gpu.queueFamily;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = source.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass;
    rp.framebuffer = output.framebuffer;
    rp.renderArea.extent = {width, height};
    vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      source.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            source.pipeline_layout, 0, 1, &source.set, 0,
                            nullptr);
    VkViewport viewport{0, 0, float(width), float(height), 0, 1};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    AImageCropRect crop{};
    if (!lease.image() || AImage_getCropRect(lease.image(), &crop) != AMEDIA_OK)
      crop = {0, 0, int32_t(source.width), int32_t(source.height)};
    const auto push = video_push(
        {float(crop.left) / source.width, float(crop.top) / source.height,
         float(crop.right - crop.left) / source.width,
         float(crop.bottom - crop.top) / source.height},
        settings, stereo, mapping, lease.color);
    vkCmdPushConstants(command, source.pipeline_layout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    if (sampled_output) {
      VkImageMemoryBarrier output_barrier{
          VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      output_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      output_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      output_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      output_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      output_barrier.srcQueueFamilyIndex = output_barrier.dstQueueFamilyIndex =
          VK_QUEUE_FAMILY_IGNORED;
      output_barrier.image = target;
      output_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &output_barrier);
    }
    barrier.srcQueueFamilyIndex = gpu.queueFamily;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    if (finish)
      finish(command);
    timer.end(command);
    vkCheck(vkEndCommandBuffer(command), "video end commands");
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (sync.semaphore) {
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &sync.semaphore;
      submit.pWaitDstStageMask = &wait;
    }
    vkCheck(vkResetFences(gpu.device, 1, &fence), "video reset fence");
    vkCheck(vkQueueSubmit(gpu.queue, 1, &submit, fence), "video submit");
    pending_read.submitted = true;
    const auto completion_start = std::chrono::steady_clock::now();
    // Initial conservative synchronization: retain AImage until every GPU read
    // finishes and before xrReleaseSwapchainImage. No CPU pixel access.
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5'000'000'000ull),
            "video release completion");
    pending_read.submitted = false;
    timings.completion_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - completion_start)
            .count();
    last_gpu_ms = timer.result();
    if (sync.backup >= 0) {
      ::close(sync.backup);
      sync.backup = -1;
    }
    ++count;
    timings.total_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  }
  void draw(VkImage target, VideoExtent input, VideoExtent output,
            android::ImageLease &lease, const ImageSettings &settings,
            int stereo, bool sampled) {
    if (!settings.quality) {
      convert(target, output.width, output.height, lease, sampled, settings,
              stereo);
      return;
    }
    if (!processor || processor_input.width != input.width ||
        processor_input.height != input.height ||
        processor_output.width != output.width ||
        processor_output.height != output.height ||
        processor_quality != settings.quality) {
      vkDeviceWaitIdle(gpu.device);
      for (auto &pair : outputs) {
        vkDestroyFramebuffer(gpu.device, pair.second.framebuffer, nullptr);
        vkDestroyImageView(gpu.device, pair.second.view, nullptr);
      }
      outputs.clear();
      processor.reset();
      processor = std::make_unique<ImageProcessor>(gpu, format, assets, input,
                                                   output, settings.quality);
      processor_input = input;
      processor_output = output;
      processor_quality = settings.quality;
    }
    convert(processor->source_image(), input.width, input.height, lease, true,
            ImageSettings{}, stereo, {}, [&](VkCommandBuffer command) {
              processor->record(command, target, settings, stereo, sampled);
            });
  }
  void draw_view(VkImage target, VideoExtent reconstruction, VideoExtent output,
                 android::ImageLease &lease, const ImageSettings &settings,
                 int stereo, const VideoView &mapping, bool sampled) {
    if (!settings.quality) {
      convert(target, output.width, output.height, lease, sampled, settings,
              stereo, mapping);
      return;
    }
    if (!processor || processor_input.width != reconstruction.width ||
        processor_input.height != reconstruction.height ||
        processor_output.width != output.width ||
        processor_output.height != output.height ||
        processor_quality != settings.quality) {
      vkDeviceWaitIdle(gpu.device);
      for (auto &pair : outputs) {
        vkDestroyFramebuffer(gpu.device, pair.second.framebuffer, nullptr);
        vkDestroyImageView(gpu.device, pair.second.view, nullptr);
      }
      outputs.clear();
      processor.reset();
      processor = std::make_unique<ImageProcessor>(
          gpu, format, assets, reconstruction, output, settings.quality);
      processor_input = reconstruction;
      processor_output = output;
      processor_quality = settings.quality;
    }
    auto projectionControls = ImageSettings{};
    projectionControls.zoom = settings.zoom;
    projectionControls.pan_x = settings.pan_x;
    projectionControls.pan_y = settings.pan_y;
    auto corrections = settings;
    corrections.zoom = 1;
    corrections.pan_x = corrections.pan_y = 0;
    convert(processor->source_image(), reconstruction.width,
            reconstruction.height, lease, true, projectionControls, stereo,
            mapping, [&](VkCommandBuffer command) {
              processor->record(command, target, corrections, 0, sampled);
            });
  }
  AssetReader assets;
};
VideoRenderer::VideoRenderer(VulkanContext &g, VkFormat f, const AssetReader &r)
    : impl_(std::make_unique<Impl>(g, f, r)) {}
VideoRenderer::~VideoRenderer() = default;
void VideoRenderer::draw(VkImage i, std::uint32_t w, std::uint32_t h,
                         android::ImageLease lease, bool sampled_output) {
  impl_->convert(i, w, h, lease, sampled_output, ImageSettings{}, 0);
}
void VideoRenderer::draw_retained(VkImage target, VideoExtent source,
                                  VideoExtent output,
                                  android::ImageLease &lease,
                                  const ImageSettings &image, int stereo,
                                  bool sampled) {
  impl_->draw(target, source, output, lease, image, stereo, sampled);
}
void VideoRenderer::draw_view(VkImage target, VideoExtent reconstruction,
                              VideoExtent output, android::ImageLease &lease,
                              const ImageSettings &settings, int stereo,
                              const VideoView &mapping, bool sampled) {
  impl_->draw_view(target, reconstruction, output, lease, settings, stereo,
                   mapping, sampled);
}
std::uint64_t VideoRenderer::presented() const { return impl_->count; }
} // namespace cyvr::player

namespace cyvr::player {
std::optional<double> VideoRenderer::gpu_ms() const {
  return impl_->last_gpu_ms;
}
VideoRenderTimings VideoRenderer::timings() const { return impl_->timings; }
} // namespace cyvr::player
