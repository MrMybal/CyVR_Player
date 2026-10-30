#include "cyvr/player/subtitle_rasterizer.hpp"
#include <ass/ass.h>
#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <filesystem>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace cyvr::player {
struct SubtitleRasterizer::Impl {
    ASS_Library* library{};ASS_Renderer* renderer{};ASS_Track* track{};
    std::shared_ptr<const SubtitleConfig> config;
    std::vector<std::shared_ptr<const SubtitleCue>> cues;
    std::vector<std::uint8_t> default_font;
    int width{},height{};
    static void quiet(int,const char*,va_list,void*){}
    Impl(std::vector<std::uint8_t> font):default_font(std::move(font)){
        library=ass_library_init();if(!library)throw MediaError("Cannot initialize subtitle rendering");
        ass_set_message_cb(library,quiet,nullptr);
        // Fonts are local system resources; attachments are added without writing files.
#ifdef __ANDROID__
        ass_set_fonts_dir(library,"/system/fonts");
#endif
        renderer=ass_renderer_init(library);
        if(!renderer){ass_library_done(library);library=nullptr;throw MediaError("Cannot initialize subtitle rasterizer");}
        ass_set_cache_limits(renderer,1000,32);
    }
    ~Impl(){if(track)ass_free_track(track);if(renderer)ass_renderer_done(renderer);if(library)ass_library_done(library);}
    void reset(const SubtitleSnapshot& s){
        if(config!=s.config){
            ass_clear_fonts(library);
            if(!default_font.empty())ass_add_font(library,"NotoSans-Regular.ttf",reinterpret_cast<const char*>(default_font.data()),int(default_font.size()));
            if(s.config)for(const auto& font:s.config->fonts)ass_add_font(library,font.name.c_str(),reinterpret_cast<const char*>(font.data.data()),int(font.data.size()));
            ass_set_fonts(renderer,nullptr,"Noto Sans",ASS_FONTPROVIDER_NONE,nullptr,1);
        }
        if(track)ass_free_track(track);track=ass_new_track(library);
        if(!track)throw MediaError("Cannot allocate subtitle styles");
        std::string header=s.config?s.config->header:std::string{};
        if(header.empty())header="[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Noto Sans,54,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,48,48,48,1\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        ass_process_codec_private(track,header.data(),int(header.size()));
        unsigned order=0;
        for(const auto& cue:s.cues)for(const auto& event:cue->ass){
            // Normalize read-order across overlapping events while preserving all styles.
            const auto comma=event.find(',');std::string chunk=std::to_string(order++)+(comma==std::string::npos?",0,Default,,0,0,0,,"+event:event.substr(comma));
            ass_process_chunk(track,chunk.data(),int(chunk.size()),cue->start_us/1000,(cue->end_us-cue->start_us)/1000);
        }
        config=s.config;cues=s.cues;
    }
};
SubtitleRasterizer::SubtitleRasterizer(std::vector<std::uint8_t> font):impl_(std::make_unique<Impl>(std::move(font))){}
SubtitleRasterizer::~SubtitleRasterizer()=default;
std::optional<SubtitleImage> SubtitleRasterizer::render(const SubtitleSnapshot& s,int width,int height){
    if(width<1 || height<1 || width>4096 || height>4096)throw MediaError("Invalid subtitle canvas");
    auto& p=*impl_;const bool changed=p.config!=s.config || p.cues!=s.cues || p.width!=width || p.height!=height;
    if(changed)p.reset(s);
    p.width=width;p.height=height;ass_set_frame_size(p.renderer,width,height);ass_set_storage_size(p.renderer,width,height);
    // libass reports "changed" unconditionally for a track without ASS events.
    // Bitmap-only subtitles must keep their cached image between display sets.
    int dirty{};auto* images=p.track->n_events?ass_render_frame(p.renderer,p.track,s.position_us/1000,&dirty):nullptr;
    if(!changed && !dirty)return {};
    struct Layer {int x,y,w,h,stride;const std::uint8_t* pixels;std::uint32_t color;bool mask;};
    std::vector<Layer> layers;
    for(auto* i=images;i;i=i->next)if(i->w>0 && i->h>0)layers.push_back({i->dst_x,i->dst_y,i->w,i->h,i->stride,i->bitmap,i->color,true});
    // Bitmap coordinates use the decoder's original canvas, independent of SBS/VR cropping.
    struct Scaled {SubtitleBitmap bitmap;};std::vector<Scaled> scaled;
    for(const auto& cue:s.cues)for(const auto& b:cue->bitmaps){
        const int cw=cue->canvas_width>0?cue->canvas_width:width,ch=cue->canvas_height>0?cue->canvas_height:height;
        const int w=std::max(1,int(std::lround(double(b.width)*width/cw))),h=std::max(1,int(std::lround(double(b.height)*height/ch)));
        if(std::uint64_t(w)*h>16*1024*1024)throw MediaError("Subtitle image exceeds the raster budget");
        SubtitleBitmap out{int(std::lround(double(b.x)*width/cw)),int(std::lround(double(b.y)*height/ch)),w,h,{}};out.rgba.resize(std::size_t(w)*h*4);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto* src=b.rgba.data()+(std::size_t(std::min(b.height-1,y*b.height/h))*b.width+std::min(b.width-1,x*b.width/w))*4;std::copy(src,src+4,out.rgba.data()+(std::size_t(y)*w+x)*4);}
        scaled.push_back({std::move(out)});
    }
    for(const auto& value:scaled){const auto& b=value.bitmap;layers.push_back({b.x,b.y,b.width,b.height,b.width*4,b.rgba.data(),0,false});}
    int x0=width,y0=height,x1=0,y1=0;
    for(const auto& l:layers){x0=std::min(x0,std::clamp(l.x,0,width));y0=std::min(y0,std::clamp(l.y,0,height));x1=std::max(x1,std::clamp(l.x+l.w,0,width));y1=std::max(y1,std::clamp(l.y+l.h,0,height));}
    SubtitleImage out{x0,y0,std::max(0,x1-x0),std::max(0,y1-y0),width,height,{}};
    if(!out.width || !out.height)return out;
    out.rgba.resize(std::size_t(out.width)*out.height*4);
    for(const auto& l:layers)for(int y=std::max(y0,l.y);y<std::min(y1,l.y+l.h);++y)for(int x=std::max(x0,l.x);x<std::min(x1,l.x+l.w);++x){
        const auto* src=l.pixels+std::size_t(y-l.y)*l.stride+(x-l.x)*(l.mask?1:4);
        const unsigned a=l.mask?unsigned(*src)*(255-(l.color&255))/255:src[3];
        const unsigned rgb[]={l.mask?(l.color>>24):src[0],l.mask?((l.color>>16)&255):src[1],l.mask?((l.color>>8)&255):src[2]};
        auto* dst=out.rgba.data()+(std::size_t(y-y0)*out.width+x-x0)*4;
        for(int c=0;c<3;++c)dst[c]=std::uint8_t((rgb[c]*a+unsigned(dst[c])*(255-a)+127)/255);
        dst[3]=std::uint8_t(a+(unsigned(dst[3])*(255-a)+127)/255);
    }
    for(std::size_t n=0;n<out.rgba.size();n+=4)if(out.rgba[n+3])for(int c=0;c<3;++c)out.rgba[n+c]=std::uint8_t(std::min(255u,(unsigned(out.rgba[n+c])*255+out.rgba[n+3]/2)/out.rgba[n+3]));
    return out;
}
}
