#pragma once
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace cyvr::player::android {
struct ReaderState;

// Move-only lease. The reader stays alive while any lease exists.
// If used by Vulkan, retain this lease until GPU completion, or release it
// with a sync FD covering ALL reads. Releasing just the AHardwareBuffer ref
// does not prevent the producer from reusing the buffer.
class ImageLease {
public:
    ImageLease() = default;
    ~ImageLease();
    ImageLease(ImageLease&& other) noexcept;
    ImageLease& operator=(ImageLease&& other) noexcept;
    ImageLease(const ImageLease&) = delete;
    ImageLease& operator=(const ImageLease&) = delete;
    explicit operator bool() const noexcept { return image_ != nullptr; }
    AImage* image() const noexcept { return image_; }
    // Borrowed AHB, valid until release. Importer must hold the lease in flight.
    AHardwareBuffer* hardware_buffer() const;
    // Transfers the acquire sync FD to the caller (-1 means already ready).
    // Import it into a temporary Vulkan binary semaphore, or wait and close it.
    int take_acquire_fence() noexcept;
    // Consumes release_fd. Without GPU use, destructor forwards the acquire FD
    // directly, avoiding an unnecessary CPU wait for a discarded image.
    void release_after_gpu(int release_fd) noexcept;
    std::int64_t pts_us = 0;
    std::uint64_t epoch = 0;
private:
    friend class SurfaceDecoder;
    ImageLease(AImage* image, int fence, std::shared_ptr<ReaderState> owner,
               std::int64_t pts, std::uint64_t generation);
    void reset() noexcept;
    AImage* image_ = nullptr;
    int acquire_fd_ = -1;
    std::shared_ptr<ReaderState> owner_;
};

struct MediaFormatDeleter { void operator()(AMediaFormat* format) const noexcept; };
using MediaFormatPtr = std::unique_ptr<AMediaFormat, MediaFormatDeleter>;
enum class DrainKind { try_again, format_changed, output };
struct DrainResult {
    DrainKind kind = DrainKind::try_again;
    std::int64_t pts_us = 0;
    bool end_of_stream = false;
};

// Native decoding building block, not a demuxer or player. All calls must be
// serialized; never call on the OpenXR render thread. Name comes from the
// Java CodecProbe; no implicit selection of a software decoder.
class SurfaceDecoder {
public:
    SurfaceDecoder() = default;
    ~SurfaceDecoder();
    SurfaceDecoder(const SurfaceDecoder&) = delete;
    SurfaceDecoder& operator=(const SurfaceDecoder&) = delete;
    // Format contains MIME, dimensions and codec-specific data (csd-*).
    // Recreate the decoder on seek, using a new epoch and new reader, so that
    // delayed images from the previous stream cannot enter the new timeline.
    void open(const char* codec_name, AMediaFormat* format, std::uint64_t epoch,
              std::int32_t max_images = 6);
    void close() noexcept;
    // One complete codec access unit. Returns false for backpressure; caller
    // retains the packet and retries. An exception requires closing this decoder.
    bool queue_access_unit(const std::uint8_t* data, std::size_t size,
                           std::int64_t pts_us, std::uint32_t codec_flags = 0);
    DrainResult drain_one();
    // Preserves presentation order; acquireLatestImage would discard future PTS.
    ImageLease try_acquire_next();
    MediaFormatPtr output_format() const;
private:
    AMediaCodec* codec_ = nullptr;
    bool started_ = false;
    bool input_ended_ = false;
    std::shared_ptr<ReaderState> reader_;
};
} // namespace cyvr::player::android
#endif
