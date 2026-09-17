#pragma once

#include <sys/types.h>

#include <chrono>

#include "clipboard.hpp"

namespace clipsync {
inline constexpr auto clipboard_read_timeout = std::chrono::seconds(10);
inline constexpr auto clipboard_write_timeout = std::chrono::seconds(10);
inline constexpr auto event_debounce = std::chrono::milliseconds(30);
inline constexpr auto watch_restart_delay = std::chrono::seconds(1);
inline constexpr auto write_retry_delay = std::chrono::milliseconds(100);
using Arguments = std::vector<std::string>;

class FileDescriptor {
   public:
    explicit FileDescriptor(int fd = -1) : fd_(fd) {}
    ~FileDescriptor();
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return fd_; }

   private:
    int fd_;
};

void log(const std::string& level, const std::string& message);
std::string join(const Arguments& args, const std::string& separator = " ");
std::string status_string(int status);
std::optional<pid_t> spawn_command(const std::string& command, const Arguments& args, int input = -1, int output = -1,
                                   bool quiet_errors = false);
std::optional<int> wait_child(pid_t child);
void kill_and_wait(pid_t child);
std::optional<int> wait_with_timeout(pid_t child, std::chrono::milliseconds timeout);
std::optional<Bytes> read_clipboard(const std::string& command, const Arguments& args);
bool write_clipboard(const std::string& command, const Arguments& args, const Bytes& data);
bool write_clipboard_with_retry(const std::string& command, const Arguments& args, const Bytes& data);
bool make_pipe(int descriptors[2]);
}  // namespace clipsync
