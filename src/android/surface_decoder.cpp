#include "cyvr/player/android/surface_decoder.hpp"
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <unistd.h>

namespace cyvr::player::android {
struct ReaderState {
    AImageReader* reader = nullptr;
    std::uint64_t epoch = 0;
    ~ReaderState() { if (reader) AImageReader_delete(reader); }
};
namespace {
void check(media_status_t status, const char* operation) {
    if (status != AMEDIA_OK)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}
}
void MediaFormatDeleter::operator()(AMediaFormat* format) const noexcept {
    if (format) AMediaFormat_delete(format);
}
ImageLease::ImageLease(AImage* image, int fence, std::shared_ptr<ReaderState> owner,
                       std::int64_t pts, std::uint64_t generation)
    : pts_us(pts), epoch(generation), image_(image), acquire_fd_(fence), owner_(std::move(owner)) {}
ImageLease::~ImageLease() { reset(); }
ImageLease::ImageLease(ImageLease&& other) noexcept { *this = std::move(other); }
ImageLease& ImageLease::operator=(ImageLease&& other) noexcept {
    if (this != &other) {
        reset();
        image_ = std::exchange(other.image_, nullptr);
        acquire_fd_ = std::exchange(other.acquire_fd_, -1);
        owner_ = std::move(other.owner_);
        pts_us = other.pts_us;
        epoch = other.epoch;
    }
    return *this;
}
void ImageLease::reset() noexcept {
    if (image_) AImage_deleteAsync(image_, std::exchange(acquire_fd_, -1));
    else if (acquire_fd_ >= 0) ::close(std::exchange(acquire_fd_, -1));
    image_ = nullptr;
    owner_.reset();
}
AHardwareBuffer* ImageLease::hardware_buffer() const {
    if (!image_) throw std::logic_error("Image lease is empty");
    AHardwareBuffer* buffer = nullptr;
    check(AImage_getHardwareBuffer(image_, &buffer), "AImage_getHardwareBuffer");
    return buffer;
}
int ImageLease::take_acquire_fence() noexcept { return std::exchange(acquire_fd_, -1); }
void ImageLease::release_after_gpu(int release_fd) noexcept {
    // Caller must first consume the acquire FD before GPU use. In the unusual
    // case it remains owned here, close our reference; the provided release FD
    // must already include the producer dependency in its synchronization chain.
    if (acquire_fd_ >= 0) ::close(std::exchange(acquire_fd_, -1));
    if (image_) AImage_deleteAsync(std::exchange(image_, nullptr), release_fd);
    else if (release_fd >= 0) ::close(release_fd);
    owner_.reset();
}
SurfaceDecoder::~SurfaceDecoder() { close(); }
void SurfaceDecoder::close() noexcept {
    if (codec_) {
        if (started_) AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
    }
    codec_ = nullptr;
    started_ = input_ended_ = false;
    reader_.reset();
}
void SurfaceDecoder::open(const char* name, AMediaFormat* format,
                          std::uint64_t epoch, std::int32_t max_images) {
    if (codec_) throw std::logic_error("Close the existing decoder before open");
    std::int32_t width = 0, height = 0;
    if (!name || !*name || !format || max_images < 2 || max_images > 16 ||
        !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width) || width <= 0 ||
        !AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height) || height <= 0)
        throw std::invalid_argument("Decoder requires a codec name, dimensions and 2..16 image slots");
    try {
        reader_ = std::make_shared<ReaderState>();
        reader_->epoch = epoch;
        check(AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, max_images, &reader_->reader),
            "AImageReader_newWithUsage");
        ANativeWindow* window = nullptr;
        check(AImageReader_getWindow(reader_->reader, &window), "AImageReader_getWindow");
        codec_ = AMediaCodec_createCodecByName(name);
        if (!codec_) throw std::runtime_error("Cannot create the selected MediaCodec");
        check(AMediaCodec_configure(codec_, format, window, nullptr, 0), "AMediaCodec_configure");
        check(AMediaCodec_start(codec_), "AMediaCodec_start");
        started_ = true;
    } catch (...) { close(); throw; }
}
bool SurfaceDecoder::queue_access_unit(const std::uint8_t* data, std::size_t size,
                                      std::int64_t pts, std::uint32_t flags) {
    if (!started_ || input_ended_) throw std::logic_error("Decoder input is not accepting packets");
    if ((size && !data) || pts < 0)
        throw std::invalid_argument("Packet requires data and a nonnegative rebased timestamp");
    const auto index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return false;
    if (index < 0) throw std::runtime_error("MediaCodec input dequeue failed");
    std::size_t capacity = 0;
    auto* destination = AMediaCodec_getInputBuffer(codec_, std::size_t(index), &capacity);
    if ((size && !destination) || size > capacity)
        throw std::runtime_error("Access unit exceeds MediaCodec input capacity; recreate with max-input-size");
    if (size) std::memcpy(destination, data, size);
    check(AMediaCodec_queueInputBuffer(codec_, std::size_t(index), 0, size,
                                      static_cast<std::uint64_t>(pts), flags), "AMediaCodec_queueInputBuffer");
    input_ended_ = (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    return true;
}
DrainResult SurfaceDecoder::drain_one() {
    if (!started_) throw std::logic_error("Decoder is closed");
    AMediaCodecBufferInfo info{};
    const auto index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
        return {};
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        return {DrainKind::format_changed, 0, false};
    if (index < 0) throw std::runtime_error("MediaCodec output dequeue failed");
    const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    const bool config = (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
    const bool render = !config && (!eos || info.size > 0);
    check(AMediaCodec_releaseOutputBuffer(codec_, std::size_t(index), render), "AMediaCodec_releaseOutputBuffer");
    return {DrainKind::output, info.presentationTimeUs, eos};
}
ImageLease SurfaceDecoder::try_acquire_next() {
    if (!reader_) throw std::logic_error("Decoder is closed");
    AImage* image = nullptr;
    int fence = -1;
    const auto status = AImageReader_acquireNextImageAsync(reader_->reader, &image, &fence);
    if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE || status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED)
        return {};
    check(status, "AImageReader_acquireNextImageAsync");
    ImageLease lease(image, fence, reader_, 0, reader_->epoch);
    std::int64_t timestamp_ns = 0;
    check(AImage_getTimestamp(image, &timestamp_ns), "AImage_getTimestamp");
    lease.pts_us = timestamp_ns / 1000;
    return lease;
}
MediaFormatPtr SurfaceDecoder::output_format() const {
    if (!started_) throw std::logic_error("Decoder is closed");
    MediaFormatPtr result(AMediaCodec_getOutputFormat(codec_));
    if (!result) throw std::runtime_error("MediaCodec output format unavailable");
    return result;
}
} // namespace cyvr::player::android
