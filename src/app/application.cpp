#include "cyvr/player/application.hpp"
#include "cyvr/player/locale.hpp"
#include "previews.hpp"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace cyvr::player {
namespace {
double controls_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
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
          message = std::to_string(result.size()) + " items / 10000 maximum";
        } else {
          FileBrowser browser(platform.root);
          BrowseOptions options;
          options.max_entries = 10000;
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
  thumbnail_atlas_.resize(ui_atlas_width*ui_atlas_height*4);
  thumbnail_dirty_=true;
  page_ = int(ui_.page);
  ui_.renderApplication = [this](cyvr::DrawList &d) { return draw(d); };
  ui_.window = cyvr::WindowOptions{};
  refresh_locations();
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
  ui_.applicationPointer = [this](size_t hand,cyvr::Vec2 pos,bool down){return browser_pointer(hand,pos,down);};
  ui_.applicationInput = [this](const cyvr::ControllerInput& input){return controller_input(input);};
  ui_.applicationScroll = [this](cyvr::Vec2 point,cyvr::Vec2 delta){return scroll_library(point,delta.y);};
  browse(platform_.root);
}
PlayerApplication::~PlayerApplication() {
  save();
  ui_.renderApplication = {};ui_.renderApplicationDock={};
  ui_.renderApplicationDebug = {};
  ui_.handleApplicationAction = {};
  ui_.handleApplicationSlider = {};
  ui_.applicationTick = {};
  ui_.applicationBack = {};
  ui_.applicationController = {};ui_.applicationInput = {};
  ui_.applicationScroll={};
  ui_.applicationPointer = {};
  ui_.compactInterface = false;
}
void PlayerApplication::browse(std::string path) {
  directory_ = std::move(path);folder_name_.clear();refresh_locations();
  offset_ = 0;browser_scroll_=0;browser_drag_[0]=browser_drag_[1]=false;
  entries_.clear();
  preview_present_.fill(false);
  browser_->scan(directory_);
  ui_.invalidate();
  state_dirty_ = true;
}
void PlayerApplication::open(std::string location, std::string title) {
  cancel_scrub();
  show_interface();
  if (std::none_of(playlist_.begin(),playlist_.end(),[&](const auto& item){return item.location==location;})) {
    playlist_.clear();
    if (std::any_of(entries_.begin(),entries_.end(),[&](const auto& item){return item.location==location;}))
      for(const auto& item:entries_)if(!item.directory && (classify_extension(item.name)==FileKind::video || classify_extension(item.name)==FileKind::splat || classify_extension(item.name)==FileKind::disc))playlist_.push_back(item);
  }
  controls_.compact(controls_now());
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
  format_manual_=!auto_format_;format_track_=-1;
  format_media_id_=session_.open(std::move(location),std::move(title));
  refresh_video_format();
  page_ = 1;
  ui_.page = cyvr::Page(page_);
  state_dirty_ = true;
  ui_.invalidate();
}
void PlayerApplication::refresh_video_format(bool force){
  if(!auto_format_ && !force)return;
  const bool current=state_.ready && state_.media_id==format_media_id_ && !state_.volumetric;
  detected_layout_=detect_video_layout(source_,title_,current?state_.layout:VideoLayoutHints{},current?state_.width:0,current?state_.height:0,current?state_.pixel_aspect:1,current && state_.multiview);
  if(force || !format_manual_){stereo=detected_layout_.stereo;projection=detected_layout_.projection;half_stereo=detected_layout_.half;swap_eyes=detected_layout_.swap;image.zoom=std::clamp(image.zoom,minimum_zoom(),4.f);++processing_request;state_dirty_=true;}
  ui_.invalidate();
}
void PlayerApplication::tick() {
  const double activity_now=controls_now();
  if (controls_.mode()==PlaybackControls::Mode::hidden && !ui_.interfaceHidden)controls_.activity(activity_now);
  if (page_ != int(ui_.page)) {
    cancel_scrub();
    page_ = int(ui_.page);
    state_dirty_ = true;
  }
  const bool was_ready=state_.ready;
  if (session_.revision() != state_.revision) {
    auto state = session_.state();
    const bool formatChanged=state.ready && state.media_id==format_media_id_ && (state.video_track!=format_track_ || state.multiview!=state_.multiview);
    // Position changes alone are checkpointed every five seconds. Avoid
    // synchronous state-file writes twice a second on the rendering thread.
    if (state.ready != state_.ready || state.playing != state_.playing ||
        state.seeking != state_.seeking || state.ended != state_.ended ||
        state.title != state_.title)
      state_dirty_ = true;
    state_ = std::move(state);
    if(formatChanged){format_track_=state_.video_track;refresh_video_format();}
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
      if(playlist_.empty() && std::any_of(entries_.begin(),entries_.end(),[&](const auto& item){return item.location==source_;}))
        for(const auto& item:entries_)if(!item.directory && (classify_extension(item.name)==FileKind::video || classify_extension(item.name)==FileKind::splat || classify_extension(item.name)==FileKind::disc))playlist_.push_back(item);
      if (browse_status_ != "Reading folder...")set_browser_scroll(browser_scroll_);
      ui_.invalidate();
    }
  }
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const auto preview_key = std::make_tuple(suspended_, offset_, browser_visible_count(), browser_revision_);
  if (page_ == 0 && (preview_key != preview_key_ || preview_requests_.empty())) {
    preview_key_ = preview_key;
    std::vector<std::string> requests;
    if (!suspended_) {
      for (int row = 0; row < browser_visible_count(); ++row) {
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
  }
  {
    std::unique_lock<std::mutex> lock(previews_->mutex, std::try_to_lock);
    if (lock.owns_lock() && preview_revision_ != previews_->revision) {
      preview_revision_ = previews_->revision;
      preview_present_ = previews_->present;
      std::copy(previews_->atlas.begin(),previews_->atlas.end(),thumbnail_atlas_.begin());
      thumbnail_dirty_ = true;
      ui_.invalidate();
    }
  }
  if (ui_.debugVisible && now - last_performance_update_ >= .5) {
    last_performance_update_ = now;
    ui_.invalidate();
  }
  const bool immersive_video=ui_.presentation==cyvr::Presentation::Immersive && state_.ready;
  if(ui_.presentation==cyvr::Presentation::Immersive){
    constexpr cyvr::Background environments[]{cyvr::Background::Black,cyvr::Background::Dark,cyvr::Background::Studio,cyvr::Background::Night,cyvr::Background::Passthrough,cyvr::Background::Lounge,cyvr::Background::Observatory,cyvr::Background::Terrace};
    cinema.sanitize();
    if(cinema.environment==4 && !ui_.passthroughAvailable){cinema.environment=1;state_dirty_=true;ui_.setToast(std::string(tr("Passthrough is unavailable; using a dark background.")));}
    const auto background=environments[cinema.environment];
    if(ui_.settings.background!=background || ui_.settings.environmentDim!=cinema.ambient || ui_.settings.opacity!=cinema.panel_opacity){
      ui_.settings.background=background;ui_.settings.environmentDim=cinema.ambient;ui_.settings.opacity=cinema.panel_opacity;ui_.invalidate();
    }
  }
  controls_.update(immersive_video,activity_now,ui_.interacting() || scrubbing_ || ui_.modalOpen);
  if(tracks_open_ && state_.ready && !was_ready)controls_.expand(activity_now);
  const bool compact=immersive_video && controls_.mode()!=PlaybackControls::Mode::full;
  if(ui_.compactInterface!=compact){ui_.compactInterface=compact;ui_.invalidate();}
  if(immersive_video)ui_.setInterfaceHidden(controls_.mode()==PlaybackControls::Mode::hidden);
  if (state_writer_.take_failure())
    state_dirty_ = true;
  if ((state_dirty_ && now - last_save_ > .5) ||
      (state_.playing && now - last_save_ >= 5.)) {
    if (!platform_.state_file.empty()) {
      state_writer_.post(platform_.state_file, snapshot());
      state_dirty_ = false;
    }
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
  control_activity();
  cancel_scrub();
  if(id>=1900 && id<=1912){
    if(id==1900){window_settings_open_=true;page_=4;ui_.page=cyvr::Page(page_);}
    else if(id==1901)window_settings_open_=false;
    else if(id==1902)ui_.window.resizable=!ui_.window.resizable;
    else if(id==1903)ui_.window.faceViewerWhileMoving=!ui_.window.faceViewerWhileMoving;
    else if(id==1904){ui_.settings.minimalDock=!ui_.settings.minimalDock;ui_.saveRequested=true;}
    else if(id==1905){ui_.window={};ui_.settings.follow=cyvr::FollowMode::FaceViewer;ui_.settings.minimalDock=true;ui_.saveRequested=true;}
    else if(id>=1910){ui_.settings.follow=cyvr::FollowMode(id-1910);ui_.saveRequested=true;}
    state_dirty_=true;ui_.invalidate();return true;
  }
  if(id==1064){
    if(ui_.presentation!=cyvr::Presentation::Immersive)return true;
    if(session_.state().ready)id=612;
    else {ui_.windowState=cyvr::WindowState::Closed;ui_.invalidate();return true;}
  }
  if(id==612 && ui_.presentation==cyvr::Presentation::Immersive && session_.state().ready){
    controls_.compact(controls_now());ui_.compactInterface=true;ui_.windowState=cyvr::WindowState::Open;ui_.invalidate();return true;
  }
  if(id==611 || id==1361){
    ui_.windowState=ui_.windowState==cyvr::WindowState::Open?cyvr::WindowState::Minimized:cyvr::WindowState::Open;
    controls_.expand(controls_now());ui_.compactInterface=false;ui_.invalidate();return true;
  }
  if(id>=1400 && id<1441)return track_action(id);
  if(id==1760){subtitle_settings_open_=false;state_dirty_=true;ui_.invalidate();return true;}
  if(id==1761 || id==1762){subtitle_settings_tab_=id-1761;state_dirty_=true;ui_.invalidate();return true;}
  if(id==1770)subtitle_settings.custom=!subtitle_settings.custom;
  else if(id==1771){subtitle_settings.bold=!subtitle_settings.bold;subtitle_settings.custom=true;}
  else if(id==1772){subtitle_settings.italic=!subtitle_settings.italic;subtitle_settings.custom=true;}
  else if(id==1773){subtitle_settings.box=!subtitle_settings.box;subtitle_settings.custom=true;}
  else if(id==1774)subtitle_settings.follow_depth=!subtitle_settings.follow_depth;
  else if(id==1775)subtitle_settings={};
  else if(id>=1790 && id<1796){subtitle_settings.color=id-1790;subtitle_settings.custom=true;}
  if(id>=1770 && id<1796){state_dirty_=true;ui_.invalidate();return true;}
  if((id>=1030 && id<=1032) || (id>=1040 && id<=1042) || id==1051 || id==1052)format_manual_=true;
  if (id >= 1000 && id <= 1004) {
    controls_.expand(controls_now());
    page_ = id - 1000;ui_.compactInterface=false;
    ui_.page = cyvr::Page(page_);
    state_dirty_ = true;
    ui_.invalidate();
    return true;
  }
  if (id == 1010 && !parents_.empty()) {
    auto parent = parents_.back();
    parents_.pop_back();
    browse(parent);
  } else if (id == 1011) {
    previews_->request(preview_requests_,true);browse(directory_);preview_requests_.clear();
  }
  else if (id == 1012 && platform_.request_tree)
    platform_.request_tree();
  else if (id == 1013) {
    set_browser_scroll(browser_scroll_-browser_row_height());
  } else if (id == 1014) {
    set_browser_scroll(browser_scroll_+browser_row_height());
  } else if(id==1015 || id==1016){const int first=offset_;browser_view_=id-1015;set_browser_scroll(float(first/browser_columns())*browser_row_height());preview_present_.fill(false);preview_requests_.clear();}
  else if(id==1017)toggle_favorite();
  else if(id==1018 || id==1019){sidebar_tab_=id-1018;sidebar_offset_=0;refresh_locations();}
  else if(id==1026)sidebar_offset_=std::max(0,sidebar_offset_-7);
  else if(id==1027)sidebar_offset_+=7;
  else if(id>=1800 && id<1807){const auto index=sidebar_offset_+id-1800;if(sidebar_tab_){if(index<int(favorites_.size())){auto favorite=favorites_[index];browse(favorite.location);parents_=favorite.parents;folder_name_=favorite.name;}}else if(index<int(locations_.size())){parents_.clear();browse(locations_[index].location);}}
  else if (id >= 1100 && id < 1100+int(preview_slots) &&
             offset_ + id - 1100 < int(entries_.size())) {
    auto item = entries_[offset_ + id - 1100];
    if (item.directory) {
      parents_.push_back(directory_);
      browse(item.location);folder_name_=item.name;
    } else if(classify_extension(item.name)==FileKind::key){
      if(session_.clearkey_file(item.location)){page_=1;ui_.page=cyvr::Page(1);tracks_open_=true;track_tab_=0;controls_.expand(controls_now());}
      else ui_.setToast(std::string(tr("Open a video before choosing a ClearKey file.")));
    } else if(classify_extension(item.name)==FileKind::subtitle){
      if(session_.external_subtitle(item.location)){page_=1;ui_.page=cyvr::Page(1);tracks_open_=true;track_tab_=2;controls_.expand(controls_now());}
      else ui_.setToast(std::string(tr("Open a seekable video before choosing subtitles.")));
    } else
      open(item.location, item.name);
  } else if (id == 1020) {
    resume_playing_ = !state_.playing;
    session_.pause(state_.playing);
  } else if (id == 1021 || id == 1022) {
    const auto media = session_.state();
    session_.seek(std::clamp<std::int64_t>(
        (media.seeking ? media.seek_target_us : media.position_us) +
            (id == 1021 ? -10000000 : 10000000),
        0, media.duration_us > 0 ? media.duration_us : INT64_MAX));
  } else if (id == 1023) {
    show_interface();
    source_.clear();
    title_.clear();
    requested_position_ = 0;
    resume_playing_ = false;
    preview_ready = false;
    session_.stop();
  } else if (id == 1024) {
    if (session_.state().ready) {
      controls_.hide();
      ui_.setInterfaceHidden(true);
    }
  } else if (id == 1360) {
    controls_.expand(controls_now()); ui_.compactInterface=false;ui_.windowState=cyvr::WindowState::Open;
  } else if (id == 1362 || id == 1363) {
    adjacent_video(id==1362 ? -1 : 1,true);
  } else if (id >= 1030 && id <= 1032)
    stereo = id - 1030;
  else if (id >= 1040 && id <= 1042)
    projection = id - 1040;
  else if(id==1053){format_manual_=!auto_format_;refresh_video_format(true);}
  else if(id==1054){auto_format_=!auto_format_;format_manual_=!auto_format_;if(auto_format_)refresh_video_format();}
  else if (id == 1050)
    ++video_placement_request;
  else if (id == 1051)
    half_stereo = !half_stereo;
  else if (id == 1052)
    swap_eyes = !swap_eyes;
  else if (id >= 1300 && id <= 1304)
    display_tab_ = id - 1300;
  else if(id>=1610 && id<=1617)cinema.environment=id-1610;
  else if(id==1620)cinema.curvature=0;
  else if(id==1621)cinema.curvature=60;
  else if(id==1622){cinema.width=8;cinema.distance=5;cinema.curvature=70;cinema.elevation=cinema.pitch=cinema.roll=0;}
  else if(id==1623)cinema.reset_screen();
  else if(id>=1660 && id<=1661)cinema_tab_=id-1660;
  else if(id==1663)cinema_tab_=2;
  else if(id==1670)ambilight.enabled=!ambilight.enabled;
  else if(id==1671)ambilight.auto_bars=!ambilight.auto_bars;
  else if(id==1672)ambilight={};
  else if(id==1662){controls_.expand(controls_now());ui_.compactInterface=false;page_=3;ui_.page=cyvr::Page(3);display_tab_=3;}
  else if (id >= 1310 && id <= 1313) {
    image.quality = id - 1310;
    ++processing_request;
  } else if (id >= 1320 && id <= 1323) {
    image.upscale = id - 1320;
    ++processing_request;
  } else if (id >= 1350 && id <= 1352) {
    image.sampling = id-1350;
    ++processing_request;
  } else if (id == 1353) {
    image.sampling=0;image.denoise=0;image.detail=0;
    ++processing_request;
  } else if(id==1354){smooth_video=!smooth_video;
  } else if(id==1681){scene.reset();cinema.elevation=cinema.pitch=cinema.roll=0;}
  else if(id==1682){scene.stable=!scene.stable;}
  else if(id==1683){scene.reset();cinema.elevation=cinema.pitch=cinema.roll=0;image.pan_x=image.pan_y=0;++video_placement_request;}
  else if(id==1680 || (id>=1364 && id<=1369)){
    controls_.expand(controls_now());ui_.compactInterface=false;
    if(id==1364)page_=0;else if(id==1365){page_=1;tracks_open_=false;}else if(id==1368){page_=1;tracks_open_=true;}else if(id==1369)page_=4;else{page_=3;display_tab_=(id==1367 || id==1680)?4:2;}ui_.page=cyvr::Page(page_);
  } else if(id==1370){if(volume>0){unmuted_volume_=volume;volume=0;}else volume=unmuted_volume_;session_.volume(volume);}
  else if(id==1063){page_=4;ui_.page=cyvr::Page(4);controls_.expand(controls_now());ui_.compactInterface=false;}
  else if (id == 1330) {
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
  else if (id == 1256) {
    if (platform_.open_browser)
      platform_.open_browser(ui_.settings.language);
  } else if (id == 1255)
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
  image.zoom=std::clamp(image.zoom,minimum_zoom(),4.f);
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
  s.browser_scroll=browser_scroll_;
  s.stereo = stereo;
  s.projection = projection;
  s.half_stereo = half_stereo;
  s.swap_eyes = swap_eyes;
  s.image = image;
  s.format_manual=format_manual_;
  s.auto_format=auto_format_;s.smooth_video=smooth_video;
  s.cinema=cinema;s.ambilight=ambilight;s.cinema_tab=cinema_tab_;
  s.tracks=session_.selection();
  s.subtitle_settings=subtitle_settings;s.subtitle_settings_open=subtitle_settings_open_;s.subtitle_settings_tab=subtitle_settings_tab_;
  s.window_resizable=ui_.window.resizable;s.window_face_viewer=ui_.window.faceViewerWhileMoving;s.window_settings_open=window_settings_open_;
  s.tracks_open=tracks_open_;s.track_tab=track_tab_;s.track_offset=track_offset_;
  s.display_tab = display_tab_;s.browser_view=browser_view_;s.favorites=favorites_;s.scene=scene;s.volume=volume;
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
  state_writer_.drain();
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
  format_manual_=s.format_manual;auto_format_=s.auto_format;format_track_=-1;smooth_video=s.smooth_video;
  stereo = s.stereo;
  projection = s.projection;
  half_stereo = s.half_stereo;
  swap_eyes = s.swap_eyes;
  image = s.image;
  cinema=s.cinema;cinema.sanitize();ambilight=s.ambilight;ambilight.sanitize();cinema_tab_=std::clamp(s.cinema_tab,0,2);
  display_tab_ = s.display_tab;browser_view_=s.browser_view;favorites_=s.favorites;scene=s.scene;volume=s.volume;session_.volume(volume);
  browser_scroll_=s.browser_scroll>=0?s.browser_scroll:float(offset_/browser_columns())*browser_row_height();
  subtitle_settings=s.subtitle_settings;subtitle_settings.sanitize();subtitle_settings_open_=s.subtitle_settings_open;subtitle_settings_tab_=s.subtitle_settings_tab;
  ui_.window.resizable=s.window_resizable;ui_.window.faceViewerWhileMoving=s.window_face_viewer;window_settings_open_=s.window_settings_open;
  tracks_open_=s.tracks_open;track_tab_=s.track_tab;track_offset_=s.track_offset;
  shift_ = s.shift;
  symbols_ = s.symbols;
  source_ = s.source;
  title_ = s.title;
  requested_position_ = s.position_us;
  resume_playing_ = s.playing;
  if (resume_media && !source_.empty())
    format_media_id_=session_.open(source_, title_, s.position_us, s.playing,s.tracks);
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
  control_activity();
  if(id>=1640 && id<=1652){
    if(phase==cyvr::SliderPhase::Cancel || !std::isfinite(fraction))return true;
    const auto f=std::clamp(fraction,0.f,1.f);
    switch(id){
      case 1640:cinema.width=1+11*f;break;
      case 1641:cinema.distance=1+11*f;break;
      case 1642:cinema.curvature=120*f;break;
      case 1643:cinema.elevation=-3+6*f;break;
      case 1644:cinema.pitch=-90+180*f;break;
      case 1645:cinema.roll=-180+360*f;break;
      case 1646:cinema.ambient=f;break;
      case 1647:cinema.panel_opacity=.25f+.75f*f;break;
      case 1648:ambilight.intensity=2*f;break;
      case 1649:ambilight.spread=.05f+.55f*f;break;
      case 1650:ambilight.smoothing=1.5f*f;break;
      case 1651:ambilight.saturation=2*f;break;
      case 1652:ambilight.inset=.2f*f;break;
    }
    cinema.sanitize();ambilight.sanitize();state_dirty_=true;ui_.invalidate();return true;
  }
  if(id>=1780 && id<=1786){
    if(phase==cyvr::SliderPhase::Cancel || !std::isfinite(fraction))return true;
    const float f=std::clamp(fraction,0.f,1.f);
    switch(id){case 1780:subtitle_settings.scale=.5f+2*f;break;case 1781:subtitle_settings.opacity=f;break;
      case 1782:subtitle_settings.outline=6*f;subtitle_settings.custom=true;break;case 1783:subtitle_settings.shadow=6*f;subtitle_settings.custom=true;break;
      case 1784:subtitle_settings.x=-.45f+.9f*f;break;case 1785:subtitle_settings.y=-.45f+.9f*f;break;
      case 1786:subtitle_settings.distance=.5f+11.5f*f;subtitle_settings.follow_depth=false;break;}
    subtitle_settings.sanitize();state_dirty_=true;ui_.invalidate();return true;
  }
  if(id==1371 || (id>=1690 && id<=1695)){
    if(phase==cyvr::SliderPhase::Cancel || !std::isfinite(fraction))return true;
    const auto f=std::clamp(fraction,0.f,1.f);
    if(id==1371){volume=f;if(volume>0)unmuted_volume_=volume;session_.volume(volume);}
    else{float* values[]{&scene.x,&scene.y,&scene.z,&scene.yaw,&scene.pitch,&scene.roll};*values[id-1690]=(id<1693?20.f:360.f)*(f-.5f);scene.sanitize();}
    state_dirty_=true;ui_.invalidate();return true;
  }
  if (id >= 1340 && id <= 1349) {
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
      image.zoom = minimum_zoom() + f * (4-minimum_zoom());
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
    case 1348:
      image.denoise = f;
      ++processing_request;
      break;
    case 1349:
      image.detail = f;
      ++processing_request;
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
void PlayerApplication::control_activity() {
  controls_.activity(controls_now());
  if(ui_.interfaceHidden)ui_.revealInterface();
  if(ui_.presentation==cyvr::Presentation::Immersive && session_.state().ready)
    ui_.compactInterface=controls_.mode()!=PlaybackControls::Mode::full;
}
bool PlayerApplication::show_interface() {
  const bool hidden=ui_.interfaceHidden;
  control_activity();
  return hidden;
}
bool PlayerApplication::adjacent_video(int direction, bool play) {
  // Drawing asks this every frame; folders can hold thousands of entries.
  if(playlist_position_>=playlist_.size() || playlist_[playlist_position_].location!=source_){
    const auto it=std::find_if(playlist_.begin(),playlist_.end(),[&](const auto& item){return item.location==source_;});
    if(it==playlist_.end())return false;
    playlist_position_=std::size_t(std::distance(playlist_.begin(),it));
  }
  const auto index=static_cast<std::ptrdiff_t>(playlist_position_)+direction;
  if(index<0 || index>=static_cast<std::ptrdiff_t>(playlist_.size()))return false;
  if(play){const auto item=playlist_[std::size_t(index)];open(item.location,item.name);}
  return true;
}
void PlayerApplication::video_input(float seek, float zoom, float x, float y) {
  if (seek != 0) {
    const auto media = session_.state();
    if (media.ready && media.seekable)
      session_.seek(std::max<std::int64_t>(
          0, (media.seeking ? media.seek_target_us : media.position_us) +
                 std::int64_t(seek * 1000000)));
  }
  image.zoom += zoom;
  image.zoom = std::clamp(image.zoom,minimum_zoom(),4.f);
  image.pan_x += x;
  image.pan_y += y;
  image.sanitize();
  state_dirty_ = true;
  ui_.invalidate();
}
void PlayerApplication::set_logo(const std::vector<std::uint8_t>& rgba) {
  if(rgba.size()!=logo_size*logo_size*4)throw std::invalid_argument("Invalid player logo dimensions");
  std::copy(rgba.begin(),rgba.end(),thumbnail_atlas_.begin()+logo_atlas_y*ui_atlas_width*4);
  logo_ready_=true;thumbnail_dirty_=true;ui_.invalidate();
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
