#!/usr/bin/env python3
"""Capture QEMU EGL readback through a private Unix VNC socket (RFB 3.8).

QEMU 10's qemu_console_surface returns NULL for SCANOUT_TEXTURE, so QMP
screendump fails even when EGL's 2D listener has the rendered framebuffer.
This reads that listener, not guest memory or a substitute rendered image.
Only a local Unix socket with no authentication is supported; no TCP listener.
"""
import argparse
import json
import socket
import struct
from PIL import Image


def capture(path, output, cursor_at=None):
    """Optional cursor position comes from the guest's `cursor` ctl witness.

    QEMU VNC sends cursor pixels/hotspot but does not send its position. Do not
    infer one from the Mac pointer. With cursor_at we save both the received
    plane and a JSON witness, then compose those real plane pixels at that
    explicit guest position, as a VNC viewer does. Missing cursor data fails.
    """
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
        encodings = [0] if cursor_at is None else [0, -314]  # Raw + AlphaCursor
        sock.sendall(struct.pack('!BBH', 2, 0, len(encodings))
                     + b''.join(struct.pack('!i', e) for e in encodings))
        sock.sendall(struct.pack('!BBHHHH', 3, 0, 0, 0, width, height))
        frame = Image.new('RGB', (width, height))
        covered = 0
        cursor = None
        hotspot = None
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
                if encoding == -314 and cursor_at is not None:
                    if not (0 < w <= 256 and 0 < h <= 256 and x < w and y < h):
                        raise RuntimeError('invalid cursor geometry')
                    if struct.unpack('!i', read(4))[0] != 0:
                        raise RuntimeError('cursor must use Raw encoding')
                    # QEMU's AlphaCursor carries host-native premultiplied
                    # ARGB words (our supported QEMU hosts are little-endian).
                    bgra = read(w * h * 4)
                    rgba = bytearray(len(bgra))
                    rgba[0::4], rgba[1::4] = bgra[2::4], bgra[1::4]
                    rgba[2::4], rgba[3::4] = bgra[0::4], bgra[3::4]
                    cursor = Image.frombytes('RGBa', (w, h), bytes(rgba)).convert('RGBA')
                    hotspot = (x, y)
                    continue
                if encoding != 0 or x + w > width or y + h > height:
                    raise RuntimeError('unexpected encoding or rectangle bounds')
                pixels = read(w * h * 4)
                frame.paste(Image.frombytes('RGB', (w, h), pixels, 'raw', 'BGRX'), (x, y))
                covered += w * h
            if covered < width * height:
                continue  # cursor-only update can precede the framebuffer
            if covered > width * height:
                raise RuntimeError('overlapping initial framebuffer updates')
            if cursor_at is not None:
                if cursor is None:
                    continue
                cx, cy = cursor_at
                if not (0 <= cx < width and 0 <= cy < height):
                    raise RuntimeError('cursor position outside display')
                alpha = cursor.getchannel('A')
                visible = sum(value != 0 for value in alpha.tobytes())
                frame.save(str(output) + '.framebuffer.png')
                cursor.save(str(output) + '.cursor.png')
                with open(str(output) + '.cursor.json', 'w') as witness:
                    json.dump({'position_source': 'explicit guest witness',
                               'position': [cx, cy], 'hotspot': hotspot,
                               'size': cursor.size, 'visible_pixels': visible}, witness)
                frame.paste(cursor, (cx - hotspot[0], cy - hotspot[1]), cursor)
            frame.save(output)
            return


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket')
    parser.add_argument('output')
    parser.add_argument('--cursor-at', nargs=2, type=int, metavar=('X', 'Y'),
                        help='include guest plane at position verified from guest ctl')
    args = parser.parse_args()
    capture(args.socket, args.output, args.cursor_at)
