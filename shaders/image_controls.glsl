vec3 correctImage(vec3 c,vec4 settings) {
    c=max(c*exp2(settings.x),vec3(0));
    c=(c-.18)*settings.y+.18;
    c=mix(vec3(dot(c,vec3(.2126,.7152,.0722))),c,settings.z);
    return pow(clamp(c,0.,1.),vec3(1./settings.w));
}
vec2 zoomImage(vec2 uv,vec4 view) {
    float span=1./view.x;
    return (uv-.5)*span+.5+view.yz*(1.-span)*.5;
}
