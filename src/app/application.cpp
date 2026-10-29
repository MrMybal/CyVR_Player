#include "cyvr/player/application.hpp"
#include "cyvr/player/locale.hpp"
#include "previews.hpp"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace cyvr::player {
namespace {
std::string keyboard(bool shift, bool symbols) {
  if (symbols)
    return R"~(!@#$%^&*()[]{}<>+=_?:;/.,~-|\"'`)~";
  return shift ? "1234567890QWERTYUIOPASDFGHJKL:ZXCVBNM./-"
               : "1234567890qwertyuiopasdfghjkl:zxcvbnm./-";
}
} // namespace
struct PlayerApplication::BrowserWorker {
  Platform platform;
  std::mutex mutex;
  std::condition_variable wake;
  std::string request, status;
  bool pending{}, quit{};
  std::uint64_t revision{}, generation{};
  std::vector<BrowserItem> entries;
  std::shared_ptr<Cancellation> cancellation = std::make_shared<Cancellation>();
  std::thread worker;
  explicit BrowserWorker(Platform p)
      : platform(std::move(p)), worker([this] { run(); }) {}
  ~BrowserWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      quit = true;
      cancellation->cancel();
    }
    if (platform.cancel_documents) {
      try {
        platform.cancel_documents();
      } catch (...) {
      }
    }
    wake.notify_all();
    worker.join();
  }
  void scan(std::string path) {
    if (platform.cancel_documents)
      platform.cancel_documents();
    std::lock_guard<std::mutex> lock(mutex);
    cancellation->cancel();
    request = std::move(path);
    pending = true;
    ++generation;
    status = "Reading folder...";
    entries.clear();
    ++revision;
    wake.notify_one();
  }
  void run() {
    for (;;) {
      std::string path;
      std::uint64_t ticket;
      std::shared_ptr<Cancellation> cancel;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return pending || quit; });
        if (quit)
          return;
        path = request;
        pending = false;
        ticket = generation;
        cancellation = cancel = std::make_shared<Cancellation>();
      }
      std::vector<BrowserItem> result;
      std::string message;
      try {
        if (path.rfind("content://", 0) == 0) {
          if (!platform.documents)
            throw MediaError("Android document provider unavailable");
          result = platform.documents(path);
          message = std::to_string(result.size()) + " items / 512 maximum";
        } else {
          FileBrowser browser(platform.root);
          BrowseOptions options;
          options.max_entries = 512;
          const auto listing =
              browser.list(std::filesystem::u8path(path).lexically_relative(
                               std::filesystem::u8path(platform.root)),
                           options, cancel.get());
          for (const auto &item : listing.entries) {
            if (item.kind == FileKind::audio)
              continue;
            result.push_back({(browser.root() / item.relative_path).u8string(),
                              item.name, item.kind == FileKind::directory,
                              item.size.value_or(0)});
          }
          message = std::to_string(result.size()) + " items" +
                    (listing.truncated ? " / list limited to 512" : "");
        }
      } catch (const std::exception &e) {
        message = e.what();
      }
      std::lock_guard<std::mutex> lock(mutex);
      if (ticket == generation && !quit) {
        entries = std::move(result);
        status = std::move(message);
        ++revision;
      }
    }
  }
};
PlayerApplication::PlayerApplication(cyvr::UiModel &ui, Platform p)
    : ui_(ui), platform_(std::move(p)), session_(platform_),
      browser_(std::make_unique<BrowserWorker>(platform_)),
      previews_(std::make_unique<PreviewWorker>(platform_)) {
  page_ = int(ui_.page);
  ui_.renderApplication = [this](cyvr::DrawList &d) { return draw(d); };
  ui_.renderApplicationDebug = [this](cyvr::DrawList &d) {
    return draw_performance(d);
  };
  ui_.handleApplicationAction = [this](int id) { return action(id); };
  ui_.handleApplicationSlider = [this](int id, float value,
                                       cyvr::SliderPhase phase) {
    return slide(id, value, phase);
  };
  ui_.applicationTick = [this](double) { tick(); };
  ui_.applicationBack = [this] { return back(); };
  ui_.applicationController = [this](int hand, cyvr::Vec2 stick, float trigger,
                                     float squeeze, cyvr::Pose pose,
                                     bool valid) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(
        std::chrono::duration<float>(now - controller_time_[hand]).count(), 0.f,
        .05f);
    controller_time_[hand] = now;
    if (!valid) {
      zoom_gesture_[hand] = false;
      stick_latched_[hand] = false;
      return false;
    }
    if (hand == 0) {
      if (std::abs(stick.x) < .3f)
        stick_latched_[hand] = false;
      if (std::abs(stick.x) > .75f && !stick_latched_[hand]) {
        video_input(stick.x > 0 ? 10.f : -10.f, 0);
        stick_latched_[hand] = true;
      }
    } else if (std::abs(stick.y) > .2f)
      video_input(0, stick.y * dt);
    const bool gesture =
        squeeze > .65f && trigger > .55f && session_.state().ready;
    if (gesture) {
      if (!zoom_gesture_[hand]) {
        zoom_start_[hand] = image.zoom;
        zoom_y_[hand] = pose.position.y;
      }
      image.zoom = std::clamp(zoom_start_[hand] +
                                  (pose.position.y - zoom_y_[hand]) * 4.f,
                              1.f, 4.f);
      state_dirty_ = true;
      ui_.invalidate();
    }
    const bool consume = gesture || zoom_gesture_[hand];
    zoom_gesture_[hand] = gesture;
    return consume;
  };
  browse(platform_.root);
}
PlayerApplication::~PlayerApplication() {
  save();
  ui_.renderApplication = {};
  ui_.renderApplicationDebug = {};
  ui_.handleApplicationAction = {};
  ui_.handleApplicationSlider = {};
  ui_.applicationTick = {};
  ui_.applicationBack = {};
  ui_.applicationController = {};
}
void PlayerApplication::browse(std::string path) {
  directory_ = std::move(path);
  offset_ = 0;
  entries_.clear();
  preview_present_.fill(false);
  browser_->scan(directory_);
  ui_.invalidate();
  state_dirty_ = true;
}
void PlayerApplication::open(std::string location, std::string title) {
  cancel_scrub();
  show_interface();
  if (title.empty())
    title = location.find("://") != std::string::npos
                ? "Video stream"
                : std::filesystem::u8path(location).filename().u8string();
  source_ = location;
  title_ = title;
  requested_position_ = 0;
  resume_playing_ = true;
  preview_ready = false;
  render_monitor.reset();
  session_.open(std::move(location), std::move(title));
  page_ = 1;
  ui_.page = cyvr::Page(page_);
  state_dirty_ = true;
  ui_.invalidate();
}
void PlayerApplication::tick() {
  if (page_ != int(ui_.page)) {
    cancel_scrub();
    page_ = int(ui_.page);
    state_dirty_ = true;
  }
  auto state = session_.state();
  if (state.revision != state_.revision) {
    // Position changes alone are checkpointed every five seconds. Avoid
    // synchronous state-file writes twice a second on the rendering thread.
    if (state.ready != state_.ready || state.playing != state_.playing ||
        state.seeking != state_.seeking || state.ended != state_.ended ||
        state.title != state_.title)
      state_dirty_ = true;
    state_ = std::move(state);
    if (!state_.ready || !state_.seekable || state_.duration_us <= 0)
      cancel_scrub();
    if (!state_.ready && state_.status != "Opening media...")
      show_interface();
    ui_.invalidate();
  }
  {
    std::unique_lock<std::mutex> lock(browser_->mutex, std::try_to_lock);
    if (lock.owns_lock() && browser_revision_ != browser_->revision) {
      browser_revision_ = browser_->revision;
      entries_ = browser_->entries;
      browse_status_ = browser_->status;
      if (browse_status_ != "Reading folder...")
        offset_ = std::min(offset_, entries_.empty()
                                        ? 0
                                        : ((int(entries_.size()) - 1) / 7) * 7);
      ui_.invalidate();
    }
  }
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  std::vector<std::string> requests;
  if (page_ == 0 && !state_.playing && !suspended_) {
    for (int row = 0; row < 7; ++row) {
      const auto n = offset_ + row;
      requests.push_back(n < int(entries_.size()) && !entries_[n].directory &&
                                 classify_extension(entries_[n].name) ==
                                     FileKind::video
                             ? entries_[n].location
                             : "");
    }
  }
  if (requests != preview_requests_) {
    preview_requests_ = requests;
    previews_->request(std::move(requests));
  }
  {
    std::unique_lock<std::mutex> lock(previews_->mutex, std::try_to_lock);
    if (lock.owns_lock() && preview_revision_ != previews_->revision) {
      preview_revision_ = previews_->revision;
      preview_present_ = previews_->present;
      thumbnail_atlas_ = previews_->atlas;
      thumbnail_dirty_ = true;
      ui_.invalidate();
    }
  }
  if (ui_.debugVisible && now - last_performance_update_ >= .5) {
    last_performance_update_ = now;
    ui_.invalidate();
  }
  if ((state_dirty_ && now - last_save_ > .5) ||
      (state_.playing && now - last_save_ >= 5.)) {
    save();
    last_save_ = now;
  }
}
void PlayerApplication::button(cyvr::DrawList &d, int id, cyvr::Rect r,
                               std::string_view text, bool selected,
                               bool enabled) {
  text = tr(text);
  const bool hover = ui_.hovered() == id,
             light = ui_.settings.theme == cyvr::Theme::Light,
             ocean = ui_.settings.theme == cyvr::Theme::Ocean;
  const auto accent = light ? 0x126854u : ocean ? 0x95bdffu : 0x86e7cfu;
  d.rect(r,
         cyvr::Color::hex(selected ? accent
                          : light  ? (hover ? 0xacd6ceu : 0xc1d3dbu)
                          : ocean  ? (hover ? 0x385984u : 0x293e66u)
                          : hover  ? 0x344b56u
                                   : 0x263741u,
                          enabled ? 1.f : .45f),
         12);
  d.text(r.x + (r.w - d.textWidth(23, text)) * .5f, r.y + (r.h - 31) * .5f, 23,
         text,
         cyvr::Color::hex(selected  ? (light ? 0xffffffu : 0x102d28u)
                          : enabled ? (light ? 0x142b35u : 0xf0f5f6u)
                                    : 0x778b94u));
  ui_.applicationWidget(id, r, cyvr::Control::Button, enabled);
}
void PlayerApplication::text(char c) {
  if (page_ != 2)
    return;
  if (c == '\b') {
    if (!url_.empty())
      url_.pop_back();
  } else if (c >= 32 && c <= 126 && url_.size() < 2048)
    url_ += c;
  ui_.invalidate();
  state_dirty_ = true;
}
bool PlayerApplication::action(int id) {
  cancel_scrub();
  if (id >= 1000 && id <= 1003) {
    page_ = id - 1000;
    ui_.page = cyvr::Page(page_);
    state_dirty_ = true;
    ui_.invalidate();
    return true;
  }
  if (id == 1010 && !parents_.empty()) {
    auto parent = parents_.back();
    parents_.pop_back();
    browse(parent);
  } else if (id == 1011)
    browse(directory_);
  else if (id == 1012 && platform_.request_tree)
    platform_.request_tree();
  else if (id == 1013) {
    offset_ = std::max(0, offset_ - 7);
    preview_present_.fill(false);
  } else if (id == 1014) {
    offset_ += 7;
    preview_present_.fill(false);
  } else if (id >= 1100 && id < 1107 &&
             offset_ + id - 1100 < int(entries_.size())) {
    auto item = entries_[offset_ + id - 1100];
    if (item.directory) {
      parents_.push_back(directory_);
      browse(item.location);
    } else if (classify_extension(item.name) == FileKind::splat)
      ui_.setToast(std::string(tr(
          "Gaussian Splatting loading and rendering are not connected yet.")));
    else
      open(item.location, item.name);
  } else if (id == 1020) {
    resume_playing_ = !state_.playing;
    session_.pause(state_.playing);
  } else if (id == 1021 || id == 1022)
    session_.seek(std::clamp<std::int64_t>(
        state_.position_us + (id == 1021 ? -10000000 : 10000000), 0,
        state_.duration_us > 0 ? state_.duration_us : INT64_MAX));
  else if (id == 1023) {
    show_interface();
    source_.clear();
    title_.clear();
    requested_position_ = 0;
    resume_playing_ = false;
    preview_ready = false;
    session_.stop();
  } else if (id == 1024) {
    if (session_.state().ready)
      ui_.setInterfaceHidden(true);
  } else if (id >= 1030 && id <= 1032)
    stereo = id - 1030;
  else if (id >= 1040 && id <= 1042)
    projection = id - 1040;
  else if (id == 1050)
    ui_.recenterRequested = true;
  else if (id == 1051)
    half_stereo = !half_stereo;
  else if (id == 1052)
    swap_eyes = !swap_eyes;
  else if (id >= 1300 && id <= 1302)
    display_tab_ = id - 1300;
  else if (id >= 1310 && id <= 1313) {
    image.quality = id - 1310;
    ++processing_request;
  } else if (id >= 1320 && id <= 1323) {
    image.upscale = id - 1320;
    ++processing_request;
  } else if (id == 1330) {
    image = ImageSettings{};
    ++processing_request;
  } else if (id == 1331)
    video_input(0, -.25f);
  else if (id == 1332)
    video_input(0, .25f);
  else if (id == 1333) {
    image.zoom = 1;
    image.pan_x = image.pan_y = 0;
  } else if (id == 1334)
    ui_.debugVisible = !ui_.debugVisible;
  else if (id == 1060)
    ui_.setLanguage(ui_.settings.language == "fr" ? "en" : "fr");
  else if (id == 1061) {
    ui_.settings.theme = cyvr::Theme((int(ui_.settings.theme) + 1) % 3);
    ui_.saveRequested = true;
  } else if (id == 1062) {
    save();
    if (ui_.presentation == cyvr::Presentation::Immersive)
      ui_.panelRequested = true;
    else if (ui_.immersiveAvailable)
      ui_.immersiveRequested = true;
  } else if (id >= 1200 && id < 1240) {
    const auto keys = keyboard(shift_, symbols_);
    if (std::size_t(id - 1200) < keys.size())
      text(keys[id - 1200]);
  } else if (id == 1250)
    text('\b');
  else if (id == 1251)
    url_.clear();
  else if (id == 1252)
    url_ = "https://";
  else if (id == 1253)
    symbols_ = !symbols_;
  else if (id == 1255)
    shift_ = !shift_;
  else if (id == 1254) {
    try {
      MediaSource::network(url_);
      open(url_, "Video stream");
    } catch (const std::exception &e) {
      ui_.setToast(e.what());
    }
  } else
    return false;
  ui_.invalidate();
  state_dirty_ = true;
  return true;
}
std::string_view PlayerApplication::tr(std::string_view text) const {
  return player_translate(ui_.settings.language, text);
}
ApplicationState PlayerApplication::snapshot() const {
  const auto media = session_.state();
  ApplicationState s;
  s.directory = directory_;
  s.parents = parents_;
  s.url = url_;
  s.source = source_;
  s.title = title_;
  s.page = page_;
  s.offset = offset_;
  s.stereo = stereo;
  s.projection = projection;
  s.half_stereo = half_stereo;
  s.swap_eyes = swap_eyes;
  s.image = image;
  s.display_tab = display_tab_;
  s.shift = shift_;
  s.symbols = symbols_;
  s.position_us =
      media.ready ? (media.seeking ? media.seek_target_us : media.position_us)
                  : requested_position_;
  if (media.ended)
    s.position_us = std::max<std::int64_t>(0, s.position_us - 50000);
  s.playing = suspended_    ? resume_playing_
              : media.ready ? media.playing
                            : resume_playing_;
  return s;
}
void PlayerApplication::save() const {
  if (!platform_.state_file.empty() &&
      save_application_state(platform_.state_file, snapshot()))
    state_dirty_ = false;
}
bool PlayerApplication::restore(bool resume_media) {
  const auto saved = load_application_state(platform_.state_file);
  if (!saved)
    return false;
  const auto &s = *saved;
  browse(s.directory.empty() ? platform_.root : s.directory);
  parents_ = s.parents;
  url_ = s.url;
  page_ = s.page;
  ui_.page = cyvr::Page(page_);
  offset_ = s.offset;
  stereo = s.stereo;
  projection = s.projection;
  half_stereo = s.half_stereo;
  swap_eyes = s.swap_eyes;
  image = s.image;
  display_tab_ = s.display_tab;
  shift_ = s.shift;
  symbols_ = s.symbols;
  source_ = s.source;
  title_ = s.title;
  requested_position_ = s.position_us;
  resume_playing_ = s.playing;
  if (resume_media && !source_.empty())
    session_.open(source_, title_, s.position_us, s.playing);
  ui_.invalidate();
  return true;
}
void PlayerApplication::active(bool resumed) {
  if (!resumed && !suspended_) {
    cancel_scrub();
    const auto s = session_.state();
    if (s.ready)
      resume_playing_ = s.playing;
    suspended_ = true;
    save();
    session_.pause(true);
  } else if (resumed && suspended_) {
    suspended_ = false;
    session_.pause(!resume_playing_);
  }
}
bool PlayerApplication::back() {
  if (show_interface())
    return true;
  if (ui_.presentation == cyvr::Presentation::Immersive) {
    save();
    ui_.panelRequested = true;
    return true;
  }
  if (page_ != 0) {
    action(1000);
    return true;
  }
  if (!parents_.empty()) {
    action(1010);
    return true;
  }
  return false;
}
void PlayerApplication::cancel_scrub() {
  if (!scrubbing_)
    return;
  scrubbing_ = false;
  ui_.invalidate();
}
bool PlayerApplication::slide(int id, float fraction, cyvr::SliderPhase phase) {
  if (id >= 1340 && id <= 1347) {
    if (phase == cyvr::SliderPhase::Cancel || !std::isfinite(fraction))
      return true;
    const float f = std::clamp(fraction, 0.f, 1.f);
    switch (id) {
    case 1340:
      image.exposure = -2 + f * 4;
      break;
    case 1341:
      image.contrast = .5f + f;
      break;
    case 1342:
      image.saturation = f * 2;
      break;
    case 1343:
      image.gamma = .5f + f * 1.5f;
      break;
    case 1344:
      image.zoom = 1 + f * 3;
      break;
    case 1345:
      image.pan_x = -1 + f * 2;
      break;
    case 1346:
      image.pan_y = -1 + f * 2;
      break;
    case 1347:
      image.sharpness = f;
      break;
    }
    image.sanitize();
    state_dirty_ = true;
    ui_.invalidate();
    return true;
  }
  if (id != 1025)
    return false;
  if (phase == cyvr::SliderPhase::Cancel || !std::isfinite(fraction)) {
    cancel_scrub();
    return true;
  }
  const auto media = session_.state();
  if (!media.ready || !media.seekable || media.duration_us <= 0) {
    cancel_scrub();
    return true;
  }
  if (!scrubbing_) {
    if (phase != cyvr::SliderPhase::Begin)
      return true;
    scrubbing_ = true;
    scrub_duration_us_ = media.duration_us;
  }
  const auto last = std::min(scrub_duration_us_, media.duration_us) - 1;
  fraction = std::clamp(fraction, 0.f, 1.f);
  scrub_position_us_ =
      fraction >= 1.f ? last : std::int64_t(double(last) * fraction);
  if (phase == cyvr::SliderPhase::Commit) {
    scrubbing_ = false;
    session_.seek(scrub_position_us_);
    requested_position_ = scrub_position_us_;
    state_dirty_ = true;
  }
  ui_.invalidate();
  return true;
}
std::int64_t PlayerApplication::timeline_position() const {
  if (scrubbing_)
    return scrub_position_us_;
  const auto media = session_.state();
  return media.seeking ? media.seek_target_us : media.position_us;
}
bool PlayerApplication::show_interface() { return ui_.revealInterface(); }
void PlayerApplication::video_input(float seek, float zoom, float x, float y) {
  if (seek != 0) {
    const auto media = session_.state();
    if (media.ready && media.seekable)
      session_.seek(std::max<std::int64_t>(
          0, (media.seeking ? media.seek_target_us : media.position_us) +
                 std::int64_t(seek * 1000000)));
  }
  image.zoom += zoom;
  image.pan_x += x;
  image.pan_y += y;
  image.sanitize();
  state_dirty_ = true;
  ui_.invalidate();
}
bool PlayerApplication::take_thumbnails(std::vector<std::uint8_t> &rgba,
                                        bool force) {
  if ((!thumbnail_dirty_ && !force) || thumbnail_atlas_.empty())
    return false;
  rgba = thumbnail_atlas_;
  thumbnail_dirty_ = false;
  return true;
}
} // namespace cyvr::player
