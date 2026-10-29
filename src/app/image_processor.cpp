#include "cyvr/player/image_processor.hpp"
#include "cyvr/player/gpu_timer.hpp"
#include "cyvr/player/postprocess.hpp"
#include <cstring>
#include <map>
namespace cyvr::player {
using namespace cyvr;
struct ImageProcessor::Impl {
  VulkanContext &gpu;
  VkFormat format;
  VideoExtent input, output;
  int configured_quality{};
  GpuTimer timer;
  std::optional<double> last_gpu_ms;
  struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
  };
  Image source, temp;
  struct Target {
    VkImageView view{};
    VkFramebuffer framebuffer{};
  };
  std::map<VkImage, Target> targets;
  VkRenderPass pass{};
  VkPipelineLayout layout{};
  VkPipeline pipelines[5]{};
  VkDescriptorSetLayout set_layout{};
  VkDescriptorPool descriptors{};
  VkDescriptorSet sets[2]{};
  VkSampler sampler{};
  VkShaderModule vert{}, frag{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  struct alignas(16) Push {
    float controls[4]{0, 1, 1, 1}, view[4]{1, 0, 0, .5f};
    int dimensions[4]{}, mode[4]{};
    std::uint32_t constants[16]{};
  };
  static_assert(sizeof(Push) == 128);
  Impl(VulkanContext &g, VkFormat f, VideoExtent i, VideoExtent o)
      : gpu(g), format(f), input(i), output(o), timer(g) {}
  void image(Image &i, VideoExtent extent) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    vkCheck(vkCreateImage(gpu.device, &info, nullptr, &i.image),
            "filter image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(gpu.device, i.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(gpu.device, &alloc, nullptr, &i.memory),
            "filter memory");
    vkCheck(vkBindImageMemory(gpu.device, i.image, i.memory, 0), "filter bind");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = i.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCheck(vkCreateImageView(gpu.device, &view, nullptr, &i.view),
            "filter image view");
  }
  void initialize(const AssetReader &assets, int quality) {
    configured_quality = quality;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.physical, &properties);
    if (!input.width || !input.height || !output.width || !output.height ||
        input.width > properties.limits.maxImageDimension2D ||
        input.height > properties.limits.maxImageDimension2D ||
        output.width > properties.limits.maxImageDimension2D ||
        output.height > properties.limits.maxImageDimension2D)
      throw std::runtime_error(
          "Display target exceeds this Vulkan device's image dimensions.");
    image(source, input);
    if (quality == 2)
      image(temp, output);
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &a;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    vkCheck(vkCreateRenderPass(gpu.device, &rp, nullptr, &pass), "filter pass");
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(gpu.device, &s, nullptr, &sampler),
            "filter sampler");
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 1;
    sl.pBindings = &b;
    vkCheck(vkCreateDescriptorSetLayout(gpu.device, &sl, nullptr, &set_layout),
            "filter set layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    vkCheck(vkCreateDescriptorPool(gpu.device, &dp, nullptr, &descriptors),
            "filter descriptors");
    const VkDescriptorSetLayout layouts[]{set_layout, set_layout};
    VkDescriptorSetAllocateInfo ds{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = descriptors;
    ds.descriptorSetCount = 2;
    ds.pSetLayouts = layouts;
    vkCheck(vkAllocateDescriptorSets(gpu.device, &ds, sets), "filter sets");
    for (int i = 0; i < 2; ++i) {
      VkDescriptorImageInfo t{sampler, i && temp.view ? temp.view : source.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = sets[i];
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &t;
      vkUpdateDescriptorSets(gpu.device, 1, &w, 0, nullptr);
    }
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    vkCheck(vkCreatePipelineLayout(gpu.device, &pl, nullptr, &layout),
            "filter pipeline layout");
    auto shader = [&](const char *name, VkShaderModule &module) {
      auto bytes = assets(name);
      std::vector<std::uint32_t> words((bytes.size() + 3) / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      VkShaderModuleCreateInfo sh{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      sh.codeSize = bytes.size();
      sh.pCode = words.data();
      vkCheck(vkCreateShaderModule(gpu.device, &sh, nullptr, &module), name);
    };
    shader("video.vert.spv", vert);
    shader("video_filter.frag.spv", frag);
    VkPipelineShaderStageCreateInfo stages[2]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &ba;
    VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,
                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo gp{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &blend;
    gp.pDynamicState = &dyn;
    gp.layout = layout;
    gp.renderPass = pass;
    VkSpecializationMapEntry entry{0, 0, sizeof(int)};
    VkSpecializationInfo specialize{1, &entry, sizeof(int), nullptr};
    for (int mode = 0; mode < 5; ++mode) {
      if (!((quality == 0 && mode == 0) || (quality == 1 && mode == 1) ||
            (quality == 2 && (mode == 2 || mode == 3)) ||
            (quality == 3 && mode == 4)))
        continue;
      specialize.pData = &mode;
      stages[1].pSpecializationInfo = &specialize;
      vkCheck(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &gp,
                                        nullptr, &pipelines[mode]),
              "filter pipeline");
    }
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = gpu.queueFamily;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(gpu.device, &cp, nullptr, &pool),
            "filter command pool");
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(gpu.device, &cb, &command),
            "filter command");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(gpu.device, &fc, nullptr, &fence), "filter fence");
  }
  void render(VkCommandBuffer command, VkImage target, VideoExtent size,
              int descriptor, Push &push, bool sampled) {
    auto &out = targets[target];
    if (!out.view) {
      VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      v.image = target;
      v.viewType = VK_IMAGE_VIEW_TYPE_2D;
      v.format = format;
      v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCheck(vkCreateImageView(gpu.device, &v, nullptr, &out.view),
              "filter output view");
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = pass;
      fb.attachmentCount = 1;
      fb.pAttachments = &out.view;
      fb.width = size.width;
      fb.height = size.height;
      fb.layers = 1;
      vkCheck(vkCreateFramebuffer(gpu.device, &fb, nullptr, &out.framebuffer),
              "filter output framebuffer");
    }
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = out.framebuffer;
    begin.renderArea.extent = {size.width, size.height};
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipelines[push.mode[1]]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                            1, &sets[descriptor], 0, nullptr);
    VkViewport viewport{0, 0, float(size.width), float(size.height), 0, 1};
    VkRect2D scissor{{0, 0}, {size.width, size.height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    if (sampled) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = target;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
    }
  }
  void record(VkCommandBuffer command, VkImage target,
              const ImageSettings &settings, int stereo, bool sampled) {
    auto image = settings;
    image.sanitize();
    if (image.quality != configured_quality)
      throw std::invalid_argument(
          "Recreate the image processor when changing quality modes");
    if ((stereo == 1 && (input.width % 2 || output.width % 2)) ||
        (stereo == 2 && (input.height % 2 || output.height % 2)))
      throw std::runtime_error(
          "Stereo filters require even packed dimensions.");
    Push push;
    push.dimensions[0] = input.width;
    push.dimensions[1] = input.height;
    push.dimensions[2] = output.width;
    push.dimensions[3] = output.height;
    push.mode[0] = stereo;
    const Extent eyeIn{input.width / (stereo == 1 ? 2 : 1),
                       input.height / (stereo == 2 ? 2 : 1)},
        eyeOut{output.width / (stereo == 1 ? 2 : 1),
               output.height / (stereo == 2 ? 2 : 1)};
    int descriptor = 0;
    if (image.quality == 2) {
      if (!temp.image)
        throw std::runtime_error(
            "FSR output extent must exceed the input extent");
      const auto c = easu_constants(eyeIn, eyeOut);
      std::memcpy(push.constants, &c, sizeof(c));
      push.mode[1] = 2;
      render(command, temp.image, output, 0, push, true);
      descriptor = 1;
      push.dimensions[0] = output.width;
      push.dimensions[1] = output.height;
      const auto r = rcas_constants((1 - image.sharpness) * 2);
      std::memcpy(push.constants, &r, sizeof(r));
      push.mode[1] = 3;
    } else if (image.quality == 1) {
      const auto c = cas_constants(eyeIn, image.sharpness);
      std::memcpy(push.constants, &c, sizeof(c));
      push.mode[1] = 1;
    } else if (image.quality == 3)
      push.mode[1] = 4;
    push.controls[0] = image.exposure;
    push.controls[1] = image.contrast;
    push.controls[2] = image.saturation;
    push.controls[3] = image.gamma;
    push.view[0] = image.zoom;
    push.view[1] = image.pan_x;
    push.view[2] = image.pan_y;
    push.view[3] = image.sharpness;
    render(command, target, output, descriptor, push, sampled);
  }
  void process(VkImage target, const ImageSettings &settings, int stereo,
               bool sampled) {
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
            "filter completion");
    vkCheck(vkResetCommandBuffer(command, 0), "filter reset");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &begin), "filter begin");
    timer.begin(command);
    record(command, target, settings, stereo, sampled);
    timer.end(command);
    vkCheck(vkEndCommandBuffer(command), "filter end");
    vkCheck(vkResetFences(gpu.device, 1, &fence), "filter reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(gpu.queue, 1, &submit, fence), "filter submit");
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
            "filter output completion");
    last_gpu_ms = timer.result();
  }
  ~Impl() {
    vkDeviceWaitIdle(gpu.device);
    for (auto &pair : targets) {
      if (pair.second.framebuffer)
        vkDestroyFramebuffer(gpu.device, pair.second.framebuffer, nullptr);
      if (pair.second.view)
        vkDestroyImageView(gpu.device, pair.second.view, nullptr);
    }
    for (auto pipeline : pipelines)
      if (pipeline)
        vkDestroyPipeline(gpu.device, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(gpu.device, layout, nullptr);
    if (descriptors)
      vkDestroyDescriptorPool(gpu.device, descriptors, nullptr);
    if (set_layout)
      vkDestroyDescriptorSetLayout(gpu.device, set_layout, nullptr);
    if (vert)
      vkDestroyShaderModule(gpu.device, vert, nullptr);
    if (frag)
      vkDestroyShaderModule(gpu.device, frag, nullptr);
    if (sampler)
      vkDestroySampler(gpu.device, sampler, nullptr);
    if (pool)
      vkDestroyCommandPool(gpu.device, pool, nullptr);
    if (fence)
      vkDestroyFence(gpu.device, fence, nullptr);
    for (auto *i : {&source, &temp}) {
      if (i->view)
        vkDestroyImageView(gpu.device, i->view, nullptr);
      if (i->image)
        vkDestroyImage(gpu.device, i->image, nullptr);
      if (i->memory)
        vkFreeMemory(gpu.device, i->memory, nullptr);
    }
    if (pass)
      vkDestroyRenderPass(gpu.device, pass, nullptr);
  }
};
ImageProcessor::ImageProcessor(VulkanContext &gpu, VkFormat f,
                               const AssetReader &assets, VideoExtent in,
                               VideoExtent out, int quality)
    : impl_(std::make_unique<Impl>(gpu, f, in, out)) {
  impl_->initialize(assets, quality);
}
ImageProcessor::~ImageProcessor() = default;
VkImage ImageProcessor::source_image() const { return impl_->source.image; }
VkImageView ImageProcessor::source_view() const { return impl_->source.view; }
void ImageProcessor::process(VkImage output, const ImageSettings &image,
                             int stereo, bool sampled) {
  impl_->process(output, image, stereo, sampled);
}
} // namespace cyvr::player

namespace cyvr::player {
void ImageProcessor::record(VkCommandBuffer command, VkImage target,
                            const ImageSettings &image, int stereo,
                            bool sampled) {
  impl_->record(command, target, image, stereo, sampled);
}
std::optional<double> ImageProcessor::gpu_ms() const {
  return impl_->last_gpu_ms;
}
} // namespace cyvr::player
