use chrono::Utc;
use memfd::MemfdOptions;
use std::env;
use std::fs;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::path::Path;
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use xxhash_rust::xxh3::xxh3_128;

const CLIPBOARD_READ_TIMEOUT: Duration = Duration::from_secs(10);
const CLIPBOARD_WRITE_TIMEOUT: Duration = Duration::from_secs(10);
const EVENT_DEBOUNCE: Duration = Duration::from_millis(30);
const WATCH_RESTART_DELAY: Duration = Duration::from_secs(1);
const WRITE_RETRY_DELAY: Duration = Duration::from_millis(100);

const PREFERRED_IMAGE_MIMES: &[&str] = &[
    "image/png",
    "image/jpeg",
    "image/gif",
    "image/webp",
    "image/bmp",
    "image/tiff",
    "image/svg+xml",
    "image/x-xpixmap",
];

#[derive(Clone, Debug, Eq, PartialEq)]
struct Fingerprint {
    kind: String,
    hash: u128,
}

#[derive(Default)]
struct SyncState {
    x11: Option<Fingerprint>,
    wayland: Option<Fingerprint>,
}

#[derive(Clone, Copy)]
enum Direction {
    XToWayland,
    WaylandToX,
}

#[derive(Clone, Copy)]
enum DataMode {
    Raw,
    UriList,
}

struct MimeChoice {
    read_mime: String,
    write_mime: String,
    kind: String,
    mode: DataMode,
}

fn log(level: &str, msg: &str) {
    let now = Utc::now().format("%H:%M:%S");
    println!("[{}] [{}] {}", now, level, msg);
}

fn wait_with_timeout(mut child: Child, timeout: Duration) -> Option<ExitStatus> {
    let start = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => return Some(status),
            Ok(None) if start.elapsed() >= timeout => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
            Ok(None) => thread::sleep(Duration::from_millis(10)),
            Err(_) => return None,
        }
    }
}

fn read_clipboard(cmd: &str, args: &[&str]) -> Option<Vec<u8>> {
    let mfd = MemfdOptions::default().create("clip_read").ok()?;
    let file = mfd.into_file();
    let file_out = file.try_clone().ok()?;

    let child = match Command::new(cmd)
        .args(args)
        .stdout(Stdio::from(file_out))
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            log("ERROR", &format!("无法启动 {cmd}: {error}"));
            return None;
        }
    };

    match wait_with_timeout(child, CLIPBOARD_READ_TIMEOUT) {
        Some(status) if status.success() => {}
        Some(_) => return None,
        None => {
            log("WARN", &format!("读取剪贴板超时: {cmd} {}", args.join(" ")));
            return None;
        }
    }

    let mut data = Vec::new();
    let mut file_read = file;
    file_read.seek(SeekFrom::Start(0)).ok()?;
    file_read.read_to_end(&mut data).ok()?;
    Some(data)
}

fn write_clipboard(cmd: &str, args: &[&str], data: &[u8]) -> bool {
    let Ok(mfd) = MemfdOptions::default().create("clip_write") else {
        log("ERROR", "创建剪贴板写入缓冲区失败");
        return false;
    };
    let mut file = mfd.into_file();
    if file.write_all(data).is_err() || file.seek(SeekFrom::Start(0)).is_err() {
        log("ERROR", "准备剪贴板写入数据失败");
        return false;
    }

    let child = match Command::new(cmd)
        .args(args)
        .stdin(Stdio::from(file))
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            log("ERROR", &format!("无法启动 {cmd}: {error}"));
            return false;
        }
    };

    match wait_with_timeout(child, CLIPBOARD_WRITE_TIMEOUT) {
        Some(status) if status.success() => true,
        Some(status) => {
            log(
                "WARN",
                &format!("写入剪贴板失败: {cmd} {} ({status})", args.join(" ")),
            );
            false
        }
        None => {
            log("WARN", &format!("写入剪贴板超时: {cmd} {}", args.join(" ")));
            false
        }
    }
}

fn write_clipboard_with_retry(cmd: &str, args: &[&str], data: &[u8]) -> bool {
    if write_clipboard(cmd, args, data) {
        return true;
    }
    thread::sleep(WRITE_RETRY_DELAY);
    log("INFO", "重试剪贴板写入");
    write_clipboard(cmd, args, data)
}

fn parse_types(data: &[u8]) -> Vec<String> {
    String::from_utf8_lossy(data)
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(ToOwned::to_owned)
        .collect()
}

fn find_type(types: &[String], wanted: &str) -> Option<String> {
    types
        .iter()
        .find(|mime| mime.eq_ignore_ascii_case(wanted))
        .cloned()
}

fn choose_mime(types: &[String], direction: Direction) -> Option<MimeChoice> {
    if let Some(mime) = find_type(types, "x-special/gnome-copied-files") {
        return Some(MimeChoice {
            read_mime: mime,
            write_mime: "text/uri-list".to_owned(),
            kind: "files".to_owned(),
            mode: DataMode::UriList,
        });
    }

    if let Some(mime) = find_type(types, "text/uri-list") {
        return Some(MimeChoice {
            read_mime: mime,
            write_mime: "text/uri-list".to_owned(),
            kind: "files".to_owned(),
            mode: DataMode::UriList,
        });
    }

    for preferred in PREFERRED_IMAGE_MIMES {
        if let Some(mime) = find_type(types, preferred) {
            return Some(MimeChoice {
                read_mime: mime,
                write_mime: (*preferred).to_owned(),
                kind: preferred.to_ascii_lowercase(),
                mode: DataMode::Raw,
            });
        }
    }

    if let Some(mime) = types
        .iter()
        .find(|mime| mime.to_ascii_lowercase().starts_with("image/"))
    {
        let canonical_mime = mime.to_ascii_lowercase();
        return Some(MimeChoice {
            read_mime: mime.clone(),
            write_mime: canonical_mime.clone(),
            kind: canonical_mime,
            mode: DataMode::Raw,
        });
    }

    if let Some(mime) = find_type(types, "application/x-qt-image") {
        return Some(MimeChoice {
            read_mime: mime,
            write_mime: "application/x-qt-image".to_owned(),
            kind: "application/x-qt-image".to_owned(),
            mode: DataMode::Raw,
        });
    }

    let text_mime = [
        "text/plain;charset=utf-8",
        "text/plain;charset=UTF-8",
        "text/plain",
        "UTF8_STRING",
        "COMPOUND_TEXT",
        "TEXT",
        "STRING",
    ]
    .iter()
    .find_map(|wanted| find_type(types, wanted))
    .or_else(|| {
        types
            .iter()
            .find(|mime| {
                let mime = mime.to_ascii_lowercase();
                mime.starts_with("text/plain;") && mime.contains("charset=utf-8")
            })
            .cloned()
    });

    if let Some(read_mime) = text_mime {
        let write_mime = match direction {
            Direction::XToWayland => "text/plain;charset=utf-8",
            Direction::WaylandToX => "UTF8_STRING",
        };
        return Some(MimeChoice {
            read_mime,
            write_mime: write_mime.to_owned(),
            kind: "text/plain".to_owned(),
            mode: DataMode::Raw,
        });
    }

    if let Some(mime) = find_type(types, "text/html") {
        return Some(MimeChoice {
            read_mime: mime,
            write_mime: "text/html".to_owned(),
            kind: "text/html".to_owned(),
            mode: DataMode::Raw,
        });
    }

    None
}

fn normalize_uri_list(data: &[u8]) -> Vec<u8> {
    let source = String::from_utf8_lossy(data);
    let mut result = String::new();

    for line in source.lines() {
        let line = line.trim().trim_end_matches('\0');
        if line.is_empty() || line.eq_ignore_ascii_case("copy") || line.eq_ignore_ascii_case("cut")
        {
            continue;
        }

        if line.starts_with('/') {
            result.push_str("file://");
        }
        result.push_str(line);
        result.push('\n');
    }

    result.into_bytes()
}

fn prepare_data(data: &[u8], mode: DataMode) -> Vec<u8> {
    match mode {
        DataMode::Raw => data.to_vec(),
        DataMode::UriList => normalize_uri_list(data),
    }
}

fn fingerprint(kind: &str, data: &[u8]) -> Option<Fingerprint> {
    if data.is_empty() {
        return None;
    }
    Some(Fingerprint {
        kind: kind.to_owned(),
        hash: xxh3_128(data),
    })
}

fn sync_x_to_wayland(shared_state: &Mutex<SyncState>) {
    let mut state = shared_state.lock().unwrap();
    let Some(types_raw) =
        read_clipboard("xclip", &["-selection", "clipboard", "-t", "TARGETS", "-o"])
    else {
        state.x11 = None;
        return;
    };
    let types = parse_types(&types_raw);
    let Some(choice) = choose_mime(&types, Direction::XToWayland) else {
        state.x11 = None;
        return;
    };
    let Some(source_data) = read_clipboard(
        "xclip",
        &["-selection", "clipboard", "-o", "-t", &choice.read_mime],
    ) else {
        state.x11 = None;
        return;
    };
    let write_data = prepare_data(&source_data, choice.mode);
    let Some(current) = fingerprint(&choice.kind, &write_data) else {
        state.x11 = None;
        return;
    };

    state.x11 = Some(current.clone());
    if state.wayland.as_ref() == Some(&current) {
        return;
    }

    log(
        "X2W",
        &format!(
            "同步 {}，{} 字节 (Hash: {:08x})",
            choice.write_mime,
            write_data.len(),
            (current.hash >> 96) as u32
        ),
    );
    if write_clipboard_with_retry("wl-copy", &["-t", &choice.write_mime], &write_data) {
        state.wayland = Some(current);
    } else {
        log("WARN", "X11 → Wayland 同步失败；保留状态以便下次事件重试");
    }
}

fn sync_wayland_to_x(shared_state: &Mutex<SyncState>) {
    let mut state = shared_state.lock().unwrap();
    let Some(types_raw) = read_clipboard("wl-paste", &["--list-types"]) else {
        state.wayland = None;
        return;
    };
    let types = parse_types(&types_raw);
    let Some(choice) = choose_mime(&types, Direction::WaylandToX) else {
        state.wayland = None;
        return;
    };
    let Some(source_data) = read_clipboard("wl-paste", &["-n", "-t", &choice.read_mime]) else {
        state.wayland = None;
        return;
    };
    let write_data = prepare_data(&source_data, choice.mode);
    let Some(current) = fingerprint(&choice.kind, &write_data) else {
        state.wayland = None;
        return;
    };

    state.wayland = Some(current.clone());
    if state.x11.as_ref() == Some(&current) {
        return;
    }

    log(
        "W2X",
        &format!(
            "同步 {}，{} 字节 (Hash: {:08x})",
            choice.write_mime,
            write_data.len(),
            (current.hash >> 96) as u32
        ),
    );
    if write_clipboard_with_retry(
        "xclip",
        &["-selection", "clipboard", "-i", "-t", &choice.write_mime],
        &write_data,
    ) {
        state.x11 = Some(current);
    } else {
        log("WARN", "Wayland → X11 同步失败；保留状态以便下次事件重试");
    }
}

fn command_exists(name: &str) -> bool {
    let Some(path) = env::var_os("PATH") else {
        return false;
    };
    env::split_paths(&path).any(|dir| {
        let candidate = dir.join(name);
        candidate.is_file()
            && candidate
                .metadata()
                .map(|metadata| {
                    #[cfg(unix)]
                    {
                        use std::os::unix::fs::PermissionsExt;
                        metadata.permissions().mode() & 0o111 != 0
                    }
                    #[cfg(not(unix))]
                    {
                        true
                    }
                })
                .unwrap_or(false)
    })
}

fn check_dependencies() {
    let missing: Vec<&str> = ["xclip", "wl-copy", "wl-paste", "clipnotify"]
        .into_iter()
        .filter(|name| !command_exists(name))
        .collect();
    if !missing.is_empty() {
        log("FATAL", &format!("缺少依赖命令: {}", missing.join(", ")));
        std::process::exit(1);
    }
}

fn get_xdg_runtime_dir() -> String {
    env::var("XDG_RUNTIME_DIR")
        .unwrap_or_else(|_| format!("/run/user/{}", unsafe { libc::getuid() }))
}

fn discover_wayland_display(xdg_runtime_dir: &str) -> String {
    if let Ok(display) = env::var("WAYLAND_DISPLAY") {
        if !display.is_empty() {
            return display;
        }
    }

    let Ok(entries) = fs::read_dir(xdg_runtime_dir) else {
        return String::new();
    };
    entries
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .find(|name| name.starts_with("wayland-") && !name.contains('.'))
        .unwrap_or_default()
}

fn discover_x11_display() -> String {
    if let Ok(display) = env::var("DISPLAY") {
        if !display.is_empty() {
            return display;
        }
    }

    let Ok(entries) = fs::read_dir("/tmp/.X11-unix") else {
        return String::new();
    };
    entries
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .find_map(|name| name.strip_prefix('X').map(|number| format!(":{number}")))
        .unwrap_or_default()
}

fn main() {
    check_dependencies();

    let xdg_runtime_dir = get_xdg_runtime_dir();
    env::set_var("XDG_RUNTIME_DIR", &xdg_runtime_dir);

    if env::var("XAUTHORITY").is_err() {
        if let Ok(home) = env::var("HOME") {
            let candidate = format!("{home}/.Xauthority");
            if Path::new(&candidate).exists() {
                env::set_var("XAUTHORITY", candidate);
            }
        }
    }

    let wayland_display = discover_wayland_display(&xdg_runtime_dir);
    let display = discover_x11_display();
    if wayland_display.is_empty() || display.is_empty() {
        log("FATAL", "找不到 X11 或 Wayland 显示服务");
        std::process::exit(1);
    }
    env::set_var("WAYLAND_DISPLAY", &wayland_display);
    env::set_var("DISPLAY", &display);

    log(
        "INIT",
        &format!("DISPLAY={display}, WAYLAND_DISPLAY={wayland_display}"),
    );

    let shared_state = Arc::new(Mutex::new(SyncState::default()));
    let x11_state = Arc::clone(&shared_state);
    thread::spawn(move || {
        log("INFO", "X11 监听线程已启动");
        loop {
            match Command::new("clipnotify").status() {
                Ok(status) if status.success() => {
                    thread::sleep(EVENT_DEBOUNCE);
                    sync_x_to_wayland(&x11_state);
                }
                Ok(status) => {
                    log("WARN", &format!("clipnotify 异常退出 ({status})，稍后重试"));
                    thread::sleep(WATCH_RESTART_DELAY);
                }
                Err(error) => {
                    log("ERROR", &format!("无法启动 clipnotify: {error}"));
                    thread::sleep(WATCH_RESTART_DELAY);
                }
            }
        }
    });

    log("INFO", "Wayland 监听线程已启动");
    log("SYS", "双向剪贴板同步服务已准备就绪");

    loop {
        let mut watcher = match Command::new("wl-paste")
            .args(["--watch", "echo"])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(child) => child,
            Err(error) => {
                log("ERROR", &format!("无法启动 Wayland 监听器: {error}"));
                thread::sleep(WATCH_RESTART_DELAY);
                continue;
            }
        };

        if let Some(stdout) = watcher.stdout.take() {
            for line in BufReader::new(stdout).lines() {
                match line {
                    Ok(_) => {
                        thread::sleep(EVENT_DEBOUNCE);
                        sync_wayland_to_x(&shared_state);
                    }
                    Err(error) => {
                        log("WARN", &format!("读取 Wayland 监听事件失败: {error}"));
                        break;
                    }
                }
            }
        }

        let _ = watcher.kill();
        let _ = watcher.wait();
        log("WARN", "Wayland 监听器已退出，稍后重启");
        thread::sleep(WATCH_RESTART_DELAY);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn types(items: &[&str]) -> Vec<String> {
        items.iter().map(|item| (*item).to_owned()).collect()
    }

    #[test]
    fn standard_image_wins_over_qt_fallback() {
        let choice = choose_mime(
            &types(&["application/x-qt-image", "image/png"]),
            Direction::XToWayland,
        )
        .unwrap();
        assert_eq!(choice.read_mime, "image/png");
        assert_eq!(choice.write_mime, "image/png");
    }

    #[test]
    fn qt_image_is_not_treated_as_uri_list() {
        let choice =
            choose_mime(&types(&["application/x-qt-image"]), Direction::WaylandToX).unwrap();
        assert_eq!(choice.write_mime, "application/x-qt-image");
        assert!(matches!(choice.mode, DataMode::Raw));
    }

    #[test]
    fn gif_and_unknown_images_are_supported() {
        let gif = choose_mime(&types(&["image/gif"]), Direction::XToWayland).unwrap();
        assert_eq!(gif.write_mime, "image/gif");

        let avif = choose_mime(&types(&["image/avif"]), Direction::XToWayland).unwrap();
        assert_eq!(avif.write_mime, "image/avif");
    }

    #[test]
    fn whitespace_is_significant_in_text_fingerprints() {
        let compact = fingerprint("text/plain", b"ab").unwrap();
        let spaced = fingerprint("text/plain", b"a b").unwrap();
        let whitespace = fingerprint("text/plain", b" \n\t").unwrap();
        assert_ne!(compact, spaced);
        assert_ne!(spaced, whitespace);
    }

    #[test]
    fn parameterized_utf8_text_is_supported() {
        let choice = choose_mime(
            &types(&["text/plain;format=flowed;charset=UTF-8"]),
            Direction::XToWayland,
        )
        .unwrap();
        assert_eq!(choice.write_mime, "text/plain;charset=utf-8");
    }

    #[test]
    fn gnome_file_list_is_normalized() {
        let normalized = normalize_uri_list(b"copy\n/home/user/a.txt\nfile:///tmp/b.txt\n");
        assert_eq!(normalized, b"file:///home/user/a.txt\nfile:///tmp/b.txt\n");
    }
}
