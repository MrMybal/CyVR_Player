#include "cyvr/player/android/renderer.hpp"
#include "cyvr/player/android/video_renderer.hpp"
#include "cyvr/player/locale.hpp"
#include "cyvr/player/media.hpp"
#include "cyvr/player/video_placement.hpp"
#include "cyvr/player/subtitle_rasterizer.hpp"
#include "cyvr/player/ambilight.hpp"
#include "cyvr/player/splat_renderer.hpp"
#include <android/log.h>
#include <cstring>
#include <deque>
#if CYVR_ENABLE_IMMERSIVE
#include "cyvr/android_xr.hpp"
#include <openxr/openxr_platform.h>
#endif
namespace cyvr::player {
using namespace cyvr;
namespace {
#if CYVR_ENABLE_IMMERSIVE
using Base = AndroidXrRenderExtension;
void xrCheck(XrResult result, const char *what) {
  if (XR_FAILED(result))
    throw std::runtime_error(std::string(what) + " / OpenXR " +
                             std::to_string(result));
}
Pose pose_from_xr(const XrPosef &pose) {
  return {{pose.position.x, pose.position.y, pose.position.z},
          {pose.orientation.x, pose.orientation.y, pose.orientation.z,
           pose.orientation.w}};
}
#else
using Base = AndroidRenderExtension;
#endif
class Renderer final : public Base {
  std::function<PlayerApplication *()> application_;
  bool immersive_{};
  VulkanContext *gpu_{};
  VulkanRenderer *uiRenderer_{};
  VkFormat format_{};
  AssetReader assets_;
  VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
  std::vector<const char *> deviceExtensions_;
  std::unique_ptr<VideoRenderer> video_;
  VkImage texture_{};
  VkImageView textureView_{};
  VkDeviceMemory textureMemory_{};
  VkSampler sampler_{};
  uint32_t width_{}, height_{};
  std::uint64_t frames_{};
  bool initialized_{};
  android::ImageLease retained_;
  ImageSettings rendered_image_;
  CinemaSettings rendered_cinema_,last_cinema_;
  CinemaSettings reported_cinema_;
  int reported_surface_{-1};
  int rendered_stereo_{-1}, resource_quality_{-1}, rendered_projection_{-1};
  bool rendered_swap_{};
  float last_zoom_{1};
  std::chrono::steady_clock::time_point zoom_changed_{};
  VideoExtent panel_input_{};
  UiModel *ui_{};
  bool upload_thumbnails_{true};
  std::uint64_t failed_request_{UINT64_MAX};
  double last_report_{};
  TimeSamples native_acquire_,video_import_,video_completion_,host_cpu_;
  double native_acquire_last_{};
  std::uint64_t measured_epoch_{UINT64_MAX}, measured_request_{UINT64_MAX};
  std::unique_ptr<SubtitleRasterizer> captions_;
  std::unique_ptr<VulkanRenderer> caption_renderer_;
  bool caption_visible_{};
  int caption_width_{},caption_height_{};
  std::weak_ptr<const SubtitleConfig> failed_caption_;
  std::unique_ptr<SplatRenderer> splat_renderer_;
  std::shared_ptr<const splat::SplatCloud> rendered_splat_;
  std::shared_ptr<const splat::SplatCloud> failed_splat_;
  ImageSettings failed_splat_settings_;
#if CYVR_ENABLE_IMMERSIVE
  AndroidXrContext xr_{};
  VideoPlacement video_placement_;
  XrSpace video_space_{},stable_space_{};
  std::uint64_t placed_request_{UINT64_MAX};
  struct SpaceChange {
    XrTime time;
    bool valid;
    Pose new_in_previous;
    XrSpace space;
  };
  std::deque<SpaceChange> space_changes_;
  struct EyeTarget {
    XrSwapchain swapchain{};
    std::vector<VkImage> images;
    std::unique_ptr<VideoRenderer> renderer;
    VideoExtent input{}, output{};
    int quality{-1};
  };
  std::array<EyeTarget, 2> enhanced_;
  std::array<VideoExtent, 2> eye_extents_{};
  std::array<XrCompositionLayerProjectionView, 2> enhanced_views_{};
  XrCompositionLayerProjection enhanced_layer_{
      XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrSwapchain swapchain_{};
  std::vector<VkImage> images_;
  std::array<XrCompositionLayerQuad, 2> quads_{};
  std::array<XrCompositionLayerEquirect2KHR, 2> spheres_{};
  std::array<XrCompositionLayerCylinderKHR,2> cylinders_{};
  bool cylinder_enabled_{};
  std::unique_ptr<AmbilightRenderer> ambilight_;
  std::unique_ptr<VideoRenderer> ambilight_capture_;
  XrSwapchain ambilight_swapchain_{};
  std::vector<VkImage> ambilight_images_;
  XrCompositionLayerQuad ambilight_quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerCylinderKHR ambilight_cylinder_{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
  AmbilightSettings ambilight_settings_;
  ImageSettings ambilight_image_;
  std::uint64_t ambilight_epoch_{UINT64_MAX},ambilight_failed_epoch_{UINT64_MAX};
  std::int64_t ambilight_pts_{-1};
  int ambilight_stereo_{-1};
  float ambilight_aspect_{};
  bool ambilight_active_{};
  bool ambilight_settled_{};
  std::chrono::steady_clock::time_point ambilight_updated_{},ambilight_changed_{};
  double ambilight_report_{};
  XrSwapchain caption_swapchain_{};
  std::vector<Target> caption_targets_;
  XrCompositionLayerQuad caption_quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerCylinderKHR caption_cylinder_{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
  struct GaussianEye{XrSwapchain swapchain{};std::vector<VkImage> images;VideoExtent extent{};};
  std::array<GaussianEye,2> gaussian_eyes_;
#endif
public:
  Renderer(std::function<PlayerApplication *()> application, bool immersive)
      : application_(std::move(application)), immersive_(immersive) {}
  ~Renderer() override { graphicsStopping(true); }
  void configureDevice(VulkanContext &gpu, VkDeviceCreateInfo &info) override {
    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        vkGetInstanceProcAddr(gpu.instance,
                              "vkEnumerateDeviceExtensionProperties"));
    auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(gpu.instance, "vkGetPhysicalDeviceFeatures2"));
    if (!enumerate || !features)
      throw std::runtime_error("Vulkan 1.1 capability queries unavailable");
    uint32_t count{};
    vkCheck(enumerate(gpu.physical, nullptr, &count, nullptr),
            "video extension count");
    std::vector<VkExtensionProperties> supported(count);
    vkCheck(enumerate(gpu.physical, nullptr, &count, supported.data()),
            "video extensions");
    deviceExtensions_.clear();
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
      deviceExtensions_.push_back(info.ppEnabledExtensionNames[i]);
    for (auto name :
         {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
          VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
          VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
      if (std::none_of(supported.begin(), supported.end(), [&](const auto &p) {
            return std::strcmp(p.extensionName, name) == 0;
          }))
        throw std::runtime_error(std::string("Missing video GPU extension: ") +
                                 name);
      deviceExtensions_.push_back(name);
    }
    VkPhysicalDeviceFeatures2 available{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    available.pNext = &ycbcr_;
    features(gpu.physical, &available);
    if (!ycbcr_.samplerYcbcrConversion)
      throw std::runtime_error("Hardware YCbCr conversion unavailable");
    ycbcr_.pNext = const_cast<void *>(info.pNext);
    info.pNext = &ycbcr_;
    info.enabledExtensionCount = uint32_t(deviceExtensions_.size());
    info.ppEnabledExtensionNames = deviceExtensions_.data();
  }
  void graphicsReady(VulkanContext &gpu, VulkanRenderer &renderer,
                     VkFormat format, const AssetReader &assets,
                     UiModel &ui) override {
    ui_ = &ui;
    if (gpu_ && format_ != format)
      graphicsStopping(true);
    gpu_ = &gpu;
    uiRenderer_ = &renderer;
    upload_thumbnails_ = true;
    format_ = format;
    assets_ = assets;
    if(auto app=application_())app->set_logo(assets("branding/player-logo.rgba"));
    if (!sampler_) {
      VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      s.magFilter = s.minFilter = VK_FILTER_LINEAR;
      s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
      s.addressModeU = s.addressModeV = s.addressModeW =
          VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      vkCheck(vkCreateSampler(gpu.device, &s, nullptr, &sampler_),
              "video preview sampler");
    }
    if (textureView_ && initialized_) {
      renderer.setVideoTexture(textureView_, sampler_);
      if (auto app = application_())
        app->preview_ready = true;
    }
    if(caption_renderer_ && caption_visible_)renderer.setOverlayTexture(caption_renderer_->imageAtlasView(),caption_renderer_->imageSampler());
  }
  void graphicsStopping(bool deviceStopping) override {
    if (!deviceStopping) {
      uiRenderer_ = nullptr;
      return;
    }
#if CYVR_ENABLE_IMMERSIVE
    if(stable_space_){xrDestroySpace(stable_space_);stable_space_=XR_NULL_HANDLE;}
#endif
    if (!gpu_)
      return;
    vkDeviceWaitIdle(gpu_->device);
    if (uiRenderer_)
      uiRenderer_->setVideoTexture();
    destroy_splats();
#if CYVR_ENABLE_IMMERSIVE
    destroy_ambilight();
#endif
    destroyVideo();
    destroy_captions();
    retained_ = {};
    if (sampler_)
      vkDestroySampler(gpu_->device, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    uiRenderer_ = nullptr;
    gpu_ = nullptr;
    assets_ = {};
  }
  int pollTimeoutMs() const override {
    auto app = application_();
    if (!app)
      return 33;
    auto state = app->session().state();
    return state.playing || (state.ready && !app->preview_ready) ? 8 : 33;
  }
  void frameSubmitted(UiModel &ui) override {
    host_cpu_.add(ui.stats.cpuMs);
    if (auto app = application_()) {
      const double now =
          std::chrono::duration<double>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count();
      app->render_monitor.submissions.add(now);
      ++app->render_monitor.frames;
      if (now - last_report_ >= 2.) {
        last_report_ = now;
        const auto video = app->session().metrics();
        const auto timing=video_?video_->timings():VideoRenderTimings{};
        __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","PERF_STAGES host_cpu_ms=%.3f video_wall_ms=%.3f native_acquire_ms=%.3f import_ms=%.3f completion_ms=%.3f imports=%llu cache_resets=%llu cache_evictions=%llu cached=%zu quality=%d upscale=%d environment=%d ambilight=%d hidden=%d compact=%d",
          host_cpu_.mean().value_or(-1),app->render_monitor.work.mean().value_or(-1),native_acquire_.mean().value_or(-1),video_import_.mean().value_or(-1),video_completion_.mean().value_or(-1),
          static_cast<unsigned long long>(timing.imports),static_cast<unsigned long long>(timing.cache_resets),static_cast<unsigned long long>(timing.cache_evictions),timing.cached_images,app->image.quality,app->image.upscale,app->cinema.environment,int(app->ambilight.enabled),int(ui.interfaceHidden),int(ui.compactInterface));
        __android_log_print(
            ANDROID_LOG_INFO, "CyVRPlayer",
            "PERFORMANCE app_submissions_s=%.2f video_gpu_s=%.2f "
            "decoded_s=%.2f video_gpu_ms=%.3f video_gpu_p95_ms=%.3f "
            "late_drops=%llu queue=%llu render_errors=%llu",
            app->render_monitor.submissions.sample(now).value_or(-1),
            video.submit_rate.value_or(-1), video.decode_rate.value_or(-1),
            app->render_monitor.gpu.mean().value_or(-1),
            app->render_monitor.gpu.p95().value_or(-1),
            static_cast<unsigned long long>(video.dropped),
            static_cast<unsigned long long>(video.queued),
            static_cast<unsigned long long>(app->render_monitor.errors));
      }
    }
  }
  bool beforeRender(UiModel &ui, double) override {
    if (reportedHidden_ != int(ui.interfaceHidden) || reportedCompact_ != int(ui.compactInterface)) {
      reportedHidden_ = int(ui.interfaceHidden);
      reportedCompact_ = int(ui.compactInterface);
      __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer",
                          "INTERFACE hidden=%d host=%s", reportedHidden_,
                          immersive_ ? "immersive" : "panel");
      __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","INTERFACE mode=%s",ui.interfaceHidden?"hidden":ui.compactInterface?"compact":"full");
    }
    auto app = application_();
    if (!app || !gpu_)
      return false;
    std::vector<std::uint8_t> thumbnails;
    if (uiRenderer_ && app->take_thumbnails(thumbnails, upload_thumbnails_)) {
      uiRenderer_->uploadImages(ui_atlas_width, ui_atlas_height, thumbnails);
      upload_thumbnails_ = false;
      unsigned count = 0;
      for (unsigned row = 0; row < 7; ++row)
        if (thumbnails[(row * 160 * 90 + 1) * 4 + 3])
          ++count;
      __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer", "THUMBNAILS count=%u",
                          count);
    }
    auto state = app->session().state();
    if (!state.ready) {
      app->caption_ready=false;caption_visible_=false;
      if(splat_renderer_ || failed_splat_ || rendered_splat_)destroy_splats();
      bool video_resources=bool(video_) || bool(texture_);
#if CYVR_ENABLE_IMMERSIVE
      video_resources=video_resources || bool(swapchain_) || bool(enhanced_[0].renderer);
#endif
      if(video_resources) {vkDeviceWaitIdle(gpu_->device);destroyVideo();}
      app->preview_ready = false;
      initialized_ = false;
      retained_ = {};
      return false;
    }
    update_captions(*app,ui,state);
    if(state.volumetric)return immersive_?false:gaussian_panel(*app,ui);
    if(splat_renderer_ || failed_splat_ || rendered_splat_)destroy_splats();
    auto image = app->session().take_frame();
    const bool new_frame = bool(image);
    if (image)
      retained_ = std::move(image);
    if (!retained_)
      return false;
    if (last_zoom_ != app->image.zoom || last_cinema_.width!=app->cinema.width || last_cinema_.distance!=app->cinema.distance || last_cinema_.curvature!=app->cinema.curvature) {
      last_zoom_ = app->image.zoom;
      last_cinema_=app->cinema;
      zoom_changed_ = std::chrono::steady_clock::now();
    }
    if (measured_epoch_ != retained_.epoch ||
        measured_request_ != app->processing_request) {
      measured_epoch_ = retained_.epoch;
      measured_request_ = app->processing_request;
      app->render_monitor.gpu.reset();
      app->render_monitor.work.reset();
    }
    const bool enhance =
        app->image.reconstruct_view() && failed_request_ != app->processing_request;
    if (immersive_ && enhance)
      return false; // Reproject/filter at predicted eye poses, below.
#if CYVR_ENABLE_IMMERSIVE
    if(immersive_ && app->projection==0 && app->cinema.curvature>0 && !cylinder_enabled_)return false;
#endif
    auto controls = app->image;
    controls.quality = enhance ? controls.quality : 0;
    if (!enhance && failed_request_ == app->processing_request) {
      controls.sampling=0;controls.denoise=controls.detail=0;
    }
    auto extent = VideoExtent{uint32_t(state.width), uint32_t(state.height)};
    ViewProcessingPlan plan{extent, extent};
    if (enhance) {
      const double aspect = eye_aspect(
          state.width, state.height, StereoLayout(app->stereo),
          app->half_stereo && app->stereo ? (app->stereo == 1 ? 2. : .5)
                                          : state.pixel_aspect);
      const float scale =
          std::min(ui.viewportSize.x / 1440, ui.viewportSize.y / 990);
      float w = ui.interfaceHidden ? ui.viewportSize.x : 900 * scale,
            h = ui.interfaceHidden ? ui.viewportSize.y : 506 * scale;
      const auto display =
          VideoExtent{uint32_t(std::max(1.f, std::min(w, float(h * aspect)))),
                      uint32_t(std::max(1.f, std::min(h, float(w / aspect))))};
      const VideoExtent available{
          uint32_t(std::max(1.f, state.width / float(app->stereo == 1 ? 2 : 1) /
                                     controls.zoom)),
          uint32_t(
              std::max(1.f, state.height / float(app->stereo == 2 ? 2 : 1) /
                                controls.zoom))};
      plan = view_processing_plan(display, available, controls);
      // Keep filtering resources while a zoom gesture is moving. Reconstruct
      // at the updated detail estimate once it settles, without stalling every
      // controller sample on pipeline/image recreation.
      if (zoom_settling() && video_ && resource_quality_ == controls.quality &&
          width_ == plan.output.width && height_ == plan.output.height &&
          panel_input_.width)
        plan.input = panel_input_;
      extent = plan.output;
    }
    if (!new_frame && initialized_ && rendered_image_ == app->image &&
        rendered_stereo_ == app->stereo &&
        rendered_projection_ == app->projection &&
        rendered_swap_ == app->swap_eyes && width_ == extent.width &&
        height_ == extent.height && panel_input_.width == plan.input.width &&
        panel_input_.height == plan.input.height)
      return false;
    try {
      const auto before = std::chrono::steady_clock::now();
      if (!video_ || width_ != extent.width || height_ != extent.height ||
          resource_quality_ != controls.quality) {
        createVideo(extent.width, extent.height);
        resource_quality_ = controls.quality;
      }
      if (immersive_ && app->projection == 0) {
        controls.zoom = 1;
        controls.pan_x = controls.pan_y = 0;
      }
#if CYVR_ENABLE_IMMERSIVE
      if (immersive_) {
        uint32_t index{};
        XrSwapchainImageAcquireInfo acquire{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const auto acquire_start=std::chrono::steady_clock::now();
        xrCheck(xrAcquireSwapchainImage(swapchain_, &acquire, &index),
                "acquire native video");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        xrCheck(xrWaitSwapchainImage(swapchain_, &wait), "wait native video");
        native_acquire_last_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-acquire_start).count();native_acquire_.add(native_acquire_last_);
        try {
          video_->draw_retained(images_.at(index),
                                {uint32_t(state.width), uint32_t(state.height)},
                                {width_, height_}, retained_, controls,
                                app->stereo, false);
        } catch (...) {
          XrSwapchainImageReleaseInfo release{
              XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
          xrReleaseSwapchainImage(swapchain_, &release);
          throw;
        }
        XrSwapchainImageReleaseInfo release{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrCheck(xrReleaseSwapchainImage(swapchain_, &release),
                "release native video");
      } else
#endif
      {
        if (enhance) {
          VideoView mapping;
          mapping.kind = 1;
          mapping.eye = app->swap_eyes ? 1 : 0;
          video_->draw_view(texture_, plan.input, plan.output, retained_,
                            controls, app->stereo, mapping, true);
        } else
          video_->draw_retained(
              texture_, {uint32_t(state.width), uint32_t(state.height)},
              {width_, height_}, retained_, controls, app->stereo, true);
        if (uiRenderer_)
          uiRenderer_->setVideoTexture(textureView_, sampler_);
        app->preview_ready = true;
        app->preview_is_view = enhance;
      }
      if (enhance)
        app->processing_status = status(plan, false, ui.settings.language);
      else if (failed_request_ != app->processing_request)
        app->processing_status.clear();
      app->processing_active = enhance;
      panel_input_ = plan.input;
      rendered_image_ = app->image;
      rendered_stereo_ = app->stereo;
      rendered_projection_ = app->projection;
      rendered_swap_ = app->swap_eyes;
      if (!new_frame || !initialized_)
        __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer",
                            "IMAGE_APPLIED quality=%d source=%dx%d input=%ux%u "
                            "output=%ux%u view=%d host=%s",
                            controls.quality, state.width, state.height,
                            plan.input.width, plan.input.height, width_,
                            height_, enhance,
                            immersive_ ? "immersive" : "panel");
      initialized_ = true;
      ++frames_;
      record_video(*app, before, video_->gpu_ms());
      video_import_.add(video_->timings().import_ms);video_completion_.add(video_->timings().completion_ms);
      if (frames_ == 1 || frames_ % 24 == 0)
        __android_log_print(
            ANDROID_LOG_INFO, "CyVRPlayer",
            "VIDEO_GPU_WORK operations=%llu host=%s width=%u height=%u",
            static_cast<unsigned long long>(frames_),
            immersive_ ? "immersive" : "panel", width_, height_);
      return !immersive_;
    } catch (const std::exception &e) {
      ++app->render_monitor.errors;
      if (enhance) {
        processing_error(*app, ui, e.what());
        return true;
      }
      app->show_interface();
      app->session().pause(true);
      app->preview_ready = false;
      ui.setToast(e.what());
      __android_log_print(ANDROID_LOG_ERROR, "CyVRPlayer", "RENDER_ERROR %s",
                          e.what());
      return true;
    }
  }
#if CYVR_ENABLE_IMMERSIVE
  std::vector<const char *> instanceExtensions() const override {
    return {XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME};
  }
  std::vector<const char*> optionalInstanceExtensions()const override{return {XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};}
  void xrReady(const AndroidXrContext &context) override {
    xr_ = context;
    XrReferenceSpaceCreateInfo room{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};room.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE;room.poseInReferenceSpace.orientation.w=1;
    if(XR_FAILED(xrCreateReferenceSpace(xr_.session,&room,&stable_space_)))stable_space_=XR_NULL_HANDLE;
    __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","VIDEO_ROOM_SPACE available=%d",int(stable_space_!=XR_NULL_HANDLE));
    cylinder_enabled_=std::find(context.enabledApplicationExtensions.begin(),context.enabledApplicationExtensions.end(),XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)!=context.enabledApplicationExtensions.end();
    __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","CINEMA_CAPABILITIES native_cylinder=%d",int(cylinder_enabled_));
    video_placement_.reset();
    video_space_ = XR_NULL_HANDLE;
    space_changes_.clear();
    placed_request_ = UINT64_MAX;
    uint32_t count{};
    xrCheck(xrEnumerateViewConfigurationViews(
                xr_.instance, xr_.system,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
            "display view count");
    if (count != 2)
      throw std::runtime_error("Stereo display views required");
    std::array<XrViewConfigurationView, 2> views{
        { {XR_TYPE_VIEW_CONFIGURATION_VIEW},
          { XR_TYPE_VIEW_CONFIGURATION_VIEW } }};
    xrCheck(xrEnumerateViewConfigurationViews(
                xr_.instance, xr_.system,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count,
                views.data()),
            "display view dimensions");
    for (int i = 0; i < 2; ++i)
      eye_extents_[i] = {views[i].recommendedImageRectWidth,
                         views[i].recommendedImageRectHeight};
  }
  void referenceSpaceChanging(
      const XrEventDataReferenceSpaceChangePending &change) override {
    if (video_placement_.placed() &&
        ((change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL && video_space_==xr_.local) ||
         (change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_STAGE && video_space_==stable_space_)))
      space_changes_.push_back({change.changeTime, bool(change.poseValid),
                                change.poseValid
                                    ? pose_from_xr(change.poseInPreviousSpace)
                                    : Pose {},video_space_});
  }
  uint32_t compositionLayers(const AndroidXrFrame &frame,
                              const XrCompositionLayerBaseHeader **output,
                              uint32_t capacity) override {
    auto app=application_();
    if(app && app->session().state().volumetric){try{return gaussian_layers(frame,output,capacity,*app);}catch(const std::exception& e){++app->render_monitor.errors;failed_splat_=app->session().splats().cloud;failed_splat_settings_=app->image;app->session().pause(true);app->show_interface();last_ui().setToast(e.what());return 0;}}
    auto count=video_layers(frame,output,capacity);
    if(app && count && count<capacity){
      try {
        if(const auto halo=ambilight_layer(*app)){
          for(auto i=count;i>0;--i)output[i]=output[i-1];output[0]=halo;++count;
        }
      }catch(const std::exception& e){
        ambilight_failed_epoch_=retained_.epoch;ambilight_active_=false;
        __android_log_print(ANDROID_LOG_ERROR,"CyVRPlayer","AMBILIGHT_ERROR %s",e.what());
        last_ui().setToast(std::string(player_translate(last_ui().settings.language,"Ambilight could not be rendered. Toggle it to retry.")));
      }
    }else if(!app || !app->ambilight.enabled){ambilight_active_=false;ambilight_failed_epoch_=UINT64_MAX;}
    if(app && caption_visible_ && caption_swapchain_ && count<capacity && video_placement_.placed()){
      const auto media=app->session().state();
      auto pose=video_placement_.screen(app->image,app->projection?CinemaSettings{}:app->cinema,app->scene);
      if(app->projection)pose=video_placement_.screen(ImageSettings{},CinemaSettings{},app->scene.rotation_only());
      pose.position=pose.position+rotate(pose.orientation,{0,0,.01f});
      auto& q=caption_quad_;q={XR_TYPE_COMPOSITION_LAYER_QUAD};q.space=video_space_;
      q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
      q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;q.subImage.swapchain=caption_swapchain_;
      q.subImage.imageRect={{0,0},{caption_width_,caption_height_}};
      q.pose={{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},{pose.position.x,pose.position.y,pose.position.z}};
      const auto w=app->projection?4.f:app->cinema.width*app->image.zoom;
      q.size={w,w*float(caption_height_)/caption_width_};
      if(!app->projection && app->cinema.curvature>0 && cylinder_enabled_){
        auto& cylinder=caption_cylinder_;cylinder={XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        cylinder.space=q.space;cylinder.layerFlags=q.layerFlags;cylinder.eyeVisibility=q.eyeVisibility;cylinder.subImage=q.subImage;
        const auto radius=app->cinema.radius(app->image.zoom);
        const auto middle=video_placement_.screen(app->image,app->cinema,app->scene);
        const auto center=cylinder_center(middle,radius);
        cylinder.pose={{center.orientation.x,center.orientation.y,center.orientation.z,center.orientation.w},{center.position.x,center.position.y,center.position.z}};
        cylinder.radius=std::max(.01f,radius-.01f);cylinder.centralAngle=app->cinema.angle();cylinder.aspectRatio=float(caption_width_)/caption_height_;
        output[count++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
      }else output[count++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
    }
    return count;
  }
  uint32_t video_layers(const AndroidXrFrame &frame,
                             const XrCompositionLayerBaseHeader **output,
                             uint32_t capacity) {
    auto app = application_();
    if (!app)
      return 0;
    const auto media = app->session().state();
    if (!media.ready || !place_video(frame, *app))
      return 0;
    const bool projected=retained_ && (app->image.reconstruct_view() || (!app->projection && app->cinema.curvature>0 && !cylinder_enabled_)) && failed_request_!=app->processing_request;
    const int path=app->projection?4:projected?(app->cinema.curvature>0?3:2):(app->cinema.curvature>0?1:0);
    if((reported_surface_!=path || reported_cinema_!=app->cinema) && !last_ui().interacting()){
      reported_surface_=path;reported_cinema_=app->cinema;
      __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","CINEMA_SURFACE path=%d width=%.3f distance=%.3f curve=%.3f height=%.3f tilt=%.3f roll=%.3f environment=%d ambient=%.3f",path,app->cinema.width,app->cinema.distance,app->cinema.curvature,app->cinema.elevation,app->cinema.pitch,app->cinema.roll,app->cinema.environment,app->cinema.ambient);
    }
    if (retained_ && (app->image.reconstruct_view() || (!app->projection && app->cinema.curvature>0 && !cylinder_enabled_)) &&
        failed_request_ != app->processing_request) {
      try {
        return projected_video(frame, output, capacity, *app);
      } catch (const std::exception &e) {
        ++app->render_monitor.errors;
        processing_error(*app, last_ui(), e.what());
        return 0;
      }
    }
    if (!initialized_ || !swapchain_)
      return 0;
    const uint32_t count = app->stereo ? 2 : 1;
    if (count > capacity)
      throw std::runtime_error("Video layer budget exceeded");
    for (uint32_t eye = 0; eye < count; ++eye) {
      auto &q = quads_[eye];
      q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.space = video_space_;
      q.eyeVisibility =
          app->stereo ? (eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT)
                      : XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain = swapchain_;
      q.subImage.imageRect = {{0, 0}, { int32_t(width_), int32_t(height_) }};
      const int packed = app->swap_eyes ? 1 - int(eye) : int(eye);
      auto &rect = q.subImage.imageRect;
      if (app->stereo == 1) {
        rect.extent.width /= 2;
        rect.offset.x = packed * rect.extent.width;
      }
      if (app->stereo == 2) {
        rect.extent.height /= 2;
        rect.offset.y = packed * rect.extent.height;
      }
      const auto pose = video_placement_.screen(app->image,app->projection?CinemaSettings{}:app->cinema,app->scene);
      const auto p = pose.position;
      const auto r = pose.orientation;
      q.pose = {{r.x, r.y, r.z, r.w}, { p.x, p.y, p.z }};
      const double par = app->half_stereo && app->stereo
                             ? (app->stereo == 1 ? 2. : .5)
                             : media.pixel_aspect;
      q.size = {app->cinema.width * app->image.zoom,
                app->cinema.width * app->image.zoom /
                    float(eye_aspect(width_, height_, StereoLayout(app->stereo),
                                     par))};
      if (app->projection) {
        auto &s = spheres_[eye];
        s = {XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
        // The shader keeps edge RGB below transparent coverage for filters.
        // Native VR180 must request straight-alpha composition as well, or
        // the compositor would display those edge pixels around a zoomed-out picture.
        if(app->projection==1 && app->image.zoom<1)
          s.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        s.space = q.space;
        s.eyeVisibility = q.eyeVisibility;
        s.subImage = q.subImage;
        s.pose.orientation = q.pose.orientation;
        s.radius = 0;
        s.centralHorizontalAngle = app->projection == 1 ? pi : 2 * pi;
        s.upperVerticalAngle = pi * .5f;
        s.lowerVerticalAngle = -pi * .5f;
        output[eye] =
            reinterpret_cast<const XrCompositionLayerBaseHeader *>(&s);
      } else if(app->cinema.curvature>0 && cylinder_enabled_){
        auto& cylinder=cylinders_[eye];cylinder={XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        cylinder.space=q.space;cylinder.eyeVisibility=q.eyeVisibility;cylinder.subImage=q.subImage;
        cylinder.radius=app->cinema.radius(app->image.zoom);cylinder.centralAngle=app->cinema.angle();cylinder.aspectRatio=q.size.width/q.size.height;
        const auto center=cylinder_center(pose,cylinder.radius);
        cylinder.pose={{center.orientation.x,center.orientation.y,center.orientation.z,center.orientation.w},{center.position.x,center.position.y,center.position.z}};
        output[eye]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
      }else
        output[eye] =
            reinterpret_cast<const XrCompositionLayerBaseHeader *>(&q);
    }
    return count;
  }
#endif
private:
#if CYVR_ENABLE_IMMERSIVE
  void destroy_ambilight(){
    ambilight_capture_.reset();ambilight_.reset();
    if(ambilight_swapchain_)xrDestroySwapchain(ambilight_swapchain_);
    ambilight_swapchain_=XR_NULL_HANDLE;ambilight_images_.clear();ambilight_active_=false;
    ambilight_epoch_=ambilight_failed_epoch_=UINT64_MAX;ambilight_pts_=-1;
  }
  void create_ambilight(){
    if(ambilight_ && ambilight_capture_ && ambilight_swapchain_ && !ambilight_images_.empty())return;
    destroy_ambilight();
    ambilight_=std::make_unique<AmbilightRenderer>(*gpu_,format_,assets_);
    ambilight_capture_=std::make_unique<VideoRenderer>(*gpu_,format_,assets_);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format=format_;info.sampleCount=1;info.width=info.height=128;info.faceCount=info.arraySize=info.mipCount=1;
    xrCheck(xrCreateSwapchain(xr_.session,&info,&ambilight_swapchain_),"ambilight swapchain");
    uint32_t count{};xrCheck(xrEnumerateSwapchainImages(ambilight_swapchain_,0,&count,nullptr),"ambilight images count");
    std::vector<XrSwapchainImageVulkanKHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrCheck(xrEnumerateSwapchainImages(ambilight_swapchain_,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"ambilight images");
    for(const auto& image:images)ambilight_images_.push_back(image.image);
  }
  const XrCompositionLayerBaseHeader* ambilight_layer(PlayerApplication& app){
    const auto media=app.session().state();
    if(!app.ambilight.enabled || app.projection || media.volumetric || !retained_ || !video_placement_.placed()){
      ambilight_active_=false;ambilight_failed_epoch_=UINT64_MAX;return nullptr;
    }
    if(ambilight_failed_epoch_==retained_.epoch)return nullptr;
    create_ambilight();
    const auto now=std::chrono::steady_clock::now();
    auto image=app.image;image.quality=0;image.zoom=1;image.pan_x=image.pan_y=0;
    const bool first=!ambilight_active_ || ambilight_epoch_!=retained_.epoch;
    const bool capture=first || ambilight_pts_!=retained_.pts_us || ambilight_image_!=image || ambilight_stereo_!=app.stereo;
    const float par=app.half_stereo && app.stereo?(app.stereo==1?2.f:.5f):media.pixel_aspect;
    const float aspect=float(eye_aspect(media.width,media.height,StereoLayout(app.stereo),par));
    const bool settingsChanged=ambilight_settings_!=app.ambilight || ambilight_aspect_!=aspect;
    const float dt=std::chrono::duration<float>(now-ambilight_updated_).count();
    const bool settle=!capture && !settingsChanged && std::chrono::duration<float>(now-ambilight_changed_).count()>=5*app.ambilight.smoothing;
    const bool update=first || (dt>=1.f/30 && (capture || settingsChanged || !ambilight_settled_));
    if(update){
      if(first || settle)ambilight_->reset();
      if(capture){
        VideoView mapping;mapping.kind=6;
        ambilight_capture_->draw_view(ambilight_->source_image(),{64,36},{64,36},retained_,image,app.stereo,mapping,true);
      }
      uint32_t index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      xrCheck(xrAcquireSwapchainImage(ambilight_swapchain_,&acquire,&index),"ambilight acquire");
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
      xrCheck(xrWaitSwapchainImage(ambilight_swapchain_,&wait),"ambilight wait");
      try{ambilight_->process(ambilight_images_.at(index),app.ambilight,aspect,dt);}
      catch(...){XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(ambilight_swapchain_,&release);throw;}
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrCheck(xrReleaseSwapchainImage(ambilight_swapchain_,&release),"ambilight release");
      if(capture || settingsChanged)ambilight_changed_=now;
      ambilight_updated_=now;ambilight_settings_=app.ambilight;ambilight_image_=image;ambilight_settled_=settle || app.ambilight.smoothing==0;
      ambilight_epoch_=retained_.epoch;ambilight_pts_=retained_.pts_us;ambilight_stereo_=app.stereo;ambilight_aspect_=aspect;ambilight_active_=true;
      const auto time=std::chrono::duration<double>(now.time_since_epoch()).count();
      if(time-ambilight_report_>=5){ambilight_report_=time;const auto captureMs=capture?ambilight_capture_->gpu_ms():std::optional<double>(0);
        const auto glowMs=ambilight_->gpu_ms();__android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","AMBILIGHT capture=64x36 glow=128x128 capture_gpu_ms=%.3f glow_gpu_ms=%.3f curve=%.2f",captureMs.value_or(-1),glowMs.value_or(-1),app.cinema.curvature);}
    }
    const auto middle=video_placement_.screen(app.image,app.cinema,app.scene);
    const float width=app.cinema.width*app.image.zoom,outerW=width*(1+2*app.ambilight.spread),outerH=width/aspect+2*width*app.ambilight.spread;
    auto pose=middle;pose.position=pose.position+rotate(pose.orientation,{0,0,-.02f});
    auto& quad=ambilight_quad_;quad={XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=video_space_;
    quad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;quad.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain=ambilight_swapchain_;quad.subImage.imageRect={{0,0},{128,128}};
    quad.pose={{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},{pose.position.x,pose.position.y,pose.position.z}};quad.size={outerW,outerH};
    if(app.cinema.curvature>0 && cylinder_enabled_){
      auto& cylinder=ambilight_cylinder_;cylinder={XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};cylinder.space=quad.space;cylinder.layerFlags=quad.layerFlags;cylinder.eyeVisibility=quad.eyeVisibility;cylinder.subImage=quad.subImage;
      const auto radius=app.cinema.radius(app.image.zoom);const auto center=cylinder_center(middle,radius);
      cylinder.pose={{center.orientation.x,center.orientation.y,center.orientation.z,center.orientation.w},{center.position.x,center.position.y,center.position.z}};
      cylinder.radius=radius+.02f;cylinder.centralAngle=app.cinema.angle()*(1+2*app.ambilight.spread);cylinder.aspectRatio=outerW/outerH;
      return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
    }
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
  }
#endif
  void destroy_splats(){
    splat_renderer_.reset();rendered_splat_.reset();failed_splat_.reset();
#if CYVR_ENABLE_IMMERSIVE
    for(auto& eye:gaussian_eyes_){if(eye.swapchain)xrDestroySwapchain(eye.swapchain);eye={};}
#endif
  }
  bool gaussian_panel(PlayerApplication& app,UiModel& ui){
    const auto snapshot=app.session().splats();if(!snapshot.cloud)return false;
    if(failed_splat_==snapshot.cloud && failed_splat_settings_==app.image)return false;
    const float scale=std::min(ui.viewportSize.x/1440,ui.viewportSize.y/990);
    const float w=ui.interfaceHidden?ui.viewportSize.x:900*scale,h=ui.interfaceHidden?ui.viewportSize.y:506*scale;
    const VideoExtent display{uint32_t(std::max(1.f,std::min(w,h*16/9))),uint32_t(std::max(1.f,std::min(h,w*9/16)))};
    const auto plan=view_processing_plan(display,display,app.image);
    if(rendered_splat_==snapshot.cloud && initialized_ && rendered_image_==app.image && width_==plan.output.width && height_==plan.output.height)return false;
    try{
      if(width_!=plan.output.width || height_!=plan.output.height || !texture_){splat_renderer_.reset();createVideo(plan.output.width,plan.output.height);}
      if(!splat_renderer_)splat_renderer_=std::make_unique<SplatRenderer>(*gpu_,format_,assets_);
      Pose camera{{-app.image.pan_x*2,app.image.pan_y*2,3.5f/app.image.zoom},{}};
      splat_renderer_->draw(texture_,plan.output,snapshot,camera,{-1,1,9.f/16,-9.f/16},app.image,true);
      if(uiRenderer_)uiRenderer_->setVideoTexture(textureView_,sampler_);
      app.preview_ready=true;app.preview_is_view=true;app.processing_active=app.image.quality!=0;
      rendered_splat_=snapshot.cloud;rendered_image_=app.image;initialized_=true;app.session().frame_submitted(snapshot.epoch,snapshot.pts_us);ui.invalidate();return true;
    }catch(const std::exception& e){++app.render_monitor.errors;failed_splat_=snapshot.cloud;failed_splat_settings_=app.image;app.session().pause(true);app.show_interface();ui.setToast(e.what());app.preview_ready=false;return false;}
  }
#if CYVR_ENABLE_IMMERSIVE
  uint32_t gaussian_layers(const AndroidXrFrame& frame,const XrCompositionLayerBaseHeader** output,uint32_t capacity,PlayerApplication& app){
    const auto snapshot=app.session().splats();if(!capacity || !snapshot.cloud || !place_video(frame,app))return 0;
    if(failed_splat_==snapshot.cloud && failed_splat_settings_==app.image)return 0;
    std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;locate.displayTime=frame.displayTime;locate.space=video_space_;XrViewState state{XR_TYPE_VIEW_STATE};uint32_t count{};
    xrCheck(xrLocateViews(xr_.session,&locate,&state,2,&count,views.data()),"locate Gaussian views");if(count!=2 || !(state.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT))return 0;
    const VideoExtent native{std::max(eye_extents_[0].width,eye_extents_[1].width),std::max(eye_extents_[0].height,eye_extents_[1].height)};const auto plan=view_processing_plan(native,native,app.image);
    if(std::any_of(gaussian_eyes_.begin(),gaussian_eyes_.end(),[&](const auto& eye){return !eye.swapchain || eye.extent.width!=plan.output.width || eye.extent.height!=plan.output.height;})){vkDeviceWaitIdle(gpu_->device);destroy_splats();}
    if(!splat_renderer_)splat_renderer_=std::make_unique<SplatRenderer>(*gpu_,format_,assets_);
    const auto model=video_placement_.screen(app.image,CinemaSettings{},app.scene);const auto inverse=inverse_rotation(model.orientation);
    for(int n=0;n<2;++n){auto& eye=gaussian_eyes_[n];
      if(!eye.swapchain || eye.extent.width!=plan.output.width || eye.extent.height!=plan.output.height){
        vkDeviceWaitIdle(gpu_->device);if(eye.swapchain)xrDestroySwapchain(eye.swapchain);eye={};
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;info.format=format_;info.width=plan.output.width;info.height=plan.output.height;info.sampleCount=info.faceCount=info.arraySize=info.mipCount=1;
        xrCheck(xrCreateSwapchain(xr_.session,&info,&eye.swapchain),"create Gaussian eye");uint32_t images{};xrCheck(xrEnumerateSwapchainImages(eye.swapchain,0,&images,nullptr),"Gaussian image count");std::vector<XrSwapchainImageVulkanKHR> targets(images,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});xrCheck(xrEnumerateSwapchainImages(eye.swapchain,images,&images,reinterpret_cast<XrSwapchainImageBaseHeader*>(targets.data())),"Gaussian images");for(auto& image:targets)eye.images.push_back(image.image);eye.extent=plan.output;
      }
      const auto& v=views[n];Pose camera;camera.position=rotate(inverse,Vec3{v.pose.position.x,v.pose.position.y,v.pose.position.z}-model.position)*(1.f/app.image.zoom);camera.orientation=multiply_rotation(inverse,{v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w});
      uint32_t index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};xrCheck(xrAcquireSwapchainImage(eye.swapchain,&acquire,&index),"acquire Gaussian eye");XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;xrCheck(xrWaitSwapchainImage(eye.swapchain,&wait),"wait Gaussian eye");
      try{splat_renderer_->draw(eye.images.at(index),eye.extent,snapshot,camera,{std::tan(v.fov.angleLeft),std::tan(v.fov.angleRight),std::tan(v.fov.angleUp),std::tan(v.fov.angleDown)},app.image,false);}catch(...){XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(eye.swapchain,&release);throw;}
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrCheck(xrReleaseSwapchainImage(eye.swapchain,&release),"release Gaussian eye");auto& projected=enhanced_views_[n];projected={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};projected.pose=v.pose;projected.fov=v.fov;projected.subImage.swapchain=eye.swapchain;projected.subImage.imageRect={{0,0},{int32_t(eye.extent.width),int32_t(eye.extent.height)}};
    }
    enhanced_layer_={XR_TYPE_COMPOSITION_LAYER_PROJECTION};enhanced_layer_.space=video_space_;enhanced_layer_.viewCount=2;enhanced_layer_.views=enhanced_views_.data();output[0]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&enhanced_layer_);
    app.session().frame_submitted(snapshot.epoch,snapshot.pts_us);return 1;
  }
#endif
  void destroy_captions(){
    if(uiRenderer_)uiRenderer_->setOverlayTexture();
#if CYVR_ENABLE_IMMERSIVE
    if(caption_renderer_)for(auto& target:caption_targets_)caption_renderer_->destroyTarget(target);
    caption_targets_.clear();
    if(caption_swapchain_)xrDestroySwapchain(caption_swapchain_);
    caption_swapchain_=XR_NULL_HANDLE;
#endif
    caption_renderer_.reset();captions_.reset();caption_visible_=false;
  }
  void update_captions(PlayerApplication& app,UiModel& ui,const PlayerState& state){
    const auto snapshot=app.session().subtitles();
    if(auto failed=failed_caption_.lock();failed && failed==snapshot.config)return;
    if(!captions_ && !snapshot.config){app.caption_ready=false;caption_visible_=false;return;}
    try{
      if(!captions_)captions_=std::make_unique<SubtitleRasterizer>(assets_("subtitle-font.ttf"));
      int width=std::max(1920,int(ui.viewportSize.x));
#if CYVR_ENABLE_IMMERSIVE
      if(immersive_)width=int(eye_extents_[0].width);
#endif
      width=std::clamp(width,1024,4096);
      const double par=app.half_stereo && app.stereo?(app.stereo==1?2.:.5):state.pixel_aspect;
      const double aspect=app.projection?16./9:eye_aspect(state.width,state.height,StereoLayout(app.stereo),par);
      const int height=std::clamp(int(std::lround(width/std::max(.25,aspect))),256,4096);
      auto image=captions_->render(snapshot,width,height);
      if(!image)return;
      caption_visible_=!image->rgba.empty();app.caption_ready=caption_visible_;
      ui.invalidate();
      if(!caption_visible_)return;
      if(!caption_renderer_){caption_renderer_=std::make_unique<VulkanRenderer>(*gpu_);caption_renderer_->initialize(format_,assets_);}
      caption_renderer_->uploadImages(image->width,image->height,image->rgba);
      app.caption_rect={float(image->x)/width,float(image->y)/height,float(image->width)/width,float(image->height)/height};
      if(uiRenderer_)uiRenderer_->setOverlayTexture(caption_renderer_->imageAtlasView(),caption_renderer_->imageSampler());
#if CYVR_ENABLE_IMMERSIVE
      if(immersive_){
        if(!caption_swapchain_ || caption_width_!=width || caption_height_!=height){
          vkDeviceWaitIdle(gpu_->device);
          for(auto& target:caption_targets_)caption_renderer_->destroyTarget(target);caption_targets_.clear();
          if(caption_swapchain_)xrDestroySwapchain(caption_swapchain_);caption_swapchain_=XR_NULL_HANDLE;
          XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;info.format=format_;info.sampleCount=1;info.width=width;info.height=height;info.faceCount=info.arraySize=info.mipCount=1;
          xrCheck(xrCreateSwapchain(xr_.session,&info,&caption_swapchain_),"create subtitle layer");
          uint32_t count{};xrCheck(xrEnumerateSwapchainImages(caption_swapchain_,0,&count,nullptr),"subtitle image count");
          std::vector<XrSwapchainImageVulkanKHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
          xrCheck(xrEnumerateSwapchainImages(caption_swapchain_,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"subtitle images");
          for(const auto& item:images)caption_targets_.push_back(caption_renderer_->createTarget(width,height,item.image));
          caption_width_=width;caption_height_=height;
        }
        uint32_t index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};xrCheck(xrAcquireSwapchainImage(caption_swapchain_,&acquire,&index),"acquire subtitle image");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;xrCheck(xrWaitSwapchainImage(caption_swapchain_,&wait),"wait subtitle image");
        try{DrawList draw;draw.image({float(image->x),float(image->y),float(image->width),float(image->height)},{0,0,1,1});caption_renderer_->draw(caption_targets_.at(index),draw);}
        catch(...){XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(caption_swapchain_,&release);throw;}
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrCheck(xrReleaseSwapchainImage(caption_swapchain_,&release),"release subtitle image");
      }
#endif
      __android_log_print(ANDROID_LOG_INFO,"CyVRPlayer","SUBTITLE_VISIBLE canvas=%dx%d image=%dx%d",width,height,image->width,image->height);
    }catch(const std::exception& e){failed_caption_=snapshot.config;app.caption_ready=false;caption_visible_=false;ui.setToast(e.what());__android_log_print(ANDROID_LOG_ERROR,"CyVRPlayer","SUBTITLE_RENDER_ERROR %s",e.what());}
  }
  bool zoom_settling() const {
    return std::chrono::steady_clock::now() - zoom_changed_ <
           std::chrono::milliseconds(250);
  }
  void record_video(PlayerApplication &app,
                    std::chrono::steady_clock::time_point before,
                    std::optional<double> gpu) {
    app.session().frame_submitted(retained_.epoch, retained_.pts_us);
    app.render_monitor.work.add(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - before)
                                    .count());
    if (gpu)
      app.render_monitor.gpu.add(*gpu);
  }
  UiModel &last_ui() { return *ui_; }
  std::string status(const ViewProcessingPlan &plan, bool eye,
                     const std::string &language) {
    return std::string(
               player_translate(language, eye ? "Per-eye reconstruction"
                                              : "Display reconstruction")) +
           ": " + std::to_string(plan.input.width) + "x" +
           std::to_string(plan.input.height) + " -> " +
           std::to_string(plan.output.width) + "x" +
           std::to_string(plan.output.height);
  }
  void processing_error(PlayerApplication &app, UiModel &ui,
                        const char *error) {
    failed_request_ = app.processing_request;
    app.processing_active = false;
    app.processing_status = std::string(player_translate(ui.settings.language,
                                                         "Enhancement error")) +
                            ": " + error;
    ui.setToast(app.processing_status);
    ui.invalidate();
    __android_log_print(ANDROID_LOG_ERROR, "CyVRPlayer",
                        "FILTER_ERROR requested=%d %s", app.image.quality,
                        error);
  }
#if CYVR_ENABLE_IMMERSIVE
  bool place_video(const AndroidXrFrame &frame, PlayerApplication &app) {
    while (!space_changes_.empty() &&
           space_changes_.front().time <= frame.displayTime) {
      const auto change = space_changes_.front();
      space_changes_.pop_front();
      if(app.scene.stable && change.space==video_space_){
        if(change.valid)video_placement_.transform(inverse_pose(change.new_in_previous));
        else last_ui().setToast(std::string(player_translate(last_ui().settings.language,"Tracking origin changed; adjust video placement if needed.")));
      }
    }
    const XrSpace preferred =
        frame.panelSpace==xr_.view?xr_.view:(app.scene.stable && stable_space_?stable_space_:xr_.local);
    constexpr auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                           XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (video_placement_.placed() && video_space_!=preferred && preferred!=xr_.view) {
      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(xrLocateSpace(video_space_, preferred, frame.displayTime,
                                     &location)) &&
          (location.locationFlags & valid) == valid) {
        video_placement_.transform(pose_from_xr(location.pose));
        video_space_ = preferred;
      }
    }
    if (!video_placement_.placed() ||
        placed_request_ != app.video_placement_request) {
      Pose head;
      if (preferred != xr_.view) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(xr_.view, preferred, frame.displayTime,
                                    &location)) ||
            (location.locationFlags & valid) != valid)
          return video_placement_.placed();
        head = pose_from_xr(location.pose);
      }
      video_placement_.place(head, last_ui().settings.distance,app.video_placement_request!=0);
      video_space_ = preferred;
      placed_request_ = app.video_placement_request;
      __android_log_print(
          ANDROID_LOG_INFO, "CyVRPlayer",
          "VIDEO_PLACED independent_panel=1 space=%s request=%llu",
          video_space_ == xr_.view ? "view" : video_space_==stable_space_?"room":"local",
          static_cast<unsigned long long>(placed_request_));
    }
    return video_placement_.placed();
  }
  void destroy_enhanced() {
    for (auto &eye : enhanced_) {
      eye.renderer.reset();
      if (eye.swapchain)
        xrDestroySwapchain(eye.swapchain);
      eye.swapchain = XR_NULL_HANDLE;
      eye.images.clear();
      eye.quality = -1;
    }
  }
  void ensure_enhanced(const std::array<ViewProcessingPlan, 2> &plans,
                       int quality) {
    bool changed = bool(video_) || bool(swapchain_);
    for (int i = 0; i < 2; ++i) {
      const auto &eye = enhanced_[i];
      const auto &plan = plans[i];
      changed = changed || !eye.swapchain || eye.quality != quality ||
                eye.input.width != plan.input.width ||
                eye.input.height != plan.input.height ||
                eye.output.width != plan.output.width ||
                eye.output.height != plan.output.height;
    }
    if (!changed)
      return;
    vkDeviceWaitIdle(gpu_->device);
    destroyVideo();
    for (int i = 0; i < 2; ++i) {
      auto &eye = enhanced_[i];
      eye.input = plans[i].input;
      eye.output = plans[i].output;
      eye.quality = quality;
      XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      info.format = format_;
      info.sampleCount = 1;
      info.width = eye.output.width;
      info.height = eye.output.height;
      info.faceCount = info.arraySize = info.mipCount = 1;
      xrCheck(xrCreateSwapchain(xr_.session, &info, &eye.swapchain),
              "create display-view video target");
      uint32_t count{};
      xrCheck(xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr),
              "display-view image count");
      std::vector<XrSwapchainImageVulkanKHR> images(
          count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrCheck(
          xrEnumerateSwapchainImages(
              eye.swapchain, count, &count,
              reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
          "display-view images");
      for (const auto &image : images)
        eye.images.push_back(image.image);
      eye.renderer = std::make_unique<VideoRenderer>(*gpu_, format_, assets_);
    }
  }
  uint32_t projected_video(const AndroidXrFrame &frame,
                           const XrCompositionLayerBaseHeader **output,
                           uint32_t capacity, PlayerApplication &app) {
    const auto before = std::chrono::steady_clock::now();
    if (!capacity)
      return 0;
    std::array<XrView, 2> views{{ {XR_TYPE_VIEW}, { XR_TYPE_VIEW } }};
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frame.displayTime;
    locate.space = video_space_;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count{};
    xrCheck(
        xrLocateViews(xr_.session, &locate, &state, 2, &count, views.data()),
        "locate video display views");
    if (count != 2)
      return 0;
    // The shared host can explicitly place its scene in VIEW space before
    // tracking is available. Keep that head-relative fallback usable too.
    if (!(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
      if (video_space_ != xr_.view)
        return 0;
      for (auto &view : views)
        view.pose = {{0, 0, 0, 1}, {0, 0, 0}};
    }
    for (const auto &view : views)
      if (!std::isfinite(view.fov.angleLeft) ||
          !std::isfinite(view.fov.angleRight) ||
          !std::isfinite(view.fov.angleUp) ||
          !std::isfinite(view.fov.angleDown) ||
          view.fov.angleRight <= view.fov.angleLeft ||
          view.fov.angleUp <= view.fov.angleDown)
        return 0;
    const auto media = app.session().state();
    const auto videoPose = video_placement_.screen(app.image,app.projection?CinemaSettings{}:app.cinema,app.scene);
    const auto videoPosition = videoPose.position;
    const auto inverse = inverse_rotation(videoPose.orientation);
    const double par = app.half_stereo && app.stereo
                           ? (app.stereo == 1 ? 2. : .5)
                           : media.pixel_aspect;
    const float aspect = float(
        eye_aspect(media.width, media.height, StereoLayout(app.stereo), par));
    const float screenW = app.cinema.width * app.image.zoom, screenH = screenW / aspect;
    std::array<ViewProcessingPlan, 2> plans;
    std::array<VideoView, 2> mapping;
    for (int i = 0; i < 2; ++i) {
      const auto &view = views[i];
      auto &map = mapping[i];
      map.kind = app.projection == 0 ? 2 : app.projection == 1 ? 3 : 4;
      if(!app.projection && app.cinema.curvature>0)map.kind=5;
      map.eye = app.swap_eyes ? 1 - i : i;
      map.fov[0] = std::tan(view.fov.angleLeft);
      map.fov[1] = std::tan(view.fov.angleRight);
      map.fov[2] = std::tan(view.fov.angleUp);
      map.fov[3] = std::tan(view.fov.angleDown);
      const auto &q = view.pose.orientation;
      map.orientation = multiply_rotation(inverse, {q.x, q.y, q.z, q.w});
      map.origin =
          rotate(inverse, Vec3{view.pose.position.x, view.pose.position.y,
                               view.pose.position.z} -
                              videoPosition);
      map.screen_width = screenW;
      map.screen_height = screenH;
      map.screen_radius=app.cinema.radius(app.image.zoom);
      const float hfov = view.fov.angleRight - view.fov.angleLeft,
                  vfov = view.fov.angleUp - view.fov.angleDown;
      const float crop = app.projection ? app.image.zoom : 1;
      const float sourceW = media.width / float(app.stereo == 1 ? 2 : 1) / crop,
                  sourceH =
                      media.height / float(app.stereo == 2 ? 2 : 1) / crop;
      const auto distance=std::max(.1f,map.origin.z);
      const auto halfAngle=app.cinema.angle()*.5f;
      const float cinemaAngular=map.screen_radius>0?2*std::atan2(map.screen_radius*std::sin(halfAngle),distance-map.screen_radius*(1-std::cos(halfAngle))):2*std::atan(screenW/(2*distance));
      const float angularW = app.projection
                                 ? (app.projection == 1 ? pi : 2 * pi)
                                 : std::max(.01f,cinemaAngular);
      const float angularH = app.projection ? pi : 2 * std::atan(screenH / (2*distance));
      const VideoExtent available{
          uint32_t(std::max(1.f, std::ceil(sourceW * hfov / angularW))),
          uint32_t(std::max(1.f, std::ceil(sourceH * vfov / angularH)))};
      plans[i] = view_processing_plan(eye_extents_[i], available, app.image);
      const auto &previous = enhanced_[i];
      if (zoom_settling() && previous.renderer &&
          previous.quality == app.image.quality &&
          previous.output.width == plans[i].output.width &&
          previous.output.height == plans[i].output.height)
        plans[i].input = previous.input;
    }
    const bool configChanged =
        !initialized_ || rendered_image_ != app.image || rendered_cinema_!=app.cinema ||
        rendered_stereo_ != app.stereo ||
        rendered_projection_ != app.projection ||
        rendered_swap_ != app.swap_eyes ||
        enhanced_[0].input.width != plans[0].input.width ||
        enhanced_[0].input.height != plans[0].input.height;
    ensure_enhanced(plans, app.image.quality);
    for (int i = 0; i < 2; ++i) {
      auto &eye = enhanced_[i];
      uint32_t index{};
      XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      xrCheck(xrAcquireSwapchainImage(eye.swapchain, &acquire, &index),
              "acquire enhanced view");
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait.timeout = XR_INFINITE_DURATION;
      xrCheck(xrWaitSwapchainImage(eye.swapchain, &wait), "wait enhanced view");
      auto settings = app.image;
      if (!app.projection) {
        settings.zoom = 1;
        settings.pan_x = settings.pan_y = 0;
      }
      try {
        eye.renderer->draw_view(eye.images.at(index), eye.input, eye.output,
                                retained_, settings, app.stereo, mapping[i],
                                false);
      } catch (...) {
        XrSwapchainImageReleaseInfo release{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(eye.swapchain, &release);
        throw;
      }
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrCheck(xrReleaseSwapchainImage(eye.swapchain, &release),
              "release enhanced view");
      auto &projected = enhanced_views_[i];
      projected = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
      projected.pose = views[i].pose;
      projected.fov = views[i].fov;
      projected.subImage.swapchain = eye.swapchain;
      projected.subImage.imageRect = {{0, 0},
                                      { int32_t(eye.output.width),
                                        int32_t(eye.output.height) }};
    }
    enhanced_layer_ = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    enhanced_layer_.space = video_space_;
    enhanced_layer_.layerFlags =
        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    enhanced_layer_.viewCount = 2;
    enhanced_layer_.views = enhanced_views_.data();
    output[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(
        &enhanced_layer_);
    app.processing_active = true;
    if (configChanged)
      app.processing_status =
          status(plans[0], true, last_ui().settings.language);
    if (configChanged) {
      last_ui().invalidate();
      __android_log_print(ANDROID_LOG_INFO, "CyVRPlayer",
                          "VIEW_ENHANCEMENT quality=%d source=%dx%d "
                          "input=%ux%u output=%ux%u projection=%d eyes=2",
                          app.image.quality, media.width, media.height,
                          plans[0].input.width, plans[0].input.height,
                          plans[0].output.width, plans[0].output.height,
                          app.projection);
    }
    rendered_image_ = app.image;
    rendered_cinema_=app.cinema;
    rendered_stereo_ = app.stereo;
    rendered_projection_ = app.projection;
    rendered_swap_ = app.swap_eyes;
    initialized_ = true;
    ++frames_;
    const auto left = enhanced_[0].renderer->gpu_ms(),
               right = enhanced_[1].renderer->gpu_ms();
    record_video(app, before,
                 left && right ? std::optional<double>(*left + *right)
                               : std::nullopt);
    return 1;
  }
#endif
  int reportedHidden_{-1};
  int reportedCompact_{-1};
  void destroyVideo() {
#if CYVR_ENABLE_IMMERSIVE
    destroy_enhanced();
#endif
    video_.reset();
#if CYVR_ENABLE_IMMERSIVE
    if (swapchain_)
      xrDestroySwapchain(swapchain_);
    swapchain_ = XR_NULL_HANDLE;
    images_.clear();
#endif
    if (textureView_)
      vkDestroyImageView(gpu_->device, textureView_, nullptr);
    if (texture_)
      vkDestroyImage(gpu_->device, texture_, nullptr);
    if (textureMemory_)
      vkFreeMemory(gpu_->device, textureMemory_, nullptr);
    texture_ = VK_NULL_HANDLE;
    textureView_ = VK_NULL_HANDLE;
    textureMemory_ = VK_NULL_HANDLE;
    initialized_ = false;
    width_ = height_ = 0;
  }
  void createVideo(uint32_t width, uint32_t height) {
    if (uiRenderer_)
      uiRenderer_->setVideoTexture();
    vkDeviceWaitIdle(gpu_->device);
    destroyVideo();
    width_ = width;
    height_ = height;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_->physical, &properties);
    if (!width || !height || width > properties.limits.maxImageDimension2D ||
        height > properties.limits.maxImageDimension2D)
      throw std::runtime_error(
          "Native video dimensions exceed this GPU's image limits");
    if(auto app=application_();!app || !app->session().state().volumetric)video_ = std::make_unique<VideoRenderer>(*gpu_, format_, assets_);
#if CYVR_ENABLE_IMMERSIVE
    if (immersive_) {
      XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
      info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      info.format = format_;
      info.sampleCount = 1;
      info.width = width;
      info.height = height;
      info.faceCount = info.arraySize = info.mipCount = 1;
      xrCheck(xrCreateSwapchain(xr_.session, &info, &swapchain_),
              "allocate native-resolution video");
      uint32_t count{};
      xrCheck(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr),
              "video image count");
      std::vector<XrSwapchainImageVulkanKHR> images(
          count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrCheck(
          xrEnumerateSwapchainImages(
              swapchain_, count, &count,
              reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
          "video images");
      for (const auto &image : images)
        images_.push_back(image.image);
      return;
    }
#endif
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format_;
    image.extent = {width, height, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateImage(gpu_->device, &image, nullptr, &texture_),
            "native preview image");
    VkMemoryRequirements memory{};
    vkGetImageMemoryRequirements(gpu_->device, texture_, &memory);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = memory.size;
    allocate.memoryTypeIndex = gpu_->memoryType(
        memory.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(gpu_->device, &allocate, nullptr, &textureMemory_),
            "native preview memory");
    vkCheck(vkBindImageMemory(gpu_->device, texture_, textureMemory_, 0),
            "native preview binding");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format_;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCheck(vkCreateImageView(gpu_->device, &view, nullptr, &textureView_),
            "native preview view");
  }
};
} // namespace
std::shared_ptr<AndroidRenderExtension>
make_android_renderer(std::function<PlayerApplication *()> app,
                      bool immersive) {
  return std::make_shared<Renderer>(std::move(app), immersive);
}
} // namespace cyvr::player
