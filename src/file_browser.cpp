#include "cyvr/player/file_browser.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace cyvr::player {
namespace fs = std::filesystem;
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    return value;
}
bool has(const std::string& ext, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(),values.end(),[&](const char* v){ return ext==v; });
}
}
FileKind classify_extension(const fs::path& path) {
    const auto ext = lower(path.extension().u8string());
    if(has(ext,{".jwk",".clearkey"}))return FileKind::key;
    if(has(ext,{".iso",".img"}))return FileKind::disc;
    if (has(ext,{".mp4",".mkv",".webm",".avi",".mov",".m4v",".ts",".mts",".m2ts",".mpg",".mpeg",".wmv",".flv",".ogv",".vob",".asf",".divx",".m2v",".m2p",".mxf",".rm",".rmvb",".ogm",".3gp",".3g2",".mjpeg",".mjpg",".nut",".y4m",".ivf",".bik",".hevc",".h265",".265",".h264",".264",".av1",".gif"})) return FileKind::video;
    if (has(ext,{".srt",".ass",".ssa",".vtt",".sub",".sup",".idx",".ttml"})) return FileKind::subtitle;
    if (has(ext,{".mp3",".flac",".ogg",".opus",".aac",".m4a",".wav",".wma",".ac3"})) return FileKind::audio;
    if (has(ext,{".ply",".spz",".splat",".sog",".lodsplat",".splatseq"})) return FileKind::splat;
    if (has(ext,{".m3u8",".mpd",".m3u",".pls"})) return FileKind::playlist;
    return FileKind::other;
}
FileBrowser::FileBrowser(fs::path root) : root_(fs::canonical(std::move(root))) {
    if (!fs::is_directory(root_)) throw std::invalid_argument("Browser root must be a directory");
}
fs::path FileBrowser::resolve(const fs::path& relative) const {
    if (relative.is_absolute() || relative.has_root_name())
        throw std::invalid_argument("Browser navigation requires a relative path");
    const auto target = fs::canonical(root_/relative);
    const auto rel = target.lexically_relative(root_);
    if (rel.empty() && target!=root_) throw std::invalid_argument("Path is outside the browser root");
    for (const auto& part : rel)
        if (part=="..") throw std::invalid_argument("Path is outside the browser root");
    return target;
}
DirectoryListing FileBrowser::list(const fs::path& relative, const BrowseOptions& options, const Cancellation* cancel) const {
    if (!options.max_entries || options.max_entries>100000 || !options.scan_budget || options.scan_budget>1000000)
        throw std::invalid_argument("Invalid directory scan budget");
    const auto directory = resolve(relative);
    DirectoryListing result;
    const auto search = lower(options.search);
    for (const auto& item : fs::directory_iterator(directory,fs::directory_options::skip_permission_denied)) {
        if (cancel && cancel->cancelled()) { result.cancelled=true; break; }
        if (result.scanned==options.scan_budget) { result.truncated=true; break; }
        ++result.scanned;
        const auto name = item.path().filename().u8string();
        if ((!options.show_hidden && !name.empty() && name[0]=='.') || lower(name).find(search)==std::string::npos) continue;
        std::error_code error;
        const auto actual = fs::canonical(item.path(),error);
        if (error) continue;
        const auto rel = actual.lexically_relative(root_);
        bool outside = rel.empty();
        for (const auto& part : rel) outside = outside || part=="..";
        if (outside) continue;
        const bool directory_entry = item.is_directory(error);
        if (error) continue;
        if (!directory_entry && !item.is_regular_file(error)) continue;
        const auto kind = directory_entry ? FileKind::directory : classify_extension(item.path());
        if (!options.show_other && kind==FileKind::other) continue;
        if (result.entries.size()==options.max_entries) { result.truncated=true; break; }
        FileEntry entry{name,item.path().lexically_relative(root_),kind,{}};
        if (!directory_entry) {
            auto size = item.file_size(error);
            if (!error) entry.size=size;
        }
        result.entries.push_back(std::move(entry));
    }
    std::sort(result.entries.begin(),result.entries.end(),[](const FileEntry& a,const FileEntry& b){
        if ((a.kind==FileKind::directory)!=(b.kind==FileKind::directory)) return a.kind==FileKind::directory;
        const auto an=lower(a.name),bn=lower(b.name);
        return an==bn ? a.name<b.name : an<bn;
    });
    return result;
}
MediaSource FileBrowser::open_file(const fs::path& relative) const {
    const auto target=resolve(relative);
    if (!fs::is_regular_file(target)) throw std::invalid_argument("Selection is not a regular file");
    return MediaSource::file(target.u8string());
}
} // namespace cyvr::player
