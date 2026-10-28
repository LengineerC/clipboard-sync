#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

#include "clipboard.hpp"
#include "process.hpp"

namespace {
using namespace clipsync;
namespace fs = std::filesystem;

std::optional<std::string> unicode_env(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    const std::string text(value);
    if (utf8_lossy(Bytes(text.begin(), text.end())) != text) return std::nullopt;
    return text;
}
bool command_exists(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    const std::string paths(path);
    for (std::size_t begin = 0;;) {
        const auto end = paths.find(':', begin);
        const auto directory = paths.substr(begin, end == std::string::npos ? end : end - begin);
        const auto candidate = fs::path(directory) / name;
        struct stat metadata{};
        if (stat(candidate.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) && (metadata.st_mode & 0111))
            return true;
        if (end == std::string::npos) return false;
        begin = end + 1;
    }
}
std::string discover_display(const char* variable, const std::string& directory, bool wayland) {
    const auto display = unicode_env(variable);
    if (display && !display->empty()) return *display;
    std::error_code error;
    fs::directory_iterator iterator(directory, error), end;
    while (!error && iterator != end) {
        const auto name = iterator->path().filename().string();
        // Match Rust's into_string(): skip filenames that are not UTF-8.
        if (utf8_lossy(Bytes(name.begin(), name.end())) == name) {
            if (wayland && name.compare(0, 8, "wayland-") == 0 && name.find('.') == std::string::npos) return name;
            if (!wayland && !name.empty() && name.front() == 'X') return ":" + name.substr(1);
        }
        iterator.increment(error);
    }
    return "";
}
void watch_x11(SyncState& state) {
    log("INFO", "X11 监听线程已启动");
    for (;;) {
        const auto child = spawn_command("clipnotify", {});
        const auto status = child ? wait_child(*child) : std::nullopt;
        if (status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0) {
            std::this_thread::sleep_for(event_debounce);
            sync_clipboard(state, Direction::XToWayland);
        } else {
            if (status) log("WARN", "clipnotify 异常退出 (" + status_string(*status) + ")，稍后重试");
            std::this_thread::sleep_for(watch_restart_delay);
        }
    }
}
void handle_wayland_line(SyncState& state, std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::this_thread::sleep_for(event_debounce);
    sync_clipboard(state, Direction::WaylandToX);
}
void read_wayland_events(SyncState& state, int fd) {
    std::string pending;
    char buffer[8192];
    for (;;) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) continue;
            log("WARN", "读取 Wayland 监听事件失败: " + std::string(std::strerror(errno)));
            return;
        }
        if (count == 0) {
            if (!pending.empty()) {
                if (utf8_lossy(Bytes(pending.begin(), pending.end())) != pending)
                    log("WARN", "读取 Wayland 监听事件失败: stream did not contain valid UTF-8");
                else
                    handle_wayland_line(state, pending);
            }
            return;
        }
        pending.append(buffer, static_cast<std::size_t>(count));
        for (;;) {
            const auto newline = pending.find('\n');
            if (newline == std::string::npos) break;
            const auto line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (utf8_lossy(Bytes(line.begin(), line.end())) != line) {
                log("WARN", "读取 Wayland 监听事件失败: stream did not contain valid UTF-8");
                return;
            }
            handle_wayland_line(state, line);
        }
    }
}
void watch_wayland(SyncState& state) {
    log("INFO", "Wayland 监听线程已启动");
    log("SYS", "双向剪贴板同步服务已准备就绪");
    for (;;) {
        int descriptors[2];
        if (!make_pipe(descriptors)) {
            log("ERROR", "无法启动 Wayland 监听器: " + std::string(std::strerror(errno)));
            std::this_thread::sleep_for(watch_restart_delay);
            continue;
        }
        FileDescriptor input(descriptors[0]);
        std::optional<pid_t> child;
        {
            FileDescriptor output(descriptors[1]);
            child = spawn_command("wl-paste", {"--watch", "echo"}, -1, output.get(), true);
        }
        if (!child) {
            std::this_thread::sleep_for(watch_restart_delay);
            continue;
        }
        read_wayland_events(state, input.get());
        kill_and_wait(*child);
        log("WARN", "Wayland 监听器已退出，稍后重启");
        std::this_thread::sleep_for(watch_restart_delay);
    }
}
}  // namespace

int main() {
    Arguments missing;
    for (const auto* name : {"xclip", "wl-copy", "wl-paste", "clipnotify"})
        if (!command_exists(name)) missing.emplace_back(name);
    const auto x11_provider = X11Provider::executable();
    if (x11_provider.empty() || access(x11_provider.c_str(), X_OK) != 0)
        missing.emplace_back("clipboard-sync-x11（须与主程序放在同一目录）");
    if (!missing.empty()) {
        log("FATAL", "缺少依赖命令: " + join(missing, ", "));
        return 1;
    }
    const auto runtime = unicode_env("XDG_RUNTIME_DIR").value_or("/run/user/" + std::to_string(getuid()));
    setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    if (!unicode_env("XAUTHORITY")) {
        if (const auto home = unicode_env("HOME")) {
            const auto authority = *home + "/.Xauthority";
            std::error_code error;
            if (fs::exists(authority, error)) setenv("XAUTHORITY", authority.c_str(), 1);
        }
    }
    const auto wayland = discover_display("WAYLAND_DISPLAY", runtime, true);
    const auto x11 = discover_display("DISPLAY", "/tmp/.X11-unix", false);
    if (wayland.empty() || x11.empty()) {
        log("FATAL", "找不到 X11 或 Wayland 显示服务");
        return 1;
    }
    setenv("WAYLAND_DISPLAY", wayland.c_str(), 1);
    setenv("DISPLAY", x11.c_str(), 1);
    log("INIT", "DISPLAY=" + x11 + ", WAYLAND_DISPLAY=" + wayland);
    SyncState state;
    std::thread([&state] { watch_x11(state); }).detach();
    watch_wayland(state);
}
