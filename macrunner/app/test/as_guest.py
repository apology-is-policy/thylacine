"""What a guest's driver would read of QEMU's virtio GPU, with no guest: the display's rectangle
(GET_DISPLAY_INFO), the size its EDID gives in millimetres (GET_EDID), and whether a display event
is pending. Spoken over QEMU's qtest socket, so the QEMU must have been started with
    -machine virt,highmem=off -accel qtest -qtest unix:<socket>,server=on,wait=off
and one virtio-gpu-pci on its first bus. Every run resets the device and brings it up again.

usage: as_guest.py <qtest socket>
prints: display WxH on N, edid AxB mm prefers WxH, event N
    or: display WxH on N, edid AxB cm no preferred timing, event N
the second when the block has no detailed timing, which is what QEMU makes for a large mode.
"""
import socket
import struct
import sys

ECAM = 0x3F000000  # virt's PCI configuration space with highmem off
BAR = 0x10000000   # the start of its PCI memory window, which is where the device's registers go
RAM = 0x40000000
DESC, AVAIL, USED, REQ, RESP = RAM, RAM + 0x1000, RAM + 0x2000, RAM + 0x3000, RAM + 0x4000
QSZ = 8
GET_DISPLAY_INFO, OK_DISPLAY_INFO = 0x0100, 0x1101
GET_EDID, OK_EDID = 0x010A, 0x1104


class Qtest:
    def __init__(self, path):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(10)
        s.connect(path)
        self.f = s.makefile("rwb", buffering=0)

    def cmd(self, line):
        self.f.write((line + "\n").encode())
        while True:
            r = self.f.readline().decode().strip()
            if not r:
                sys.exit("as_guest: qtest closed after '%s'" % line)
            if r.startswith("IRQ"):
                continue
            if not r.startswith("OK"):
                sys.exit("as_guest: '%s' -> '%s'" % (line, r))
            return r[2:].strip()

    def rd(self, size, addr):
        return int(self.cmd("read%s 0x%x" % (size, addr)), 16)

    def wr(self, size, addr, value):
        self.cmd("write%s 0x%x 0x%x" % (size, addr, value))

    def read(self, addr, n):
        return bytes.fromhex(self.cmd("read 0x%x %d" % (addr, n))[2:])

    def write(self, addr, data):
        self.cmd("write 0x%x %d 0x%s" % (addr, len(data), data.hex()))


def find_gpu(q):
    for dev in range(32):
        cfg = ECAM + (dev << 15)
        if q.rd("l", cfg) == 0x10501AF4:  # device 0x1050, vendor 0x1af4
            return cfg
    sys.exit("as_guest: no virtio GPU on the first PCI bus")


# The virtio capabilities: kind -> (bar, offset in it, the notify multiplier).
def capabilities(q, cfg):
    found = {}
    p = q.rd("b", cfg + 0x34)
    while p:
        if q.rd("b", cfg + p) == 0x09:
            kind = q.rd("b", cfg + p + 3)
            mult = q.rd("l", cfg + p + 16) if kind == 2 else 0
            found.setdefault(kind, (q.rd("b", cfg + p + 4), q.rd("l", cfg + p + 8), mult))
        p = q.rd("b", cfg + p + 1)
    for kind in (1, 2, 4):
        if kind not in found:
            sys.exit("as_guest: the device has no virtio capability %d" % kind)
    if len({found[k][0] for k in (1, 2, 4)}) != 1:
        sys.exit("as_guest: the device's registers are in more than one BAR: %s" % found)
    return found


def bring_up(q, common):
    q.wr("b", common + 0x14, 0)  # reset
    q.wr("b", common + 0x14, 1)
    q.wr("b", common + 0x14, 3)
    q.wr("l", common + 0x00, 0)
    offered = q.rd("l", common + 0x04)
    q.wr("l", common + 0x08, 0)
    q.wr("l", common + 0x0C, offered & 2)  # the EDID, if the device has one
    q.wr("l", common + 0x08, 1)
    q.wr("l", common + 0x0C, 1)  # VIRTIO_F_VERSION_1
    q.wr("b", common + 0x14, 11)
    if not q.rd("b", common + 0x14) & 8:
        sys.exit("as_guest: the device refused the features")
    q.cmd("memset 0x%x %d 0" % (RAM, 0x3000))
    q.wr("w", common + 0x16, 0)  # the control queue
    q.wr("w", common + 0x18, QSZ)
    q.wr("q", common + 0x20, DESC)
    q.wr("q", common + 0x28, AVAIL)
    q.wr("q", common + 0x30, USED)
    q.wr("w", common + 0x1C, 1)
    notify_off = q.rd("w", common + 0x1E)
    q.wr("b", common + 0x14, 15)
    return bool(offered & 2), notify_off


# The nth request since the device came up: `req` out, `resp_len` bytes back.
def ask(q, notify, n, req, resp_len):
    q.write(REQ, req)
    q.cmd("memset 0x%x %d 0" % (RESP, resp_len))
    q.write(DESC, struct.pack("<QIHH", REQ, len(req), 1, 1) + struct.pack("<QIHH", RESP, resp_len, 2, 0))
    q.wr("w", AVAIL + 4 + 2 * (n % QSZ), 0)
    q.wr("w", AVAIL + 2, n + 1)
    q.wr("w", notify, 0)
    for _ in range(500):
        if q.rd("w", USED + 2) == n + 1:
            return q.read(RESP, resp_len)
    sys.exit("as_guest: the device did not answer request %d" % n)


def header(kind):
    return struct.pack("<IIQII", kind, 0, 0, 0, 0)


# The image size of a base EDID block and the mode it prefers, found where libhalcyon's
# parse_edid_mm finds the size: in the first detailed timing when there is one (its pixel clock is
# not zero), and otherwise in the whole centimetres of bytes 21 and 22, with no mode. A guest
# takes the size only when both sides are 1 to 2000 mm; this says whatever is there.
def edid_size(block):
    if block[:8] != bytes([0, 255, 255, 255, 255, 255, 255, 0]) or sum(block[:128]) % 256:
        sys.exit("as_guest: not an EDID block (%s...)" % block[:8].hex())
    d = block[54:72]
    if d[0] or d[1]:
        mm = (d[12] | (d[14] >> 4) << 8, d[13] | (d[14] & 15) << 8)
        prefers = (d[2] | (d[4] >> 4) << 8, d[5] | (d[7] >> 4) << 8)
        return "edid %dx%d mm prefers %dx%d" % (mm + prefers)
    return "edid %dx%d cm no preferred timing" % (block[21], block[22])


def main():
    q = Qtest(sys.argv[1])
    cfg = find_gpu(q)
    caps = capabilities(q, cfg)
    bar = caps[1][0]
    q.wr("l", cfg + 0x10 + 4 * bar, BAR)
    q.wr("l", cfg + 0x14 + 4 * bar, 0)
    q.wr("w", cfg + 0x04, q.rd("w", cfg + 0x04) | 2)
    common, device = BAR + caps[1][1], BAR + caps[4][1]
    # Read before the reset, and then cleared, so that the next run sees only what came after this one.
    event = q.rd("l", device)
    q.wr("l", device + 4, event)
    has_edid, notify_off = bring_up(q, common)
    notify = BAR + caps[2][1] + notify_off * caps[2][2]
    info = ask(q, notify, 0, header(GET_DISPLAY_INFO), 24 + 16 * 24)
    kind = struct.unpack_from("<I", info)[0]
    if kind != OK_DISPLAY_INFO:
        sys.exit("as_guest: GET_DISPLAY_INFO answered %#x" % kind)
    # QEMU answers with no position (virtio_gpu_base_fill_display_info sets a width and a height),
    # so the offsets a display was told are not to be seen from here.
    _x, _y, w, h, on, _flags = struct.unpack_from("<6I", info, 24)
    edid = "no edid"
    if has_edid:
        got = ask(q, notify, 1, header(GET_EDID) + struct.pack("<II", 0, 0), 24 + 8 + 1024)
        kind, size = struct.unpack_from("<I", got)[0], struct.unpack_from("<I", got, 24)[0]
        if kind != OK_EDID or size < 128:
            sys.exit("as_guest: GET_EDID answered %#x, %d bytes" % (kind, size))
        edid = edid_size(got[32:32 + 128])
    print("display %dx%d on %d, %s, event %d" % (w, h, on, edid, event & 1))


main()
