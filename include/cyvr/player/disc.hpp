#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVIOContext;
struct AVCodecParameters;
namespace cyvr::player {
struct DiscTitle {
  int index{};
  std::int64_t duration_us{};
  unsigned chapters{}, angles{1};
  std::string name;
};
struct DiscChapter {
  std::int64_t start_us{}, end_us{};
  std::string name;
};
struct DiscInfo {
  std::string type;
  std::string note;
  std::vector<DiscTitle> titles;
  int selected{-1}, angle{};
};
// A bounded, seekable logical movie stream backed by the original ISO. The
// caller owns the image AVIO and serializes every operation on the worker.
class DiscImage {
public:
  static std::unique_ptr<DiscImage> open(AVIOContext *image, int title,
                                         int angle);
  ~DiscImage();
  const DiscInfo &info() const;
  const std::vector<DiscChapter> &chapters() const;
  std::int64_t duration_us() const;
  const char *demuxer() const;
  int read(std::uint8_t *, int) noexcept;
  std::int64_t seek(std::int64_t, int) noexcept;
  std::int64_t seek_time(std::int64_t);
  // Convert each clip/cell's source PTS into one continuous title timeline.
  std::int64_t timestamp_delta(std::int64_t byte_position) const;
  std::string language(int stream_id) const;
  void configure_subtitle(AVCodecParameters *) const;
  void configure_video(AVCodecParameters *) const;

private:
  struct Impl;
  explicit DiscImage(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
} // namespace cyvr::player
