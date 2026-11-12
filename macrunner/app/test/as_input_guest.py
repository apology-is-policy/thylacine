"""What a guest's input drivers would read of QEMU's virtio input devices, with no guest. Spoken over
QEMU's qtest socket (-machine virt,highmem=off -accel qtest, and not paused: QEMU's monitor refuses
input for a machine that is stopped). The devices are the virtio-input functions on the first PCI
bus, taken in slot order, which is the order Lictor is granted them in and brings them up in
(usr/lictor/src/main.rs, the loop over pci_extra).

usage: as_input_guest.py <qtest socket> up               bring every device up in slot order; prints each kind
       as_input_guest.py <qtest socket> drain            the events each device has delivered since, as they are
       as_input_guest.py <qtest socket> reports <w> <h>  the same, a pointer device's as the reports the
                                                         compositor makes of them on a screen w x h
A device is given 64 buffers, and QEMU drops a report it has no buffers for: of more than 64 events
on one device between two reads, not all are seen.
"""
import socket
import struct
import sys

ECAM = 0x3F000000
BAR0 = 0x10000000
RAM = 0x40000000
QSZ = 64
EV = {0: "syn", 1: "key", 2: "rel", 3: "abs"}
BTN = {0x110: "left", 0x111: "right", 0x112: "middle", 0x113: "side", 0x114: "extra"}


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
                sys.exit("as_input_guest: qtest closed after '%s'" % line)
            if r.startswith("IRQ"):
                continue
            if not r.startswith("OK"):
                sys.exit("as_input_guest: '%s' -> '%s'" % (line, r))
            return r[2:].strip()

    def rd(self, size, addr):
        return int(self.cmd("read%s 0x%x" % (size, addr)), 16)

    def wr(self, size, addr, value):
        self.cmd("write%s 0x%x 0x%x" % (size, addr, value))

    def read(self, addr, n):
        return bytes.fromhex(self.cmd("read 0x%x %d" % (addr, n))[2:])

    def write(self, addr, data):
        self.cmd("write 0x%x %d 0x%s" % (addr, len(data), data.hex()))


def capabilities(q, cfg):
    found = {}
    p = q.rd("b", cfg + 0x34)
    while p:
        if q.rd("b", cfg + p) == 0x09:
            kind = q.rd("b", cfg + p + 3)
            mult = q.rd("l", cfg + p + 16) if kind == 2 else 0
            found.setdefault(kind, (q.rd("b", cfg + p + 4), q.rd("l", cfg + p + 8), mult))
        p = q.rd("b", cfg + p + 1)
    return found


# The input functions in slot order: (index, config space, register base, capabilities).
def devices(q, program):
    out = []
    for dev in range(32):
        cfg = ECAM + (dev << 15)
        if q.rd("l", cfg) != 0x10521AF4:  # device 0x1040 + 18, vendor 0x1af4
            continue
        i = len(out)
        caps = capabilities(q, cfg)
        bar = caps[1][0]
        base = BAR0 + 0x100000 * (i + 1)
        if program:
            q.wr("l", cfg + 0x10 + 4 * bar, base)
            q.wr("l", cfg + 0x14 + 4 * bar, 0)
            q.wr("w", cfg + 0x04, q.rd("w", cfg + 0x04) | 2)
        out.append((i, cfg, base, caps))
    if not out:
        sys.exit("as_input_guest: no virtio input device on the first PCI bus")
    return out


def ram(i):
    b = RAM + 0x10000 * (i + 1)
    return b, b + 0x1000, b + 0x2000, b + 0x3000  # desc, avail, used, buffers


def ev_bits(q, devcfg, ev):
    q.wr("b", devcfg + 0, 0x11)
    q.wr("b", devcfg + 1, ev)
    return q.rd("b", devcfg + 2)


# As the guest tells them apart (usr/lictor/src/backend/input.rs): what reports ABS is the tablet,
# what reports REL and no ABS is the mouse, and the rest is the keyboard.
def kind_of(q, devcfg):
    if ev_bits(q, devcfg, 3):
        return "tablet"
    if ev_bits(q, devcfg, 2):
        return "mouse"
    return "keyboard"


def up(q):
    for i, _cfg, base, caps in devices(q, True):
        common, devcfg = base + caps[1][1], base + caps[4][1]
        desc, avail, used, bufs = ram(i)
        q.wr("b", common + 0x14, 0)
        q.wr("b", common + 0x14, 1)
        q.wr("b", common + 0x14, 3)
        q.wr("l", common + 0x08, 0)
        q.wr("l", common + 0x0C, 0)
        q.wr("l", common + 0x08, 1)
        q.wr("l", common + 0x0C, 1)  # VIRTIO_F_VERSION_1
        q.wr("b", common + 0x14, 11)
        if not q.rd("b", common + 0x14) & 8:
            sys.exit("as_input_guest: device %d refused the features" % i)
        q.cmd("memset 0x%x %d 0" % (desc, 0x4000))
        q.wr("w", common + 0x16, 0)  # the event queue
        if q.rd("w", common + 0x18) < QSZ:
            sys.exit("as_input_guest: device %d has an event queue under %d" % (i, QSZ))
        q.wr("w", common + 0x18, QSZ)
        q.wr("q", common + 0x20, desc)
        q.wr("q", common + 0x28, avail)
        q.wr("q", common + 0x30, used)
        q.write(desc, b"".join(struct.pack("<QIHH", bufs + 8 * k, 8, 2, 0) for k in range(QSZ)))
        q.write(avail + 4, b"".join(struct.pack("<H", k) for k in range(QSZ)))
        q.wr("w", avail + 2, QSZ)
        q.wr("w", common + 0x1C, 1)
        notify = base + caps[2][1] + q.rd("w", common + 0x1E) * caps[2][2]
        kind = kind_of(q, devcfg)
        q.wr("b", common + 0x14, 15)  # DRIVER_OK: QEMU makes this device its newest handler
        q.wr("w", notify, 0)
        # Where `drain` keeps how far it has read: the word after the buffers.
        q.wr("w", bufs + 8 * QSZ, 0)
        print("input[%d] is the %s" % (i, kind))


# Each device's kind and the events (type, code, value) it has delivered since the last read.
def taken(q):
    for i, _cfg, base, caps in devices(q, False):
        devcfg = base + caps[4][1]
        _desc, avail, used, bufs = ram(i)
        seen = q.rd("w", bufs + 8 * QSZ)
        idx = q.rd("w", used + 2)
        events = []
        while seen != idx:
            ident, length = struct.unpack("<II", q.read(used + 4 + 8 * (seen % QSZ), 8))
            etype, code, value = struct.unpack("<HHI", q.read(bufs + 8 * ident, 8))
            if length != 8:
                sys.exit("as_input_guest: an event of %d bytes" % length)
            events.append((etype, code, value if value < 1 << 31 else value - (1 << 32)))
            # The buffer goes back to the device.
            a = q.rd("w", avail + 2)
            q.wr("w", avail + 4 + 2 * (a % QSZ), ident)
            q.wr("w", avail + 2, (a + 1) & 0xFFFF)
            seen = (seen + 1) & 0xFFFF
        q.wr("w", bufs + 8 * QSZ, seen)
        yield kind_of(q, devcfg), events


def raw(event):
    return "%s %#x %d" % (EV.get(event[0], event[0]), event[1], event[2])


def drain(q):
    for kind, events in taken(q):
        print("%s: %s" % (kind, "; ".join(raw(e) for e in events) if events else "nothing"))


# A pointer device's events as the reports the compositor makes of them (usr/tapestryd/src/main.rs,
# the tablet's drain and the mouse's): one report to a SYN, and a tablet's two axis values as the
# pixel value * (side - 1) / 32767 rounded down.
def as_reports(events, w, h):
    out, axes, rest = [], {}, []
    for etype, code, value in events:
        if etype == 0:
            parts = []
            if set(axes) == {0, 1}:
                parts.append("at %d,%d" % (min(axes[0], 32767) * (w - 1) // 32767, min(axes[1], 32767) * (h - 1) // 32767))
            elif axes:
                parts.append("ONE AXIS ONLY %r" % axes)
            if parts or rest:
                out.append(" ".join(parts + rest))
            axes, rest = {}, []
        elif etype == 3 and code in (0, 1):
            axes[code] = value
        elif etype == 1 and code in BTN:
            rest.append("%s %s" % ("press" if value else "release", BTN[code]))
        elif (etype, code) == (2, 8):
            rest.append("wheel-up" if value > 0 else "wheel-down")
        else:
            rest.append(raw((etype, code, value)))
    if axes or rest:
        out.append("NO SYN AFTER %r %r" % (axes, rest))
    return out


def reports(q):
    w, h = int(sys.argv[3]), int(sys.argv[4])
    for kind, events in taken(q):
        if kind == "keyboard":
            said = "; ".join(raw(e) for e in events)
        else:
            said = "".join(r + ";" for r in as_reports(events, w, h))
        print("%s: %s" % (kind, said or "nothing"))


q = Qtest(sys.argv[1])
{"up": up, "drain": drain, "reports": reports}[sys.argv[2]](q)
