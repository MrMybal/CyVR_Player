#pragma once
#include "cyvr/player/source.hpp"
#include <filesystem>
#include <optional>
#include <vector>

namespace cyvr::player {
enum class FileKind { directory, video, audio, splat, playlist, subtitle, key, other };
struct FileEntry {
    std::string name;
    std::filesystem::path relative_path;
    FileKind kind;
    std::optional<std::uintmax_t> size;
};
struct BrowseOptions {
    std::string search;
    bool show_hidden = false;
    bool show_other = false;
    std::size_t max_entries = 10000;
    std::size_t scan_budget = 100000;
};
struct DirectoryListing {
    std::vector<FileEntry> entries;
    bool truncated = false;
    bool cancelled = false;
    std::size_t scanned = 0;
};
FileKind classify_extension(const std::filesystem::path& file);
// Metadata-only local browsing, no thumbnail decoding and no filesystem writes.
// Canonical confinement protects navigation against accidental traversal. It is
// not an OS security sandbox against concurrent adversarial symlink changes.
class FileBrowser {
public:
    explicit FileBrowser(std::filesystem::path root);
    DirectoryListing list(const std::filesystem::path& relative = {},
                          const BrowseOptions& options = {}, const Cancellation* cancel = nullptr) const;
    MediaSource open_file(const std::filesystem::path& relative) const;
    const std::filesystem::path& root() const noexcept { return root_; }
private:
    std::filesystem::path resolve(const std::filesystem::path& relative) const;
    std::filesystem::path root_;
};
} // namespace cyvr::player
