#pragma once
#include "cyvr/player/subtitles.hpp"
#include <optional>
namespace cyvr::player {
struct SubtitleImage {
    int x{},y{},width{},height{},canvas_width{},canvas_height{};
    std::vector<std::uint8_t> rgba;
};
// A cropped, straight-alpha image. nullopt means the previous image is unchanged;
// an image with no pixels clears the overlay. ASS shaping is provided by libass.
class SubtitleRasterizer {
public:
    explicit SubtitleRasterizer(std::vector<std::uint8_t> default_font);
    ~SubtitleRasterizer();
    std::optional<SubtitleImage> render(const SubtitleSnapshot&,int width,int height);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
