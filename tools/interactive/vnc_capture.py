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
import zlib


def write_png(path, width, height, pixels, channels):
    """8-bit RGB (3) or RGBA (4), filter 0 on every row."""
    stride = width * channels
    raw = b''.join(b'\0' + bytes(pixels[y * stride:(y + 1) * stride]) for y in range(height))

    def chunk(kind, data):
        return (struct.pack('!I', len(data)) + kind + data
                + struct.pack('!I', zlib.crc32(kind + data) & 0xffffffff))

    color = {3: 2, 4: 6}[channels]
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'
                + chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, color, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


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
        frame = bytearray(width * height * 3)
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
                    premul = bytearray(len(bgra))
                    premul[0::4], premul[1::4] = bgra[2::4], bgra[1::4]
                    premul[2::4], premul[3::4] = bgra[0::4], bgra[3::4]
                    cursor = (w, h, bytes(premul))
                    hotspot = (x, y)
                    continue
                if encoding != 0 or x + w > width or y + h > height:
                    raise RuntimeError('unexpected encoding or rectangle bounds')
                pixels = read(w * h * 4)
                for row in range(h):
                    src = pixels[row * w * 4:(row + 1) * w * 4]
                    at = ((y + row) * width + x) * 3
                    frame[at:at + w * 3:3] = src[2::4]
                    frame[at + 1:at + w * 3:3] = src[1::4]
                    frame[at + 2:at + w * 3:3] = src[0::4]
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
                cw, ch, premul = cursor
                visible = sum(value != 0 for value in premul[3::4])
                write_png(str(output) + '.framebuffer.png', width, height, frame, 3)
                # Saved straight (not premultiplied), as a viewer reads a PNG.
                straight = bytearray(premul)
                for i in range(0, len(premul), 4):
                    a = premul[i + 3]
                    for c in range(3):
                        straight[i + c] = min(255, premul[i + c] * 255 // a) if a else 0
                write_png(str(output) + '.cursor.png', cw, ch, straight, 4)
                with open(str(output) + '.cursor.json', 'w') as witness:
                    json.dump({'position_source': 'explicit guest witness',
                               'position': [cx, cy], 'hotspot': hotspot,
                               'size': [cw, ch], 'visible_pixels': visible}, witness)
                # Source-over from the straight colour, with Pillow's rounding (its paste
                # with a mask), so a capture is the same bytes it was before Pillow left.
                ox, oy = cx - hotspot[0], cy - hotspot[1]
                for j in range(ch):
                    for i in range(cw):
                        px, py = ox + i, oy + j
                        if not (0 <= px < width and 0 <= py < height):
                            continue
                        s = (j * cw + i) * 4
                        a = premul[s + 3]
                        if a == 0:
                            continue
                        d = (py * width + px) * 3
                        for c in range(3):
                            mix = frame[d + c] * (255 - a) + straight[s + c] * a + 128
                            frame[d + c] = ((mix >> 8) + mix) >> 8
            write_png(output, width, height, frame, 3)
            return


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket')
    parser.add_argument('output')
    parser.add_argument('--cursor-at', nargs=2, type=int, metavar=('X', 'Y'),
                        help='include guest plane at position verified from guest ctl')
    args = parser.parse_args()
    capture(args.socket, args.output, args.cursor_at)
