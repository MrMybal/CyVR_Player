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
  using cyvr::Color;
  const bool light=ui_.settings.theme==cyvr::Theme::Light;
  const auto white=Color::hex(light?0x142b35:0xf0f5f6),muted=Color::hex(light?0x48616b:0xa7b7bf);
  d.text(36,184,32,tr("Tracks, subtitles and chapters"),white);
  button(d,1401,{1176,181,228,52},"Back to playback");
  const char* tabs[]={"Video tracks","Audio tracks","Subtitles","Chapters"};
  for(int n=0;n<4;++n)button(d,1410+n,{36.f+n*344,252,330,52},tabs[n],track_tab_==n);
  auto media=session_.state();const auto tracks=filtered(media,track_tab_);
  const auto total=track_tab_==3?int(media.chapters.size()):int(tracks.size());
  track_offset_=std::min(track_offset_,total?((total-1)/7)*7:0);
  for(int row=0;row<7 && track_offset_+row<total;++row){
    const auto index=track_offset_+row;std::string label;bool selected=false;
    if(track_tab_==3){const auto& c=media.chapters[index];label=std::to_string(index+1)+". "+c.title+"  ("+std::to_string(c.start_us/60000000)+":"+(c.start_us/1000000%60<10?"0":"")+std::to_string(c.start_us/1000000%60)+")";}
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
  if(track_tab_==2)button(d,1433,{270,790,530,52},"Choose external subtitles",false,media.ready);
  if(track_tab_==0){
    button(d,1434,{36,790,430,52},"Import ClearKey (JWK)");
    button(d,1435,{480,790,300,52},"Remove ClearKey",false,!session_.selection().clearkey_file.empty());
    if(!session_.selection().clearkey_file.empty())d.text(36,866,22,tr("ClearKey file selected"),white);
  }
  button(d,1430,{1008,790,190,52},"Previous",false,track_offset_>0);
  button(d,1431,{1212,790,192,52},"Next",false,track_offset_+7<total);
  const auto external=session_.selection().external_subtitle;
  if(track_tab_==2 && !external.empty())d.text(36,866,22,tr("External subtitles selected"),white);
  const auto warning=track_tab_==1?media.audio_status:track_tab_==2?media.subtitle_status:std::string{};
  if(!warning.empty())d.text(36,866,20,warning.substr(0,100),Color::hex(0xf2c683));
  if(!media.seekable)d.text(36,913,20,tr("Track switching requires a seekable source."),muted);
}
bool PlayerApplication::track_action(int id){
  if(id==1400){tracks_open_=true;controls_.expand(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());}
  else if(id==1401)tracks_open_=false;
  else if(id>=1410 && id<=1413){track_tab_=id-1410;track_offset_=0;}
  else if(id==1430)track_offset_=std::max(0,track_offset_-7);
  else if(id==1431)track_offset_+=7;
  else if(id==1433){tracks_open_=false;page_=0;ui_.page=cyvr::Page(0);ui_.setToast(std::string(tr("Choose a subtitle file in the library.")));}
  else if(id==1434){tracks_open_=false;page_=0;ui_.page=cyvr::Page(0);ui_.setToast(std::string(tr("Choose a .jwk file in the library.")));}
  else if(id==1435)session_.clearkey_file({});
  else if(id==1432 || (id>=1420 && id<1427)){
    const auto state=session_.state();const auto tracks=filtered(state,track_tab_);const int index=track_offset_+id-1420;
    bool accepted=false;
    if(track_tab_==3 && id!=1432 && index>=0 && index<int(state.chapters.size())){session_.seek(state.chapters[index].start_us);accepted=state.seekable;}
    else if(track_tab_<3 && (id==1432 || (index>=0 && index<int(tracks.size())))){
      const TrackKind kinds[]={TrackKind::video,TrackKind::audio,TrackKind::subtitle};
      accepted=session_.select_track(kinds[track_tab_],id==1432?-1:tracks[index]->index);
    }
    if(!accepted)ui_.setToast(std::string(tr("Track switching requires a seekable source.")));
  }else return false;
  state_dirty_=true;ui_.invalidate();return true;
}
}
