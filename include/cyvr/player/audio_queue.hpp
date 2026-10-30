#pragma once
#include <cstdint>
#include <memory>
namespace cyvr::player {
struct AudioQueueStats {
    std::uint64_t rendered{},underrun{},gap{},expired{};
    std::int64_t buffered{};
};
// Single producer and real-time callback consumer. Empty input inserts output
// silence without consuming media; future timestamp gaps intentionally do.
class AudioQueue {
public:
    static constexpr unsigned block_frames=2048;
    AudioQueue();
    ~AudioQueue();
    bool push(const float* stereo,unsigned frames,std::int64_t start);
    void finish() noexcept;
    void render(float* stereo,unsigned frames,bool running=true) noexcept;
    std::int64_t media_at(std::int64_t device_frame) const noexcept;
    std::int64_t submitted() const noexcept;
    AudioQueueStats stats() const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
