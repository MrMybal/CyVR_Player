#version 450
layout(set=0,binding=0) uniform sampler2D source;
layout(set=0,binding=1) uniform sampler2D history;
layout(push_constant) uniform Settings {vec4 shape;vec4 control;} settings;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
float edgeLuma(vec2 p){return dot(texture(source,p).rgb,vec3(.2126,.7152,.0722));}
// Ignore encoded letterboxing, without reading the movie back to the CPU.
float bars(int axis,bool farEdge){
  float boundary=0.;
  for(int i=0;i<8;++i){
    float p=(float(i)+.5)/32.;if(farEdge)p=1.-p;
    float l=0.;
    for(int j=0;j<3;++j){float q=.3+float(j)*.2;l=max(l,edgeLuma(axis==0?vec2(p,q):vec2(q,p)));}
    if(l>.004)break;
    boundary=float(i+1)/32.;
  }
  return boundary;
}
void main(){
  vec2 scale=settings.shape.xy;
  float aspect=settings.control.w;
  vec2 p=(uv-.5)*scale+.5;
  vec2 delta=max(max(-p,p-1.),0.);
  float distanceFromScreen=length(vec2(delta.x,delta.y/aspect));
  float spread=(scale.x-1.)*.5;
  float fade=exp(-4.*pow(distanceFromScreen/max(spread,.001),2.));
  // Transparent beneath the screen, with a narrow overlap to avoid seams.
  float outside=max(max(-p.x,p.x-1.),max(-p.y,p.y-1.)/aspect);
  fade*=smoothstep(-.008,0.,outside);
  vec2 lo=vec2(0),hi=vec2(1);
  if(settings.control.z>.5){lo=vec2(bars(0,false),bars(1,false));hi=1.-vec2(bars(0,true),bars(1,true));}
  vec2 inset=settings.control.y*(hi-lo);
  lo+=inset;hi-=inset;
  vec2 edge=mix(lo,hi,clamp(p,0.,1.));
  vec3 rgb=vec3(0);float total=0.;
  for(int i=-3;i<=3;++i)for(int j=-3;j<=3;++j){
    vec2 o=vec2(i,j);float weight=exp(-dot(o,o)/4.);
    rgb+=weight*texture(source,clamp(edge+o*vec2(.022,.022*aspect),lo,hi)).rgb;total+=weight;
  }
  rgb/=total;
  float luma=dot(rgb,vec3(.2126,.7152,.0722));
  rgb=max(mix(vec3(luma),rgb,settings.shape.z),0.);
  float peak=max(max(rgb.r,rgb.g),rgb.b);
  float alpha=clamp(peak*settings.shape.w*fade,0.,.95);
  vec4 glow=vec4(rgb/max(peak,1e-6)*alpha,alpha);
  color=mix(texture(history,uv),glow,settings.control.x);
}
