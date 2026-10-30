#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <array>
#include <vector>

namespace cyvr::player {
class Cancellation {
public:
    void cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }
private:
    std::atomic_bool cancelled_{false};
};

enum class SourceKind { file, network, descriptor };
// A descriptor is borrowed only until Demuxer::open duplicates it. Offset and
// length describe an Android AssetFileDescriptor slice; -1 length means unknown.
struct MediaSource {
    SourceKind kind;
    std::string location;
    int fd = -1;
    std::int64_t offset = 0, length = -1;
    static MediaSource file(std::string path);
    static MediaSource network(std::string url);
    static MediaSource descriptor(int fd, std::int64_t offset = 0, std::int64_t length = -1);
    std::string display_name() const;
};

struct ContentKey {std::array<std::uint8_t,16> id{},key{};};
struct StreamOptions {
    int open_timeout_ms = 10000;
    int read_timeout_ms = 10000;
    std::int64_t probe_bytes = 4 * 1024 * 1024;
    std::int64_t analyze_us = 3000000;
    bool low_latency = false;
    bool rtsp_over_tcp = true;
    // Empty uses backend system trust where available. Android Mbed TLS uses
    // a PEM exported by SystemCertificates from the platform trust manager.
    std::string ca_file;
    std::vector<ContentKey> clear_keys;
    void validate() const;
};
} // namespace cyvr::player
