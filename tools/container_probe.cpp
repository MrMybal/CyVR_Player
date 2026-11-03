#include "cyvr/player/application.hpp"
#include "cyvr/player/subtitle_rasterizer.hpp"
#include "cyvr/gpu.hpp"
#include <fstream>
#include <iostream>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
}
using namespace cyvr::player;
int main(int argc,char** argv){
  av_log_set_level(AV_LOG_QUIET);
  try{
    if(argc!=3)throw std::runtime_error("container_probe MEDIA ASSETS");
    const auto media=MediaSource::file(argv[1]);const auto assets=std::filesystem::u8path(argv[2]);
    auto read=[&](const std::string& name){std::ifstream f(assets/name,std::ios::binary);if(!f)throw std::runtime_error("Missing test asset");return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),{});};
    Demuxer info;info.open(media);const auto tracks=info.tracks();
    if(info.chapters().size()!=2 || info.chapters()[1].start_us!=1000000 || info.fonts().size()!=1)throw std::runtime_error("MKV chapters or font attachments missing");
    int videos=0,audios=0,subtitles=0;
    for(const auto& track:tracks){
      if(track.kind!=TrackKind::video && track.kind!=TrackKind::audio && track.kind!=TrackKind::subtitle)continue;
      Demuxer in;in.open(media);
      if(track.kind==TrackKind::subtitle){
        SubtitleDecoder decoder(in,track.index);auto config=std::make_shared<SubtitleConfig>();config->header=decoder.header();config->fonts=in.fonts();SubtitleTimeline timeline(config);
        for(;;){auto p=in.read();if(p.status==ReadStatus::end)break;if(p.status==ReadStatus::packet && p.packet->stream_index==track.index)timeline.add(decoder.decode(p.packet.get()));}
        SubtitleRasterizer renderer(read("subtitle-font.ttf"));
        const auto active=timeline.at(600000);if(active.cues.empty())throw std::runtime_error("Embedded subtitle timing lost");
        auto image=renderer.render(active,1920,1080);
        if(!image || image->rgba.empty() || std::none_of(image->rgba.begin()+3,image->rgba.end(),[](auto b){return b!=0;}))throw std::runtime_error("Embedded subtitle did not rasterize");
        if(track.codec=="subrip"){
          SubtitleSettings style;style.custom=true;style.color=1;style.bold=true;style.scale=1.4f;style.opacity=.5f;style.y=-.1f;
          auto styled=renderer.render(active,1920,1080,style);if(!styled || styled->rgba.empty())throw std::runtime_error("Custom subtitles failed to render");
          bool yellow=false;for(std::size_t n=0;n<styled->rgba.size();n+=4){if(styled->rgba[n+3]>128)throw std::runtime_error("Subtitle opacity ignored");if(styled->rgba[n+3]>20 && styled->rgba[n]>180 && styled->rgba[n+1]>150 && styled->rgba[n+2]<150)yellow=true;}
          if(!yellow || styled->width<=image->width || styled->y>=image->y)throw std::runtime_error("Subtitle color, size or placement ignored");
          if(renderer.render(active,1920,1080,style))throw std::runtime_error("Unchanged styled subtitle must stay cached");
        }
        auto clear=renderer.render(timeline.at(1150000),1920,1080);if(!clear || !clear->rgba.empty())throw std::runtime_error("Subtitle did not clear in the gap");
        auto next=renderer.render(timeline.at(1500000),1920,1080);if(!next || next->rgba.empty())throw std::runtime_error("Second subtitle missing");
        auto seek=renderer.render(timeline.at(600000),1920,1080);if(!seek || seek->rgba.empty())throw std::runtime_error("Subtitle rewind failed");
        ++subtitles;
      }else{
        SoftwareDecoder decoder(in,track.index);std::uint64_t count=0;
        auto drain=[&]{for(;;){auto f=decoder.receive();if(!f.frame)break;count+=track.kind==TrackKind::video?1:f.frame->nb_samples;}};
        for(;;){auto p=in.read();if(p.status==ReadStatus::end)break;if(p.status!=ReadStatus::packet || p.packet->stream_index!=track.index)continue;if(!decoder.send(p.packet.get())){drain();if(!decoder.send(p.packet.get()))throw std::runtime_error("Decoder stalled");}drain();}
        if(!decoder.send(nullptr)){drain();decoder.send(nullptr);}drain();
        if(track.kind==TrackKind::video){if(count!=48)throw std::runtime_error("Video track frame count mismatch");++videos;}
        else {if(count<96000)throw std::runtime_error("Audio track sample count mismatch");++audios;}
      }
    }
    if(videos!=2 || audios!=2 || subtitles!=2)throw std::runtime_error("Track coverage mismatch");
    {
      Demuxer in;in.open(MediaSource::file((std::filesystem::u8path(argv[1]).parent_path()/"captions.sup").u8string()));
      if(in.tracks().empty())throw std::runtime_error("PGS track not detected");
      SubtitleDecoder decoder(in,in.tracks().front().index);auto config=std::make_shared<SubtitleConfig>();config->header=decoder.header();SubtitleTimeline timeline(config);
      for(;;){auto p=in.read();if(p.status==ReadStatus::end)break;if(p.status==ReadStatus::packet)timeline.add(decoder.decode(p.packet.get()));}
      auto snapshot=timeline.at(600000);if(snapshot.cues.empty() || snapshot.cues[0]->bitmaps.empty())throw std::runtime_error("PGS bitmap decoding failed");
      SubtitleRasterizer renderer(read("subtitle-font.ttf"));auto image=renderer.render(snapshot,320,180);
      if(!image || image->width!=2 || image->height!=2 || image->rgba[3]!=255 || image->rgba[0]<240)throw std::runtime_error("PGS palette, geometry or alpha lost");
      if(renderer.render(timeline.at(700000),320,180))throw std::runtime_error("Static PGS subtitle triggered a redundant GPU upload");
      SubtitleSettings placement;placement.x=.1f;placement.y=-.1f;placement.opacity=.5f;
      auto moved=renderer.render(snapshot,320,180,placement);
      if(!moved || moved->x!=image->x+32 || moved->y!=image->y-18 || moved->rgba[3]!=128 || moved->rgba[0]!=image->rgba[0])throw std::runtime_error("Bitmap subtitle placement, opacity or palette changed incorrectly");
      auto cleared=renderer.render(timeline.at(1150000),320,180);if(!cleared || !cleared->rgba.empty())throw std::runtime_error("PGS overlay did not clear");
      if(!timeline.at(1150000).cues.empty())throw std::runtime_error("PGS display clear failed");
    }
    cyvr::UiModel ui;Platform platform;platform.root=std::filesystem::u8path(argv[1]).parent_path().u8string();platform.hardware_codec=[](const std::string&,int,int){return std::string{};};PlayerApplication app(ui,platform);
    auto wait=[&](auto predicate){for(int n=0;n<600;++n){app.tick();if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(5));}throw std::runtime_error("Track switch timed out");};
    app.open(argv[1],"Container fixture");wait([&]{return app.session().state().ready;});app.session().pause(true);app.session().seek(750000);
    int video=-1,audio=-1,sub=-1;
    for(const auto& t:tracks){if(t.kind==TrackKind::video)video=t.index;if(t.kind==TrackKind::audio)audio=t.index;if(t.kind==TrackKind::subtitle)sub=t.index;}
    for(auto item:{std::pair<TrackKind,int>{TrackKind::video,video},{TrackKind::audio,audio},{TrackKind::subtitle,sub}}){
      if(!app.session().select_track(item.first,item.second))throw std::runtime_error("Valid track switch rejected");
      wait([&]{const auto s=app.session().state();return s.ready && (item.first==TrackKind::video?s.video_track:item.first==TrackKind::audio?s.audio_track:s.subtitle_track)==item.second;});
      const auto s=app.session().state();if(s.playing || s.position_us!=750000 || app.session().selection().video!=video)throw std::runtime_error("Track switch lost pause or prior selection");
    }
    if(app.session().select_track(TrackKind::audio,999) || app.session().select_track(TrackKind::video,-1))throw std::runtime_error("Invalid track accepted");
    app.action(1001);app.action(1400);cyvr::DrawList draw;ui.drawMain(draw);
    for(int tab=0;tab<4;++tab){app.action(1410+tab);ui.drawMain(draw);if(std::none_of(ui.widgets().begin(),ui.widgets().end(),[](auto& w){return w.id==1420 && w.enabled;}))throw std::runtime_error("Track/chapters menu has no selectable rows");}
    // Real Vulkan overlay sampling, independent of video and thumbnail bindings.
    cyvr::VulkanContext gpu;gpu.createStandalone();cyvr::VulkanRenderer overlay(gpu),output(gpu);overlay.initialize(VK_FORMAT_R8G8B8A8_UNORM,read);output.initialize(VK_FORMAT_R8G8B8A8_UNORM,read);
    overlay.uploadImages(1,1,{255,0,0,128});output.setOverlayTexture(overlay.imageAtlasView(),overlay.imageSampler());auto target=output.createTarget(16,16);cyvr::DrawList test;test.rect({0,0,16,16},cyvr::Color::hex(0x0000ff));test.overlay({0,0,16,16});output.draw(target,test);auto pixels=output.readback(target);output.destroyTarget(target);
    const auto* px=pixels.data()+(8*16+8)*4;if(std::abs(int(px[0])-128)>1 || px[1] || std::abs(int(px[2])-127)>1)throw std::runtime_error("Subtitle overlay alpha blend failed");
    std::cout<<"MKV: 2 video, 2 audio, 2 subtitle tracks decoded; styled subtitles, gaps, rewind, font attachments, chapters, track switching and GPU alpha overlay passed\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
