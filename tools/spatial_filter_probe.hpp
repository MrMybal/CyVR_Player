// Synthetic pictures only. Exercise the actual Android projection shader.
#pragma once
#include <array>
inline int check_spatial_filters(cyvr::VulkanContext &gpu,
                                 cyvr::VulkanRenderer &renderer,
                                 const cyvr::AssetReader &assets) {
  using namespace cyvr;
  using namespace cyvr::player;
  int checks = 0;
  VulkanRenderer encoded(gpu);
  encoded.initialize(VK_FORMAT_R8G8B8A8_UNORM, assets);
  // A CPU separable reconstruction reference at non-integer source coordinates.
  constexpr int side = 16, w = 48,
                h = 80; // Avoid implementation-defined nearest ties.
  auto source = encoded.createTarget(side, side);
  DrawList pattern;
  std::array<std::array<double, 3>, side * side> values{};
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x) {
      const unsigned r = 48 + (x * 13 + y * 7) % 128,
                     g = 52 + (x * 5 + y * 17) % 122,
                     b = 60 + (x * 19 + y * 3) % 110;
      values[y * side + x] = {r / 255., g / 255., b / 255.};
      pattern.rect({float(x), float(y), 1, 1},
                   Color{r / 255.f, g / 255.f, b / 255.f, 1});
    }
  encoded.draw(source, pattern);
  readable(gpu, source.image);
  auto target = renderer.createTarget(w, h);
  {
    ProjectionProbe projection(gpu, assets, source.view, VK_FILTER_NEAREST);
    auto sample = [&](int x, int y, int c) {
      return values[std::clamp(y, 0, side - 1) * side +
                    std::clamp(x, 0, side - 1)][c];
    };
    auto kernel = [](double t) {
      t = std::abs(t);
      return t < 1   ? 1. - 2.5 * t * t + 1.5 * t * t * t
             : t < 2 ? 2. - 4. * t + 2.5 * t * t - .5 * t * t * t
                     : 0.;
    };
    auto display = [](double e) {
      const auto l =
          e < .081 ? e / 4.5 : std::pow((e + .099) / 1.099, 1. / .45);
      return int(std::round(
          255 *
          (l < .0031308 ? 12.92 * l : 1.055 * std::pow(l, 1. / 2.4) - .055)));
    };
    for (int method = 0; method < 3; ++method) {
      ImageSettings settings;
      settings.sampling = method;
      VideoView view;
      view.kind = 1;
      projection.run(target.image, {w, h},
                     video_push({0, 0, 1, 1}, settings, 0, view));
      auto pixels = renderer.readback(target);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          for (int c = 0; c < 3; ++c) {
            const double px = std::clamp((x + .5) * side / w - .5, 0.,
                                         double(side - 1)),
                         py = std::clamp((y + .5) * side / h - .5, 0.,
                                         double(side - 1));
            const int ix = int(std::floor(px)), iy = int(std::floor(py));
            double expected = 0, lo = 1, hi = 0;
            if (method == 0)
              expected =
                  sample(int(std::floor(px + .5)), int(std::floor(py + .5)), c);
            else
              for (int dy = (method == 1 ? 0 : -1); dy <= (method == 1 ? 1 : 2);
                   ++dy)
                for (int dx = (method == 1 ? 0 : -1);
                     dx <= (method == 1 ? 1 : 2); ++dx) {
                  const auto v = sample(ix + dx, iy + dy, c);
                  lo = std::min(lo, v);
                  hi = std::max(hi, v);
                  const auto weight =
                      method == 1 ? (dx ? px - ix : 1. - (px - ix)) *
                                        (dy ? py - iy : 1. - (py - iy))
                                  : kernel(px - ix - dx) * kernel(py - iy - dy);
                  expected += v * weight;
                }
            if (method == 2)
              expected = std::clamp(expected, lo, hi);
            if (std::abs(int(pixels[(y * w + x) * 4 + c]) - display(expected)) >
                2)
              throw std::runtime_error(
                  "Spatial interpolation differs from CPU reference: mode=" +
                  std::to_string(method) + " pixel=" + std::to_string(x) + "," +
                  std::to_string(y) +
                  " expected=" + std::to_string(display(expected)) +
                  " got=" + std::to_string(pixels[(y * w + x) * 4 + c]));
          }
      ++checks;
    }
  }
  renderer.destroyTarget(target);
  encoded.destroyTarget(source);
  // No constant-colour shift or opposite-eye bleeding, through all upscalers,
  // including a cropped decoder buffer, zoom/pan and the VR180 alpha boundary.
  for (int packing : {0, 1, 2}) {
    auto src =
        encoded.createTarget(packing == 1 ? 64 : 32, packing == 2 ? 64 : 32);
    DrawList d;
    d.rect({0, 0, float(src.width), float(src.height)}, Color::hex(0xff0000));
    if (packing)
      d.rect({packing == 1 ? 32.f : 0, packing == 2 ? 32.f : 0, 32, 32},
             Color::hex(0x0000ff));
    encoded.draw(src, d);
    readable(gpu, src.image);
    {
      for (int method = 0; method < 3; ++method)
        for (int quality = 0; quality < 4; ++quality)
          for (int eye = 0; eye < (packing ? 2 : 1); ++eye)
            for (bool panorama : {false, true}) {
              ImageSettings settings;
              settings.sampling = method;
              settings.denoise = .8f;
              settings.detail = .7f;
              settings.quality = quality;
              settings.zoom = panorama ? .5f : 1.8f;
              settings.pan_x = .7f;
              settings.pan_y = -.8f;
              VideoView view;
              view.kind = panorama ? 3 : 1;
              view.eye = eye;
              const VideoExtent input{quality == 1 ? 64u : 32u,
                                      quality == 1 ? 64u : 32u},
                  output{64, 64};
              auto out = renderer.createTarget(64, 64);
              std::unique_ptr<ImageProcessor> processor;
              {
                ProjectionProbe projection(gpu, assets, src.view,
                                           VK_FILTER_NEAREST);
                if (quality) {
                  processor = std::make_unique<ImageProcessor>(
                      gpu, VK_FORMAT_R8G8B8A8_SRGB, assets, input, output,
                      quality);
                  auto projectionSettings = settings;
                  projectionSettings.exposure = 0;
                  projectionSettings.contrast = projectionSettings.gamma =
                      projectionSettings.saturation = 1;
                  auto corrections = settings;
                  corrections.zoom = 1;
                  corrections.pan_x = corrections.pan_y = 0;
                  projection.run(processor->source_image(), input,
                                 video_push({.125f, .125f, .75f, .75f},
                                            projectionSettings, packing, view),
                                 true, [&](VkCommandBuffer cmd) {
                                   processor->record(cmd, out.image,
                                                     corrections, 0, false);
                                 });
                } else
                  projection.run(out.image, output,
                                 video_push({.125f, .125f, .75f, .75f},
                                            settings, packing, view));
                auto pixels = renderer.readback(out);
                int visible = 0;
                for (std::size_t n = 0; n < pixels.size(); n += 4)
                  if (pixels[n + 3] > 240) {
                    ++visible;
                    if (pixels[n + (eye ? 2 : 0)] < 250 ||
                        pixels[n + (eye ? 0 : 2)] > 4 || pixels[n + 1] > 4)
                      throw std::runtime_error(
                          "Spatial filters changed a constant eye or crossed "
                          "its boundary");
                  }
                if (!visible || (panorama && pixels[3] > 4))
                  throw std::runtime_error(
                      "Spatial filters lost VR180 coverage: method=" +
                      std::to_string(method) +
                      " quality=" + std::to_string(quality) +
                      " packing=" + std::to_string(packing) +
                      " visible=" + std::to_string(visible) +
                      " alpha=" + std::to_string(pixels[3]));
              }
              processor.reset();
              renderer.destroyTarget(out);
              ++checks;
            }
    }
    encoded.destroyTarget(src);
  }
  // Measurable noise removal on flat regions; detail must increase contrast on
  // low-contrast steps while preserving the mean. No implementation oracle.
  for (bool noisy : {false, true}) {
    auto src = encoded.createTarget(32, 32);
    DrawList d;
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        int v = noisy ? 128 + (((x + y) & 1) ? 7 : -7)
                      : (((x / 4) & 1) ? 147 : 109);
        d.rect({float(x), float(y), 1, 1},
               Color{v / 255.f, v / 255.f, v / 255.f, 1});
      }
    encoded.draw(src, d);
    readable(gpu, src.image);
    auto out = renderer.createTarget(32, 32);
    {
      ProjectionProbe projection(gpu, assets, src.view, VK_FILTER_NEAREST);
      double reference = 0;
      for (bool enabled : {false, true}) {
        ImageSettings settings;
        if (enabled) {
          settings.denoise = noisy ? 1 : 0;
          settings.detail = noisy ? 0 : 1;
        }
        VideoView view;
        view.kind = 1;
        projection.run(out.image, {32, 32},
                       video_push({0, 0, 1, 1}, settings, 0, view));
        auto pixels = renderer.readback(out);
        double sum = 0, sum2 = 0;
        int count = 0;
        for (int y = 3; y < 29; ++y)
          for (int x = 3; x < 29; ++x) {
            double v = pixels[(y * 32 + x) * 4];
            sum += v;
            sum2 += v * v;
            ++count;
          }
        double variance = sum2 / count - std::pow(sum / count, 2);
        if (!enabled)
          reference = variance;
        else if (noisy ? variance > reference * .75
                       : variance < reference * 1.05)
          throw std::runtime_error(
              noisy ? "Noise reduction did not remove synthetic grain"
                    : "Fine detail did not increase local contrast");
        ++checks;
      }
    }
    renderer.destroyTarget(out);
    encoded.destroyTarget(src);
  }
  std::cout << "{\"spatial_filter_checks\":" << checks
            << ",\"cpu_interpolation_reference\":true,\"noise_reduction\":true,"
               "\"fine_detail\":true}\n";
  return checks;
}
