#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace cyvr::player {
// Bounded, allocation-free storage. Frame owns its resources and exposes
// int64_t pts_us and uint64_t epoch. Access must be serialized by the caller.
// Present the returned frame until the next due frame. GPU in-flight frames
// must be held separately until their fence completes, including during seek.
template<class Frame, std::size_t Capacity = 6>
class PresentationQueue {
    static_assert(Capacity > 0);
public:
    explicit PresentationQueue(std::uint64_t epoch = 0) : epoch_(epoch) {}
    bool try_push(Frame&& frame) {
        if (frame.epoch != epoch_ || size_ == Capacity ||
            (last_pts_ && frame.pts_us < *last_pts_)) return false;
        last_pts_ = frame.pts_us;
        slots_[(head_ + size_) % Capacity].emplace(std::move(frame));
        ++size_;
        return true;
    }
    // Retire only frames already due. Never consume a future frame, even when
    // a decoder fills the queue faster than the video's presentation rate.
    std::optional<Frame> take_due(std::int64_t target_pts_us) {
        std::optional<Frame> selected;
        while (size_ && slots_[head_]->pts_us <= target_pts_us) {
            selected.reset();
            selected.emplace(std::move(*slots_[head_]));
            slots_[head_].reset();
            head_ = (head_ + 1) % Capacity;
            --size_;
        }
        return selected;
    }
    void reset(std::uint64_t epoch) {
        for (auto& slot : slots_) slot.reset();
        head_ = size_ = 0;
        last_pts_.reset();
        epoch_ = epoch;
    }
    std::size_t size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }
private:
    std::array<std::optional<Frame>, Capacity> slots_{};
    std::size_t head_ = 0, size_ = 0;
    std::uint64_t epoch_;
    std::optional<std::int64_t> last_pts_;
};
} // namespace cyvr::player
