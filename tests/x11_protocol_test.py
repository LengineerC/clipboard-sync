"""Exercise the real owner against a private Xvfb, never the user's clipboard."""
import ctypes as C
import ctypes.util
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile
import time


class Selection(C.Structure):
    _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                ('display', C.c_void_p), ('requestor', C.c_ulong),
                ('selection', C.c_ulong), ('target', C.c_ulong),
                ('property', C.c_ulong), ('time', C.c_ulong)]


class Property(C.Structure):
    _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                ('display', C.c_void_p), ('window', C.c_ulong), ('atom', C.c_ulong),
                ('time', C.c_ulong), ('state', C.c_int)]


class Event(C.Union):
    _fields_ = [('type', C.c_int), ('selection', Selection), ('property', Property),
                ('pad', C.c_long * 24)]


def check(value, message):
    if not value:
        raise AssertionError(message)


def read_line(stream, timeout=5):
    with selectors.DefaultSelector() as selector:
        selector.register(stream, selectors.EVENT_READ)
        check(selector.select(timeout), 'readiness acknowledgement timed out')
        return stream.readline()


class Client:
    def __init__(self, display):
        self.x = C.CDLL(ctypes.util.find_library('X11'))
        pointer = C.c_void_p
        word = C.c_ulong
        signatures = {
            'XOpenDisplay': (pointer, [C.c_char_p]),
            'XDefaultRootWindow': (word, [pointer]),
            'XCreateSimpleWindow': (word, [pointer, word, C.c_int, C.c_int, C.c_uint,
                                          C.c_uint, C.c_uint, word, word]),
            'XSelectInput': (C.c_int, [pointer, word, C.c_long]),
            'XInternAtom': (word, [pointer, C.c_char_p, C.c_int]),
            'XConvertSelection': (C.c_int, [pointer, word, word, word, word, word]),
            'XPending': (C.c_int, [pointer]),
            'XNextEvent': (C.c_int, [pointer, C.POINTER(Event)]),
            'XFlush': (C.c_int, [pointer]),
            'XGetWindowProperty': (C.c_int, [pointer, word, word, C.c_long, C.c_long,
                                            C.c_int, word, C.POINTER(word), C.POINTER(C.c_int),
                                            C.POINTER(word), C.POINTER(word), C.POINTER(pointer)]),
            'XFree': (C.c_int, [pointer]),
            'XDestroyWindow': (C.c_int, [pointer, word]),
            'XCloseDisplay': (C.c_int, [pointer]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self.x, name)
            function.restype, function.argtypes = result, args
        self.display = self.x.XOpenDisplay(display.encode())
        check(self.display, 'cannot connect to private Xvfb')
        self.clipboard, self.png, self.incr, self.targets = (
            self.atom(name) for name in ('CLIPBOARD', 'image/png', 'INCR', 'TARGETS'))

    def atom(self, name):
        return self.x.XInternAtom(self.display, name.encode(), 0)

    def window(self):
        window = self.x.XCreateSimpleWindow(self.display, self.x.XDefaultRootWindow(self.display),
                                             0, 0, 1, 1, 0, 0, 0)
        self.x.XSelectInput(self.display, window, 1 << 22)  # PropertyChangeMask
        return window

    def event(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while self.x.XPending(self.display):
                event = Event()
                self.x.XNextEvent(self.display, C.byref(event))
                if predicate(event):
                    return event
            time.sleep(0.001)
        raise AssertionError('X11 response timed out')

    def request(self, window, target, prop):
        self.x.XConvertSelection(self.display, self.clipboard, target, prop, window, 0)
        self.x.XFlush(self.display)
        event = self.event(lambda e: e.type == 31 and e.selection.requestor == window
                           and e.selection.target == target)
        return event.selection.property

    def property(self, window, prop, delete=True):
        kind, count, after = C.c_ulong(), C.c_ulong(), C.c_ulong()
        fmt, data = C.c_int(), C.c_void_p()
        status = self.x.XGetWindowProperty(self.display, window, prop, 0, 1 << 20,
                                           int(delete), 0, C.byref(kind), C.byref(fmt),
                                           C.byref(count), C.byref(after), C.byref(data))
        check(status == 0 and after.value == 0, 'property read failed or truncated')
        try:
            if fmt.value == 32:
                value = list(C.cast(data, C.POINTER(C.c_ulong))[:count.value])
            else:
                value = C.string_at(data, count.value * fmt.value // 8) if data.value else b''
            return kind.value, fmt.value, value
        finally:
            if data.value:
                self.x.XFree(data)

    def receive(self, window, prop):
        kind, fmt, value = self.property(window, prop)
        if kind != self.incr:
            check(kind == self.png and fmt == 8, 'wrong direct image type')
            return value
        check(fmt == 32 and len(value) == 1, 'invalid INCR size announcement')
        announced = value[0]
        received = bytearray()
        while True:
            self.x.XFlush(self.display)
            self.event(lambda e: e.type == 28 and e.property.window == window
                       and e.property.atom == prop and e.property.state == 0)
            kind, fmt, chunk = self.property(window, prop)
            check(kind == self.png and fmt == 8, 'wrong INCR chunk type')
            if not chunk:
                check(len(received) == announced, 'INCR data truncated')
                return bytes(received)
            received.extend(chunk)

    def destroy(self, window):
        self.x.XDestroyWindow(self.display, window)
        self.x.XFlush(self.display)

    def close(self):
        self.x.XCloseDisplay(self.display)


def run(binary):
    xvfb = shutil.which('Xvfb')
    if not xvfb:
        print('SKIP: Xvfb is unavailable; real X11 protocol test was not run')
        return 77
    providers = []
    client = None
    server = None
    with tempfile.TemporaryDirectory(prefix='clipsync-x11-') as directory:
        with open(Path(directory) / 'Xvfb.log', 'wb') as log:
            readfd, writefd = os.pipe()
            try:
                server = subprocess.Popen([xvfb, '-displayfd', str(writefd), '-screen', '0',
                                           '800x600x24', '-nolisten', 'tcp'],
                                          pass_fds=(writefd,), stdout=log, stderr=log)
            finally:
                os.close(writefd)
            try:
                with os.fdopen(readfd, 'rb') as output:
                    number = read_line(output).strip().decode()
                check(number.isdigit(), 'Xvfb failed to allocate a private display')
                display = ':' + number
                env = dict(os.environ, DISPLAY=display)
                client = Client(display)

                def publish(data):
                    with tempfile.TemporaryFile(dir=directory) as input_file:
                        input_file.write(data)
                        input_file.seek(0)
                        process = subprocess.Popen([binary, 'image/png', str(os.getpid())],
                                                   stdin=input_file, stdout=subprocess.PIPE,
                                                   stderr=log, env=env)
                    providers.append(process)
                    check(read_line(process.stdout) == b'READY\n', 'owner failed to acknowledge ownership')
                    return process

                # Match the image size from the frozen QQ session.
                image = (bytes(range(256)) * 12555)[:3213813]
                old = publish(image)
                slow, fast = client.window(), client.window()
                slow_prop, fast_prop = client.atom('SLOW'), client.atom('FAST')
                check(client.request(slow, client.png, slow_prop) == slow_prop, 'slow request failed')
                check(client.property(slow, slow_prop, delete=False)[0] == client.incr,
                      'large image did not enter INCR')
                # Leave the first reader stalled at INCR. TARGETS and other
                # image requests must still work, including on the same window.
                target_prop = client.atom('TYPES')
                check(client.request(fast, client.targets, target_prop) == target_prop, 'TARGETS blocked')
                check(client.png in client.property(fast, target_prop)[2], 'image MIME missing')
                check(client.request(fast, client.png, fast_prop) == fast_prop, 'second reader blocked')
                check(client.receive(fast, fast_prop) == image, 'second reader image corrupted')
                check(client.request(slow, client.png, fast_prop) == fast_prop, 'same-window reader blocked')
                check(client.receive(slow, fast_prop) == image, 'same-window image corrupted')
                check(client.request(fast, client.atom('unsupported/type'), fast_prop) == 0,
                      'unsupported format was not refused')
                client.destroy(slow)
                check(client.request(fast, client.png, fast_prop) == fast_prop, 'destroyed reader broke owner')
                check(client.receive(fast, fast_prop) == image, 'post-destroy image corrupted')

                # Accepted old-image transfers survive a new clipboard owner.
                slow = client.window()
                check(client.request(slow, client.png, slow_prop) == slow_prop, 'replacement setup failed')
                replacement = b'new image\x00\xff'
                new = publish(replacement)
                check(client.receive(slow, slow_prop) == image, 'owner replacement interrupted old transfer')
                check(old.wait(timeout=5) == 0, 'retired owner did not exit cleanly')
                # Legacy requestors are allowed to pass property=None.
                check(client.request(fast, client.png, 0) == client.png, 'property=None fallback failed')
                check(client.receive(fast, client.png) == replacement, 'small direct image corrupted')
                check(new.poll() is None, 'new owner exited after successful publication')
                print('real X11 concurrent INCR, TARGETS, interruption, replacement and direct reads passed')
                return 0
            finally:
                for process in providers:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=5)
                    process.stdout.close()
                if client:
                    client.close()
                if server and server.poll() is None:
                    server.terminate()
                if server:
                    server.wait(timeout=5)


if __name__ == '__main__':
    sys.exit(run(str(Path(sys.argv[1]).resolve())))
