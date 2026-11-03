#include "cyvr/player/application.hpp"
namespace cyvr::player {
bool PlayerApplication::controller_input(const cyvr::ControllerInput& input) {
  const auto hand=input.hand;if(hand>=2)return false;
  const auto stick=input.stick;
  const float trigger=input.trigger,squeeze=input.grip;
  const float dt=std::isfinite(input.deltaSeconds)?std::clamp(input.deltaSeconds,0.f,.05f):0;
  const bool trigger_press=trigger>=.55f && last_trigger_[hand]<.55f;
  const bool press=trigger_press || (squeeze>=.65f && last_grip_[hand]<.65f);
  last_trigger_[hand]=trigger;last_grip_[hand]=squeeze;
  if(ui_.interfaceHidden){
    if(press){wake_gesture_[hand]=true;control_activity();}
    else {
      bool changed=false;
      if(input.poseValid && trigger<.35f && squeeze<.65f && session_.video_state().ready){
        if(video_manipulation && ui_.presentation==cyvr::Presentation::Immersive && std::abs(stick.x)>.22f && video_manipulation(input,true)){
          state_dirty_=true;ui_.invalidate();changed=true;
        }
        if(hand==1 && std::abs(stick.y)>.2f){video_input(0,stick.y*dt);changed=true;}
      }
      return changed;
    }
  }else if(press)control_activity();
  if(wake_gesture_[hand]){
    if(trigger<=.1f && squeeze<=.1f)wake_gesture_[hand]=false;
    return true;
  }
  const bool library=page_==0 && !ui_.compactInterface && ui_.windowState==cyvr::WindowState::Open;
  if(library && video_manipulation){auto cancel=input;cancel.poseValid=false;video_manipulation(cancel,false);}
  const bool over_files=library && input.pointer.valid && cyvr::Rect{280,304,1124,500}.contains(input.pointer.pixels);
  if(over_files && std::abs(stick.y)>.22f && trigger<.55f){
    const float strength=std::clamp((std::abs(stick.y)-.22f)/.78f,0.f,1.f);
    scroll_library(input.pointer.pixels,-std::copysign(180.f+720.f*strength,stick.y)*dt);
    // Keep the actual hit alive. Consuming here used to clear the host pointer.
    return false;
  }
  if(library)return false;
  const bool ui_hit=!ui_.interfaceHidden && input.pointer.valid &&
    (!ui_.compactInterface || cyvr::Rect{228,660,984,298}.contains(input.pointer.pixels));
  const bool ready=session_.video_state().ready;
  const bool zoom=squeeze>.65f && trigger>.55f && ready && input.poseValid;
  if(video_manipulation && ui_.presentation==cyvr::Presentation::Immersive && ready){
    if(video_manipulation(input,!ui_hit && !ui_.modalOpen && !zoom)){
      placement_changed();return true;
    }
  }
  if(!input.poseValid){zoom_gesture_[hand]=false;return false;}
  // Preserve the explicit grip + trigger zoom gesture. Trigger alone is a grab.
  if(zoom){
    if(!zoom_gesture_[hand]){zoom_start_[hand]=image.zoom;zoom_y_[hand]=input.pose.position.y;}
    image.zoom=std::clamp(zoom_start_[hand]+(input.pose.position.y-zoom_y_[hand])*4.f,minimum_zoom(),4.f);
    placement_changed();
  }else if(!ui_hit && hand==1 && std::abs(stick.y)>.2f)video_input(0,stick.y*dt);
  const bool consume=zoom || zoom_gesture_[hand];zoom_gesture_[hand]=zoom;
  return consume;
}
void PlayerApplication::placement_changed(){
  state_dirty_=true;control_activity();ui_.invalidate();
}
} // namespace cyvr::player
