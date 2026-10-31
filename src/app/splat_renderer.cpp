// The backend uses Vulkan C entry points; include it before CyVR_UI's dispatch declarations.
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include "rendering/vulkan/frame/VulkanFrameCompute.h"
#include "rendering/vulkan/frame/VulkanCameraUniforms.h"
#include "rendering/vulkan/raster/VulkanSplatPipeline.h"
#include "cyvr/player/splat_renderer.hpp"
#include "cyvr/player/image_processor.hpp"
#include "cyvr/player/demuxer.hpp"
#include <map>
#include <cstring>
namespace cyvr::player {
struct SplatRenderer::Impl {
    cyvr::VulkanContext& gpu;VkFormat format;cyvr::AssetReader assets;
    splatkit::VulkanContext backend;
    VkRenderPass pass{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    std::unique_ptr<splatkit::VulkanCameraUniforms> camera;
    std::unique_ptr<splatkit::VulkanSplatPipeline> pipeline;
    std::optional<splatkit::VulkanFrameCompute::Pipelines> programs;
    std::unique_ptr<splatkit::VulkanFrameCompute> compute;
    std::unique_ptr<splatkit::VulkanWorld> world;
    std::shared_ptr<const splat::SplatCloud> cloud;
    std::unique_ptr<ImageProcessor> processor;
    VideoExtent extent{};int quality{-1};
    struct Target{VkImageView view{};VkFramebuffer framebuffer{};};std::map<VkImage,Target> targets;
    std::uint64_t serial{};
    Impl(cyvr::VulkanContext& g,VkFormat f,const cyvr::AssetReader& a):gpu(g),format(f),assets(a),backend(g.instance,g.physical,g.device,g.queue,g.queueFamily){}
    void initialize(){
        auto& g=gpu;const auto f=format;
        if(!splatkit::VulkanFrameCompute::supported(backend))throw MediaError("This GPU cannot run the Gaussian sorting pipeline");
        VkAttachmentDescription attachment{};attachment.format=f;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};info.attachmentCount=1;info.pAttachments=&attachment;info.subpassCount=1;info.pSubpasses=&sub;cyvr::vkCheck(::vkCreateRenderPass(g.device,&info,nullptr,&pass),"Gaussian render pass");
        auto c=splatkit::VulkanCameraUniforms::create(backend);if(!c)throw MediaError(c.error().message);camera=std::move(c.value());
        auto p=splatkit::VulkanSplatPipeline::create(backend,pass,*camera);if(!p)throw MediaError(p.error().message);pipeline=std::move(p.value());
        programs=splatkit::VulkanFrameCompute::Pipelines::compile(backend);if(!programs)throw MediaError("Gaussian compute shaders unavailable");
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=g.queueFamily;cyvr::vkCheck(::vkCreateCommandPool(g.device,&cp,nullptr,&pool),"Gaussian command pool");
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cb.commandPool=pool;cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;cyvr::vkCheck(::vkAllocateCommandBuffers(g.device,&cb,&command),"Gaussian command buffer");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;cyvr::vkCheck(::vkCreateFence(g.device,&fc,nullptr,&fence),"Gaussian fence");
    }
    void clear_targets(){for(auto& pair:targets){if(pair.second.framebuffer)::vkDestroyFramebuffer(gpu.device,pair.second.framebuffer,nullptr);if(pair.second.view)::vkDestroyImageView(gpu.device,pair.second.view,nullptr);}targets.clear();}
    ~Impl(){::vkDeviceWaitIdle(gpu.device);clear_targets();processor.reset();world.reset();compute.reset();programs.reset();pipeline.reset();camera.reset();if(fence)::vkDestroyFence(gpu.device,fence,nullptr);if(pool)::vkDestroyCommandPool(gpu.device,pool,nullptr);if(pass)::vkDestroyRenderPass(gpu.device,pass,nullptr);}
    void draw(VkImage output,VideoExtent size,const SplatSnapshot& frame,cyvr::Pose pose,const std::array<float,4>& fov,const ImageSettings& settings,bool sampled){
        if(!frame.cloud)return;
        cyvr::vkCheck(::vkWaitForFences(gpu.device,1,&fence,VK_TRUE,5000000000ull),"wait Gaussian frame");
        if(cloud!=frame.cloud){
            world=splatkit::VulkanWorld::upload(backend,*frame.cloud,3,false);if(!world)throw MediaError("Gaussian world exceeds GPU memory limits");
            if(!compute || !cloud || cloud->count()!=frame.cloud->count()){
                auto c=splatkit::VulkanFrameCompute::create(backend,*programs,world->count,nullptr,world->count);if(!c)throw MediaError(c.error().message);compute=std::move(c.value());
                splatkit::RenderPolicy policy;policy.sortDepth=splatkit::SortKeyBits::full32;std::string reason;if(!compute->applyRenderPolicy(policy,&reason))throw MediaError(reason);
            }
            pipeline->bindWorld(*world);cloud=frame.cloud;
        }
        if(extent.width!=size.width || extent.height!=size.height || quality!=settings.quality){clear_targets();processor=std::make_unique<ImageProcessor>(gpu,format,assets,size,size,settings.quality);extent=size;quality=settings.quality;}
        const auto rotation=splat::Mat4::rotation({pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w});auto transform=rotation;transform.at(0,3)=pose.position.x;transform.at(1,3)=pose.position.y;transform.at(2,3)=pose.position.z;const auto view=transform.rigidInverse();
        const float n=.01f,z=1000;auto projection=splat::Mat4{};
        projection.at(0,0)=2/(fov[1]-fov[0]);projection.at(1,1)=2/(fov[2]-fov[3]);projection.at(0,2)=(fov[1]+fov[0])/(fov[1]-fov[0]);projection.at(1,2)=(fov[2]+fov[3])/(fov[2]-fov[3]);projection.at(2,2)=z/(n-z);projection.at(2,3)=n*z/(n-z);projection.at(3,2)=-1;
        const bool linear=format==VK_FORMAT_R8G8B8A8_SRGB || format==VK_FORMAT_B8G8R8A8_SRGB;
        auto uniform=camera->update(0,view,projection,{pose.position.x,pose.position.y,pose.position.z},{size.width,size.height},linear,0,0);
        if(!uniform)throw MediaError("Invalid Gaussian camera");
        cyvr::vkCheck(::vkResetCommandBuffer(command,0),"reset Gaussian commands");VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;cyvr::vkCheck(::vkBeginCommandBuffer(command,&begin),"begin Gaussian commands");
        splatkit::SplatRenderer::Range range{0,world->count};splatkit::SplatRenderer::Frame input;input.orderSource=splatkit::SplatRenderer::OrderSource::gpu;input.ranges=&range;input.rangeCount=1;input.shDegree=world->shDegree;input.view=view;input.proj=projection;input.cameraPosition={pose.position.x,pose.position.y,pose.position.z};
        auto draw=compute->encode(command,0,uniform,*world->splats,*world->sh,world->shDegree,input);if(!draw)throw MediaError("Gaussian GPU sorting failed");
        pipeline->bindProjected(0,draw->projected,draw->order,draw->capacity);
        const VkImage target=processor->source_image();auto& t=targets[target];if(!t.view){VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=target;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};cyvr::vkCheck(::vkCreateImageView(gpu.device,&vi,nullptr,&t.view),"Gaussian image view");VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pass;fb.attachmentCount=1;fb.pAttachments=&t.view;fb.width=size.width;fb.height=size.height;fb.layers=1;cyvr::vkCheck(::vkCreateFramebuffer(gpu.device,&fb,nullptr,&t.framebuffer),"Gaussian framebuffer");}
        VkClearValue clear{};clear.color.float32[3]=1;VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.framebuffer=t.framebuffer;rp.renderArea.extent={size.width,size.height};rp.clearValueCount=1;rp.pClearValues=&clear;::vkCmdBeginRenderPass(command,&rp,VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{0,float(size.height),float(size.width),-float(size.height),0,1};VkRect2D scissor{{0,0},{size.width,size.height}};::vkCmdSetViewport(command,0,1,&viewport);::vkCmdSetScissor(command,0,1,&scissor);pipeline->drawProjected(command,0,draw->arguments);::vkCmdEndRenderPass(command);
        // The processor samples this image in its own render pass.
        VkImageMemoryBarrier readable{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};readable.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;readable.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;readable.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;readable.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;readable.srcQueueFamilyIndex=readable.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;readable.image=target;readable.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        ::vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&readable);
        auto controls=settings;controls.zoom=1;controls.pan_x=controls.pan_y=0;processor->record(command,output,controls,0,sampled);
        cyvr::vkCheck(::vkEndCommandBuffer(command),"end Gaussian commands");cyvr::vkCheck(::vkResetFences(gpu.device,1,&fence),"reset Gaussian fence");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;cyvr::vkCheck(::vkQueueSubmit(gpu.queue,1,&submit,fence),"submit Gaussian frame");compute->submitted(0,++serial);cyvr::vkCheck(::vkWaitForFences(gpu.device,1,&fence,VK_TRUE,5000000000ull),"complete Gaussian frame");compute->collectCompleted(serial);
    }
};
SplatRenderer::SplatRenderer(cyvr::VulkanContext& gpu,VkFormat format,const cyvr::AssetReader& assets):impl_(std::make_unique<Impl>(gpu,format,assets)){impl_->initialize();}
SplatRenderer::~SplatRenderer()=default;
void SplatRenderer::draw(VkImage output,VideoExtent size,const SplatSnapshot& frame,cyvr::Pose camera,const std::array<float,4>& fov,const ImageSettings& settings,bool sampled){impl_->draw(output,size,frame,camera,fov,settings,sampled);}
std::uint32_t SplatRenderer::drawn()const{return impl_->compute?impl_->compute->stats().drawn:0;}
}
