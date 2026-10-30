#include "cyvr/player/disc.hpp"
#include "cyvr/player/demuxer.hpp"
extern "C" {
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/nav_read.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avio.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libbluray/bluray.h>
#include <libbluray/log_control.h>
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <map>
#include <sstream>

namespace cyvr::player {
namespace {
constexpr std::int64_t block_size = 2048;
constexpr int pci_start = 0x2d;
std::int64_t us(std::int64_t ticks) { return ticks * 100 / 9; }
std::int64_t dvd_ticks(const dvd_time_t &t) {
  auto bcd = [](unsigned n) { return (n >> 4) * 10 + (n & 15); };
  const auto seconds = bcd(t.hour) * 3600 + bcd(t.minute) * 60 + bcd(t.second);
  const int rate = t.frame_u >> 6;
  return std::int64_t(seconds) * 90000 + (rate == 1   ? 3600
                                          : rate == 3 ? 3003
                                                      : 0) *
                                             bcd(t.frame_u & 63);
}
std::string lang(unsigned code) {
  char a = char(code >> 8), b = char(code & 255);
  return a >= 'a' && a <= 'z' && b >= 'a' && b <= 'z' ? std::string{a, b}
                                                      : std::string{};
}
struct IfoCloser {
  void operator()(ifo_handle_t *p) const {
    if (p)
      ifoClose(p);
  }
};
using Ifo = std::unique_ptr<ifo_handle_t, IfoCloser>;
struct BdInfoCloser {
  void operator()(BLURAY_TITLE_INFO *p) const {
    if (p)
      bd_free_title_info(p);
  }
};
using BdTitle = std::unique_ptr<BLURAY_TITLE_INFO, BdInfoCloser>;
} // namespace
struct DiscImage::Impl {
  AVIOContext *image{};
  BLURAY *bd{};
  dvd_reader_t *dvd{};
  dvd_file_t *vob{};
  Ifo vmg, vts;
  DiscInfo info;
  std::vector<DiscChapter> chapters;
  std::map<int, std::string> languages;
  struct Segment {
    std::int64_t begin{}, end{}, time{}, duration{}, source_pts{};
    std::uint32_t sector{}, last_vobu{};
  };
  std::vector<Segment> segments;
  std::array<std::uint32_t, 16> palette{};
  int dvd_width{}, dvd_height{};
  bool dvd_wide{};
  std::int64_t size{}, position{}, duration{};
  std::array<std::uint8_t, 65536> buffer{};
  std::int64_t buffer_at{-1};
  int buffer_size{};
  dvd_reader_stream_cb dvd_callbacks{dvd_seek, dvd_read, nullptr};
  bool prime_bd() {
    const int n = bd_read(bd, buffer.data(), int(buffer.size()));
    if (n <= 0) {
      buffer_at = -1;
      buffer_size = 0;
      return false;
    }
    buffer_size = n;
    buffer_at = std::int64_t(bd_tell(bd)) - n;
    position = buffer_at;
    return buffer_at >= 0;
  }
  ~Impl() {
    if (vob)
      DVDCloseFile(vob);
    vts.reset();
    vmg.reset();
    if (dvd)
      DVDClose(dvd);
    if (bd)
      bd_close(bd);
  }
  static int bd_read_blocks(void *opaque, void *data, int lba, int count) {
    auto &self = *static_cast<Impl *>(opaque);
    if (lba < 0 || count < 0 || count > INT_MAX / block_size)
      return -1;
    if (avio_seek(self.image, std::int64_t(lba) * block_size, SEEK_SET) < 0)
      return -1;
    const int result = avio_read(self.image, static_cast<unsigned char *>(data),
                                 count * int(block_size));
    return result < 0 ? -1 : result / int(block_size);
  }
  static int dvd_seek(void *opaque, std::uint64_t pos) {
    auto &self = *static_cast<Impl *>(opaque);
    return pos <= INT64_MAX && avio_seek(self.image, std::int64_t(pos),
                                         SEEK_SET) == std::int64_t(pos)
               ? 0
               : -1;
  }
  static int dvd_read(void *opaque, void *data, int count) {
    auto &self = *static_cast<Impl *>(opaque);
    return avio_read(self.image, static_cast<unsigned char *>(data), count);
  }
  static void dvd_log(void *, dvd_logger_level_t, const char *, va_list) {}
  const Segment &segment(std::int64_t pos) const {
    auto it =
        std::upper_bound(segments.begin(), segments.end(), pos,
                         [](auto p, const Segment &s) { return p < s.begin; });
    return it == segments.begin() ? segments.front() : *std::prev(it);
  }
  pci_t nav(std::uint32_t sector) const {
    std::array<unsigned char, 2048> data{};
    if (sector > INT_MAX ||
        DVDReadBlocks(vob, int(sector), 1, data.data()) != 1)
      throw MediaError("DVD: cannot read navigation block");
    if (std::memcmp(data.data() + pci_start - 7, "\0\0\1\xbf", 4) ||
        data[pci_start - 1] != 0)
      throw MediaError(
          "DVD: invalid navigation data (corrupt or protected image)");
    pci_t pci{};
    navRead_PCI(&pci, data.data() + pci_start);
    return pci;
  }
  bool open_bd(int requested, int angle) {
    bd_set_debug_mask(0);
    bd = bd_init();
    if (!bd)
      throw std::bad_alloc();
    if (!bd_open_stream(bd, this, bd_read_blocks)) {
      bd_close(bd);
      bd = nullptr;
      return false;
    }
    const auto *disc = bd_get_disc_info(bd);
    if (!disc || !disc->bluray_detected) {
      bd_close(bd);
      bd = nullptr;
      return false;
    }
    if ((disc->aacs_detected && !disc->aacs_handled) ||
        (disc->bdplus_detected && !disc->bdplus_handled))
      throw MediaError(
          "Blu-ray: AACS/BD+ protected image; use an unencrypted disc image");
    info.type = "Blu-ray";
    if (disc->content_exist_3D)
      info.note = "Blu-ray 3D SSIF is unavailable; playing the 2D base view.";
    const auto count = bd_get_titles(bd, TITLES_FILTER_DUP_TITLE, 0);
    if (!count || count > 4096)
      throw MediaError("Blu-ray: no readable titles or title limit exceeded");
    int selected = -1;
    std::uint64_t longest = 0;
    for (unsigned n = 0; n < count; ++n) {
      BdTitle t(bd_get_title_info(bd, n, 0));
      if (!t || !t->duration)
        continue;
      if (t->duration > std::uint64_t(24) * 3600 * 90000)
        continue;
      info.titles.push_back({int(n), us(std::int64_t(t->duration)),
                             t->chapter_count,
                             std::max(1u, unsigned(t->angle_count)),
                             std::to_string(t->playlist) + ".mpls"});
      if (t->duration > longest) {
        selected = int(n);
        longest = t->duration;
      }
    }
    if (requested >= 0)
      selected = requested;
    if (std::none_of(info.titles.begin(), info.titles.end(),
                     [&](const auto &t) { return t.index == selected; }))
      throw MediaError("Blu-ray: invalid title selection");
    BdTitle t(bd_get_title_info(bd, unsigned(selected), unsigned(angle)));
    if (!t || angle >= std::max(1, int(t->angle_count)))
      throw MediaError("Blu-ray: invalid angle selection");
    if (t->clip_count > 4096 || t->chapter_count > 10000)
      throw MediaError("Blu-ray: playlist limit exceeded");
    if (!bd_select_title(bd, unsigned(selected)) ||
        !bd_select_angle(bd, unsigned(angle)))
      throw MediaError("Blu-ray: cannot select title/angle");
    info.selected = selected;
    info.angle = angle;
    duration = us(std::int64_t(t->duration));
    for (unsigned n = 0; n < t->clip_count; ++n) {
      const auto &clip = t->clips[n];
      const auto bytes = std::int64_t(clip.pkt_count) * 192;
      segments.push_back({size, size + bytes, std::int64_t(clip.start_time),
                          std::int64_t(clip.out_time - clip.in_time),
                          std::int64_t(clip.in_time), 0, 0});
      size += bytes;
      for (unsigned a = 0; a < clip.audio_stream_count; ++a)
        languages[clip.audio_streams[a].pid] =
            reinterpret_cast<const char *>(clip.audio_streams[a].lang);
      for (unsigned a = 0; a < clip.pg_stream_count; ++a)
        languages[clip.pg_streams[a].pid] =
            reinterpret_cast<const char *>(clip.pg_streams[a].lang);
    }
    size = std::int64_t(bd_get_title_size(bd));
    for (unsigned n = 0; n < t->chapter_count; ++n) {
      const auto &c = t->chapters[n];
      chapters.push_back(
          {us(std::int64_t(c.start)), us(std::int64_t(c.start + c.duration)),
           c.chapter_name ? std::string(c.chapter_name).substr(0, 512)
                          : std::string{}});
    }
    if (size <= 0 || segments.empty())
      throw MediaError("Blu-ray: empty title");
    return true;
  }
  struct DvdMap {
    std::vector<Segment> segments;
    std::vector<DiscChapter> chapters;
    pgc_t *pgc{};
    std::int64_t ticks{}, bytes{};
  };
  DvdMap map_dvd(ifo_handle_t *ifo, int ttn, int angle) {
    DvdMap result;
    if (!ifo || !ifo->vts_ptt_srpt || !ifo->vts_pgcit || !ifo->vtsi_mat ||
        ttn < 1 || ttn > ifo->vts_ptt_srpt->nr_of_srpts)
      return result;
    const auto &title = ifo->vts_ptt_srpt->title[ttn - 1];
    if (!title.ptt || !title.nr_of_ptts || title.nr_of_ptts > 10000)
      return result;
    for (unsigned p = 0; p < title.nr_of_ptts; ++p) {
      const auto &part = title.ptt[p];
      if (part.pgcn < 1 || part.pgcn > ifo->vts_pgcit->nr_of_pgci_srp)
        return {};
      auto *pgc = ifo->vts_pgcit->pgci_srp[part.pgcn - 1].pgc;
      if (!pgc || !pgc->program_map || !pgc->cell_playback || part.pgn < 1 ||
          part.pgn > pgc->nr_of_programs || pgc->pg_playback_mode)
        return {};
      if (!result.pgc)
        result.pgc = pgc;
      const auto start = pgc->program_map[part.pgn - 1];
      unsigned end = pgc->nr_of_cells + 1;
      if (p + 1 < title.nr_of_ptts && title.ptt[p + 1].pgcn == part.pgcn) {
        const auto next = title.ptt[p + 1].pgn;
        if (next > part.pgn && next <= pgc->nr_of_programs)
          end = pgc->program_map[next - 1];
        else
          return {};
      }
      if (start < 1 || start >= end || end > unsigned(pgc->nr_of_cells) + 1)
        return {};
      const auto chapter_start = us(result.ticks);
      int angle_cell = 0;
      for (unsigned c = start; c < end; ++c) {
        const auto &cell = pgc->cell_playback[c - 1];
        if (cell.block_type == BLOCK_TYPE_ANGLE_BLOCK) {
          if (cell.block_mode == BLOCK_MODE_FIRST_CELL)
            angle_cell = 0;
          if (angle_cell++ != angle)
            continue;
          // Interleaved ILVU branching needs the DVD VM, not a raw
          // concatenation of angle cells. Never emit the wrong view.
          if (cell.interleaved)
            return {};
        }
        if (cell.last_sector < cell.first_sector || cell.last_sector > INT_MAX)
          return {};
        const auto bytes =
            (std::int64_t(cell.last_sector) - cell.first_sector + 1) *
            block_size;
        const auto ticks = dvd_ticks(cell.playback_time);
        result.segments.push_back({result.bytes, result.bytes + bytes,
                                   result.ticks, ticks, 0, cell.first_sector,
                                   cell.last_vobu_start_sector});
        result.bytes += bytes;
        result.ticks += ticks;
        if (result.segments.size() > 4096 ||
            result.ticks > std::int64_t(24) * 3600 * 90000)
          return {};
      }
      result.chapters.push_back({chapter_start, us(result.ticks), {}});
    }
    return result;
  }
  void open_dvd(int requested, int angle) {
    const dvd_logger_cb logger{dvd_log};
    dvd = DVDOpenStream2(this, &logger, &dvd_callbacks);
    if (!dvd)
      throw MediaError("ISO: no DVD-Video or Blu-ray filesystem found");
    vmg.reset(ifoOpen(dvd, 0));
    if (!vmg || !vmg->tt_srpt || !vmg->tt_srpt->title ||
        !vmg->tt_srpt->nr_of_srpts || vmg->tt_srpt->nr_of_srpts > 99)
      throw MediaError("ISO: no readable DVD-Video titles (data "
                       "discs/DVD-Audio/DVD-VR are not supported)");
    info.type = "DVD-Video";
    int selected = -1;
    std::int64_t longest = 0;
    for (int n = 0; n < vmg->tt_srpt->nr_of_srpts; ++n) {
      const auto &title = vmg->tt_srpt->title[n];
      Ifo ifo(ifoOpen(dvd, title.title_set_nr));
      const auto map = map_dvd(ifo.get(), title.vts_ttn, 0);
      if (map.segments.empty() || !map.ticks)
        continue;
      info.titles.push_back({n,
                             us(map.ticks),
                             unsigned(map.chapters.size()),
                             std::max(1u, unsigned(title.nr_of_angles)),
                             {}});
      if (map.ticks > longest) {
        longest = map.ticks;
        selected = n;
      }
    }
    if (requested >= 0)
      selected = requested;
    if (std::none_of(info.titles.begin(), info.titles.end(),
                     [&](const auto &t) { return t.index == selected; }))
      throw MediaError(
          "DVD: no supported title or invalid selection (interleaved "
          "angles/interactive program chains need DVD menus)");
    const auto &title = vmg->tt_srpt->title[selected];
    if (angle >= std::max(1, int(title.nr_of_angles)))
      throw MediaError("DVD: invalid angle selection");
    vts.reset(ifoOpen(dvd, title.title_set_nr));
    auto map = map_dvd(vts.get(), title.vts_ttn, angle);
    if (map.segments.empty())
      throw MediaError("DVD: unsupported interactive/angle program chain");
    vob = DVDOpenFile(dvd, title.title_set_nr, DVD_READ_TITLE_VOBS);
    if (!vob)
      throw MediaError("DVD: cannot open movie VOBs");
    info.selected = selected;
    info.angle = angle;
    segments = std::move(map.segments);
    chapters = std::move(map.chapters);
    size = map.bytes;
    duration = us(map.ticks);
    std::copy(std::begin(map.pgc->palette), std::end(map.pgc->palette),
              palette.begin());
    const auto &video = vts->vtsi_mat->vts_video_attr;
    dvd_wide = video.display_aspect_ratio == 3;
    dvd_width = video.picture_size == 0 || video.picture_size == 3 ? 720
                : video.picture_size == 1                          ? 704
                                                                   : 352;
    dvd_height = video.video_format ? 576 : 480;
    if (video.picture_size == 3)
      dvd_height /= 2;
    for (auto &s : segments)
      s.source_pts = nav(s.sector).pci_gi.vobu_s_ptm;
    for (unsigned n = 0; n < vts->vtsi_mat->nr_of_vts_audio_streams && n < 8;
         ++n) {
      if (!(map.pgc->audio_control[n] & 0x8000))
        continue;
      const auto physical = (map.pgc->audio_control[n] >> 8) & 7;
      const auto &a = vts->vtsi_mat->vts_audio_attr[n];
      const int base = a.audio_format == 0                          ? 0x80
                       : a.audio_format == 2 || a.audio_format == 3 ? 0x1c0
                       : a.audio_format == 4                        ? 0xa0
                       : a.audio_format == 6                        ? 0x88
                                                                    : -1;
      if (base >= 0)
        languages[base + physical] = lang(a.lang_code);
    }
    for (unsigned n = 0; n < vts->vtsi_mat->nr_of_vts_subp_streams && n < 32;
         ++n)
      if (map.pgc->subp_control[n] & 0x80000000u) {
        const auto code = lang(vts->vtsi_mat->vts_subp_attr[n].lang_code);
        for (unsigned shift : {0u, 8u, 16u, 24u})
          languages[0x20 + ((map.pgc->subp_control[n] >> shift) & 31)] = code;
      }
  }
};
DiscImage::DiscImage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DiscImage::~DiscImage() = default;
std::unique_ptr<DiscImage> DiscImage::open(AVIOContext *image, int title,
                                           int angle) {
  if (!image || !(image->seekable & AVIO_SEEKABLE_NORMAL) ||
      avio_size(image) < 32 * block_size)
    throw MediaError("ISO playback needs a complete, seekable image (HTTP "
                     "servers must support byte ranges)");
  if (title < -1 || title > 4095 || angle < 0 || angle > 8)
    throw MediaError("Invalid disc title/angle");
  auto p = std::make_unique<Impl>();
  p->image = image;
  if (!p->open_bd(title, angle))
    p->open_dvd(title, angle);
  return std::unique_ptr<DiscImage>(new DiscImage(std::move(p)));
}
const DiscInfo &DiscImage::info() const { return impl_->info; }
const std::vector<DiscChapter> &DiscImage::chapters() const {
  return impl_->chapters;
}
std::int64_t DiscImage::duration_us() const { return impl_->duration; }
const char *DiscImage::demuxer() const { return impl_->bd ? "mpegts" : "mpeg"; }
int DiscImage::read(std::uint8_t *data, int count) noexcept {
  auto &p = *impl_;
  if (count <= 0)
    return AVERROR(EINVAL);
  if (p.position >= p.size)
    return AVERROR_EOF;
  if (p.bd) {
    int copied = 0;
    if (p.buffer_at >= 0 && p.position >= p.buffer_at &&
        p.position < p.buffer_at + p.buffer_size) {
      copied = int(std::min<std::int64_t>(count, p.buffer_at + p.buffer_size -
                                                     p.position));
      std::memcpy(data, p.buffer.data() + p.position - p.buffer_at,
                  std::size_t(copied));
      p.position += copied;
      if (copied == count)
        return copied;
    }
    const int n = bd_read(p.bd, data + copied, count - copied);
    if (n > 0)
      p.position = std::int64_t(bd_tell(p.bd));
    return n > 0    ? copied + n
           : copied ? copied
           : n == 0 ? AVERROR_EOF
                    : AVERROR(EIO);
  }
  int total = 0;
  while (count > 0 && p.position < p.size) {
    const auto &s = p.segment(p.position);
    if (p.buffer_at < 0 || p.position < p.buffer_at ||
        p.position >= p.buffer_at + p.buffer_size) {
      const auto aligned =
          s.begin + ((p.position - s.begin) / block_size) * block_size;
      const auto sectors =
          int(std::min<std::int64_t>(p.buffer.size(), s.end - aligned) /
              block_size);
      const int result = int(
          DVDReadBlocks(p.vob, int(s.sector + (aligned - s.begin) / block_size),
                        std::size_t(sectors), p.buffer.data()));
      if (result <= 0)
        return total ? total : AVERROR(EIO);
      p.buffer_at = aligned;
      p.buffer_size = result * int(block_size);
      // Scrambled PES payloads cannot be decoded by this build.
      for (int i = 0; i < p.buffer_size; i += int(block_size)) {
        const auto *pack = p.buffer.data() + i;
        if (std::memcmp(pack, "\0\0\1\xba", 4))
          continue;
        const int pes = 14 + (pack[13] & 7);
        if (!std::memcmp(pack + pes, "\0\0\1", 3) &&
            (pack[pes + 3] == 0xbd || pack[pes + 3] >= 0xc0) &&
            (pack[pes + 6] & 0xc0) == 0x80 && (pack[pes + 6] & 0x30))
          return AVERROR(EACCES);
      }
    }
    const int bytes = int(std::min<std::int64_t>(
        {count, p.buffer_at + p.buffer_size - p.position, s.end - p.position}));
    std::memcpy(data, p.buffer.data() + p.position - p.buffer_at,
                std::size_t(bytes));
    data += bytes;
    count -= bytes;
    total += bytes;
    p.position += bytes;
  }
  return total ? total : AVERROR_EOF;
}
std::int64_t DiscImage::seek(std::int64_t offset, int whence) noexcept {
  auto &p = *impl_;
  if (whence == AVSEEK_SIZE)
    return p.size;
  whence &= ~AVSEEK_FORCE;
  const auto base = whence == SEEK_SET   ? 0
                    : whence == SEEK_CUR ? p.position
                    : whence == SEEK_END ? p.size
                                         : -1;
  if (base < 0 || offset < -base || offset > p.size - base)
    return AVERROR(EINVAL);
  const auto target = base + offset;
  if (p.bd && target < p.size) {
    const auto actual = bd_seek(p.bd, std::uint64_t(target));
    if (actual < 0 || actual > target || !p.prime_bd() || p.position > target)
      return AVERROR(EIO);
    if (target <= p.buffer_at + p.buffer_size) {
      p.position = target;
      return target;
    }
    p.position = p.buffer_at + p.buffer_size;
    p.buffer_at = -1;
    p.buffer_size = 0;
    // libbluray seeks to an access unit. AVIO byte seeks must still land
    // exactly at the requested byte; discard only the preceding GOP.
    std::array<unsigned char, 65536> discard{};
    while (p.position < target) {
      const int bytes =
          int(std::min<std::int64_t>(target - p.position, discard.size()));
      const auto old = p.position;
      if (bd_read(p.bd, discard.data(), bytes) <= 0)
        return AVERROR(EIO);
      p.position = std::int64_t(bd_tell(p.bd));
      if (p.position <= old)
        return AVERROR(EIO);
    }
  } else
    p.position = target;
  return p.position;
}
std::int64_t DiscImage::seek_time(std::int64_t position) {
  auto &p = *impl_;
  position = std::clamp<std::int64_t>(
      position, 0, std::max<std::int64_t>(0, p.duration - 1));
  if (p.bd) {
    const auto pos = bd_seek_time(p.bd, std::uint64_t(position * 9 / 100));
    if (pos < 0 || !p.prime_bd())
      throw MediaError("Blu-ray: seek failed");
    return p.position;
  }
  const auto ticks = position * 9 / 100;
  const Impl::Segment *chosen = &p.segments.front();
  for (const auto &s : p.segments)
    if (s.time <= ticks)
      chosen = &s;
  const auto &s = *chosen;
  std::uint32_t sector = s.sector;
  const auto *map = p.vts->vts_vobu_admap;
  if (map && map->vobu_start_sectors && map->last_byte >= 3) {
    const auto count = (map->last_byte + 1 - 4) / 4;
    if (count > 1000000)
      throw MediaError("DVD: navigation index limit exceeded");
    const std::uint32_t *entries = map->vobu_start_sectors;
    const auto *first = std::lower_bound(entries, entries + count, s.sector);
    const auto *end = std::upper_bound(first, entries + count, s.last_vobu);
    const auto target = s.source_pts + ticks - s.time;
    auto low = first, high = end;
    while (low < high) {
      auto mid = low + (high - low) / 2;
      const auto pts = p.nav(*mid).pci_gi.vobu_s_ptm;
      if (pts <= target)
        low = mid + 1;
      else
        high = mid;
    }
    if (low != first)
      sector = *std::prev(low);
  }
  p.position = s.begin + (std::int64_t(sector) - s.sector) * block_size;
  return p.position;
}
std::int64_t DiscImage::timestamp_delta(std::int64_t pos) const {
  const auto &s = impl_->segment(std::max<std::int64_t>(0, pos));
  return s.time - s.source_pts;
}
std::string DiscImage::language(int id) const {
  auto it = impl_->languages.find(id);
  return it == impl_->languages.end() ? std::string{} : it->second;
}
void DiscImage::configure_subtitle(AVCodecParameters *params) const {
  if (!impl_->dvd || params->codec_id != AV_CODEC_ID_DVD_SUBTITLE)
    return;
  params->width = impl_->dvd_width;
  params->height = impl_->dvd_height;
  std::ostringstream text;
  text << "size: " << impl_->dvd_width << "x" << impl_->dvd_height
       << "\npalette: ";
  for (unsigned n = 0; n < 16; ++n) {
    const auto color = impl_->palette[n];
    const int y = int((color >> 16) & 255) - 16,
              cr = int((color >> 8) & 255) - 128, cb = int(color & 255) - 128;
    auto clamp = [](int c) { return unsigned(std::clamp(c, 0, 255)); };
    const auto rgb = (clamp((298 * y + 409 * cr + 128) >> 8) << 16) |
                     (clamp((298 * y - 100 * cb - 208 * cr + 128) >> 8) << 8) |
                     clamp((298 * y + 516 * cb + 128) >> 8);
    if (n)
      text << ", ";
    text << std::hex;
    text.width(6);
    text.fill('0');
    text << rgb;
  }
  text << "\n";
  const auto extra = text.str();
  av_freep(&params->extradata);
  params->extradata = static_cast<std::uint8_t *>(
      av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!params->extradata)
    throw std::bad_alloc();
  std::memcpy(params->extradata, extra.data(), extra.size());
  params->extradata_size = int(extra.size());
}
void DiscImage::configure_video(AVCodecParameters *params) const {
  if (!impl_->dvd || params->codec_type != AVMEDIA_TYPE_VIDEO ||
      params->width <= 0 || params->height <= 0)
    return;
  av_reduce(&params->sample_aspect_ratio.num, &params->sample_aspect_ratio.den,
            std::int64_t(params->height) * (impl_->dvd_wide ? 16 : 4),
            std::int64_t(params->width) * (impl_->dvd_wide ? 9 : 3), INT_MAX);
}
} // namespace cyvr::player
