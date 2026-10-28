#pragma once

#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clipsync {
using Bytes = std::vector<unsigned char>;
enum class Direction { XToWayland, WaylandToX };
enum class DataMode { Raw, UriList };
struct MimeChoice {
    std::string read_mime;
    std::string write_mime;
    std::string kind;
    DataMode mode;
};
struct Fingerprint {
    std::string kind;
    std::uint64_t low;
    std::uint64_t high;
    bool operator==(const Fingerprint& other) const {
        return kind == other.kind && low == other.low && high == other.high;
    }
    bool operator!=(const Fingerprint& other) const { return !(*this == other); }
};
class X11Provider;
struct SyncState {
    SyncState();
    ~SyncState();
    std::optional<Fingerprint> x11;
    std::optional<Fingerprint> wayland;
    std::unique_ptr<X11Provider> x11_provider;
    std::mutex mutex;
};

std::string utf8_lossy(const Bytes& data);
std::vector<std::string> parse_types(const Bytes& data);
std::optional<MimeChoice> choose_mime(const std::vector<std::string>& types, Direction direction);
Bytes normalize_uri_list(const Bytes& data);
Bytes prepare_data(const Bytes& data, DataMode mode);
std::optional<Fingerprint> fingerprint(const std::string& kind, const Bytes& data);
void sync_clipboard(SyncState& state, Direction direction);
}  // namespace clipsync
