# Clipboard Sync（C++17）

在 X11 与 Wayland 的 `CLIPBOARD` 之间进行低延迟双向同步。

## 对应行为

- 用 `clipnotify` 监听 X11；用 `wl-paste --watch echo` 监听 Wayland，不持续轮询剪贴板。
- 每次事件等待 30 ms，再读取当前内容；两个方向通过同一个互斥锁串行同步。
- MIME 优先级与原版一致：GNOME 文件列表、URI 文件列表、标准图片、其他 `image/*`、Qt 图片、纯文本、HTML。每次只同步一种格式。
- 标准图片优先级：PNG、JPEG、GIF、WebP、BMP、TIFF、SVG、X PixMap。支持 `application/x-qt-image` 和未知图片 MIME。
- 支持 UTF-8 纯文本、参数化的 UTF-8 MIME，以及 `UTF8_STRING`、`COMPOUND_TEXT`、`TEXT`、`STRING`。原样传递字节，保留空格、换行、Tab 和二进制数据。
- `x-special/gnome-copied-files` 和 `text/uri-list` 转为 `text/uri-list`：去掉 `copy` / `cut` 行，对绝对路径加 `file://`，统一输出换行。保持原版逻辑，不额外 URL 编码或解析注释。
- 使用同样的 MIME 类别和 XXH3-128 内容指纹防止回环。没有固定时间屏蔽窗口；空内容和不支持的格式重置来源指纹，不清空目标。读取失败时保留指纹，避免一次失败的回环读取导致同一张大图被反复发布。
- X11 写入由随程序提供的 `clipboard-sync-x11` 通过 libxcb 处理，不再使用 `xclip -i` 的后台进程。主程序收到所有权就绪确认后才记录成功，并跟踪、回收实际提供数据的进程。
- 大图片使用 X11 `INCR` 协议。每个请求窗口和属性分别维护传输进度，某个接收方停滞时仍能响应其他图片请求和 `TARGETS` 查询；接收窗口销毁或连续 10 秒不推进时释放该传输。超时的请求需要接收方重新发起，不会把半张图片伪装成成功传输。
- 读取命令、Wayland 写入命令和 X11 提供进程的启动各有 10 秒超时；写入失败等待 100 ms 后重试一次，仍失败则等待后续事件重试。X11 提供进程确认就绪后会持续运行，不受启动超时限制。
- 更换 X11 剪贴板内容时，旧提供进程可以完成已经接受的传输；主程序退出时提供进程随之退出。提供进程异常退出后，下一次来源事件可以重新发布相同内容。
- 监听器异常后等待 1 秒恢复。启动时检查四个外部命令和同目录的 X11 提供程序，并寻找 X11 / Wayland 显示名称。
- 保留 `XDG_RUNTIME_DIR`、`XAUTHORITY` 回退逻辑，以及 UTC 时间的中文日志。不增加命令行参数；启动后等待事件，不主动同步现有内容。

与原版一样，同步对象是 `CLIPBOARD`，没有同步 X11 的 `PRIMARY` 选择区。

## 依赖和构建

运行需要 Linux 上的 X11 / Wayland 会话，以及：

- `xclip`
- `wl-clipboard`（`wl-copy`、`wl-paste`）
- `clipnotify`
- `libxcb`（构建时还需要头文件和 `pkg-config` / `pkgconf`）

仅支持以 Linux 为构建目标，需要 C++17 编译器、CMake 3.16 或更新版本和 POSIX 线程库。
xxHash 官方头文件已随源码附带，可离线构建。Python 3 用于集成测试；可选的 `Xvfb` 和 `libX11` 用于独立显示服务器上的真实 X11 协议测试。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/clipboard-sync
```

Linux 内存缓冲使用 `memfd_create`。CMake 会拒绝非 Linux 目标的配置。

## 编译和模拟测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

模拟测试在 Linux 上运行，需要 Python 3，但不需要安装 `xclip`、`wl-clipboard`、`clipnotify`，也不需要 X11 / Wayland 显示服务器。
集成测试会在临时目录自动创建模拟命令，并启动编译好的同步程序。测试包含两个 10 秒超时场景，整套通常需要约 35 秒。

`incr` 测试不需要显示服务器，覆盖 3,213,813 字节图片的独立分块进度、并发请求、接收方中断、超时和资源上限。
`x11_protocol` 测试会启动独立的 Xvfb，检查真实 X11 的并发图片读取、`TARGETS` 查询、接收方销毁、提供进程更换和小图片直接读取，不连接当前桌面。未安装 Xvfb 时，这项测试显示为跳过。

只运行集成测试，或直接运行脚本：

```sh
ctest --test-dir build -R '^integration$' --output-on-failure
python3 tests/integration_test.py ./build/clipboard-sync
```

如果 `ctest` 只列出 `core`，说明配置阶段没有找到 Python 3；指定解释器后重新配置：

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DPython3_EXECUTABLE="$(command -v python3)"
```

直接运行 `./build/clipboard-sync` 会检查真实的 Linux 剪贴板依赖。

## 安装

安装到 `/usr` 以配合提供的 systemd 用户服务：

```sh
sudo cmake --install build --prefix /usr
systemctl --user daemon-reload
systemctl enable --user --now clipboard-sync.service
journalctl --user -u clipboard-sync.service -f
```

若安装到其他前缀，需要相应修改服务的 `ExecStart=/usr/bin/clipboard-sync`。
`clipboard-sync` 和 `clipboard-sync-x11` 必须放在同一目录；`cmake --install` 会同时安装它们。
切换已有 Rust 服务时，先停止旧服务，安装后再启动，避免同时运行两个同步进程。

Arch Linux 可以从这个本地源码目录打包安装：

```sh
cd packaging/arch
makepkg -si
```

## 验证范围

核心测试覆盖 MIME 选择、Unicode / 无效 UTF-8 处理、URI 转换、字节保留和 XXH3-128 指纹。
集成测试启动实际 C++ 进程，用模拟的四个命令检查双向同步、1 MiB 二进制内容、回环抑制、写入重试、失败后的事件重试、10 秒读写超时、监听恢复、依赖检查和显示环境。
此外会模拟前台 X11 提供进程的就绪协议，检查 3.2 MB 图片读取失败后的去重、提供进程崩溃后的重新发布，以及无效就绪确认后的进程回收。
真实 X11 协议测试需安装 Xvfb；QQ 和 xwayland-satellite 的端到端兼容性仍需在实际桌面会话中验证。

移植时另用从参考源码提取的 Rust 纯数据函数，直接对比了 7,103 组输入，
覆盖随机字节、Unicode 空白、MIME 组合和 URI 列表；UTF-8 处理、MIME 选择、URI 转换及 XXH3-128 结果均一致。

## 许可证

沿用参考实现的 [MIT 许可证](LICENSE)。附带的 xxHash 使用 [BSD-2-Clause 许可证](third_party/xxhash/LICENSE)。
