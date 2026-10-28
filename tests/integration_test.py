"""Exercise the daemon against mock command executables, without a display server."""
import base64
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time

MOCK = r'''
import base64, fcntl, json, os, pathlib, sys, time
root = pathlib.Path(os.environ['MOCK_ROOT'])
tool = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
def record(item):
    with (root / 'calls.jsonl').open('a') as file:
        fcntl.flock(file, fcntl.LOCK_EX)
        file.write(json.dumps(item) + '\n')
def consume(name):
    path = root / name
    if not path.exists(): return False
    value = int(path.read_text())
    if value <= 0: return False
    path.write_text(str(value - 1))
    return True
if tool == 'clipnotify':
    record({'watch': 'x', 'env': {key: os.environ.get(key) for key in
            ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR', 'XAUTHORITY')}})
    with (root / 'x.events').open() as fifo:
        # Read one byte at a time so a restarting clipnotify cannot consume the next event.
        event = ''
        while not event.endswith('\n'): event += fifo.read(1)
    sys.exit(1 if event.strip() == 'fail' else 0)
if tool == 'wl-paste' and args == ['--watch', 'echo']:
    record({'watch': 'w'})
    with (root / 'w.events').open() as fifo:
        for line in fifo:
            if line.strip() == 'exit': sys.exit(1)
            if line.strip() == 'invalid':
                sys.stdout.buffer.write(b'\xff\n'); sys.stdout.buffer.flush()
            else:
                print('', flush=True)
    sys.exit(0)
side = 'x' if tool in ('xclip', 'clipboard-sync-x11') else 'w'
writing = tool in ('wl-copy', 'clipboard-sync-x11') or '-i' in args
record({'tool': tool, 'args': args})
if writing:
    data = sys.stdin.buffer.read()
    mime = args[0] if tool == 'clipboard-sync-x11' else args[args.index('-t') + 1]
    failed = consume('fail_writes')
    record({'write': side, 'mime': mime, 'data': base64.b64encode(data).decode(), 'failed': failed, 'pid': os.getpid()})
    if consume('timeout_writes'): time.sleep(30)
    if failed: sys.exit(1)
    if tool == 'clipboard-sync-x11' and consume('invalid_ready'):
        print('BAD', flush=True)
        time.sleep(30)
    value = {mime: base64.b64encode(data).decode()}
    tmp = root / (side + '.tmp')
    tmp.write_text(json.dumps(value)); tmp.replace(root / (side + '.json'))
    if tool == 'clipboard-sync-x11':
        (root / 'x.owner').write_text(str(os.getpid()))
        # A foreground owner acknowledges publication without exiting.
        print('READY', flush=True)
        while True:
            if (root / 'x.owner').read_text() != str(os.getpid()): sys.exit(0)
            if json.loads((root / 'x.json').read_text()) != value: sys.exit(0)
            time.sleep(0.02)
else:
    if consume('timeout_reads'): time.sleep(30)
    if consume('fail_reads'): sys.exit(1)
    data = json.loads((root / (side + '.json')).read_text())
    if '--list-types' in args or ('-t' in args and args[args.index('-t') + 1] == 'TARGETS'):
        sys.stdout.buffer.write(('\n'.join(data) + '\n').encode())
    else:
        mime = args[args.index('-t') + 1]
        if mime not in data: sys.exit(1)
        sys.stdout.buffer.write(base64.b64decode(data[mime]))
'''


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def until(predicate, message, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError(message)


class Session:
    def __init__(self, binary, root):
        self.root = root
        self.fifos = {}
        for side in ('x', 'w'):
            os.mkfifo(root / (side + '.events'))
            self.fifos[side] = os.open(root / (side + '.events'), os.O_RDWR)
            self.set(side, {})
        (root / 'home').mkdir()
        (root / 'home' / '.Xauthority').touch()
        (root / 'wayland-test.lock').touch()
        (root / 'wayland-test').touch()
        bindir = root / 'bin'
        bindir.mkdir()
        for name in ('xclip', 'wl-copy', 'wl-paste', 'clipnotify', 'clipboard-sync-x11'):
            executable = bindir / name
            executable.write_text('#!' + sys.executable + '\n' + MOCK)
            executable.chmod(0o755)
        self.env = dict(os.environ, PATH=str(bindir), MOCK_ROOT=str(root),
                        XDG_RUNTIME_DIR=str(root), HOME=str(root / 'home'), DISPLAY=':mock')
        for name in ('WAYLAND_DISPLAY', 'XAUTHORITY'):
            self.env.pop(name, None)
        self.logfile = (root / 'daemon.log').open('w+')
        # The native owner is resolved beside the executable, in installed and
        # build trees alike. Place the daemon beside the mock provider.
        daemon = bindir / 'clipboard-sync'
        shutil.copy2(binary, daemon)
        self.binary = str(daemon)
        self.process = subprocess.Popen([self.binary], env=self.env, stdout=self.logfile,
                                        stderr=subprocess.STDOUT, start_new_session=True)
        self.watch_ready('x')
        self.watch_ready('w')

    def calls(self):
        file = self.root / 'calls.jsonl'
        if not file.exists():
            return []
        result = []
        for line in file.read_text().splitlines():
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # A command can be in the middle of appending a record.
        return result

    def writes(self):
        return [item for item in self.calls() if 'write' in item]

    def log(self):
        return (self.root / 'daemon.log').read_text()

    def set(self, side, offers):
        value = {mime: base64.b64encode(data).decode() for mime, data in offers.items()}
        tmp = self.root / (side + '.test-tmp')
        tmp.write_text(json.dumps(value))
        tmp.replace(self.root / (side + '.json'))

    def event(self, side, event='copy'):
        os.write(self.fifos[side], (event + '\n').encode())

    def watch_ready(self, side, count=1):
        until(lambda: sum(item.get('watch') == side for item in self.calls()) >= count,
              'watcher did not start/restart: ' + side)

    def transfer(self, side, offers, mime, data, attempts=1, timeout=5):
        before = len(self.writes())
        self.set(side, offers)
        self.event(side)
        target = 'w' if side == 'x' else 'x'
        until(lambda: len(self.writes()) >= before + attempts and
              json.loads((self.root / (target + '.json')).read_text()) ==
              {mime: base64.b64encode(data).decode()}, 'transfer failed: ' + mime, timeout)
        actual = self.writes()[before:]
        check(len(actual) == attempts, 'unexpected write count')
        check(all(item['write'] == target and item['mime'] == mime and
                  base64.b64decode(item['data']) == data for item in actual), 'write corrupted')

    def ignored(self, side, offers):
        before_writes = len(self.writes())
        before_reads = sum('tool' in item for item in self.calls())
        self.set(side, offers)
        self.event(side)
        until(lambda: sum('tool' in item for item in self.calls()) > before_reads,
              'ignored event not read')
        time.sleep(0.2)
        check(len(self.writes()) == before_writes, 'ignored/replayed clipboard was written')

    def close(self):
        os.killpg(self.process.pid, signal.SIGTERM)
        self.process.wait(timeout=5)
        for fd in self.fifos.values():
            os.close(fd)
        self.logfile.close()


def run(binary):
    with tempfile.TemporaryDirectory(prefix='clipsync-test-') as directory:
        root = Path(directory)
        session = Session(binary, root)
        try:
            env = next(item['env'] for item in session.calls() if item.get('watch') == 'x')
            check(env['WAYLAND_DISPLAY'] == 'wayland-test', 'Wayland discovery failed')
            check(env['XAUTHORITY'] == str(root / 'home' / '.Xauthority'), 'Xauthority fallback failed')
            check(env['XDG_RUNTIME_DIR'] == str(root) and env['DISPLAY'] == ':mock', 'display env lost')
            check(not session.writes(), 'startup must wait for events')

            text = '  中文\n\tend\n\n'.encode()
            session.transfer('x', {'UTF8_STRING': text}, 'text/plain;charset=utf-8', text)
            session.ignored('w', {'text/plain;charset=utf-8': text})
            session.ignored('x', {'UTF8_STRING': text})
            changed = text + b' '
            session.transfer('w', {'text/plain': changed, 'text/html': b'<b>ignored</b>'}, 'UTF8_STRING', changed)
            session.ignored('x', {'UTF8_STRING': changed})
            session.transfer('w', {'text/plain': b'next'}, 'UTF8_STRING', b'next')
            session.transfer('w', {'text/plain': b'next\t'}, 'UTF8_STRING', b'next\t')
            session.ignored('x', {'text/plain': b''})
            session.ignored('w', {'application/pdf': b'unsupported'})

            binary_data = bytes(range(256)) * 4096
            for side in ('x', 'w'):
                for mime in ('image/png', 'image/jpeg', 'image/gif', 'image/webp', 'image/bmp',
                             'image/tiff', 'image/svg+xml', 'image/x-xpixmap', 'image/avif',
                             'application/x-qt-image'):
                    session.transfer(side, {mime: binary_data, 'text/plain': b'ignored'}, mime, binary_data)
            session.transfer('x', {'application/x-qt-image': b'qt', 'IMAGE/PNG': b'png'}, 'image/png', b'png')
            session.transfer('w', {'Image/HEIC': b'heic'}, 'image/heic', b'heic')

            # Reproduce the observed size, and then a failed read of our X11
            # echo. A transient failure must not republish the same large PNG.
            png = (bytes(range(256)) * 12555)[:3213813]
            session.transfer('w', {'image/png': png}, 'image/png', png)
            (root / 'fail_reads').write_text('1')
            session.ignored('x', {'image/png': png})
            session.ignored('w', {'image/png': png})
            owner_pid = session.writes()[-1]['pid']
            check(Path('/proc', str(owner_pid)).exists(), 'foreground provider was killed after READY')

            # A crashed owner invalidates the cached target, allowing the same
            # source content to be published again on the next event.
            os.kill(owner_pid, signal.SIGKILL)
            session.transfer('w', {'image/png': png}, 'image/png', png)
            until(lambda: not Path('/proc', str(owner_pid)).exists(), 'crashed provider was not reaped')
            check(session.writes()[-1]['pid'] != owner_pid, 'crashed provider was not replaced')

            # A process that runs but never acknowledges ownership is not a
            # successful write. It must be killed/reaped before retrying.
            (root / 'invalid_ready').write_text('1')
            before = len(session.writes())
            session.transfer('w', {'text/plain': b'ready retry'}, 'UTF8_STRING', b'ready retry', attempts=2)
            failed_pid = session.writes()[before]['pid']
            until(lambda: not Path('/proc', str(failed_pid)).exists(), 'bad-ready provider leaked')
            files = b'copy\n/tmp/a b\nfile:///tmp/c\r\n'
            normalized = b'file:///tmp/a b\nfile:///tmp/c\n'
            session.transfer('x', {'image/png': b'ignored', 'x-special/gnome-copied-files': files},
                             'text/uri-list', normalized)
            session.ignored('w', {'text/uri-list': normalized})
            session.transfer('w', {'text/uri-list': b'/tmp/other\x00\n'}, 'text/uri-list', b'file:///tmp/other\n')
            session.transfer('x', {'text/html': b'<i>html</i>\n'}, 'text/html', b'<i>html</i>\n')
            session.transfer('w', {'text/plain;format=flowed;charset=UTF-8': b'flowed'}, 'UTF8_STRING', b'flowed')

            (root / 'fail_writes').write_text('1')
            session.transfer('x', {'STRING': b'retry'}, 'text/plain;charset=utf-8', b'retry', attempts=2)
            (root / 'fail_writes').write_text('2')
            before = len(session.writes())
            session.set('w', {'text/plain': b'event retry'})
            session.event('w')
            until(lambda: len(session.writes()) == before + 2, 'two failed writes missing')
            until(lambda: 'Wayland → X11 同步失败' in session.log(), 'failure log missing')
            session.transfer('w', {'text/plain': b'event retry'}, 'UTF8_STRING', b'event retry')

            (root / 'fail_reads').write_text('1')
            session.ignored('x', {'UTF8_STRING': b'read failure'})
            session.transfer('x', {'UTF8_STRING': b'read failure'}, 'text/plain;charset=utf-8', b'read failure')
            (root / 'timeout_reads').write_text('1')
            session.set('x', {'UTF8_STRING': b'read timeout'})
            start = time.monotonic()
            session.event('x')
            until(lambda: '读取剪贴板超时' in session.log(), 'read timeout missing', 13)
            check(9.8 <= time.monotonic() - start < 13, 'incorrect read timeout')
            session.transfer('x', {'UTF8_STRING': b'read timeout'}, 'text/plain;charset=utf-8', b'read timeout')
            (root / 'timeout_writes').write_text('1')
            start = time.monotonic()
            session.transfer('w', {'text/plain': b'write timeout'}, 'UTF8_STRING', b'write timeout', attempts=2, timeout=14)
            check(9.8 <= time.monotonic() - start < 14, 'incorrect write timeout')
            check('X11 提供进程未确认就绪' in session.log(), 'write timeout log missing')

            for event in ('exit', 'invalid'):
                count = sum(item.get('watch') == 'w' for item in session.calls())
                session.event('w', event)
                session.watch_ready('w', count + 1)
            count = sum(item.get('watch') == 'x' for item in session.calls())
            session.event('x', 'fail')
            session.watch_ready('x', count + 1)
            session.transfer('w', {'text/plain': b'watch recovered'}, 'UTF8_STRING', b'watch recovered')
            check(session.process.poll() is None, 'daemon unexpectedly exited')

            no_deps = dict(session.env, PATH=str(root / 'empty'))
            (root / 'empty').mkdir()
            result = subprocess.run([binary], env=no_deps, capture_output=True, timeout=5)
            check(result.returncode == 1 and b'xclip, wl-copy, wl-paste, clipnotify' in result.stdout,
                  'dependency check mismatch')
            no_display = dict(session.env, XDG_RUNTIME_DIR=str(root / 'empty'), WAYLAND_DISPLAY='')
            result = subprocess.run([binary], env=no_display, capture_output=True, timeout=5)
            check(result.returncode == 1 and '找不到 X11 或 Wayland 显示服务'.encode() in result.stdout,
                  'missing display handling mismatch')
        except Exception:
            print(session.log(), file=sys.stderr)
            raise
        finally:
            session.close()
    print('integration tests passed')


if __name__ == '__main__':
    run(str(Path(sys.argv[1]).resolve()))
