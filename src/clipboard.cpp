#include "clipboard.hpp"

#include "process.hpp"
#define XXH_INLINE_ALL
#include <algorithm>
#include <iomanip>
#include <sstream>

#include "xxhash.h"

namespace clipsync {
namespace {
std::string ascii_lower(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
bool starts_with(const std::string& value, const std::string& prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}
std::optional<std::string> find_type(const std::vector<std::string>& types, const std::string& wanted) {
    const auto lowered = ascii_lower(wanted);
    for (const auto& mime : types)
        if (ascii_lower(mime) == lowered) return mime;
    return std::nullopt;
}
bool whitespace(std::uint32_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}
// Input has already been converted to valid UTF-8, so decoding cannot fail.
std::string trim(const std::string& value) {
    std::size_t first = value.size(), last = 0;
    for (std::size_t i = 0; i < value.size();) {
        const auto begin = i;
        const auto lead = static_cast<unsigned char>(value[i++]);
        std::uint32_t code = lead;
        unsigned count = 0;
        if (lead >= 0xf0) {
            code &= 7;
            count = 3;
        } else if (lead >= 0xe0) {
            code &= 15;
            count = 2;
        } else if (lead >= 0xc0) {
            code &= 31;
            count = 1;
        }
        while (count--) code = (code << 6) | (static_cast<unsigned char>(value[i++]) & 63);
        if (!whitespace(code)) {
            first = std::min(first, begin);
            last = i;
        }
    }
    return last ? value.substr(first, last - first) : std::string{};
}
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto newline = text.find('\n', begin);
        auto end = newline == std::string::npos ? text.size() : newline;
        if (newline != std::string::npos && end > begin && text[end - 1] == '\r') --end;
        result.push_back(text.substr(begin, end - begin));
        if (newline == std::string::npos) break;
        begin = newline + 1;
    }
    return result;
}
}  // namespace

std::string utf8_lossy(const Bytes& data) {
    std::string result;
    for (std::size_t i = 0; i < data.size();) {
        const auto begin = i;
        const unsigned lead = data[i++];
        if (lead < 0x80) {
            result += static_cast<char>(lead);
            continue;
        }
        unsigned length = lead >= 0xc2 && lead <= 0xdf   ? 2
                          : lead >= 0xe0 && lead <= 0xef ? 3
                          : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                         : 0;
        if (!length) {
            result += "\xef\xbf\xbd";
            continue;
        }
        bool valid = true;
        for (unsigned j = 1; j < length; ++j) {
            if (i == data.size()) {
                valid = false;
                break;
            }
            const auto next = data[i];
            if (next < 0x80 || next > 0xbf ||
                (j == 1 && ((lead == 0xe0 && next < 0xa0) || (lead == 0xed && next >= 0xa0) ||
                            (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next >= 0x90)))) {
                valid = false;
                break;
            }
            ++i;
        }
        if (valid)
            result.append(reinterpret_cast<const char*>(data.data() + begin), length);
        else
            result += "\xef\xbf\xbd";
    }
    return result;
}

std::vector<std::string> parse_types(const Bytes& data) {
    std::vector<std::string> result;
    for (const auto& line : lines(utf8_lossy(data))) {
        auto mime = trim(line);
        if (!mime.empty()) result.push_back(std::move(mime));
    }
    return result;
}

std::optional<MimeChoice> choose_mime(const std::vector<std::string>& types, Direction direction) {
    for (const auto* name : {"x-special/gnome-copied-files", "text/uri-list"})
        if (auto mime = find_type(types, name)) return MimeChoice{*mime, "text/uri-list", "files", DataMode::UriList};

    for (const auto* name : {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/tiff",
                             "image/svg+xml", "image/x-xpixmap"})
        if (auto mime = find_type(types, name)) return MimeChoice{*mime, name, name, DataMode::Raw};

    for (const auto& mime : types) {
        const auto canonical = ascii_lower(mime);
        if (starts_with(canonical, "image/")) return MimeChoice{mime, canonical, canonical, DataMode::Raw};
    }
    if (auto mime = find_type(types, "application/x-qt-image"))
        return MimeChoice{*mime, "application/x-qt-image", "application/x-qt-image", DataMode::Raw};

    std::optional<std::string> text;
    for (const auto* name : {"text/plain;charset=utf-8", "text/plain;charset=UTF-8", "text/plain", "UTF8_STRING",
                             "COMPOUND_TEXT", "TEXT", "STRING"}) {
        text = find_type(types, name);
        if (text) break;
    }
    if (!text)
        for (const auto& mime : types) {
            const auto lowered = ascii_lower(mime);
            if (starts_with(lowered, "text/plain;") && lowered.find("charset=utf-8") != std::string::npos) {
                text = mime;
                break;
            }
        }
    if (text)
        return MimeChoice{*text, direction == Direction::XToWayland ? "text/plain;charset=utf-8" : "UTF8_STRING",
                          "text/plain", DataMode::Raw};
    if (auto mime = find_type(types, "text/html")) return MimeChoice{*mime, "text/html", "text/html", DataMode::Raw};
    return std::nullopt;
}

Bytes normalize_uri_list(const Bytes& data) {
    std::string result;
    for (const auto& source : lines(utf8_lossy(data))) {
        auto line = trim(source);
        while (!line.empty() && line.back() == '\0') line.pop_back();
        const auto lowered = ascii_lower(line);
        if (line.empty() || lowered == "copy" || lowered == "cut") continue;
        if (line.front() == '/') result += "file://";
        result += line;
        result += '\n';
    }
    return Bytes(result.begin(), result.end());
}

Bytes prepare_data(const Bytes& data, DataMode mode) { return mode == DataMode::Raw ? data : normalize_uri_list(data); }

std::optional<Fingerprint> fingerprint(const std::string& kind, const Bytes& data) {
    if (data.empty()) return std::nullopt;
    const auto hash = XXH3_128bits(data.data(), data.size());
    return Fingerprint{kind, hash.low64, hash.high64};
}

SyncState::SyncState() : x11_provider(std::make_unique<X11Provider>()) {}
SyncState::~SyncState() = default;

void sync_clipboard(SyncState& state, Direction direction) {
    std::lock_guard<std::mutex> guard(state.mutex);
    if (state.x11_provider->reap()) state.x11.reset();
    const bool from_x = direction == Direction::XToWayland;
    auto& source = from_x ? state.x11 : state.wayland;
    auto& target = from_x ? state.wayland : state.x11;
    const auto types = from_x ? read_clipboard("xclip", {"-selection", "clipboard", "-t", "TARGETS", "-o"})
                              : read_clipboard("wl-paste", {"--list-types"});
    if (!types) {
        // Failure means unknown, not empty: clearing this fingerprint can turn
        // a failed echo read into another publication of the same large image.
        return;
    }
    const auto choice = choose_mime(parse_types(*types), direction);
    if (!choice) {
        source.reset();
        return;
    }
    const auto data = from_x ? read_clipboard("xclip", {"-selection", "clipboard", "-o", "-t", choice->read_mime})
                             : read_clipboard("wl-paste", {"-n", "-t", choice->read_mime});
    if (!data) {
        return;
    }
    const auto prepared = prepare_data(*data, choice->mode);
    const auto current = fingerprint(choice->kind, prepared);
    if (!current) {
        source.reset();
        return;
    }
    source = current;
    if (target == current) return;
    std::ostringstream message;
    message << "同步 " << choice->write_mime << "，" << prepared.size() << " 字节 (Hash: " << std::hex << std::setw(8)
            << std::setfill('0') << (current->high >> 32) << ')';
    log(from_x ? "X2W" : "W2X", message.str());
    const bool written = from_x ? write_clipboard_with_retry("wl-copy", {"-t", choice->write_mime}, prepared)
                                : state.x11_provider->publish(choice->write_mime, prepared);
    if (written)
        target = current;
    else
        log("WARN", from_x ? "X11 → Wayland 同步失败；保留状态以便下次事件重试"
                           : "Wayland → X11 同步失败；保留状态以便下次事件重试");
}
}  // namespace clipsync
