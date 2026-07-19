#!/usr/bin/env python3
# tools/rfb-refresh.py -- minimal RFB (VNC) client that keeps a QEMU VNC
# display backend LIVE (the #31 ls-gfx-live leg).
#
# QEMU only runs the display-refresh machinery (graphic_hw_update on the
# refresh timer + dirty-rect encoding) while a VNC client is connected and
# requesting updates, so this is the headless stand-in for a human-facing
# display backend: connect, handshake (security-none), then pump
# incremental FramebufferUpdateRequests and drain whatever arrives until
# the deadline. Nothing is decoded -- the point is the DEVICE-side display
# path running concurrently with tapestryd's controlq submissions, not the
# pixels.
#
#   tools/rfb-refresh.py DISPLAY_NUM SECONDS
#
# Prints `rfb: connected WxH` after ServerInit (the caller asserts the
# geometry to prove the client landed on the gpu0 console, not a stray
# head) and `rfb: N update-reqs, M bytes drained` at exit. Any handshake
# failure is a hard error -- a leg that cannot attach must fail loudly,
# never silently test nothing.

import socket
import struct
import sys
import time


def main() -> int:
    disp, secs = int(sys.argv[1]), float(sys.argv[2])
    s = socket.create_connection(("127.0.0.1", 5900 + disp), timeout=10)
    s.recv(12)  # "RFB 003.008\n"
    s.sendall(b"RFB 003.008\n")
    n = s.recv(1)[0]
    types = s.recv(n)
    if 1 not in types:  # security-none
        raise RuntimeError(f"no security-none offered: {types!r}")
    s.sendall(bytes([1]))
    if struct.unpack(">I", s.recv(4))[0] != 0:
        raise RuntimeError("security handshake failed")
    s.sendall(bytes([1]))  # ClientInit: shared
    si = s.recv(24)
    w, h = struct.unpack(">HH", si[:4])
    namelen = struct.unpack(">I", si[20:24])[0]
    if namelen:
        s.recv(namelen)
    print(f"rfb: connected {w}x{h}", flush=True)

    s.setblocking(False)
    reqs = 0
    drained = 0
    end = time.time() + secs
    full = struct.pack(">BBHHHH", 3, 0, 0, 0, w, h)
    incr = struct.pack(">BBHHHH", 3, 1, 0, 0, w, h)
    s.sendall(full)
    while time.time() < end:
        try:
            s.sendall(incr)
            reqs += 1
        except BlockingIOError:
            pass
        t0 = time.time()
        while time.time() - t0 < 0.02:
            try:
                b = s.recv(1 << 20)
                if not b:
                    raise RuntimeError("server closed the RFB stream")
                drained += len(b)
            except BlockingIOError:
                time.sleep(0.005)
    print(f"rfb: {reqs} update-reqs, {drained} bytes drained", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
