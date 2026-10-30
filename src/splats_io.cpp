#include "cyvr/player/splats.hpp"
#include "cyvr/player/demuxer.hpp"
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <io.h>
#else
#include <poll.h>
#include <unistd.h>
#endif
namespace cyvr::player {
std::vector<std::uint8_t> read_splat_source(const MediaSource& source,const Cancellation& cancel){
  constexpr std::size_t limit=512*1024*1024;
  std::vector<std::uint8_t> bytes;
  std::ifstream file;
  if(source.kind==SourceKind::file){file.open(std::filesystem::u8path(source.location),std::ios::binary);if(!file)throw MediaError("Cannot open Gaussian file");}
  else if(source.kind!=SourceKind::descriptor)throw MediaError("Gaussian loading currently supports local files and Android documents");
  std::array<std::uint8_t,65536> block{};std::int64_t position=0;
  for(;;){
    if(cancel.cancelled())throw MediaError("Gaussian loading cancelled");
    int count=int(block.size());if(source.kind==SourceKind::descriptor && source.length>=0)count=int(std::min<std::int64_t>(count,source.length-position));if(count<=0)break;
    int read=0;
    if(source.kind==SourceKind::file){file.read(reinterpret_cast<char*>(block.data()),count);read=int(file.gcount());if(file.bad())throw MediaError("Cannot read Gaussian file");}
    else{
#ifdef _WIN32
      if(_lseeki64(source.fd,source.offset+position,SEEK_SET)<0)throw MediaError("Cannot seek Gaussian document");read=_read(source.fd,block.data(),unsigned(count));
#else
      if(lseek(source.fd,source.offset+position,SEEK_SET)<0){if(source.offset)throw MediaError("Cannot seek Gaussian document slice");pollfd fd{source.fd,POLLIN,0};while(poll(&fd,1,20)==0)if(cancel.cancelled())throw MediaError("Gaussian loading cancelled");}
      read=int(::read(source.fd,block.data(),count));
#endif
      if(read<0)throw MediaError("Cannot read Gaussian document");
    }
    if(!read)break;if(std::size_t(read)>limit-bytes.size())throw MediaError("Gaussian input exceeds the loading budget");bytes.insert(bytes.end(),block.begin(),block.begin()+read);position+=read;
  }
  return bytes;
}
}
