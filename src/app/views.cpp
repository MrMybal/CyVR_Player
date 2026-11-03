#include "cyvr/locale.hpp"
#include "cyvr/player/application.hpp"
#include "cyvr/player/media.hpp"
#include <cstdio>
namespace cyvr::player {
namespace {
std::string fit(cyvr::DrawList &d, std::string_view value, float width,
                float size = 24) {
  return fit_text(d, value, width, size);
}
std::string clockText(std::int64_t us) {
  char out[40];
  auto s = std::max<std::int64_t>(0, us / 1000000);
  std::snprintf(out, sizeof(out), "%02lld:%02lld:%02lld",
                static_cast<long long>(s / 3600),
                static_cast<long long>(s / 60 % 60),
                static_cast<long long>(s % 60));
  return out;
}
} // namespace
bool PlayerApplication::draw_performance(cyvr::DrawList &d) {
  using cyvr::Color;
  d.clear();
  const auto white = Color::hex(0xf0f5f6), muted = Color::hex(0xa7b7bf),
             accent = Color::hex(0x86e7cf);
  d.rect({0, 0, 620, 760}, Color::hex(0x16262f, .98f), 22);
  d.text(28, 24, 20, tr("MEASURED PERFORMANCE"), accent);
  d.text(28, 66, 34, tr("Video and rendering"), white);
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const auto video = session_.metrics();
  auto value = [&](std::optional<double> n, std::string_view unit,
                   int precision = 1) {
    return n ? cyvr::formatNumber(ui_.settings.language, *n, precision) +
                   std::string(unit)
             : std::string(tr("Not measured"));
  };
  float y = 130;
  auto row = [&](std::string_view label, const std::string &number) {
    d.text(28, y, 20, tr(label), muted);
    d.text(345, y, 21, number, white);
    y += 41;
  };
  row("Source (nominal)",
      value(state_.nominal_fps > 0 ? std::optional<double>(state_.nominal_fps)
                                   : std::nullopt,
            " fps", 2));
  row("Video to GPU", value(video.submit_rate, " /s"));
  row("Decoder output", value(video.decode_rate, " /s"));
  row("App submissions", value(render_monitor.submissions.sample(now), " /s"));
  row("Runtime refresh",
      value(ui_.stats.displayHz > 0 ? std::optional<double>(ui_.stats.displayHz)
                                    : std::nullopt,
            " Hz", 0));
  row("Video GPU mean", value(render_monitor.gpu.mean(), " ms", 2));
  row("Video GPU p95", value(render_monitor.gpu.p95(), " ms", 2));
  row("Video work + waits", value(render_monitor.work.mean(), " ms", 2));
  row("Late drops / decoded",
      std::to_string(video.dropped) + " / " + std::to_string(video.decoded));
  row("Queued frames", std::to_string(video.queued));
  row("Render errors", std::to_string(render_monitor.errors));
  d.line({28, 594}, {592, 594}, muted);
  d.text(28, 613, 18, tr("Video: distinct frames; VR redraws are separate."),
         accent);
  d.text(28, 648, 18, tr("Submissions do not measure compositor display."),
         muted);
  d.text(28, 683, 18, tr("GPU: video passes, both eyes in VR; excludes UI."),
         muted);
  d.text(28, 718, 18, tr("Close with the Performance button or HUD key."),
         muted);
  return true;
}
bool PlayerApplication::draw(cyvr::DrawList &d) {
  using cyvr::Color;
  d.clear();icon_hint_.clear();
  ui_.clearApplicationWidgets();
  const bool light = ui_.settings.theme == cyvr::Theme::Light,
             ocean = ui_.settings.theme == cyvr::Theme::Ocean;
  const bool immersive = ui_.presentation == cyvr::Presentation::Immersive;
  auto video = [&](cyvr::Rect area, bool fullWindow = false) {
    d.rect(area, Color::hex(0x000000));
    if (!preview_ready || immersive || state_.width <= 0 || state_.height <= 0)
      return;
    const double par =
        half_stereo && stereo ? (stereo == 1 ? 2. : .5) : state_.pixel_aspect;
    const float aspect = float(
        eye_aspect(state_.width, state_.height, StereoLayout(stereo), par));
    const float windowAspect = fullWindow && ui_.viewportSize.y > 0
                                   ? ui_.viewportSize.x / ui_.viewportSize.y
                                   : area.w / area.h;
    auto picture = area;
    picture.w *= std::min(1.f, aspect / windowAspect);
    picture.h *= std::min(1.f, windowAspect / aspect);
    picture.x += (area.w - picture.w) * .5f;
    picture.y += (area.h - picture.h) * .5f;
    auto uv = preview_is_view
                  ? UvRect{0, 0, 1, 1}
                  : eye_rect(StereoLayout(stereo), Eye::left, swap_eyes);
    d.video(picture, {uv.x, uv.y, uv.width, uv.height});
    if(caption_ready)d.overlay({picture.x+picture.w*caption_rect.x,picture.y+picture.h*caption_rect.y,picture.w*caption_rect.w,picture.h*caption_rect.h});
  };
  if (ui_.interfaceHidden) {
    video({0, 0, 1440, 990}, true);
    return true;
  }
  auto white = Color::hex(light ? 0x142b35 : 0xf0f5f6),
       muted = Color::hex(light ? 0x48616b : 0xa7b7bf);
  auto accent = Color::hex(light ? 0x126854 : ocean ? 0x95bdff : 0x86e7cf);
  auto surface = Color::hex(light ? 0xc1d3db : ocean ? 0x293e66 : 0x22323b);
  if(immersive && ui_.compactInterface){
    d.rect({650,674,140,7},muted,3);
    ui_.applicationWidget(605,{630,660,180,34},cyvr::Control::Drag);
    if(ui_.hovered()==605)icon_hint_=std::string(tr("Move playback controls"));
    d.rect({228,694,984,264},Color::hex(light?0xe4eef1:0x15242d,.96f),20);
    const int toolbar[]{1364,1365,1366,1367,1662,1368,1369};
    const Icon glyphs[]{Icon::Library,Icon::Info,Icon::Image,Icon::Move,Icon::Cinema,Icon::Tracks,Icon::Settings};
    const char* hints[]{"Library","Movie information","Image quality","Video placement","Cinema","Tracks, subtitles and chapters","Settings"};
    for(int n=0;n<7;++n)icon_button(d,toolbar[n],{252.f+n*76,706,62,48},glyphs[n],hints[n]);
    icon_button(d,1360,{1134,706,54,48},Icon::Window,"Full interface");
    const auto position=timeline_position();
    const bool seek=state_.ready && state_.seekable && state_.duration_us>0;
    const float progress=state_.duration_us>0 ? float(std::clamp(double(position)/double(state_.duration_us),0.,1.)) : 0;
    const cyvr::Rect track{252,768,936,42};
    d.rect({track.x,784,track.w,8},surface,4);d.rect({track.x,784,track.w*progress,8},accent,4);
    if(seek)d.circle({track.x+track.w*progress,788},9,accent);
    ui_.applicationWidget(1025,track,cyvr::Control::Slider,seek);
    d.text(252,818,20,clockText(position),white);d.text(1065,818,20,state_.duration_us>0?clockText(state_.duration_us):"--:--:--",muted);
    icon_button(d,1362,{252,856,58,58},Icon::Previous,"Previous",false,adjacent_video(-1,false));
    icon_button(d,1020,{322,856,70,58},state_.playing?Icon::Pause:Icon::Play,state_.playing?"Pause":"Resume",false,state_.ready&&!state_.ended);
    icon_button(d,1023,{404,856,58,58},Icon::Stop,"Stop",false,state_.ready);
    icon_button(d,1363,{474,856,58,58},Icon::Next,"Next",false,adjacent_video(1,false));
    icon_button(d,1021,{548,856,58,58},Icon::Back10,"-10 s",false,seek);
    icon_button(d,1022,{618,856,58,58},Icon::Forward10,"+10 s",false,seek);
    icon_button(d,1370,{708,856,58,58},volume>0?Icon::Volume:Icon::Mute,"Mute",volume==0);
    const cyvr::Rect gain{786,860,324,50};d.rect({gain.x,882,gain.w,8},surface,4);d.rect({gain.x,882,gain.w*volume,8},accent,4);d.circle({gain.x+gain.w*volume,886},9,accent);ui_.applicationWidget(1371,gain,cyvr::Control::Slider);
    d.text(1128,874,18,cyvr::formatNumber(ui_.settings.language,volume*100,0)+" %",muted);
    if(!icon_hint_.empty())d.text(252,932,16,icon_hint_,accent);
    return true;
  }
  d.rect({0, 0, 1440, 990},
         Color::hex(light   ? 0xe4eef1
                    : ocean ? 0x141f3b
                            : 0x182228,
                    ui_.settings.highContrast ? 1 : ui_.settings.opacity),
         30);
  if(logo_ready_)d.image({24,10,76,76},{.5f/ui_atlas_width,(logo_atlas_y+.5f)/ui_atlas_height,159.f/ui_atlas_width,159.f/ui_atlas_height});
  d.text(logo_ready_?112:32, 22, 34, "CyVR Player", white);
  d.text(logo_ready_?358:270, 34, 18, tr("YOUR VIDEO LIBRARY"), accent);
  if(immersive && state_.ready)icon_button(d,1361,{1098,18,62,60},Icon::Minimize,"Minimize");
  icon_button(d,1063,{immersive?1174.f:1250.f,18,62,60},Icon::Settings,"Settings",page_==4);
  icon_button(d,1062,{immersive?1250.f:1330.f,18,62,60},immersive?Icon::Window:Icon::Headset,immersive?"Return to 2D":"Enter immersive",false,immersive||ui_.immersiveAvailable);
  if(immersive)icon_button(d,1064,{1330,18,62,60},Icon::Close,"Close full interface");
  const char* pages[]{"Library","Playback","Open stream","Display","Settings"};
  for(int i=0;i<5;++i)button(d,1000+i,{32.f+i*275,94,263,62},pages[i],page_==i);
  if (page_ == 0) {
    draw_library(d);
  } else if (page_ == 1) {
    if(tracks_open_){draw_tracks(d);ui_.drawWindowAffordances(d);return true;}
    d.text(36, 186, 34,
           fit(d,
               state_.title.empty() ? tr("Your next screening")
                                    : std::string_view(state_.title),
               1368, 34),
           white);
    cyvr::Rect area{32, 246, 900, 490};
    video(area);
    if (!preview_ready || immersive || state_.width <= 0 ||
        state_.height <= 0) {
      d.text(60, 445, 22,
             fit(d,
                 tr(immersive ? "Video is shown in the immersive scene."
                              : "Choose a video."),
                 840, 22),
             Color::hex(0xa7b7bf));
    }
    d.rect({956, 246, 452, 490}, surface, 18);
    d.text(980, 264, 19, tr("SOURCE / DECODER"), accent);
    d.text(980, 306, 21, fit(d, tr(state_.status), 400, 21), white);
    if (state_.width > 0)
      d.text(980, 348, 26,
             std::to_string(state_.width) + " × " +
                 std::to_string(state_.height) + " px",
             white);
    d.text(980, 390, 18, fit(d, state_.codec, 400, 18), muted);
    d.text(980, 426, 18,
           fit(d,
               tr(image.quality == 0   ? "Native"
                  : image.quality == 1 ? "FidelityFX CAS"
                  : image.quality == 2 ? "FSR 1 (EASU + RCAS)"
                                       : "Snapdragon GSR 1"),
               400, 18),
           muted);
    icon_button(d,1400,{976,480,412,54},Icon::Tracks,"Tracks, subtitles and chapters",false,!source_.empty(),true);
    icon_button(d,1440,{976,546,412,54},Icon::Tracks,"Subtitle settings",false,true,true);
    icon_button(d,1680,{976,612,412,54},Icon::Move,"Video placement",false,true,true);
    icon_button(d, 1331, {976, 682, 128, 40}, Icon::ZoomOut, "Zoom out", false, true,true);
    icon_button(d, 1332, {1118, 682, 128, 40}, Icon::ZoomIn, "Zoom in", false, true,true);
    icon_button(d, 1333, {1260, 682, 128, 40}, Icon::Fit, "Fit image", false, true,true);
    d.rect({32,754,1376,158},surface,16);
    const auto position = timeline_position();
    const bool canSeek =
        state_.ready && state_.seekable && state_.duration_us > 0;
    d.text(36, 770, 22,
           clockText(position) + " / " +
               (state_.duration_us > 0 ? clockText(state_.duration_us)
                                       : "--:--:--"),
           white);
    const cyvr::Rect timeline{330, 760, 1078, 48};
    const float progress =
        state_.duration_us > 0
            ? float(std::clamp(double(position) / double(state_.duration_us),
                               0., 1.))
            : 0.f;
    d.rect({timeline.x, 779, timeline.w, 10}, Color::hex(light?0xa6bec8:0x12212a), 5);
    auto trackColor = canSeek ? accent : muted;
    trackColor.a = canSeek ? 1.f : .4f;
    if (progress > 0)
      d.rect({timeline.x, 779, timeline.w * progress, 10}, trackColor, 5);
    if (canSeek) {
      const float knob = scrubbing_ || ui_.hovered() == 1025 ? 24.f : 20.f;
      d.rect({timeline.x + timeline.w * progress - knob * .5f, 784 - knob * .5f,
              knob, knob},
             accent, knob * .5f);
    }
    ui_.applicationWidget(1025, timeline, cyvr::Control::Slider, canSeek);
    icon_button(d,1362,{48,828,74,64},Icon::Previous,"Previous",false,adjacent_video(-1,false));
    icon_button(d,1020,{136,828,104,64},state_.playing?Icon::Pause:Icon::Play,state_.playing?"Pause":"Resume",false,state_.ready&&!state_.ended);
    icon_button(d,1023,{254,828,74,64},Icon::Stop,"Stop",false,state_.ready);
    icon_button(d,1363,{342,828,74,64},Icon::Next,"Next",false,adjacent_video(1,false));
    icon_button(d,1021,{440,828,94,64},Icon::Back10,"-10 s",false,canSeek);
    icon_button(d,1022,{548,828,94,64},Icon::Forward10,"+10 s",false,canSeek);
    icon_button(d,1370,{680,828,70,64},volume>0?Icon::Volume:Icon::Mute,"Mute",volume==0);
    const cyvr::Rect gain{776,834,268,52};d.rect({gain.x,856,gain.w,8},muted,4);d.rect({gain.x,856,gain.w*volume,8},accent,4);d.circle({gain.x+gain.w*volume,860},9,accent);ui_.applicationWidget(1371,gain,cyvr::Control::Slider);
    d.text(1064,848,20,cyvr::formatNumber(ui_.settings.language,volume*100,0)+" %",white);
    button(d,1334,{1184,834,200,52},"Performance",ui_.debugVisible);
  } else if (page_ == 2) {
    d.text(36, 187, 34, tr("Open a video stream"), white);
    d.text(36, 243, 22, tr("HTTP(S), HLS, RTSP — enter the complete address."),
           muted);
    d.rect({32, 290, 1376, 72}, Color::hex(light ? 0xc1d3db : 0x0e1b22), 12);
    d.text(52, 311, 24, fit(d, url_, 1310), white);
    std::string keys = symbols_ ? R"~(!@#$%^&*()[]{}<>+=_?:;/.,~-|\"'`)~"
                       : shift_ ? "1234567890QWERTYUIOPASDFGHJKL:ZXCVBNM./-"
                                : "1234567890qwertyuiopasdfghjkl:zxcvbnm./-";
    for (int i = 0; i < int(keys.size()); ++i)
      button(d, 1200 + i,
             {36.f + (i % 10) * 136, 390.f + (i / 10) * 83, 124, 67},
             std::string(1, keys[i]));
    button(d, 1250, {36, 746, 230, 64}, "Backspace");
    button(d, 1251, {280, 746, 180, 64}, "Clear");
    button(d, 1252, {474, 746, 150, 64}, "https://");
    button(d, 1253, {638, 746, 190, 64}, "Symbols", symbols_);
    button(d, 1255, {842, 746, 140, 64}, "Aa", shift_, !symbols_);
    button(d, 1256, {996, 746, 408, 64}, "Web browser", false, bool(platform_.open_browser) && !immersive);
    button(d, 1254, {36, 838, 1368, 66}, "Open", true, !url_.empty());
  } else if(page_==4){draw_settings(d);
  } else {
    const char *tabs[]{"Video format", "Image corrections", "Image quality", "Immersive cinema","Video placement"};
    d.text(36, 182, 34, tr(tabs[display_tab_]), white);
    for (int i = 0; i < 5; ++i)
      button(d, 1300 + i, {36.f + i * 275, 246, 263, 58}, tabs[i],
             display_tab_ == i);
    auto slider = [&](int id, float y, std::string_view label, float value,
                      float lo, float hi) {
      d.text(36, y + 8, 22, tr(label), white);
      d.text(365, y + 8, 22,
             (id==1348 || id==1349 || id==1646 || id==1647 || id==1648 || id==1649 || id==1652)?cyvr::formatNumber(ui_.settings.language,value*100,0)+" %":cyvr::formatNumber(ui_.settings.language, value, 2), accent);
      const float f = std::clamp((value - lo) / (hi - lo), 0.f, 1.f);
      const cyvr::Rect r{470, y, 934, 48};
      d.rect({r.x, y + 20, r.w, 8}, surface, 4);
      d.rect({r.x, y + 20, r.w * f, 8}, accent, 4);
      d.rect({r.x + r.w * f - 10, y + 14, 20, 20}, accent, 10);
      ui_.applicationWidget(id, r, cyvr::Control::Slider);
    };
    if (display_tab_ == 0) {
      d.text(36, 340, 23, tr("Eye layout"), muted);
      for (int i = 0; i < 3; ++i)
        icon_button(d, 1030 + i, {36.f + i * 458, 382, 444, 68},i==0?Icon::Mono:i==1?Icon::Sbs:Icon::Tb,
               i == 0   ? "2D video"
               : i == 1 ? "3D side by side (SBS)"
                        : "3D top / bottom (TB)",
               stereo == i,true,true);
      d.text(36, 490, 23, tr("Projection"), muted);
      for (int i = 0; i < 3; ++i)
        icon_button(d, 1040 + i, {36.f + i * 458, 532, 444, 68},i==0?Icon::Cinema:i==1?Icon::HalfSphere:Icon::Sphere,
               i == 0   ? "Cinema screen"
               : i == 1 ? "VR 180°"
                        : "Panorama 360°",
               projection == i, i == 0 || platform_.panorama,true);
      icon_button(d, 1050, {36, 652, 444, 68}, Icon::Recenter, "Recenter video", false, immersive,true);
      icon_button(d, 1051, {494, 652, 444, 68}, Icon::Half, "Half-resolution 3D", half_stereo,
             stereo != 0,true);
      icon_button(d, 1052, {952, 652, 452, 68}, Icon::Swap, "Swap eyes", swap_eyes, stereo != 0,true);

      icon_button(d, 1331, {36, 744, 215, 60}, Icon::ZoomOut, "Zoom out", false, true,true);
      icon_button(d, 1332, {267, 744, 215, 60}, Icon::ZoomIn, "Zoom in", false, true,true);
      icon_button(d, 1333, {498, 744, 250, 60}, Icon::Fit, "Fit image", false, true,true);
      button(d,1054,{770,744,634,60},auto_format_?"Auto-detection: On":"Auto-detection: Off",auto_format_);
      button(d,1053,{1034,814,370,52},"Detect once");
      const char* detection=(!auto_format_ || format_manual_)?"Manual video format":detected_layout_.ambiguous?"Projection not recognised; select the format manually.":detected_layout_.evidence==3?"Automatic format: decoded stereo views":detected_layout_.evidence==2?"Automatic format: video metadata":detected_layout_.evidence==1?"Automatic format: filename hints":"Automatic format: standard 2D";
      d.text(36,827,18,tr(detection),accent);
      d.text(36, 878, 20, tr("Joystick left/right: turn view / Right stick up/down: zoom"),
             muted);
      if (immersive)
        d.text(36, 905, 17,
               tr("Move the controls freely; the video stays in place."),
               muted);
    } else if (display_tab_ == 1) {
      slider(1340, 330, "Exposure (EV)", image.exposure, -2, 2);
      slider(1341, 398, "Contrast", image.contrast, .5f, 1.5f);
      slider(1342, 466, "Saturation", image.saturation, 0, 2);
      slider(1343, 534, "Gamma", image.gamma, .5f, 2);
      slider(1344, 602, "Zoom", image.zoom, minimum_zoom(), 4);
      slider(1345, 670, "Horizontal position", image.pan_x, -1, 1);
      slider(1346, 738, "Vertical position", image.pan_y, -1, 1);
      button(d, 1330, {36, 830, 340, 60}, "Reset image");
      d.text(406, 849, 20, tr("Grip + trigger, move hand up/down: zoom"),
             muted);
    } else if(display_tab_==2) {
      for (int i = 0; i < 4; ++i)
        button(d, 1310 + i, {36.f + i * 343, 324, 328, 56},
               i == 0   ? "Native"
               : i == 1 ? "FidelityFX CAS"
               : i == 2 ? "FSR 1 (EASU + RCAS)"
                        : "Snapdragon GSR 1",
               image.quality == i);
      d.text(36, 394, 21, tr("Display resolution"), muted);
      for (int i = 0; i < 4; ++i)
        button(d, 1320 + i, {36.f + i * 343, 426, 328, 52},
               i == 0   ? "Headset / window"
               : i == 1 ? "1.25x"
               : i == 2 ? "1.5x"
                        : "2x",
               image.upscale == i);
      slider(1347, 494, "CAS / FSR / GSR sharpness", image.sharpness, 0, 1);
      d.text(36, 558, 21, tr("Image smoothing"), muted);
      for(int i=0;i<3;++i)
        button(d,1350+i,{36.f+i*458,590,444,52},i==0?"Original (fast)":i==1?"Bilinear":"Bicubic",image.sampling==i);
      slider(1348, 658, "Noise reduction", image.denoise, 0, 1);
      slider(1349, 718, "Fine detail", image.detail, 0, 1);
      button(d, 1353, {36, 784, 444, 52}, "Disable spatial filters");
      button(d, 1330, {498, 784, 340, 52}, "Reset image");
      if(platform_.panorama)
        button(d,1354,{856,784,548,52},smooth_video?"120 Hz for 60 fps video: On":"120 Hz for 60 fps video: Off",smooth_video);
      d.text(
          36, 855, 19,
          tr("Bicubic and noise reduction cost more GPU time; default: Off."),
          muted);
      d.text(36, 885, 19,
             fit(d,
                 processing_status.empty()
                     ? tr("Automatic input resolution / compare with Native")
                     : std::string_view(processing_status),
                 1360, 19),
             muted);
    } else if(display_tab_==4){draw_placement(d);
    } else {
      d.rect({24,382,1392,534},Color::hex(light?0xd4e2e6:0x101c24),16);
      d.border({24,382,1392,534},Color::hex(light?0x86ada6:0x34564e),16,1.5f);
      icon_button(d,1660,{36,320,444,52},Icon::Geometry,"Screen geometry",cinema_tab_==0,true,true);
      icon_button(d,1661,{498,320,444,52},Icon::Environment,"Environment",cinema_tab_==1,true,true);
      icon_button(d,1663,{960,320,444,52},Icon::Glow,"Ambilight",cinema_tab_==2,true,true);
      d.rect({36.f+cinema_tab_*462,374,444,4},accent,2);
      if(cinema_tab_==0){
        slider(1640,390,"Screen width (m)",cinema.width,1,12);
        slider(1641,452,"Viewing distance (m)",cinema.distance,1,12);
        slider(1642,514,"Screen curvature (degrees)",cinema.curvature,0,120);
        slider(1643,576,"Screen height offset (m)",cinema.elevation,-3,3);
        slider(1644,638,"Screen tilt (degrees)",cinema.pitch,-90,90);
        slider(1645,700,"Screen roll (degrees)",cinema.roll,-180,180);
        const char* presets[]{"Flat screen","Curved screen","Large cinema","Reset screen"};
        for(int i=0;i<4;++i)button(d,1620+i,{36.f+i*343,776,330,56},presets[i]);
        d.text(36,849,18,tr("Geometry applies to cinema video; VR180/360 keep their projection."),muted);
        d.text(36,876,18,tr("Zoom and image position remain available under Image corrections."),muted);
      }else if(cinema_tab_==1){
        const char* names[]{"Black cinema","Dark background","Sunlit lounge","Venice sunset","Passthrough","Modern lounge","Observatory","Sunset terrace"};
        for(int i=0;i<8;++i)button(d,1610+i,{36.f+(i%4)*343,406.f+(i/4)*78,330,64},names[i],cinema.environment==i,true);
        slider(1646,598,"Environment brightness",cinema.ambient,0,1);
        slider(1647,674,"Interface opacity",cinema.panel_opacity,.25f,1);
        d.text(36,777,21,tr("Background lighting never changes the movie brightness."),muted);
        d.text(36,817,20,tr("Black cinema reduces distractions and environment rendering work."),muted);
        d.text(36,857,20,tr("Passthrough brightness is managed by the headset."),muted);
      }
      if(cinema_tab_==2){
        button(d,1670,{36,398,676,58},"Enable Ambilight",ambilight.enabled);
        button(d,1671,{728,398,676,58},"Ignore black bars",ambilight.auto_bars);
        slider(1648,484,"Glow intensity",ambilight.intensity,0,2);
        slider(1649,552,"Glow spread",ambilight.spread,.05f,.6f);
        slider(1650,620,"Smoothing (seconds)",ambilight.smoothing,0,1.5f);
        slider(1651,688,"Glow saturation",ambilight.saturation,0,2);
        slider(1652,756,"Edge sample inset",ambilight.inset,0,.2f);
        button(d,1672,{36,828,330,52},"Reset Ambilight");
        d.text(390,849,19,tr("A soft video-coloured glow follows the flat or curved cinema screen."),muted);
      }
      if(!immersive)d.text(36,900,16,tr("These settings take effect when you enter immersive mode."),accent);
    }
  }
  d.line({32, 929}, {1408, 929}, Color::hex(light ? 0xb0c3c9 : 0x354851));
  d.text(36, 950, 18,
         !icon_hint_.empty()?std::string_view(icon_hint_):ui_.toast().empty()
             ? tr(immersive ? (image.quality
                                   ? "Spatial enhancement active"
                                   : "Native source resolution / no upscaling")
                            : "Use Quest system controls to move, resize or "
                              "close this window.")
             : std::string_view(ui_.toast()),
         accent);
  ui_.drawWindowAffordances(d);
  return true;
}
} // namespace cyvr::player
