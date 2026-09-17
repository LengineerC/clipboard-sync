#include "process.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iostream>
#include <thread>
#ifdef __linux__
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

extern char** environ;

namespace clipsync {
namespace {
int memory_file(const char* name) {
#ifdef __linux__
    return static_cast<int>(syscall(SYS_memfd_create, name, MFD_CLOEXEC));
#else
    // Enables building/testing on other POSIX hosts; Linux always uses memfd.
    (void)name;
    char path[] = "/tmp/clipboard-sync-XXXXXX";
    const int fd = mkstemp(path);
    if (fd >= 0) {
        unlink(path);
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
#endif
}
bool success(const std::optional<int>& status) { return status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0; }
}  // namespace

FileDescriptor::~FileDescriptor() {
    if (fd_ >= 0) close(fd_);
}

void log(const std::string& level, const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);
    const auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &utc);
    std::cout << '[' << stamp << "] [" << level << "] " << message << std::endl;
}

std::string join(const Arguments& args, const std::string& separator) {
    std::string result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) result += separator;
        result += args[i];
    }
    return result;
}

std::string status_string(int status) {
    if (WIFEXITED(status)) return "exit status: " + std::to_string(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return "signal: " + std::to_string(WTERMSIG(status));
    return "wait status: " + std::to_string(status);
}

std::optional<pid_t> spawn_command(const std::string& command, const Arguments& args, int input, int output,
                                   bool quiet_errors) {
    // posix_spawnp avoids running C++ code after fork in the two-thread daemon.
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        log("ERROR", "无法启动 " + command + ": " + std::strerror(error));
        return std::nullopt;
    }
    if (input >= 0) error = posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO);
    if (!error && output >= 0) error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    if (!error && quiet_errors)
        error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t child = -1;
    if (!error) error = posix_spawnp(&child, command.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
        log("ERROR", "无法启动 " + command + ": " + std::strerror(error));
        return std::nullopt;
    }
    return child;
}

std::optional<int> wait_child(pid_t child) {
    int status = 0;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child ? std::optional<int>(status) : std::nullopt;
}

void kill_and_wait(pid_t child) {
    kill(child, SIGKILL);
    (void)wait_child(child);
}

std::optional<int> wait_with_timeout(pid_t child, std::chrono::milliseconds timeout) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int status = 0;
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) return status;
        if (result < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (std::chrono::steady_clock::now() - start >= timeout) {
            kill_and_wait(child);
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::optional<Bytes> read_clipboard(const std::string& command, const Arguments& args) {
    FileDescriptor file(memory_file("clip_read"));
    if (file.get() < 0) return std::nullopt;
    const auto child = spawn_command(command, args, -1, file.get(), true);
    if (!child) return std::nullopt;
    const auto status = wait_with_timeout(*child, clipboard_read_timeout);
    if (!status) {
        log("WARN", "读取剪贴板超时: " + command + " " + join(args));
        return std::nullopt;
    }
    if (!success(status) || lseek(file.get(), 0, SEEK_SET) < 0) return std::nullopt;
    Bytes result;
    unsigned char buffer[65536];
    for (;;) {
        const auto count = read(file.get(), buffer, sizeof(buffer));
        if (count > 0)
            result.insert(result.end(), buffer, buffer + count);
        else if (!count)
            return result;
        else if (errno != EINTR)
            return std::nullopt;
    }
}

bool write_clipboard(const std::string& command, const Arguments& args, const Bytes& data) {
    FileDescriptor file(memory_file("clip_write"));
    if (file.get() < 0) {
        log("ERROR", "创建剪贴板写入缓冲区失败");
        return false;
    }
    for (std::size_t offset = 0; offset < data.size();) {
        const auto count = write(file.get(), data.data() + offset, data.size() - offset);
        if (count > 0)
            offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else {
            log("ERROR", "准备剪贴板写入数据失败");
            return false;
        }
    }
    if (lseek(file.get(), 0, SEEK_SET) < 0) {
        log("ERROR", "准备剪贴板写入数据失败");
        return false;
    }
    const auto child = spawn_command(command, args, file.get(), -1, true);
    if (!child) return false;
    const auto status = wait_with_timeout(*child, clipboard_write_timeout);
    if (success(status)) return true;
    if (status)
        log("WARN", "写入剪贴板失败: " + command + " " + join(args) + " (" + status_string(*status) + ")");
    else
        log("WARN", "写入剪贴板超时: " + command + " " + join(args));
    return false;
}

bool write_clipboard_with_retry(const std::string& command, const Arguments& args, const Bytes& data) {
    if (write_clipboard(command, args, data)) return true;
    std::this_thread::sleep_for(write_retry_delay);
    log("INFO", "重试剪贴板写入");
    return write_clipboard(command, args, data);
}

bool make_pipe(int descriptors[2]) {
#ifdef __linux__
    return pipe2(descriptors, O_CLOEXEC) == 0;
#else
    if (pipe(descriptors) < 0) return false;
    if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) == 0) return true;
    close(descriptors[0]);
    close(descriptors[1]);
    return false;
#endif
}
}  // namespace clipsync
