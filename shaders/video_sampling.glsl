// Original spatial source filters. All coordinates clamp inside the cropped eye,
// including every reconstruction/noise-reduction tap. No temporal history.
vec3 videoTap(vec2 local,vec2 eye,vec2 eyes,vec2 size){
    local=clamp(local,.5/size,1.-.5/size);
    return texture(video,crop.uvRect.xy+(eye+local)/eyes*crop.uvRect.zw).rgb;
}
vec4 cubicWeights(float t){
    float t2=t*t,t3=t2*t;
    return vec4(-.5*t+t2-.5*t3,1.-2.5*t2+1.5*t3,
                .5*t+2.*t2-1.5*t3,-.5*t2+.5*t3);
}
vec3 sampleVideo(vec2 local,vec2 eye,vec2 eyes){
    vec2 size=vec2(textureSize(video,0))*crop.uvRect.zw/eyes;
    int method=(crop.format.w>>16)&255;
    vec2 position=local*size-.5,base=floor(position),f=fract(position);
    vec3 c;
    if(method==0)c=videoTap(local,eye,eyes,size);
    else if(method==1){
        vec3 a=videoTap((base+.5)/size,eye,eyes,size);
        vec3 b=videoTap((base+vec2(1.5,.5))/size,eye,eyes,size);
        vec3 d=videoTap((base+vec2(.5,1.5))/size,eye,eyes,size);
        vec3 e=videoTap((base+1.5)/size,eye,eyes,size);
        c=mix(mix(a,b,f.x),mix(d,e,f.x),f.y);
    }else{
        vec4 wx=cubicWeights(f.x),wy=cubicWeights(f.y);
        vec3 lo=vec3(1),hi=vec3(0);c=vec3(0);
        for(int y=0;y<4;++y)for(int x=0;x<4;++x){
            vec3 tap=videoTap((base+vec2(x-1,y-1)+.5)/size,eye,eyes,size);
            c+=tap*wx[x]*wy[y];lo=min(lo,tap);hi=max(hi,tap);
        }
        c=clamp(c,lo,hi); // Bound cubic ringing at high-contrast edges.
    }
    float noise=crop.origin.w,detail=crop.view.w;
    if(noise>0. || detail>0.){
        vec3 blur=vec3(0),smoothColor=vec3(0),lo=c,hi=c;
        float weightSum=0.;
        float sigma=mix(.015,.12,noise);
        for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
            vec3 tap=videoTap(local+vec2(x,y)/size,eye,eyes,size);
            float spatial=float((x==0?2:1)*(y==0?2:1));
            vec3 delta=tap-c;
            float range=1.+dot(delta,delta)/(sigma*sigma);
            float weight=spatial/(range*range);
            smoothColor+=weight*tap;weightSum+=weight;
            blur+=spatial*tap;lo=min(lo,tap);hi=max(hi,tap);
        }
        vec3 residual=c-blur/16.;
        float edge=max(abs(residual.r),max(abs(residual.g),abs(residual.b)));
        c=mix(c,smoothColor/weightSum,noise);
        // Suppress the small residuals targeted by noise reduction rather than
        // amplifying them again. Allow at most 2% encoded overshoot for detail.
        c=clamp(c+.75*detail*smoothstep(.015,.06,edge)*residual,
                max(lo-.02*detail,vec3(0)),min(hi+.02*detail,vec3(1)));
    }
    return c;
}
