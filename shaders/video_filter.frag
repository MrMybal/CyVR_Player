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
float perceptualChannel(ivec2 p,int channel){return pow(max(loadLinear(p)[channel],0.),1./2.2);}
vec4 gatherChannel(vec2 p,int channel){
    ivec2 i=ivec2(floor(p*vec2(eyeSize)-.5));
    return vec4(perceptualChannel(i+ivec2(0,1),channel),
                perceptualChannel(i+ivec2(1,1),channel),
                perceptualChannel(i+ivec2(1,0),channel),
                perceptualChannel(i,channel));
}
// EASU gathers each of its four footprints once per channel, red first. Decode
// each texel once; the outer two footprints only read two of their four texels.
vec3 easuTexels[4]=vec3[4](vec3(0),vec3(0),vec3(0),vec3(0));
int easuFootprint=0;
vec4 easuGather(vec2 p,int channel){
    if(channel==0){
        ivec2 i=ivec2(floor(p*vec2(eyeSize)-.5));
        if(easuFootprint!=3){
            easuTexels[0]=perceptual(loadLinear(i+ivec2(0,1)));
            easuTexels[1]=perceptual(loadLinear(i+ivec2(1,1)));
        }
        if(easuFootprint!=0){
            easuTexels[2]=perceptual(loadLinear(i+ivec2(1,0)));
            easuTexels[3]=perceptual(loadLinear(i));
        }
        ++easuFootprint;
    }
    return vec4(easuTexels[0][channel],easuTexels[1][channel],
                easuTexels[2][channel],easuTexels[3][channel]);
}
AF4 FsrEasuRF(AF2 p){return easuGather(p,0);}
AF4 FsrEasuGF(AF2 p){return easuGather(p,1);}
AF4 FsrEasuBF(AF2 p){return easuGather(p,2);}
AF4 FsrRcasLoadF(ASU2 p){return vec4(perceptual(loadLinear(p)),1);}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}
#include "fsr1/ffx_fsr1.h"
#include "gsr1.glsl"
// A per-eye view gets the full filter near the lens centre and is only resampled
// beyond it. mode.z and mode.w hold the centre and reach in output pixels.
float filterWeight(){
    if(pc.mode.w==0)return 1.;
    vec2 centre=vec2(pc.mode.z&65535,pc.mode.z>>16),reach=vec2(pc.mode.w&65535,pc.mode.w>>16);
    return 1.-smoothstep(.8,1.,length((gl_FragCoord.xy-centre)/reach));
}
void main(){
    ivec2 eyes=ivec2(pc.mode.x==1?2:1,pc.mode.x==2?2:1);
    ivec2 outSize=pc.dimensions.zw/eyes;
    ivec2 eye=min(ivec2(gl_FragCoord.xy)/outSize,eyes-1);
    eyeSize=pc.dimensions.xy/eyes;origin=eye*eyeSize;
    ivec2 outPixel=ivec2(gl_FragCoord.xy)-eye*outSize;
    vec2 local=zoomImage((vec2(outPixel)+.5)/vec2(outSize),pc.view);
    ivec2 pixel=ivec2(local*vec2(eyeSize));
    vec2 p=(vec2(origin)+clamp(local*vec2(eyeSize),vec2(.5),vec2(eyeSize)-.5))/vec2(pc.dimensions.xy);
    vec4 base=texture(source_image,p);
    vec3 c=base.rgb;
    float weight=filterMode==0?0.:filterWeight();
    if(weight>0.){
        if(filterMode==1){CasFilter(c.r,c.g,c.b,uvec2(max(pixel,ivec2(0))),pc.con0,pc.con1,true);}
        else if(filterMode==2){FsrEasuF(c,uvec2(outPixel),pc.con0,pc.con1,pc.con2,pc.con3);c=linearize(c);}
        else if(filterMode==3){FsrRcasF(c.r,c.g,c.b,uvec2(max(pixel,ivec2(0))),pc.con0);c=linearize(c);}
        else if(filterMode==4){c=GsrFilter(local);}
        c=mix(base.rgb,c,weight);
    }
    // EASU feeds RCAS, which applies the image corrections once.
    if(filterMode!=2)c=correctImage(c,pc.controls);
    // Preserve the projected view's coverage; the UI/environment remain separate.
    color=vec4(c,base.a);
}
