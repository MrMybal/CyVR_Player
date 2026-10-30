#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
struct AVFrame;
namespace cyvr::player {
struct StereoSamples {std::vector<float> samples;std::int64_t start{};};
struct AudioConversionStats {float peak{};std::uint64_t limited{},invalid{};};
// Worker-side conversion. Timestamp rounding must not splice a continuous PCM stream.
class AudioConverter {
public:
    explicit AudioConverter(int output_rate=48000);
    ~AudioConverter();
    void convert(const AVFrame&,std::optional<std::int64_t> relative_pts_us,StereoSamples&);
    void drain(StereoSamples&);
    AudioConversionStats stats()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
