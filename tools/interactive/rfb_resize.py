#!/usr/bin/env python3
"""Tell a QEMU display a new size, as a VNC viewer whose window was resized does.

An RFB client that has asked for the ExtendedDesktopSize pseudo-encoding may send
SetDesktopSize. QEMU hands the size to the display's device (ui/vnc.c, the
VNC_MSG_CLIENT_SET_DESKTOP_SIZE arm), and virtio-gpu raises its display event:
the same path a host window takes when it is resized, with no window. The size is
only told: what the guest does with it is the guest's.

Only a local Unix socket with no authentication is spoken to, as in vnc_capture.py.

usage: rfb_resize.py <vnc unix socket> WxH [WxH ...] [--gap-ms N]
  Each size is told in order. With no gap each is told after QEMU answered the
  one before; with --gap-ms the sizes go out N ms apart and the answers are read
  afterwards, which makes a train of display events.
prints, for each: rfb: told WxH, answered status S     (4: forwarded to the display)
exit status: 0 if every answer was 4.
"""
import argparse
import socket
import struct
import sys
import time

EXT_DESKTOP_SIZE = -308
DESKTOP_SIZE = -223
FORWARDED = 4


class Rfb:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        self.sock.connect(path)
        if self.read(12) != b'RFB 003.008\n':
            raise RuntimeError('expected RFB 3.8')
        self.sock.sendall(b'RFB 003.008\n')
        count = self.read(1)[0]
        if not count or 1 not in self.read(count):
            raise RuntimeError('the socket must offer None security')
        self.sock.sendall(b'\x01')
        if self.read(4) != b'\0' * 4:
            raise RuntimeError('VNC authentication refused')
        self.sock.sendall(b'\x01')  # shared: never disconnect another viewer
        self.read(4)  # the framebuffer's size as the server has it now
        fmt = self.read(16)
        self.bytes_per_pixel = fmt[0] // 8
        name_len, = struct.unpack('!I', self.read(4))
        if name_len > 4096:
            raise RuntimeError('oversized server name')
        self.read(name_len)
        encodings = [0, EXT_DESKTOP_SIZE]
        self.sock.sendall(struct.pack('!BBH', 2, 0, len(encodings))
                          + b''.join(struct.pack('!i', e) for e in encodings))

    def read(self, n):
        data = bytearray()
        while len(data) < n:
            part = self.sock.recv(n - len(data))
            if not part:
                raise RuntimeError('VNC connection ended')
            data.extend(part)
        return bytes(data)

    def tell(self, w, h):
        # SetDesktopSize: type 251, pad, width, height, one screen, pad; then the
        # screen: id, x, y, width, height, flags.
        self.sock.sendall(struct.pack('!BBHHBB', 251, 0, w, h, 1, 0)
                          + struct.pack('!IHHHHI', 0, 0, 0, w, h, 0))

    def answer(self):
        """The status of the next answer to a SetDesktopSize of ours.

        QEMU answers in a framebuffer update of one ExtendedDesktopSize rectangle
        whose x is 1 (the change was this client's) and whose y is the status. One
        with x 0 is the server saying what its size is, and is passed over.
        """
        while True:
            kind = self.read(1)[0]
            if kind == 2:  # Bell
                continue
            if kind == 3:  # ServerCutText
                self.read(3)
                n, = struct.unpack('!I', self.read(4))
                if n > 1024 * 1024:
                    raise RuntimeError('oversized clipboard message')
                self.read(n)
                continue
            if kind != 0:
                raise RuntimeError('unexpected server message %d' % kind)
            self.read(1)
            rects, = struct.unpack('!H', self.read(2))
            status = None
            for _ in range(rects):
                x, y, w, h, enc = struct.unpack('!HHHHi', self.read(12))
                if enc == EXT_DESKTOP_SIZE:
                    screens = self.read(4)[0]
                    self.read(16 * screens)
                    if x == 1:
                        status = y
                elif enc == DESKTOP_SIZE:
                    pass
                elif enc == 0:
                    if w * h * self.bytes_per_pixel > 8192 * 8192 * 4:
                        raise RuntimeError('oversized raw rectangle')
                    self.read(w * h * self.bytes_per_pixel)
                else:
                    raise RuntimeError('unexpected encoding %d' % enc)
            if status is not None:
                return status


def size(text):
    w, _, h = text.partition('x')
    w, h = int(w), int(h)
    if not (0 <= w <= 0xffff and 0 <= h <= 0xffff):
        raise argparse.ArgumentTypeError('a side is 0..65535')
    return w, h


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('socket')
    ap.add_argument('sizes', nargs='+', type=size, metavar='WxH')
    ap.add_argument('--gap-ms', type=int, default=None)
    args = ap.parse_args()
    rfb = Rfb(args.socket)
    statuses = []
    if args.gap_ms is None:
        for w, h in args.sizes:
            rfb.tell(w, h)
            statuses.append(rfb.answer())
    else:
        for i, (w, h) in enumerate(args.sizes):
            if i:
                time.sleep(args.gap_ms / 1000.0)
            rfb.tell(w, h)
        statuses = [rfb.answer() for _ in args.sizes]
    for (w, h), status in zip(args.sizes, statuses):
        print('rfb: told %dx%d, answered status %d' % (w, h, status))
    return 0 if all(s == FORWARDED for s in statuses) else 1


if __name__ == '__main__':
    sys.exit(main())
