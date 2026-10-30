#include "cyvr/player/audio_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <stdexcept>
namespace cyvr::player {
struct AudioQueue::Impl {
    static constexpr unsigned slots=128,history=2048;
    struct Block {std::int64_t start{};unsigned count{};std::array<float,block_frames*2> samples{};};
    struct Segment {std::atomic<std::uint64_t> stamp{};std::atomic<std::int64_t> device{},media{},length{};};
    std::array<Block,slots> blocks;
    std::array<Segment,history> clock;
    std::atomic<std::uint64_t> read{},write{},published{},rendered{},underrun{},gap{},expired{};
    std::atomic<std::int64_t> media_cursor{},device_cursor{},queued_end{};
    std::atomic<bool> finished{};
    std::uint64_t sequence{}; // callback only
    void segment(std::int64_t device,std::int64_t media,unsigned count,bool advancing)noexcept{
        auto& s=clock[sequence%history];s.stamp.store(sequence*2+1,std::memory_order_release);
        s.device.store(device,std::memory_order_relaxed);s.media.store(media,std::memory_order_relaxed);
        s.length.store(advancing?std::int64_t(count):-std::int64_t(count),std::memory_order_relaxed);
        s.stamp.store(sequence*2+2,std::memory_order_release);published.store(++sequence,std::memory_order_release);
    }
};
AudioQueue::AudioQueue():impl_(std::make_unique<Impl>()){}
AudioQueue::~AudioQueue()=default;
bool AudioQueue::push(const float* samples,unsigned count,std::int64_t start){
    if(!samples || !count || count>block_frames)throw std::invalid_argument("Invalid stereo audio block");
    auto& p=*impl_;const auto index=p.write.load(std::memory_order_relaxed);
    if(index-p.read.load(std::memory_order_acquire)>=Impl::slots)return false;
    auto& block=p.blocks[index%Impl::slots];block.start=start;block.count=count;
    std::memcpy(block.samples.data(),samples,std::size_t(count)*2*sizeof(float));
    p.queued_end.store(start+count,std::memory_order_release);p.write.store(index+1,std::memory_order_release);return true;
}
void AudioQueue::render(float* destination,unsigned count,bool running)noexcept{
    auto& p=*impl_;auto media=p.media_cursor.load(std::memory_order_relaxed);auto device=p.device_cursor.load(std::memory_order_relaxed);
    unsigned remaining=count;
    while(remaining){
        const auto index=p.read.load(std::memory_order_relaxed);
        if(!running || index==p.write.load(std::memory_order_acquire)){
            const bool tail=running && p.finished.load(std::memory_order_acquire);
            std::memset(destination,0,std::size_t(remaining)*2*sizeof(float));p.segment(device,media,remaining,tail);
            if(tail)media+=remaining;
            else if(running)p.underrun.fetch_add(remaining,std::memory_order_relaxed);
            device+=remaining;remaining=0;break;
        }
        const auto& block=p.blocks[index%Impl::slots];
        if(block.start+block.count<=media){p.expired.fetch_add(block.count,std::memory_order_relaxed);p.read.store(index+1,std::memory_order_release);continue;}
        unsigned take;
        if(block.start>media){
            take=unsigned(std::min<std::int64_t>(remaining,block.start-media));
            std::memset(destination,0,std::size_t(take)*2*sizeof(float));p.gap.fetch_add(take,std::memory_order_relaxed);
        }else{
            const auto offset=unsigned(media-block.start);take=std::min(remaining,block.count-offset);
            std::memcpy(destination,block.samples.data()+offset*2,std::size_t(take)*2*sizeof(float));
            p.rendered.fetch_add(take,std::memory_order_relaxed);
            if(offset+take==block.count)p.read.store(index+1,std::memory_order_release);
        }
        p.segment(device,media,take,true);media+=take;device+=take;remaining-=take;destination+=take*2;
    }
    p.media_cursor.store(media,std::memory_order_release);p.device_cursor.store(device,std::memory_order_release);
}
std::int64_t AudioQueue::media_at(std::int64_t device)const noexcept{
    if(device<=0)return 0;const auto& p=*impl_;const auto end=p.published.load(std::memory_order_acquire);
    const auto begin=end>Impl::history?end-Impl::history:0;
    for(auto n=end;n>begin;--n){const auto& s=p.clock[(n-1)%Impl::history];const auto expected=n*2;if(s.stamp.load(std::memory_order_acquire)!=expected)continue;
        const auto d=s.device.load(std::memory_order_relaxed),m=s.media.load(std::memory_order_relaxed),length=s.length.load(std::memory_order_relaxed);
        if(s.stamp.load(std::memory_order_acquire)!=expected)continue;
        if(device>=d)return m+(length>0?std::clamp<std::int64_t>(device-d,0,length):0);
    }
    return 0;
}
std::int64_t AudioQueue::submitted()const noexcept{return impl_->device_cursor.load(std::memory_order_acquire);}
void AudioQueue::finish()noexcept{impl_->finished.store(true,std::memory_order_release);}
AudioQueueStats AudioQueue::stats()const noexcept{
    auto& p=*impl_;return {p.rendered.load(),p.underrun.load(),p.gap.load(),p.expired.load(),std::max<std::int64_t>(0,p.queued_end.load()-p.media_cursor.load())};
}
}
