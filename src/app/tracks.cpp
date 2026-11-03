#include "cyvr/player/application.hpp"
#include <algorithm>
namespace cyvr::player {
namespace {
std::vector<const TrackInfo*> filtered(const PlayerState& state,int tab){
  const TrackKind kinds[]={TrackKind::video,TrackKind::audio,TrackKind::subtitle};
  std::vector<const TrackInfo*> result;
  if(tab<3)for(const auto& track:state.tracks)if(track.kind==kinds[tab] && !track.attached_picture)result.push_back(&track);
  return result;
}
}
void PlayerApplication::draw_tracks(cyvr::DrawList& d){
  if(subtitle_settings_open_){draw_subtitle_settings(d);return;}
  using cyvr::Color;
  const bool light=ui_.settings.theme==cyvr::Theme::Light;
  const auto white=Color::hex(light?0x142b35:0xf0f5f6),muted=Color::hex(light?0x48616b:0xa7b7bf);
  d.text(36,184,32,tr("Tracks, subtitles and chapters"),white);
  button(d,1401,{1176,181,228,52},"Back to playback");
  const char* tabs[]={"Video tracks","Audio tracks","Subtitles","Chapters","Disc titles"};
  for(int n=0;n<5;++n)button(d,1410+n,{36.f+n*275,252,263,52},tabs[n],track_tab_==n);
  auto media=session_.state();const auto tracks=filtered(media,track_tab_);
  const auto total=track_tab_==4?int(media.disc.titles.size()):track_tab_==3?int(media.chapters.size()):int(tracks.size());
  track_offset_=std::min(track_offset_,total?((total-1)/7)*7:0);
  for(int row=0;row<7 && track_offset_+row<total;++row){
    const auto index=track_offset_+row;std::string label;bool selected=false;
    if(track_tab_==4){const auto& t=media.disc.titles[index];label=media.disc.type+"  #"+std::to_string(t.index+1)+"  "+t.name+"  "+std::to_string(t.duration_us/60000000)+":"+(t.duration_us/1000000%60<10?"0":"")+std::to_string(t.duration_us/1000000%60)+"  /  "+std::to_string(t.chapters)+" "+std::string(tr("Chapters"));selected=t.index==media.disc.selected;}
    else if(track_tab_==3){const auto& c=media.chapters[index];label=std::to_string(index+1)+". "+(c.title.empty()?std::string(tr("Chapter")):c.title)+"  ("+std::to_string(c.start_us/60000000)+":"+(c.start_us/1000000%60<10?"0":"")+std::to_string(c.start_us/1000000%60)+")";}
    else{
      const auto& t=*tracks[index];label="#"+std::to_string(t.index)+"  "+t.codec+"  "+t.language+"  "+t.title;
      if(t.width>0)label+="  "+std::to_string(t.width)+"x"+std::to_string(t.height);
      if(t.channels>0)label+="  "+std::to_string(t.channels)+" ch / "+std::to_string(t.sample_rate)+" Hz";
      if(t.default_track)label+="  ["+std::string(tr("Default"))+"]";
      if(t.forced)label+="  ["+std::string(tr("Forced"))+"]";
      if(t.hearing_impaired)label+="  [SDH]";
      if(t.companion_video>=0)label+="  [MVC]";
      selected=t.index==(track_tab_==0?media.video_track:track_tab_==1?media.audio_track:media.subtitle_track) && (track_tab_!=2 || session_.selection().external_subtitle.empty());
      if(track_tab_==0 && t.companion_video==media.video_track && t.companion_video>=0)selected=true;
    }
    while(!label.empty() && d.textWidth(23,label)>1290){auto n=label.size()-1;while(n>0 && (static_cast<unsigned char>(label[n])&0xc0)==0x80)--n;label.resize(n);}
    button(d,1420+row,{36,325.f+row*64,1368,54},label,selected,media.ready);
  }
  if(!total)d.text(50,365,24,tr("No tracks in this category."),muted);
  if(track_tab_==1 || track_tab_==2)button(d,1432,{36,790,220,52},"Disabled",track_tab_==1?media.audio_track<0:media.subtitle_track<0 && session_.selection().external_subtitle.empty(),media.ready);
  if(track_tab_==2){button(d,1433,{270,790,530,52},"Choose external subtitles",false,media.ready);button(d,1440,{908,855,496,52},"Subtitle settings");}
  if(track_tab_==0){
    button(d,1434,{36,790,430,52},"Import ClearKey (JWK)");
    button(d,1435,{480,790,300,52},"Remove ClearKey",false,!session_.selection().clearkey_file.empty());
    if(!session_.selection().clearkey_file.empty())d.text(36,866,22,tr("ClearKey file selected"),white);
  }
  if(track_tab_==4 && media.disc.selected>=0){
    const auto it=std::find_if(media.disc.titles.begin(),media.disc.titles.end(),[&](const auto& t){return t.index==media.disc.selected;});
    if(it!=media.disc.titles.end() && it->angles>1)button(d,1436,{36,790,430,52},std::string(tr("Angle"))+" "+std::to_string(media.disc.angle+1)+" / "+std::to_string(it->angles),false,media.ready);
    d.text(36,866,20,tr("Movie titles; interactive disc menus are unavailable."),muted);
    if(!media.disc.note.empty())d.text(36,903,20,tr(media.disc.note),Color::hex(0xf2c683));
  }
  button(d,1430,{1008,790,190,52},"Previous",false,track_offset_>0);
  button(d,1431,{1212,790,192,52},"Next",false,track_offset_+7<total);
  const auto external=session_.selection().external_subtitle;
  if(track_tab_==2 && !external.empty())d.text(36,866,22,tr("External subtitles selected"),white);
  const auto warning=track_tab_==1?media.audio_status:track_tab_==2?media.subtitle_status:std::string{};
  if(!warning.empty())d.text(36,866,20,fit_text(d,warning,820,20),Color::hex(0xf2c683));
  if(!media.seekable)d.text(36,913,20,tr("Track switching requires a seekable source."),muted);
}
void PlayerApplication::draw_subtitle_settings(cyvr::DrawList& d){
  using cyvr::Color;
  const auto white=Color::hex(ui_.settings.theme==cyvr::Theme::Light?0x142b35:0xf0f5f6),accent=Color::hex(0x86e7cf),muted=Color::hex(0xa7b7bf);
  d.text(36,184,32,tr("Subtitle settings"),white);
  button(d,1760,{1176,181,228,52},"Back");
  button(d,1761,{36,252,670,52},"Text appearance",subtitle_settings_tab_==0);
  button(d,1762,{724,252,680,52},"Subtitle placement",subtitle_settings_tab_==1);
  auto slider=[&](int id,float y,std::string_view label,float value,float fraction){
    d.text(44,y+12,22,tr(label),white);d.text(378,y+12,21,cyvr::formatNumber(ui_.settings.language,value,2),accent);
    const cyvr::Rect r{504,y,888,52};d.rect({r.x,y+22,r.w,8},Color::hex(0x344b56),4);d.rect({r.x,y+22,r.w*std::clamp(fraction,0.f,1.f),8},accent,4);d.circle({r.x+r.w*std::clamp(fraction,0.f,1.f),y+26},10,accent);ui_.applicationWidget(id,r,cyvr::Control::Slider);
  };
  if(!subtitle_settings_tab_){
    button(d,1770,{36,326,1368,54},subtitle_settings.custom?"Custom text style":"Keep source styling",subtitle_settings.custom);
    slider(1780,402,"Text size",subtitle_settings.scale,(subtitle_settings.scale-.5f)/2);
    slider(1781,466,"Subtitle opacity",subtitle_settings.opacity,subtitle_settings.opacity);
    slider(1782,530,"Outline thickness",subtitle_settings.outline,subtitle_settings.outline/6);
    slider(1783,594,"Text shadow",subtitle_settings.shadow,subtitle_settings.shadow/6);
    const char* names[]{"White","Yellow","Cyan","Green","Orange","Pink"};
    for(int i=0;i<6;++i){cyvr::Rect r{36.f+i*230,670,218,54};button(d,1790+i,r,names[i],subtitle_settings.color==i);SubtitleSettings sample;sample.color=i;d.circle({r.x+18,r.y+27},6,Color::hex(sample.text_color()));}
    button(d,1771,{36,750,330,54},"Bold",subtitle_settings.bold);
    button(d,1772,{382,750,330,54},"Italic",subtitle_settings.italic);
    button(d,1773,{728,750,676,54},"Subtitle background",subtitle_settings.box);
    d.text(36,838,19,tr("Text styles affect text subtitles; image subtitles keep their colors."),muted);
  }else{
    slider(1784,348,"Horizontal offset",subtitle_settings.x,subtitle_settings.x/.9f+.5f);
    slider(1785,430,"Vertical offset",subtitle_settings.y,subtitle_settings.y/.9f+.5f);
    slider(1780,512,"Subtitle size",subtitle_settings.scale,(subtitle_settings.scale-.5f)/2);
    slider(1786,594,"VR distance (m)",subtitle_settings.distance,(subtitle_settings.distance-.5f)/11.5f);
    button(d,1774,{36,686,1368,58},"Follow movie depth",subtitle_settings.follow_depth);
    d.text(36,788,20,tr("Offsets and size also apply to image subtitles. Distance applies in VR."),muted);
  }
  button(d,1775,{36,886,530,54},"Reset subtitles");
  d.text(596,902,18,tr("Changes apply immediately, including paused video."),muted);
}
bool PlayerApplication::track_action(int id){
  if(id==1440){page_=1;ui_.page=cyvr::Page(1);tracks_open_=true;track_tab_=2;ui_.windowState=cyvr::WindowState::Open;ui_.compactInterface=false;subtitle_settings_open_=true;controls_.expand(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());ui_.invalidate();return true;}
  if(id==1400){tracks_open_=true;controls_.expand(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());}
  else if(id==1401)tracks_open_=false;
  else if(id>=1410 && id<=1414){track_tab_=id-1410;track_offset_=0;}
  else if(id==1430)track_offset_=std::max(0,track_offset_-7);
  else if(id==1431)track_offset_+=7;
  else if(id==1433){tracks_open_=false;page_=0;ui_.page=cyvr::Page(0);ui_.setToast(std::string(tr("Choose a subtitle file in the library.")));}
  else if(id==1434){tracks_open_=false;page_=0;ui_.page=cyvr::Page(0);ui_.setToast(std::string(tr("Choose a .jwk file in the library.")));}
  else if(id==1435)session_.clearkey_file({});
  else if(id==1436){const auto media=session_.state();const auto it=std::find_if(media.disc.titles.begin(),media.disc.titles.end(),[&](const auto& t){return t.index==media.disc.selected;});if(it!=media.disc.titles.end())session_.select_disc_title(it->index,(media.disc.angle+1)%int(it->angles));}
  else if(id==1432 || (id>=1420 && id<1427)){
    const auto state=session_.state();const auto tracks=filtered(state,track_tab_);const int index=track_offset_+id-1420;
    bool accepted=false;
    if(track_tab_==4 && id!=1432 && index>=0 && index<int(state.disc.titles.size()))accepted=session_.select_disc_title(state.disc.titles[index].index);
    else if(track_tab_==3 && id!=1432 && index>=0 && index<int(state.chapters.size())){session_.seek(state.chapters[index].start_us);accepted=state.seekable;}
    else if(track_tab_<3 && (id==1432 || (index>=0 && index<int(tracks.size())))){
      const TrackKind kinds[]={TrackKind::video,TrackKind::audio,TrackKind::subtitle};
      accepted=session_.select_track(kinds[track_tab_],id==1432?-1:tracks[index]->index);
    }
    if(!accepted)ui_.setToast(std::string(tr("Track switching requires a seekable source.")));
  }else return false;
  state_dirty_=true;ui_.invalidate();return true;
}
}
