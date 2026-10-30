#pragma once
#include "cyvr/player/demuxer.hpp"
#include <memory>
#include <string>
#include <vector>
namespace cyvr::player {
struct SubtitleBitmap {
    int x{},y{},width{},height{};
    std::vector<std::uint8_t> rgba;
};
struct SubtitleCue {
    std::int64_t start_us{},end_us{};
    std::vector<std::string> ass;
    std::vector<SubtitleBitmap> bitmaps;
    int canvas_width{},canvas_height{};
};
struct SubtitleConfig {
    std::string header;
    std::vector<FontAttachment> fonts;
};
struct SubtitleSnapshot {
    std::shared_ptr<const SubtitleConfig> config;
    std::vector<std::shared_ptr<const SubtitleCue>> cues;
    std::int64_t position_us{};
};
class SubtitleDecoder {
public:
    SubtitleDecoder(const Demuxer&,int stream);
    ~SubtitleDecoder();
    std::string header() const;
    std::shared_ptr<SubtitleCue> decode(const AVPacket*);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
// Thread-safe bounded event queue; no font rasterization on the media thread.
class SubtitleTimeline {
public:
    explicit SubtitleTimeline(std::shared_ptr<const SubtitleConfig> config);
    ~SubtitleTimeline();
    void add(std::shared_ptr<SubtitleCue>);
    SubtitleSnapshot at(std::int64_t position_us) const;
    void prune(std::int64_t before_us);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
