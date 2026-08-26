# Clipboard Sync

在 X11 与 Wayland 的 `CLIPBOARD` 之间进行低延迟双向同步，解决 LinuxQQ 等 XWayland/Wayland 混合应用的剪贴板兼容问题。

原仓库[linuxqq-clipsync](https://github.com/SHORiN-KiWATA/linuxqq-clipsync)能用，就是想自己改一点东西于是fork了（

## 功能

- 使用 `clipnotify` 和 `wl-paste --watch` 监听变化，避免持续轮询。
- 同步 UTF-8 文本，并保留空格、换行和 Tab。
- 同步 PNG、JPEG、GIF、WebP、BMP、TIFF、SVG、X PixMap，以及其他 `image/*` MIME。
- 支持 `application/x-qt-image`，但标准图片 MIME 存在时优先使用标准格式。
- 支持 `text/uri-list` 和 `x-special/gnome-copied-files` 文件列表。
- 在没有纯文本格式时同步 `text/html`。
- 根据内容和 MIME 指纹防止双向回环，不会屏蔽固定时间窗口内的真实复制事件。
- 剪贴板读写具有超时保护，监听器异常退出后会自动恢复。
- 启动时检查依赖，并在需要时自动寻找 X11、Wayland 显示 socket。

一次复制通常会提供多个 MIME。本程序按照“文件列表、标准图片、Qt 图片、纯文本、HTML”的顺序选择兼容性较好的单一格式进行同步。

## 依赖

- `xclip`
- `wl-clipboard`（提供 `wl-copy` 和 `wl-paste`）
- `clipnotify`

## 安装

进入`package/arch`文件夹，执行`makepkg -si`

也可以从源码构建：

```sh
cargo build --release --locked
```

## 使用

直接运行：

```sh
clipboard-sync
```

更推荐启用 systemd 用户服务：

```sh
systemctl enable --user --now clipboard-sync.service
```

查看运行日志：

```sh
journalctl --user -u clipboard-sync.service -f
```
