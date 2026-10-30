#pragma once
#include "cyvr/player/source.hpp"
#include <optional>
#include <stdexcept>
#include <vector>

struct AVPacket;
struct AVFrame;
struct AVCodecParameters;

namespace cyvr::player {
enum class TrackKind { video, audio, subtitle, other };
struct TrackInfo {
    int index = -1;
    TrackKind kind = TrackKind::other;
    std::string codec, language, title;
    int width = 0, height = 0, sample_rate = 0, channels = 0;
    std::optional<std::int64_t> duration_us;
    double nominal_fps{};
    bool default_track{}, forced{}, attached_picture{}, hearing_impaired{}, visual_impaired{};
    int companion_video{-1};
};
struct ChapterInfo { std::int64_t start_us{}, end_us{}; std::string title; };
struct FontAttachment { std::string name; std::vector<std::uint8_t> data; };
class MediaError : public std::runtime_error { using std::runtime_error::runtime_error; };
struct PacketDeleter { void operator()(AVPacket*) const noexcept; };
struct FrameDeleter { void operator()(AVFrame*) const noexcept; };
using PacketPtr = std::unique_ptr<AVPacket,PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame,FrameDeleter>;
enum class ReadStatus { packet, again, end };
struct PacketRead { ReadStatus status; PacketPtr packet; };

// Blocking I/O, called from a worker. A Cancellation can be triggered from
// another thread. All other access to one Demuxer must be serialized.
class Demuxer {
public:
    Demuxer();
    ~Demuxer();
    Demuxer(const Demuxer&) = delete;
    Demuxer& operator=(const Demuxer&) = delete;
    void open(const MediaSource& source, const StreamOptions& options = {},
              std::shared_ptr<Cancellation> cancel = std::make_shared<Cancellation>());
    void close() noexcept;
    const std::vector<TrackInfo>& tracks() const;
    std::optional<std::int64_t> duration_us() const;
    bool seekable() const;
    PacketRead read();
    // Logical position from the common container origin, not per-track zero.
    // A successful seek requires flushing the decoders and incrementing epoch.
    void seek(std::int64_t position_us);
    const AVCodecParameters* codec_parameters(int stream) const;
    int time_base_numerator(int stream) const;
    int time_base_denominator(int stream) const;
    std::optional<std::int64_t> timestamp_us(int stream, std::int64_t timestamp) const;
    std::int64_t origin_us() const;
    int best_video_stream() const;
    int best_audio_stream() const;
    // MPEG-TS can carry the MVC dependent view on a separate PID in the same
    // program. -1 means absent or ambiguous; never merge unrelated programs.
    int mvc_dependent_stream(int base_stream) const;
    const std::vector<ChapterInfo>& chapters() const;
    std::vector<FontAttachment> fonts() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class DecodeStatus { frame, again, end };
struct FrameRead { DecodeStatus status; FramePtr frame; };
// Explicit software decoder for fallback and headless validation. The Quest
// primary video path remains MediaCodec; this does not silently replace it.
class SoftwareDecoder {
public:
    SoftwareDecoder(const Demuxer& demuxer, int stream, int threads = 2,
                    std::int64_t max_pixels = 64 * 1024 * 1024, bool decode_views = false);
    ~SoftwareDecoder();
    SoftwareDecoder(const SoftwareDecoder&) = delete;
    SoftwareDecoder& operator=(const SoftwareDecoder&) = delete;
    bool send(const AVPacket* packet); // nullptr sends EOS; false means drain first
    FrameRead receive();
    void flush();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
