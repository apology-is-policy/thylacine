#!/usr/bin/env python3
"""Capture QEMU EGL readback through a private Unix VNC socket (RFB 3.8).

QEMU 10's qemu_console_surface returns NULL for SCANOUT_TEXTURE, so QMP
screendump fails even when EGL's 2D listener has the rendered framebuffer.
This reads that listener, not guest memory or a substitute rendered image.
Only a local Unix socket with no authentication is supported; no TCP listener.
"""
import socket
import struct
import sys
from PIL import Image


def capture(path, output):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(30)
        sock.connect(path)

        def read(n):
            data = bytearray()
            while len(data) < n:
                part = sock.recv(n - len(data))
                if not part:
                    raise RuntimeError('VNC connection ended')
                data.extend(part)
            return bytes(data)

        if read(12) != b'RFB 003.008\n':
            raise RuntimeError('expected RFB 3.8')
        sock.sendall(b'RFB 003.008\n')
        count = read(1)[0]
        if not count or 1 not in read(count):
            raise RuntimeError('private capture socket must offer None security')
        sock.sendall(b'\x01')
        if read(4) != b'\0' * 4:
            raise RuntimeError('VNC authentication refused')
        sock.sendall(b'\x01')  # shared; never disconnect another viewer
        width, height = struct.unpack('!HH', read(4))
        read(16)  # server pixel format; select a known one below
        name_len, = struct.unpack('!I', read(4))
        if name_len > 4096:
            raise RuntimeError('oversized server name')
        read(name_len)
        if not (0 < width <= 8192 and 0 < height <= 8192):
            raise RuntimeError('invalid framebuffer geometry')
        fmt = struct.pack('!BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        sock.sendall(b'\0\0\0\0' + fmt)
        sock.sendall(struct.pack('!BBHi', 2, 0, 1, 0))  # Raw only
        sock.sendall(struct.pack('!BBHHHH', 3, 0, 0, 0, width, height))
        frame = Image.new('RGB', (width, height))
        covered = 0
        while True:
            kind = read(1)[0]
            if kind == 2:  # Bell
                continue
            if kind == 3:  # ServerCutText (not used for capture)
                read(3)
                n, = struct.unpack('!I', read(4))
                if n > 1024 * 1024:
                    raise RuntimeError('oversized clipboard message')
                read(n)
                continue
            if kind != 0:
                raise RuntimeError(f'unexpected VNC message {kind}')
            read(1)
            rectangles, = struct.unpack('!H', read(2))
            for _ in range(rectangles):
                x, y, w, h, encoding = struct.unpack('!HHHHi', read(12))
                if encoding != 0 or x + w > width or y + h > height:
                    raise RuntimeError('unexpected encoding or rectangle bounds')
                pixels = read(w * h * 4)
                frame.paste(Image.frombytes('RGB', (w, h), pixels, 'raw', 'BGRX'), (x, y))
                covered += w * h
            if covered != width * height:
                raise RuntimeError(f'incomplete initial framebuffer: {covered}/{width*height}')
            frame.save(output)
            return


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: vnc_capture.py SOCKET OUTPUT.png')
    capture(*sys.argv[1:])
