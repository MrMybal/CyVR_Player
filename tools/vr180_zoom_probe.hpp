// Original constant-colour eyes; CPU angular coverage is independent of GLSL.
inline int check_vr180_zoom(cyvr::VulkanContext &gpu,
                            cyvr::VulkanRenderer &renderer,
                            const cyvr::AssetReader &assets) {
  using namespace cyvr;
  using namespace cyvr::player;
  int checks = 0;
  VulkanRenderer encoded(gpu);
  encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM, assets);
  constexpr int side = 128;
  for (int packing : {0, 1, 2}) {
    auto source = encoded.createTarget(packing == 1 ? side * 2 : side,
                                       packing == 2 ? side * 2 : side);
    DrawList pattern;
    pattern.rect({0, 0, float(source.width), float(source.height)},
                 Color::hex(0xff0000));
    if (packing)
      pattern.rect({packing == 1 ? float(side) : 0,
                    packing == 2 ? float(side) : 0, float(side), float(side)},
                   Color::hex(0x0000ff));
    encoded.draw(source, pattern);
    readable(gpu, source.image);
    for (int quality = 0; quality < 4; ++quality) {
      const VideoExtent input{quality < 2 ? 128u : 64u,
                              quality < 2 ? 128u : 64u},
          output{128, 128};
      auto processor =
          quality
              ? std::make_unique<ImageProcessor>(gpu, VK_FORMAT_R8G8B8A8_SRGB,
                                                 assets, input, output, quality)
              : nullptr;
      auto target = renderer.createTarget(side, side);
      ProjectionProbe projection(gpu, assets, source.view);
      for (float zoom : {.25f, .5f})
        for (float pan : {-1.f, 0.f, 1.f})
          for (int eye = 0; eye < (packing ? 2 : 1); ++eye)
            for (bool back : {false, true}) {
              ImageSettings settings;
              settings.zoom = zoom;
              settings.pan_x = pan;
              settings.quality = quality;
              VideoView view;
              view.kind = 3;
              view.eye = eye;
              view.fov[0] = -2;
              view.fov[1] = 2;
              if (back)
                view.orientation = {0, 1, 0, 0};
              auto corrections = ImageSettings{};
              corrections.quality = quality;
              projection.run(
                  processor ? processor->source_image() : target.image, input,
                  video_push({0, 0, 1, 1}, settings, packing, view),
                  bool(processor), [&](VkCommandBuffer cmd) {
                    if (processor)
                      processor->record(cmd, target.image, corrections, 0,
                                        false);
                  });
              const auto pixels = renderer.readback(target);
              int checked = 0;
              for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x) {
                  const double rx = -2 + 4 * (x + .5) / side,
                               ry = 1 - 2 * (y + .5) / side;
                  // atan2(x,-z): a forward ray has z=-1, its yaw-flipped ray
                  // z=+1.
                  const double lon =
                      back ? std::atan2(-rx, -1.) : std::atan2(rx, 1.);
                  const double latitude =
                      std::atan2(ry, std::sqrt(rx * rx + 1));
                  const double u = (lon / 3.141592653589793 + .5 - .5) / zoom +
                                   .5 + pan * (1 - 1 / zoom) * .5;
                  const double v = (-latitude / 3.141592653589793) / zoom + .5;
                  const double margin = std::min({u, 1 - u, v, 1 - v});
                  if (!back && std::abs(margin) < .12)
                    continue; // Bilinear alpha boundary.
                  const bool visible = !back && margin > 0;
                  const auto n = (std::size_t(y) * side + x) * 4;
                  if (visible) {
                    if (pixels[n + 3] < 250 ||
                        pixels[n + (eye ? 2 : 0)] < 250 ||
                        pixels[n + (eye ? 0 : 2)] > 3 || pixels[n + 1] > 3)
                      throw std::runtime_error(
                          "VR180 zoom-out lost eye isolation/visible colour");
                  } else if (pixels[n + 3] > 4)
                    throw std::runtime_error(
                        "VR180 zoom-out stretched or repeated pixels outside "
                        "coverage");
                  ++checked;
                }
              if (!checked)
                throw std::runtime_error("No VR180 coverage samples checked");
              ++checks;
            }
      processor.reset();
      renderer.destroyTarget(target);
    }
    encoded.destroyTarget(source);
  }
  return checks;
}
