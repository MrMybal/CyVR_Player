#include "cyvr/player/android/video_format.hpp"
#include <stdexcept>
#include <string>
namespace cyvr::player::android {
MediaFormatPtr make_video_format(const VideoConfiguration& config){
    if(config.width<=0 || config.height<=0 || config.codec_data.empty())
        throw std::invalid_argument("Video configuration is incomplete");
    for(const auto& data:config.codec_data)if(data.empty())throw std::invalid_argument("Video parameter sets are incomplete");
    MediaFormatPtr format(AMediaFormat_new());if(!format) throw std::bad_alloc();
    AMediaFormat_setString(format.get(),AMEDIAFORMAT_KEY_MIME,config.mime.c_str());
    AMediaFormat_setInt32(format.get(),AMEDIAFORMAT_KEY_WIDTH,config.width);
    AMediaFormat_setInt32(format.get(),AMEDIAFORMAT_KEY_HEIGHT,config.height);
    for(std::size_t i=0;i<config.codec_data.size();++i){
        const auto key="csd-"+std::to_string(i);
        AMediaFormat_setBuffer(format.get(),key.c_str(),config.codec_data[i].data(),config.codec_data[i].size());
    }
    return format;
}
}
