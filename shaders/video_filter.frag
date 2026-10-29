#version 450
#extension GL_GOOGLE_include_directive : require
#define A_GPU 1
#define A_GLSL 1
#include "fsr1/ffx_a.h"
#include "image_controls.glsl"
layout(set=0,binding=0) uniform sampler2D source_image;
layout(push_constant) uniform Settings {
    vec4 controls; vec4 view; ivec4 dimensions; ivec4 mode;
    uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3;
} pc;
layout(constant_id=0) const int filterMode=0;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
ivec2 origin,eyeSize;
vec3 loadLinear(ivec2 p){return texelFetch(source_image,origin+clamp(p,ivec2(0),eyeSize-1),0).rgb;}
vec3 perceptual(vec3 c){return pow(max(c,vec3(0)),vec3(1./2.2));}
vec3 linearize(vec3 c){return pow(max(c,vec3(0)),vec3(2.2));}
AF3 CasLoad(ASU2 p){return loadLinear(p);}
void CasInput(inout AF1 r,inout AF1 g,inout AF1 b){}
#include "cas/ffx_cas.h"
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
vec4 gatherChannel(vec2 p,int channel){
    ivec2 i=ivec2(floor(p*vec2(eyeSize)-.5));
    return vec4(perceptual(loadLinear(i+ivec2(0,1)))[channel],
                perceptual(loadLinear(i+ivec2(1,1)))[channel],
                perceptual(loadLinear(i+ivec2(1,0)))[channel],
                perceptual(loadLinear(i))[channel]);
}
AF4 FsrEasuRF(AF2 p){return gatherChannel(p,0);}
AF4 FsrEasuGF(AF2 p){return gatherChannel(p,1);}
AF4 FsrEasuBF(AF2 p){return gatherChannel(p,2);}
AF4 FsrRcasLoadF(ASU2 p){return vec4(perceptual(loadLinear(p)),1);}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}
#include "fsr1/ffx_fsr1.h"
#include "gsr1.glsl"
void main(){
    ivec2 eyes=ivec2(pc.mode.x==1?2:1,pc.mode.x==2?2:1);
    ivec2 outSize=pc.dimensions.zw/eyes;
    ivec2 eye=min(ivec2(gl_FragCoord.xy)/outSize,eyes-1);
    eyeSize=pc.dimensions.xy/eyes;origin=eye*eyeSize;
    ivec2 outPixel=ivec2(gl_FragCoord.xy)-eye*outSize;
    vec2 local=zoomImage((vec2(outPixel)+.5)/vec2(outSize),pc.view);
    ivec2 pixel=ivec2(local*vec2(eyeSize));
    vec3 c;
    if(filterMode==1){CasFilter(c.r,c.g,c.b,uvec2(max(pixel,ivec2(0))),pc.con0,pc.con1,true);}
    else if(filterMode==2){FsrEasuF(c,uvec2(outPixel),pc.con0,pc.con1,pc.con2,pc.con3);c=linearize(c);}
    else if(filterMode==3){FsrRcasF(c.r,c.g,c.b,uvec2(max(pixel,ivec2(0))),pc.con0);c=linearize(c);}
    else if(filterMode==4){c=GsrFilter(local);}
    else {
        vec2 p=(vec2(origin)+clamp(local*vec2(eyeSize),vec2(.5),vec2(eyeSize)-.5))/vec2(pc.dimensions.xy);
        c=texture(source_image,p).rgb;
    }
    // Preserve the projected view's coverage; the UI/environment remain separate.
    vec2 alphaUv=(vec2(origin)+clamp(local*vec2(eyeSize),vec2(.5),vec2(eyeSize)-.5))/vec2(pc.dimensions.xy);
    color=vec4(correctImage(c,pc.controls),texture(source_image,alphaUv).a);
}
