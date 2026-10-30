#include "cyvr/player/splat_renderer.hpp"
#include "cyvr/player/application.hpp"
#include "splat/formats/SplatCloud.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>
#include <numeric>
using namespace cyvr::player;
int main(int argc,char** argv){try{
  if(argc!=2 && argc!=4)throw std::runtime_error("splat_probe ASSETS [COUNT FRAMES]");
  std::vector<std::uint8_t> bytes(96);
  for(int n=0;n<3;++n){float p[3]={n==0?-1.f:n==1?1.f:0.f,0,n==2?-.1f:0};float scale[3]={.12f,.06f,.04f};auto* b=bytes.data()+n*32;std::memcpy(b,p,12);std::memcpy(b+12,scale,12);b[24+n]=255;b[27]=255;b[28]=255;b[29]=b[30]=b[31]=128;}
  Cancellation cancel;SplatNormalization normalization;auto cloud=decode_splats(bytes,normalization,cancel);
  if(cloud->count()!=3 || cloud->shDegree!=0 || !normalization.set)throw std::runtime_error("SPLAT decoding failed");
  const std::string ply="ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nproperty float scale_0\nproperty float scale_1\nproperty float scale_2\nproperty float rot_0\nproperty float rot_1\nproperty float rot_2\nproperty float rot_3\nproperty float opacity\nproperty float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nend_header\n0 0 0 -2 -2 -2 1 0 0 0 2 0 0 0\n";
  SplatNormalization pnorm;if(decode_splats({ply.begin(),ply.end()},pnorm,cancel)->count()!=1)throw std::runtime_error("Gaussian PLY decoding failed");
  auto broken=bytes;broken.pop_back();bool rejected=false;try{decode_splats(broken,pnorm,cancel);}catch(...){rejected=true;}if(!rejected)throw std::runtime_error("Truncated SPLAT accepted");
  cyvr::VulkanContext gpu;gpu.createStandalone();auto reader=[&](const std::string& name){std::ifstream f(std::filesystem::u8path(argv[1])/name,std::ios::binary);return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),{});};
  cyvr::VulkanRenderer targets(gpu);targets.initialize(VK_FORMAT_R8G8B8A8_SRGB,reader);auto target=targets.createTarget(320,180);
  SplatRenderer renderer(gpu,VK_FORMAT_R8G8B8A8_SRGB,reader);const SplatSnapshot frame{cloud,1,0};ImageSettings settings;
  std::vector<std::uint8_t> native;
  for(int quality=0;quality<4;++quality){settings.quality=quality;renderer.draw(target.image,{320,180},frame,{{0,0,3.5f},{}},{-1,1,.5625f,-.5625f},settings,false);auto pixels=targets.readback(target);unsigned colored=0;for(std::size_t n=0;n<pixels.size();n+=4)if(pixels[n]>32 || pixels[n+1]>32 || pixels[n+2]>32)++colored;if(colored<20 || renderer.drawn()!=3)throw std::runtime_error("Gaussian Vulkan rendering or GPU sorting failed");if(!quality)native=std::move(pixels);}
  settings.quality=0;renderer.draw(target.image,{320,180},frame,{{.06f,0,3.5f},{}},{-1,1,.5625f,-.5625f},settings,false);if(native==targets.readback(target))throw std::runtime_error("Gaussian scene did not react to stereo camera translation");
  targets.destroyTarget(target);std::cout<<"PLY/SPLAT parsing, malformed input rejection, GPU culling/radix sorting, stereo view translation and Native/CAS/FSR/GSR passed\n";
  if(argc==4){
    const auto count=std::stoul(argv[2]),frames=std::stoul(argv[3]);
    if(count<100 || count>1000000 || frames<8 || frames>1000)throw std::runtime_error("Benchmark exceeds its scene/frame budget");
    std::vector<std::uint8_t> dense(count*32);std::uint32_t random=1729;
    auto uniform=[&]{random=1664525u*random+1013904223u;return float(random>>8)/16777216.f;};
    for(std::size_t n=0;n<count;++n){auto* b=dense.data()+n*32;float p[3]={uniform()*2-1,uniform()*2-1,uniform()*2-1};float scale[3]={.003f+uniform()*.002f,.003f+uniform()*.002f,.003f+uniform()*.002f};std::memcpy(b,p,12);std::memcpy(b+12,scale,12);b[24]=std::uint8_t(uniform()*255);b[25]=std::uint8_t(uniform()*255);b[26]=std::uint8_t(uniform()*255);b[27]=180;b[28]=255;b[29]=b[30]=b[31]=128;}
    SplatNormalization norm;auto scene=decode_splats(dense,norm,cancel);SplatSnapshot sample{scene,2,0};
    auto output=targets.createTarget(1920,1920);
    for(int quality=0;quality<4;++quality){
      settings.quality=quality;std::vector<double> timings;
      for(std::size_t n=0;n<frames+4;++n){
        const auto start=std::chrono::steady_clock::now();
        for(int eye=0;eye<2;++eye)renderer.draw(output.image,{1920,1920},sample,{{(eye?1.f:-1.f)*.032f+float(n)*.0001f,0,3.5f},{}},{-1,1,1,-1},settings,false);
        if(n>=4)timings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
      }
      std::sort(timings.begin(),timings.end());const double mean=std::accumulate(timings.begin(),timings.end(),0.)/timings.size();
      std::cout<<"{\"splats\":"<<count<<",\"width_per_eye\":1920,\"height_per_eye\":1920,\"stereo_frames\":"<<frames<<",\"quality\":"<<quality<<",\"cpu_submit_wait_mean_ms\":"<<mean<<",\"cpu_submit_wait_p95_ms\":"<<timings[std::min(timings.size()-1,timings.size()*95/100)]<<"}\n"<<std::flush;
    }
    targets.destroyTarget(output);return 0;
  }
  const auto folder=std::filesystem::temp_directory_path()/("cyvr-splat-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(folder);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}}cleanup{folder};
  for(int n=0;n<2;++n){auto content=bytes;if(n)for(std::size_t i=24;i<content.size();i+=32){content[i]=0;content[i+1]=255;content[i+2]=0;}std::ofstream(folder/(std::to_string(n)+".splat"),std::ios::binary).write(reinterpret_cast<const char*>(content.data()),content.size());}
  std::ofstream(folder/"clip.splatseq")<<"CYVR_SPLAT_SEQUENCE 1\nfps 1\n0.splat\n1.splat\n";
  cyvr::UiModel ui;Platform platform;platform.root=folder.u8string();PlayerApplication app(ui,platform);app.open((folder/"clip.splatseq").u8string());
  auto wait=[&](auto test){for(int i=0;i<600;++i){app.tick();if(test())return;std::this_thread::sleep_for(std::chrono::milliseconds(5));}throw std::runtime_error("Splat sequence session timed out");};
  wait([&]{return app.session().state().ready && bool(app.session().splats().cloud);});
  if(!app.session().state().volumetric || app.session().state().duration_us!=2000000)throw std::runtime_error("Splat sequence metadata incorrect");
  const auto first=app.session().splats();app.session().frame_submitted(first.epoch,first.pts_us);app.session().pause(true);app.session().seek(1200000);
  wait([&]{const auto s=app.session().state();return !s.seeking && s.position_us==1000000 && !s.playing;});
  auto next=app.session().splats();if(next.epoch==first.epoch || next.cloud==first.cloud)throw std::runtime_error("Sequence seek retained its previous generation");
  app.session().frame_submitted(next.epoch,next.pts_us);app.session().pause(false);std::this_thread::sleep_for(std::chrono::milliseconds(20));app.session().pause(true);
  ui.presentation=cyvr::Presentation::Immersive;app.tick();cyvr::DrawList controls;ui.drawMain(controls);
  if(!ui.compactInterface)throw std::runtime_error("Immersive playback did not choose compact controls");
  for(int id:{1025,1020,1362,1363,1360})if(std::none_of(ui.widgets().begin(),ui.widgets().end(),[&](const auto& w){return w.id==id;}))throw std::runtime_error("Compact playback control missing");
  app.action(1360);ui.drawMain(controls);if(ui.compactInterface)throw std::runtime_error("Cannot open full playback interface");
  app.action(1361);app.action(1024);if(!ui.interfaceHidden)throw std::runtime_error("Cannot hide compact interface");
  if(!ui.applicationController(0,{},.7f,0,{{0,0,0},{}},true) || ui.interfaceHidden || !ui.compactInterface)throw std::runtime_error("Controller wake did not restore compact controls exclusively");
  app.session().stop();wait([&]{return !app.session().state().ready;});
  std::cout<<"Splat sequence playback, paused seek, generation changes, compact/full controls and consumed wake gesture passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
