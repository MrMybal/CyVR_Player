#version 450
#extension GL_GOOGLE_include_directive : require
#include "image_controls.glsl"
layout(set=0,binding=0) uniform sampler2D video;
layout(push_constant) uniform Crop {
    vec4 uvRect; vec4 controls; vec4 view; ivec4 format;
    vec4 fov; vec4 rotation; vec4 origin; vec4 screen;
} crop;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
vec3 rotateRay(vec4 q,vec3 v){return v+2.*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
// ITU-R BT.2100 PQ/HLG to display-linear Rec.709, before image corrections.
vec3 videoLinear(vec3 e) {
    int transfer=crop.format.w&255;
    bool hdr=transfer==6||transfer==7;
    vec3 c;
    if(transfer==6){
        vec3 p=pow(clamp(e,0.,1.),vec3(32./2523.));
        c=10000.*pow(max(p-3424./4096.,0.)/max(2413./128.-2392./128.*p,vec3(1e-6)),vec3(16384./2610.));
    }else if(transfer==7){
        c=mix(e*e/3.,(exp((e-.55991073)/.17883277)+.28466892)/12.,greaterThan(e,vec3(.5)));
        c*=1000.*pow(max(dot(c,vec3(.2627,.678,.0593)),0.),.2);
    }else if(transfer==1)c=e;
    else c=mix(e/4.5,pow((e+.099)/1.099,vec3(1./.45)),greaterThanEqual(e,vec3(.081)));
    if((crop.format.w>>8)==6)c=mat3(1.660491,-.124550,-.018151,-.587641,1.132900,-.100579,-.072850,-.008349,1.118730)*c;
    c=max(c,vec3(0));
    if(hdr){
        float luminance=dot(c,vec3(.2126,.7152,.0722));
        float x=luminance/203.;
        float white=max(crop.screen.z,203.)/203.;
        float mapped=x*(1.+x/(white*white))/(1.+x);
        c*=mapped/max(luminance,1e-6);
    }
    return c;
}
void main(){
    vec2 eyes=vec2(crop.format.x==1?2:1,crop.format.x==2?2:1);
    vec2 eye,local;
    float coverage=1.;
    int kind=crop.format.y;
    if(kind==0){eye=min(floor(uv*eyes),eyes-1.);local=uv*eyes-eye;}
    else {
        eye=vec2(crop.format.x==1?crop.format.z:0,crop.format.x==2?crop.format.z:0);
        local=uv;
        if(kind>=2){
            vec3 ray=rotateRay(crop.rotation,normalize(vec3(mix(crop.fov.x,crop.fov.y,uv.x),mix(crop.fov.z,crop.fov.w,uv.y),-1)));
            if(kind==2){
                if(ray.z>=-.00001){color=vec4(0);return;}
                float t=-crop.origin.z/ray.z;
                vec3 p=crop.origin.xyz+ray*t;
                local=vec2(p.x/crop.screen.x+.5,.5-p.y/crop.screen.y);
                if(t<=0){color=vec4(0);return;}
                if(any(lessThan(local,vec2(0)))||any(greaterThan(local,vec2(1))))coverage=0.;
            } else {
                float longitude=atan(ray.x,-ray.z);
                if(kind==3&&abs(longitude)>1.570796327)coverage=0.;
                local=vec2(longitude/(kind==3?3.141592654:6.283185307)+.5,.5-asin(clamp(ray.y,-1.,1.))/3.141592654);
            }
        }
    }
    local=zoomImage(local,crop.view);
    if(kind==4)local.x=fract(local.x);
    vec2 halfPixel=.5*eyes/(vec2(textureSize(video,0))*crop.uvRect.zw);
    local=clamp(local,halfPixel,1.-halfPixel);
    vec3 encoded=max(texture(video,crop.uvRect.xy+(eye+local)/eyes*crop.uvRect.zw).rgb,vec3(0));
    vec3 linear=videoLinear(encoded);
    // Keep edge RGB beneath transparent coverage. Spatial reconstruction taps
    // must not treat the cinema/VR180 boundary as a black part of the video.
    color=vec4(correctImage(linear,crop.controls),coverage);
}
