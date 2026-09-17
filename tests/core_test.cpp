#include <cstdlib>
#include <iostream>
#include <string>

#include "clipboard.hpp"

using namespace clipsync;
namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
Bytes bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }
void choice(const std::vector<std::string>& types, Direction direction, const std::string& read,
            const std::string& write, const std::string& kind, DataMode mode = DataMode::Raw) {
    const auto selected = choose_mime(types, direction);
    check(selected.has_value(), "missing MIME choice");
    check(selected->read_mime == read && selected->write_mime == write && selected->kind == kind &&
              selected->mode == mode,
          "unexpected MIME choice");
}
}  // namespace

int main() {
    const auto xw = Direction::XToWayland, wx = Direction::WaylandToX;
    choice({"application/x-qt-image", "image/png"}, xw, "image/png", "image/png", "image/png");
    choice({"text/html", "UTF8_STRING", "image/jpeg", "text/uri-list", "x-special/gnome-copied-files"}, wx,
           "x-special/gnome-copied-files", "text/uri-list", "files", DataMode::UriList);
    choice({"application/x-qt-image"}, wx, "application/x-qt-image", "application/x-qt-image",
           "application/x-qt-image");
    for (const auto* mime : {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/tiff",
                             "image/svg+xml", "image/x-xpixmap", "image/avif"})
        choice({"text/plain", mime}, xw, mime, mime, mime);
    choice({"image/jpeg", "IMAGE/PNG"}, wx, "IMAGE/PNG", "image/png", "image/png");
    choice({"Image/AVIF", "image/heic"}, xw, "Image/AVIF", "image/avif", "image/avif");
    for (const auto* mime : {"text/plain;charset=UTF-8", "text/plain", "UTF8_STRING", "COMPOUND_TEXT", "TEXT", "STRING",
                             "text/plain;format=flowed;charset=UTF-8"}) {
        choice({"text/html", mime}, xw, mime, "text/plain;charset=utf-8", "text/plain");
        choice({"text/html", mime}, wx, mime, "UTF8_STRING", "text/plain");
    }
    choice({"text/plain;format=flowed;charset=UTF-8", "STRING"}, wx, "STRING", "UTF8_STRING", "text/plain");
    choice({"text/html"}, wx, "text/html", "text/html", "text/html");
    check(!choose_mime({"application/pdf", "TARGETS"}, xw), "unsupported MIME must be ignored");
    check(parse_types(bytes("\r\n \tIMAGE/PNG \r\n\xc2\xa0text/plain\xe3\x80\x80\n")) ==
              std::vector<std::string>({"IMAGE/PNG", "text/plain"}),
          "Unicode MIME trimming failed");
    check(normalize_uri_list(bytes("copy\n/home/user/a.txt\nfile:///tmp/b.txt\n")) ==
              bytes("file:///home/user/a.txt\nfile:///tmp/b.txt\n"),
          "GNOME normalization failed");
    check(normalize_uri_list(bytes(std::string(" CuT\r\n\xc2\xa0/tmp/a b\xe3\x80\x80\r\nfile:///b") +
                                   std::string(2, '\0') + '\n')) == bytes("file:///tmp/a b\nfile:///b\n"),
          "URI whitespace/NUL normalization failed");
    check(normalize_uri_list(bytes("#comment\nhttps://example.com\ncopy\nCUT")) ==
              bytes("#comment\nhttps://example.com\n"),
          "URI lines must follow original behavior");
    check(utf8_lossy({0xe1, 0x80, 'a', 0xff, 0xf0, 0x9f}) ==
              "\xef\xbf\xbd"
              "a"
              "\xef\xbf\xbd\xef\xbf\xbd",
          "invalid UTF-8 must match Rust replacement boundaries");
    check(utf8_lossy({0xed, 0xa0, 0x80}) == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd", "surrogate UTF-8 accepted");
    const Bytes binary{0, 255, 13, 10, 9, 32, 0};
    check(prepare_data(binary, DataMode::Raw) == binary, "raw bytes changed");
    check(!fingerprint("text/plain", {}), "empty clipboard must be skipped");
    check(fingerprint("text/plain", bytes("ab")) != fingerprint("text/plain", bytes("a b")), "spaces must affect hash");
    check(fingerprint("text/plain", bytes("a b")) != fingerprint("text/plain", bytes(" \n\t")), "whitespace lost");
    check(fingerprint("image/png", binary) != fingerprint("image/jpeg", binary), "MIME must affect fingerprint");
    const auto known_hash = fingerprint("text/plain", bytes("abc"));
    check(known_hash->high == 0x06b05ab6733a6185ULL && known_hash->low == 0x78af5f94892f3950ULL,
          "XXH3-128 compatibility vector failed");
    std::cout << "core tests passed\n";
}
