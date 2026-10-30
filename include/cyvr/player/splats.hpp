#pragma once
#include "cyvr/player/source.hpp"
#include <array>
#include <functional>
#include <memory>
#include <vector>
namespace splat {struct SplatCloud;}
namespace cyvr::player {
struct SplatNormalization {std::array<float,3> center{};float scale{1};bool set{};};
std::shared_ptr<const splat::SplatCloud> decode_splats(const std::vector<std::uint8_t>&,SplatNormalization&,const Cancellation&);
struct SplatSnapshot {std::shared_ptr<const splat::SplatCloud> cloud;std::uint64_t epoch{};std::int64_t pts_us{};};
struct SplatSequence {std::vector<std::string> frames;double fps{30};};
SplatSequence splat_sequence(const std::string& location,const std::vector<std::uint8_t>& manifest);
std::vector<std::uint8_t> read_splat_source(const MediaSource&,const Cancellation&);
}
