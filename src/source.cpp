#include "cyvr/player/source.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace cyvr::player {
namespace {
void validate_text(const std::string& value) {
    if (value.empty() || value.size() > 32768 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c){ return c < 32 || c == 127; }))
        throw std::invalid_argument("Source contains empty, oversized or control-character text");
}
}
MediaSource MediaSource::file(std::string path) {
    validate_text(path);
    if (path.find("://") != std::string::npos || path.rfind("content:",0)==0)
        throw std::invalid_argument("Use a network source or a document descriptor for URIs");
    return {SourceKind::file,std::move(path)};
}
MediaSource MediaSource::network(std::string url) {
    validate_text(url);
    const auto end = url.find("://");
    if (end == std::string::npos || end == 0 || end + 3 == url.size())
        throw std::invalid_argument("A stream URL needs a protocol and address");
    std::transform(url.begin(),url.begin()+end,url.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    const auto scheme = url.substr(0,end);
    if (scheme!="http" && scheme!="https" && scheme!="rtsp" && scheme!="rtsps" && scheme!="udp" && scheme!="rtp" && scheme!="srt")
        throw std::invalid_argument("Unsupported stream URL protocol");
    return {SourceKind::network,std::move(url)};
}
MediaSource MediaSource::descriptor(int value, std::int64_t begin, std::int64_t count) {
    if (value < 0 || begin < 0 || count < -1 ||
        (count >= 0 && begin > std::numeric_limits<std::int64_t>::max()-count))
        throw std::invalid_argument("Invalid document descriptor slice");
    return {SourceKind::descriptor,{},value,begin,count};
}
std::string MediaSource::display_name() const {
    if (kind == SourceKind::descriptor) return "document";
    if (kind == SourceKind::file) return location;
    const auto scheme = location.find("://");
    if (scheme == std::string::npos) return "stream";
    const auto begin = scheme+3;
    const auto authority_end = location.find_first_of("/?#",begin);
    auto authority = location.substr(begin,authority_end-begin);
    const auto credentials = authority.rfind('@');
    if (credentials != std::string::npos) authority.erase(0,credentials+1);
    // Paths can contain access tokens too. Display protocol/host only.
    return location.substr(0,begin)+authority;
}
void StreamOptions::validate() const {
    if (open_timeout_ms < 1 || open_timeout_ms > 120000 || read_timeout_ms < 1 || read_timeout_ms > 120000 ||
        probe_bytes < 32768 || probe_bytes > 64*1024*1024 || analyze_us < 0 || analyze_us > 30000000)
        throw std::invalid_argument("Invalid stream timeout or probe budget");
    if (ca_file.find('\0') != std::string::npos) throw std::invalid_argument("Invalid certificate path");
}
} // namespace cyvr::player
