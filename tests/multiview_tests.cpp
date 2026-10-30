#include "cyvr/player/multiview.hpp"
#include "cyvr/player/clearkey.hpp"
#include <cstring>
#include <iostream>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/stereo3d.h>
}
using namespace cyvr::player;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
FramePtr image(int id,int eye,std::int64_t pts,int depth=8){
    FramePtr frame(av_frame_alloc());if(!frame)throw std::bad_alloc();
    frame->width=16;frame->height=8;frame->format=depth==8?AV_PIX_FMT_YUV420P:AV_PIX_FMT_YUV420P10LE;frame->pts=pts;
    if(av_frame_get_buffer(frame.get(),32)<0)throw std::bad_alloc();
    for(int p=0;p<3;++p)for(int y=0;y<(p?4:8);++y)for(int x=0;x<(p?8:16);++x){auto* row=frame->data[p]+y*frame->linesize[p];if(depth==8)row[x]=std::uint8_t(id+30);else reinterpret_cast<std::uint16_t*>(row)[x]=std::uint16_t((id+30)*4);}
    auto* side=av_frame_new_side_data(frame.get(),AV_FRAME_DATA_VIEW_ID,sizeof(id));std::memcpy(side->data,&id,sizeof(id));
    auto* s=av_stereo3d_create_side_data(frame.get());s->view=eye?AV_STEREO3D_VIEW_RIGHT:AV_STEREO3D_VIEW_LEFT;
    return frame;
}
int main(){try{
    for(int depth:{8,10}){
        StereoFrameAssembler assembler;
        require(!assembler.push(image(6,0,100,depth)),"Unpaired eye escaped");
        require(!assembler.push(image(0,1,200,depth)),"Different timestamps paired");
        auto first=assembler.push(image(0,1,100,depth));
        require(first && first->width==32 && first->height==8 && packed_stereo_frame(*first),"Stereo frame metadata lost");
        require(first->pts==100,"Stereo PTS lost");
        if(depth==8)require(first->data[0][0]==36 && first->data[0][16]==30,"Eye metadata ignored in favor of numeric view IDs");
        else require(reinterpret_cast<std::uint16_t*>(first->data[0])[0]==144 && reinterpret_cast<std::uint16_t*>(first->data[0])[16]==120,"10-bit eye precision lost");
        auto second=assembler.push(image(6,0,200,depth));require(second && assembler.empty(),"Pairing kept stale views");
        assembler.push(image(0,0,300,depth));assembler.clear();require(assembler.empty(),"Seek did not clear pending eyes");
    }
    const std::string valid=R"({"keys":[{"kty":"oct","kid":"AAECAwQFBgcICQoLDA0ODw","k":"EBESExQVFhcYGRobHB0eHw"}],"type":"temporary"})";
    auto keys=parse_clear_keys(valid);require(keys.size()==1 && keys[0].id[15]==15 && keys[0].key[0]==16,"ClearKey base64url conversion failed");
    for(const auto& bad:std::vector<std::string>{"{}",valid+"false",R"({"keys":[]})",R"({"keys":[{"kty":"RSA","kid":"AAECAwQFBgcICQoLDA0ODw","k":"EBESExQVFhcYGRobHB0eHw"}]})",R"({"keys":[{"kty":"oct","kid":"AAECAwQFBgcICQoLDA0ODx","k":"EBESExQVFhcYGRobHB0eHw"}]})",R"({"keys":[],"keys":[]})"}){
        bool rejected=false;try{parse_clear_keys(bad);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"Malformed JWK accepted");
    }
    std::cout<<"Timestamp-matched 8/10-bit stereo pairing, metadata eye order, seek isolation and strict ClearKey JWK parsing passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
