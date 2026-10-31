#include "cyvr/player/splats.hpp"
#include "cyvr/player/demuxer.hpp"
#include "splat/formats/SplatCloud.h"
#include "splat/formats/FrameTransform.h"
#include "splat/math/Covariance.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_map>
namespace cyvr::player {
namespace {
constexpr std::size_t max_splats=1000000;
struct Property {std::string name;int size{},kind{};};
Property property(std::string name,const std::string& type){
  if(type=="float" || type=="float32")return {name,4,0};
  if(type=="double" || type=="float64")return {name,8,0};
  if(type=="uchar" || type=="uint8")return {name,1,1};
  if(type=="char" || type=="int8")return {name,1,2};
  if(type=="ushort" || type=="uint16")return {name,2,1};
  if(type=="short" || type=="int16")return {name,2,2};
  if(type=="uint" || type=="uint32")return {name,4,1};
  if(type=="int" || type=="int32")return {name,4,2};
  throw MediaError("Unsupported PLY scalar type");
}
double scalar(const std::uint8_t* data,const Property& p){
  if(!p.kind){if(p.size==4){float v;std::memcpy(&v,data,4);return v;}double v;std::memcpy(&v,data,8);return v;}
  std::uint32_t value=0;for(int n=0;n<p.size;++n)value|=std::uint32_t(data[n])<<(8*n);
  if(p.kind==1)return value;
  if(p.size==1)return static_cast<std::int8_t>(value);
  if(p.size==2)return static_cast<std::int16_t>(value);
  return static_cast<std::int32_t>(value);
}
void append(splat::SplatCloud& cloud,const float* position,const float* scale,const float* quaternion,const float* rgb,float opacity){
  float length=0;for(int n=0;n<4;++n){if(!std::isfinite(quaternion[n]))throw MediaError("Invalid Gaussian rotation");length+=quaternion[n]*quaternion[n];}
  if(length<1e-12f || !std::isfinite(length))throw MediaError("Invalid Gaussian rotation");
  float q[4];for(int n=0;n<4;++n)q[n]=quaternion[n]/std::sqrt(length);
  for(int n=0;n<3;++n){if(!std::isfinite(position[n]) || std::abs(position[n])>1e9f || !std::isfinite(scale[n]) || scale[n]<=0 || scale[n]>1e6f || !std::isfinite(rgb[n]))throw MediaError("Invalid Gaussian attributes");cloud.positions.push_back(position[n]);cloud.colors.push_back(std::clamp(rgb[n],0.f,1.f));}
  if(!std::isfinite(opacity))throw MediaError("Invalid Gaussian opacity");cloud.alphas.push_back(std::clamp(opacity,0.f,1.f));
  const auto covariance=splat::covarianceFromQuaternionScale(q,scale);cloud.covariances.insert(cloud.covariances.end(),covariance.begin(),covariance.end());
}
void reserve(splat::SplatCloud& cloud,std::size_t count){
  if(!count || count>max_splats)throw MediaError("This Gaussian profile supports 1 to 1,000,000 splats per frame");
  cloud.positions.reserve(count*3);cloud.colors.reserve(count*3);cloud.covariances.reserve(count*6);cloud.alphas.reserve(count);
}
}
std::shared_ptr<const splat::SplatCloud> decode_splats(const std::vector<std::uint8_t>& bytes,SplatNormalization& normalization,const Cancellation& cancel){
  auto cloud=std::make_shared<splat::SplatCloud>();
  if(bytes.size()>512*1024*1024 || bytes.empty())throw MediaError("Gaussian input exceeds the loading budget");
  if(bytes.size()>4 && std::memcmp(bytes.data(),"ply",3)==0){
    const std::string prefix(reinterpret_cast<const char*>(bytes.data()),std::min<std::size_t>(65536,bytes.size()));
    const auto end=prefix.find("end_header");if(end==std::string::npos)throw MediaError("Invalid PLY header");
    const auto begin=prefix.find('\n',end);if(begin==std::string::npos)throw MediaError("Truncated PLY header");
    std::istringstream header(prefix.substr(0,begin));header.imbue(std::locale::classic());
    std::string line,element,format;std::size_t count=0, preceding=0;std::vector<Property> properties;
    while(std::getline(header,line)){
      std::istringstream fields(line);std::string key;fields>>key;
      if(key=="format")fields>>format;
      else if(key=="element"){std::size_t n=0;fields>>element>>n;if(!fields)throw MediaError("Invalid PLY element");if(element=="vertex")count=n;else if(!count)preceding+=n;}
      else if(key=="property" && element=="vertex"){std::string type,name;fields>>type>>name;if(type=="list")throw MediaError("Compressed/list PLY attributes are not supported by this profile");properties.push_back(property(name,type));if(properties.size()>256)throw MediaError("PLY attribute budget exceeded");}
    }
    if(preceding || (format!="ascii" && format!="binary_little_endian"))throw MediaError("Use standard ASCII or little-endian Gaussian PLY");
    reserve(*cloud,count);std::unordered_map<std::string,int> indices;std::size_t stride=0;
    for(std::size_t n=0;n<properties.size();++n){if(!indices.emplace(properties[n].name,int(n)).second)throw MediaError("Duplicate PLY property");stride+=properties[n].size;}
    const auto field=[&](const std::string& name){auto it=indices.find(name);if(it==indices.end())throw MediaError("This PLY does not contain Gaussian attributes: "+name);return it->second;};
    const int xyz[]={field("x"),field("y"),field("z")},scales[]={field("scale_0"),field("scale_1"),field("scale_2")},rot[]={field("rot_1"),field("rot_2"),field("rot_3"),field("rot_0")},colors[]={field("f_dc_0"),field("f_dc_1"),field("f_dc_2")},alpha=field("opacity");
    int rests=0;while(indices.count("f_rest_"+std::to_string(rests)))++rests;
    if(rests!=0 && rests!=9 && rests!=24 && rests!=45)throw MediaError("Incomplete Gaussian spherical harmonics");
    cloud->shDegree=rests==45?3:rests==24?2:rests==9?1:0;cloud->sh.reserve(count*rests);
    std::vector<int> harmonics(std::size_t(rests),0);for(int n=0;n<rests;++n)harmonics[std::size_t(n)]=field("f_rest_"+std::to_string(n));
    std::size_t cursor=begin+1;if(format!="ascii" && (stride>bytes.size() || count>(bytes.size()-cursor)/stride))throw MediaError("Truncated Gaussian PLY");
    std::istringstream ascii(format=="ascii"?std::string(reinterpret_cast<const char*>(bytes.data()+cursor),bytes.size()-cursor):std::string{});ascii.imbue(std::locale::classic());
    std::vector<double> values(properties.size());
    for(std::size_t n=0;n<count;++n){
      if((n&4095)==0 && cancel.cancelled())throw MediaError("Gaussian loading cancelled");
      for(std::size_t p=0;p<properties.size();++p){if(format=="ascii"){if(!(ascii>>values[p]))throw MediaError("Truncated ASCII Gaussian PLY");}else{values[p]=scalar(bytes.data()+cursor,properties[p]);cursor+=properties[p].size;}}
      float p[3],s[3],q[4],rgb[3];for(int c=0;c<3;++c){p[c]=float(values[xyz[c]]);const auto log=values[scales[c]];if(!std::isfinite(log) || log<-30 || log>13)throw MediaError("Gaussian scale exceeds the supported range");s[c]=float(std::exp(log));rgb[c]=float(.5+.28209479177387814*values[colors[c]]);}for(int c=0;c<4;++c)q[c]=float(values[rot[c]]);
      const auto opacity=values[alpha];if(!std::isfinite(opacity))throw MediaError("Invalid Gaussian opacity");append(*cloud,p,s,q,rgb,float(1/(1+std::exp(-std::clamp(opacity,-80.,80.)))));
      for(int coefficient=0;coefficient<rests/3;++coefficient)for(int c=0;c<3;++c){const auto value=values[harmonics[std::size_t(c*(rests/3)+coefficient)]];if(!std::isfinite(value)||std::abs(value)>65504)throw MediaError("Invalid Gaussian harmonics");cloud->sh.push_back(float(value));}
    }
  }else{
    if(bytes.size()%32)throw MediaError("Use Gaussian PLY or the standard 32-byte .splat format");reserve(*cloud,bytes.size()/32);
    for(std::size_t offset=0;offset<bytes.size();offset+=32){if((offset&131071)==0 && cancel.cancelled())throw MediaError("Gaussian loading cancelled");const auto* b=bytes.data()+offset;float p[3],s[3];std::memcpy(p,b,12);std::memcpy(s,b+12,12);float q[4]={(float(b[29])-128)/128,(float(b[30])-128)/128,(float(b[31])-128)/128,(float(b[28])-128)/128};float rgb[3]={b[24]/255.f,b[25]/255.f,b[26]/255.f};append(*cloud,p,s,q,rgb,b[27]/255.f);}
  }
  splat::convertFrame(*cloud,splat::CoordinateFrame::kRdf,splat::CoordinateFrame::kRub);
  for(int c=0;c<3;++c){cloud->bounds.min[c]=std::numeric_limits<float>::max();cloud->bounds.max[c]=-cloud->bounds.min[c];}
  for(std::size_t n=0;n<cloud->count();++n)for(int c=0;c<3;++c){cloud->bounds.min[c]=std::min(cloud->bounds.min[c],cloud->positions[n*3+c]);cloud->bounds.max[c]=std::max(cloud->bounds.max[c],cloud->positions[n*3+c]);}
  if(!normalization.set){float span=.01f;for(int c=0;c<3;++c){normalization.center[c]=(cloud->bounds.min[c]+cloud->bounds.max[c])*.5f;span=std::max(span,cloud->bounds.max[c]-cloud->bounds.min[c]);}normalization.scale=2/span;normalization.set=true;}
  for(std::size_t n=0;n<cloud->count();++n){for(int c=0;c<3;++c)cloud->positions[n*3+c]=(cloud->positions[n*3+c]-normalization.center[c])*normalization.scale;for(int c=0;c<6;++c)cloud->covariances[n*6+c]*=normalization.scale*normalization.scale;}
  for(int c=0;c<3;++c){cloud->bounds.min[c]=(cloud->bounds.min[c]-normalization.center[c])*normalization.scale;cloud->bounds.max[c]=(cloud->bounds.max[c]-normalization.center[c])*normalization.scale;}
  return cloud;
}
SplatSequence splat_sequence(const std::string& location,const std::vector<std::uint8_t>& manifest){
  if(manifest.size()>1024*1024 || location.find("://")!=std::string::npos)throw MediaError("Splat sequences currently require a local manifest and relative frame paths");
  std::istringstream text(std::string(manifest.begin(),manifest.end()));text.imbue(std::locale::classic());std::string line;std::getline(text,line);if(!line.empty() && line.back()=='\r')line.pop_back();if(line!="CYVR_SPLAT_SEQUENCE 1")throw MediaError("Invalid splat sequence header");
  SplatSequence sequence;const auto parent=std::filesystem::weakly_canonical(std::filesystem::u8path(location)).parent_path();
  while(std::getline(text,line)){
    if(!line.empty() && line.back()=='\r')line.pop_back();if(line.empty() || line[0]=='#')continue;
    if(line.rfind("fps ",0)==0){std::istringstream value(line.substr(4));value.imbue(std::locale::classic());value>>sequence.fps;if(!value || !std::isfinite(sequence.fps) || sequence.fps<1 || sequence.fps>120)throw MediaError("Invalid splat sequence frame rate");continue;}
    const auto relative=std::filesystem::u8path(line);if(relative.is_absolute() || relative.has_root_name())throw MediaError("Splat frame must use a relative path");const auto path=std::filesystem::weakly_canonical(parent/relative);for(const auto& part:path.lexically_relative(parent))if(part=="..")throw MediaError("Splat frame is outside its sequence folder");
    sequence.frames.push_back(path.u8string());if(sequence.frames.size()>100000)throw MediaError("Splat sequence frame budget exceeded");
  }
  if(sequence.frames.empty())throw MediaError("Splat sequence is empty");return sequence;
}
}
