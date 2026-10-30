#include "cyvr/player/ambilight.hpp"
#include "cyvr/player/gpu_timer.hpp"
#include "cyvr/player/image_settings.hpp"
#include <cstring>
#include <map>
namespace cyvr::player {
using namespace cyvr;
struct AmbilightRenderer::Impl {
  VulkanContext &gpu;
  VkFormat format;
  VideoExtent input, output;
  bool history_ready{};
  PFN_vkCmdClearColorImage clear_image{};
  PFN_vkCmdCopyImage copy_image{};
  PFN_vkCmdBlitImage blit_image{};
  GpuTimer timer;
  std::optional<double> last_gpu_ms;
  struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
  };
  Image source, temp, current;
  struct Target {
    VkImageView view{};
    VkFramebuffer framebuffer{};
  };
  std::map<VkImage, Target> targets;
  VkRenderPass pass{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkDescriptorSetLayout set_layout{};
  VkDescriptorPool descriptors{};
  VkDescriptorSet set{};
  VkSampler sampler{};
  VkShaderModule vert{}, frag{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  struct alignas(16) Push {
    float shape[4]{}, control[4]{};
  };
  static_assert(sizeof(Push) == 32);
  Impl(VulkanContext &g, VkFormat f)
      : gpu(g), format(f), input{64, 36}, output{128, 128}, timer(g) {}
  void image(Image &i, VideoExtent extent, VkFormat imageFormat) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = imageFormat;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vkCheck(vkCreateImage(gpu.device, &info, nullptr, &i.image),
            "ambilight image");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(gpu.device, i.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        gpu.memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(gpu.device, &alloc, nullptr, &i.memory),
            "ambilight memory");
    vkCheck(vkBindImageMemory(gpu.device, i.image, i.memory, 0),
            "ambilight bind");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = i.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = imageFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCheck(vkCreateImageView(gpu.device, &view, nullptr, &i.view),
            "ambilight image view");
  }
  void initialize(const AssetReader &assets) {
    clear_image = reinterpret_cast<PFN_vkCmdClearColorImage>(
        vkGetInstanceProcAddr(gpu.instance, "vkCmdClearColorImage"));
    copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(
        vkGetInstanceProcAddr(gpu.instance, "vkCmdCopyImage"));
    blit_image = reinterpret_cast<PFN_vkCmdBlitImage>(
        vkGetInstanceProcAddr(gpu.instance, "vkCmdBlitImage"));
    if (!clear_image || !copy_image || !blit_image)
      throw std::runtime_error("Vulkan image transfer commands unavailable");
    image(source, input, format);
    image(temp, output, VK_FORMAT_R16G16B16A16_SFLOAT);
    image(current, output, VK_FORMAT_R16G16B16A16_SFLOAT);
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R16G16B16A16_SFLOAT;
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
    vkCheck(vkCreateRenderPass(gpu.device, &rp, nullptr, &pass),
            "ambilight pass");
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(gpu.device, &s, nullptr, &sampler),
            "ambilight sampler");
    VkDescriptorSetLayoutBinding bindings[2]{
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2;
    sl.pBindings = bindings;
    vkCheck(vkCreateDescriptorSetLayout(gpu.device, &sl, nullptr, &set_layout),
            "ambilight set layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    vkCheck(vkCreateDescriptorPool(gpu.device, &dp, nullptr, &descriptors),
            "ambilight descriptors");
    VkDescriptorSetAllocateInfo ds{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = descriptors;
    ds.descriptorSetCount = 1;
    ds.pSetLayouts = &set_layout;
    vkCheck(vkAllocateDescriptorSets(gpu.device, &ds, &set), "ambilight set");
    for (int i = 0; i < 2; ++i) {
      VkDescriptorImageInfo t{sampler, i ? temp.view : source.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = i;
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
            "ambilight pipeline layout");
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
    shader("ambilight.frag.spv", frag);
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
    vkCheck(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &gp,
                                      nullptr, &pipeline),
            "ambilight pipeline");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = gpu.queueFamily;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(gpu.device, &cp, nullptr, &pool),
            "ambilight command pool");
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(gpu.device, &cb, &command),
            "ambilight command");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(gpu.device, &fc, nullptr, &fence), "ambilight fence");
  }
  void render(VkCommandBuffer command, VkImage target, VideoExtent size,
              Push &push) {
    auto &out = targets[target];
    if (!out.view) {
      VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      v.image = target;
      v.viewType = VK_IMAGE_VIEW_TYPE_2D;
      v.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCheck(vkCreateImageView(gpu.device, &v, nullptr, &out.view),
              "ambilight output view");
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = pass;
      fb.attachmentCount = 1;
      fb.pAttachments = &out.view;
      fb.width = size.width;
      fb.height = size.height;
      fb.layers = 1;
      vkCheck(vkCreateFramebuffer(gpu.device, &fb, nullptr, &out.framebuffer),
              "ambilight output framebuffer");
    }
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = out.framebuffer;
    begin.renderArea.extent = {size.width, size.height};
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                            1, &set, 0, nullptr);
    VkViewport viewport{0, 0, float(size.width), float(size.height), 0, 1};
    VkRect2D scissor{{0, 0}, {size.width, size.height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
  }
  void barrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
               VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags from,
               VkPipelineStageFlags to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, from, to, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
  void process(VkImage target, const AmbilightSettings &settings, float aspect,
               float elapsed) {
    auto controls = settings;
    controls.sanitize();
    aspect = std::isfinite(aspect) ? std::clamp(aspect, .1f, 10.f) : 16.f / 9;
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
            "ambilight completion");
    vkCheck(vkResetCommandBuffer(command, 0), "ambilight reset");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(command, &begin), "ambilight begin");
    timer.begin(command);
    if (!history_ready) {
      barrier(temp.image, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
      const VkClearColorValue clear{};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                          1};
      clear_image(command, temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  &clear, 1, &range);
      barrier(temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    Push push;
    push.shape[0] = 1 + 2 * controls.spread;
    push.shape[1] = 1 + 2 * controls.spread * aspect;
    push.shape[2] = controls.saturation;
    push.shape[3] = controls.enabled ? controls.intensity : 0;
    const float dt = std::isfinite(elapsed) ? std::clamp(elapsed, 0.f, 5.f) : 0;
    push.control[0] = history_ready && controls.smoothing > .001f
                          ? 1 - std::exp(-dt / controls.smoothing)
                          : 1;
    push.control[1] = controls.inset;
    push.control[2] = controls.auto_bars ? 1.f : 0.f;
    push.control[3] = aspect;
    render(command, current.image, output, push);
    barrier(current.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(target, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] =
        blit.dstOffsets[1] = {int(output.width), int(output.height), 1};
    blit_image(command, current.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
               VK_FILTER_NEAREST);
    barrier(temp.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {output.width, output.height, 1};
    copy_image(command, current.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    timer.end(command);
    vkCheck(vkEndCommandBuffer(command), "ambilight end");
    vkCheck(vkResetFences(gpu.device, 1, &fence), "ambilight reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    vkCheck(vkQueueSubmit(gpu.queue, 1, &submit, fence), "ambilight submit");
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
            "ambilight output completion");
    last_gpu_ms = timer.result();
    history_ready = true;
  }
  ~Impl() {
    vkDeviceWaitIdle(gpu.device);
    for (auto &pair : targets) {
      if (pair.second.framebuffer)
        vkDestroyFramebuffer(gpu.device, pair.second.framebuffer, nullptr);
      if (pair.second.view)
        vkDestroyImageView(gpu.device, pair.second.view, nullptr);
    }
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
    for (auto *i : {&source, &temp, &current}) {
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
AmbilightRenderer::AmbilightRenderer(VulkanContext &gpu, VkFormat format,
                                     const AssetReader &assets)
    : impl_(std::make_unique<Impl>(gpu, format)) {
  impl_->initialize(assets);
}
AmbilightRenderer::~AmbilightRenderer() = default;
VkImage AmbilightRenderer::source_image() const { return impl_->source.image; }
VkImageView AmbilightRenderer::source_view() const {
  return impl_->source.view;
}
void AmbilightRenderer::reset() { impl_->history_ready = false; }
void AmbilightRenderer::process(VkImage target,
                                const AmbilightSettings &controls, float aspect,
                                float elapsed) {
  impl_->process(target, controls, aspect, elapsed);
}
std::optional<double> AmbilightRenderer::gpu_ms() const {
  return impl_->last_gpu_ms;
}
} // namespace cyvr::player
