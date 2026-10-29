// Test-only RGB stand-in for the Android external-image input. Executes the
// actual video.vert/video.frag and shared Android push layout before filtering.
#pragma once
#include "cyvr/player/video_view.hpp"
#include <cstring>
#include <map>

class ProjectionProbe {
  cyvr::VulkanContext &gpu;
  VkRenderPass pass{};
  VkDescriptorSetLayout setLayout{};
  VkDescriptorPool descriptors{};
  VkDescriptorSet set{};
  VkSampler sampler{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkShaderModule vert{}, frag{};
  VkCommandPool pool{};
  VkCommandBuffer cmd{};
  VkFence fence{};
  struct Output {
    VkImageView view{};
    VkFramebuffer framebuffer{};
  };
  std::map<VkImage, Output> outputs;

public:
  ProjectionProbe(cyvr::VulkanContext &context, const cyvr::AssetReader &assets,
                  VkImageView source)
      : gpu(context) {
    using namespace cyvr;
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_SRGB;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    vkCheck(vkCreateRenderPass(gpu.device, &rp, nullptr, &pass),
            "projection probe pass");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCheck(vkCreateSampler(gpu.device, &si, nullptr, &sampler),
            "projection probe sampler");
    VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 1;
    sl.pBindings = &binding;
    vkCheck(vkCreateDescriptorSetLayout(gpu.device, &sl, nullptr, &setLayout),
            "projection probe set layout");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    vkCheck(vkCreateDescriptorPool(gpu.device, &dp, nullptr, &descriptors),
            "projection probe descriptors");
    VkDescriptorSetAllocateInfo ds{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ds.descriptorPool = descriptors;
    ds.descriptorSetCount = 1;
    ds.pSetLayouts = &setLayout;
    vkCheck(vkAllocateDescriptorSets(gpu.device, &ds, &set),
            "projection probe set");
    VkDescriptorImageInfo texture{sampler, source,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &texture;
    vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(cyvr::player::VideoPush)};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    vkCheck(vkCreatePipelineLayout(gpu.device, &pl, nullptr, &layout),
            "projection probe layout");
    auto shader = [&](const char *name, VkShaderModule &module) {
      const auto bytes = assets(name);
      std::vector<uint32_t> words((bytes.size() + 3) / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      VkShaderModuleCreateInfo sh{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      sh.codeSize = bytes.size();
      sh.pCode = words.data();
      vkCheck(vkCreateShaderModule(gpu.device, &sh, nullptr, &module), name);
    };
    shader("video.vert.spv", vert);
    shader("video.frag.spv", frag);
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
    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,
                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dyn;
    info.layout = layout;
    info.renderPass = pass;
    vkCheck(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &info,
                                      nullptr, &pipeline),
            "projection probe pipeline");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = gpu.queueFamily;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCheck(vkCreateCommandPool(gpu.device, &cp, nullptr, &pool),
            "projection probe pool");
    VkCommandBufferAllocateInfo cb{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb.commandPool = pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    vkCheck(vkAllocateCommandBuffers(gpu.device, &cb, &cmd),
            "projection probe command");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCheck(vkCreateFence(gpu.device, &fc, nullptr, &fence),
            "projection probe fence");
  }
  void run(VkImage target, cyvr::player::VideoExtent size,
           const cyvr::player::VideoPush &push, bool sampled = false,
           const std::function<void(VkCommandBuffer)> &finish = {}) {
    using namespace cyvr;
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
            "projection probe wait");
    auto &output = outputs[target];
    if (!output.view) {
      VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      v.image = target;
      v.viewType = VK_IMAGE_VIEW_TYPE_2D;
      v.format = VK_FORMAT_R8G8B8A8_SRGB;
      v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCheck(vkCreateImageView(gpu.device, &v, nullptr, &output.view),
              "projection probe view");
      VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      f.renderPass = pass;
      f.attachmentCount = 1;
      f.pAttachments = &output.view;
      f.width = size.width;
      f.height = size.height;
      f.layers = 1;
      vkCheck(vkCreateFramebuffer(gpu.device, &f, nullptr, &output.framebuffer),
              "projection probe framebuffer");
    }
    vkCheck(vkResetCommandBuffer(cmd, 0), "projection probe reset");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkCheck(vkBeginCommandBuffer(cmd, &begin), "projection probe begin");
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass;
    rp.framebuffer = output.framebuffer;
    rp.renderArea.extent = {size.width, size.height};
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                            &set, 0, nullptr);
    VkViewport viewport{0, 0, float(size.width), float(size.height), 0, 1};
    VkRect2D scissor{{0, 0}, {size.width, size.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    if (sampled) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = target;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
    }
    if (finish)
      finish(cmd);
    vkCheck(vkEndCommandBuffer(cmd), "projection probe end");
    vkCheck(vkResetFences(gpu.device, 1, &fence),
            "projection probe reset fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkCheck(vkQueueSubmit(gpu.queue, 1, &submit, fence),
            "projection probe submit");
    vkCheck(vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull),
            "projection probe complete");
  }
  ~ProjectionProbe() {
    using namespace cyvr;
    vkDeviceWaitIdle(gpu.device);
    for (auto &p : outputs) {
      vkDestroyFramebuffer(gpu.device, p.second.framebuffer, nullptr);
      vkDestroyImageView(gpu.device, p.second.view, nullptr);
    }
    vkDestroyFence(gpu.device, fence, nullptr);
    vkDestroyCommandPool(gpu.device, pool, nullptr);
    vkDestroyPipeline(gpu.device, pipeline, nullptr);
    vkDestroyPipelineLayout(gpu.device, layout, nullptr);
    vkDestroyShaderModule(gpu.device, vert, nullptr);
    vkDestroyShaderModule(gpu.device, frag, nullptr);
    vkDestroyDescriptorPool(gpu.device, descriptors, nullptr);
    vkDestroyDescriptorSetLayout(gpu.device, setLayout, nullptr);
    vkDestroySampler(gpu.device, sampler, nullptr);
    vkDestroyRenderPass(gpu.device, pass, nullptr);
  }
};
