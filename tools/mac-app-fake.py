#!/usr/bin/env python3
"""mac-app-fake -- the Mac app's end of the line to the Mac, for guest witnesses (docs/MAC-RUNNER.md 4.4).

It connects to the QEMU chardev socket behind the port `org.thylacine.mac`, speaks the channel
framing `mac1`, and runs one 9P2000.L session per channel over the tree version 1: `snarf` and an
empty `folders/`. It is written from 4.4, not from usr/lib/macline, so the guest's driver and this
file are two readings of the framing and a witness fails where they differ.

Every event it sees goes to --log as one line (`attach n_uname=1001 aname=''`, `snarf set 3 bytes
'hi\\n'`, `line malformed: ...`), flushed at once: the scenario asserts on the host's own record.

usage: mac-app-fake.py --socket <path> --log <path> [--snarf <text>] [--once]
  --once  serve one connection and exit when it ends (otherwise connect again, as the app does)
"""
import argparse
import os
import socket
import struct
import sys
import time

HELLO, OPEN, DATA, CLOSE, RESET = range(5)
VERSION = b"mac1"
MAXFRAME = 65536
SNARF_MAX = 16 << 20

# Linux errno values, as 9P2000.L carries them.
EPERM, ENOENT, EBADF, EACCES, EEXIST, ENOTDIR, EISDIR, EINVAL, EFBIG, EROFS, EPROTO, EOPNOTSUPP = (
    1, 2, 9, 13, 17, 20, 21, 22, 27, 30, 71, 95)
O_ACCMODE, O_WRONLY, O_RDWR = 3, 1, 2

QTDIR, QTFILE = 0x80, 0x00
S_IFDIR, S_IFREG = 0o040000, 0o100000
ROOT, SNARF, FOLDERS = 1, 2, 3
NODES = {
    ROOT: (QTDIR, S_IFDIR | 0o700, {b"snarf": SNARF, b"folders": FOLDERS}),
    SNARF: (QTFILE, S_IFREG | 0o600, None),
    FOLDERS: (QTDIR, S_IFDIR | 0o700, {}),
}


class Malformed(Exception):
    pass


class Log:
    def __init__(self, path):
        self.f = open(path, "a", buffering=1)

    def __call__(self, line):
        self.f.write("%.3f %s\n" % (time.time(), line))
        self.f.flush()


def frame(kind, chan, data=b""):
    return struct.pack("<IBI", 9 + len(data), kind, chan) + data


def hello():
    return frame(HELLO, 0, struct.pack("<H", len(VERSION)) + VERSION + struct.pack("<I", MAXFRAME))


def s9(b):
    return struct.pack("<H", len(b)) + b


class Reader:
    """A 9P message body, read field by field; any short read is a protocol error."""

    def __init__(self, b):
        self.b, self.at = b, 0

    def take(self, n):
        if self.at + n > len(self.b):
            raise ValueError("short")
        v = self.b[self.at:self.at + n]
        self.at += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def s(self):
        return self.take(self.u16())


def qid(node):
    return struct.pack("<BIQ", NODES[node][0], 0, node)


class Session:
    """One channel's 9P2000.L session over the tree version 1."""

    def __init__(self, chan, log, state):
        self.chan, self.log, self.state = chan, log, state
        self.buf = b""
        self.msize = 8192
        self.uid = None
        self.fids = {}  # fid -> {"node", "mode" (None until opened), "snap", "wbuf"}

    def feed(self, data):
        """Bytes of the channel's stream in; the replies out."""
        self.buf += data
        out = b""
        while len(self.buf) >= 4:
            size = struct.unpack("<I", self.buf[:4])[0]
            if size < 7 or size > max(self.msize, 8192):
                raise Malformed("chan %d: 9P size %d" % (self.chan, size))
            if len(self.buf) < size:
                break
            msg, self.buf = self.buf[:size], self.buf[size:]
            t, tag = msg[4], struct.unpack("<H", msg[5:7])[0]
            try:
                rtype, body = self.handle(t, Reader(msg[7:]))
            except ValueError:
                rtype, body = 7, struct.pack("<I", EPROTO)
            except OSError as e:
                rtype, body = 7, struct.pack("<I", e.errno)
            out += struct.pack("<IBH", 7 + len(body), rtype, tag) + body
        return out

    def fid(self, n):
        f = self.fids.get(n)
        if f is None:
            raise OSError(EBADF, "fid")
        return f

    def handle(self, t, r):
        if t == 100:  # Tversion msize[4] version[s]
            msize, version = r.u32(), r.s()
            self.msize = min(msize, MAXFRAME)
            self.fids.clear()
            self.log("chan %d version msize=%d %r" % (self.chan, msize, version.decode("latin-1")))
            return 101, struct.pack("<I", self.msize) + s9(b"9P2000.L" if version == b"9P2000.L" else b"unknown")
        if t == 104:  # Tattach fid afid uname aname n_uname
            fid, _afid, uname, aname, n_uname = r.u32(), r.u32(), r.s(), r.s(), r.u32()
            self.log("chan %d attach n_uname=%d uname=%r aname=%r" % (
                self.chan, n_uname, uname.decode("latin-1"), aname.decode("latin-1")))
            if aname != b"":
                raise OSError(ENOENT, "aname")
            if fid in self.fids:
                raise OSError(EEXIST, "fid")
            self.uid = n_uname
            self.fids[fid] = {"node": ROOT, "mode": None}
            return 105, qid(ROOT)
        if t == 110:  # Twalk fid newfid nwname wname*
            fid, newfid, n = r.u32(), r.u32(), r.u16()
            names = [r.s() for _ in range(n)]
            f = self.fid(fid)
            if f["mode"] is not None:
                raise OSError(EBADF, "walk of an open fid")
            if newfid != fid and newfid in self.fids:
                raise OSError(EEXIST, "newfid")
            node, qids = f["node"], []
            for name in names:
                kids = NODES[node][2]
                if name == b"..":
                    nxt = ROOT
                elif kids is None:
                    nxt = None
                else:
                    nxt = kids.get(name)
                if nxt is None:
                    break
                node = nxt
                qids.append(qid(node))
            if names and not qids:
                raise OSError(ENOENT, "walk")
            if len(qids) == len(names):
                self.fids[newfid] = {"node": node, "mode": None}
            return 111, struct.pack("<H", len(qids)) + b"".join(qids)
        if t == 12:  # Tlopen fid flags
            fid, flags = r.u32(), r.u32()
            f = self.fid(fid)
            acc = flags & O_ACCMODE
            node = f["node"]
            if NODES[node][0] == QTDIR and acc != 0:
                raise OSError(EISDIR, "dir")
            if node == SNARF and acc == O_RDWR:
                raise OSError(EACCES, "snarf O_RDWR")
            f["mode"] = acc
            if node == SNARF and acc == O_WRONLY:
                f["wbuf"] = b""
            self.log("chan %d open %s flags=%#o" % (self.chan, self.name(node), flags))
            return 13, qid(node) + struct.pack("<I", 0)
        if t == 116:  # Tread fid offset count
            fid, off, count = r.u32(), r.u64(), r.u32()
            f = self.fid(fid)
            if f["mode"] is None or f["mode"] == O_WRONLY:
                raise OSError(EBADF, "not open for reading")
            if f["node"] != SNARF:
                raise OSError(EISDIR, "dir")
            if off == 0:
                f["snap"] = self.state["snarf"]
                self.log("chan %d snarf read %d bytes" % (self.chan, len(f["snap"])))
            snap = f.get("snap", b"")
            if len(snap) > SNARF_MAX:
                raise OSError(EFBIG, "snarf")
            data = snap[off:off + min(count, self.msize - 11)]
            return 117, struct.pack("<I", len(data)) + data
        if t == 118:  # Twrite fid offset count data
            fid, _off, count = r.u32(), r.u64(), r.u32()
            data = r.take(count)
            f = self.fid(fid)
            if f["node"] != SNARF or f["mode"] != O_WRONLY:
                raise OSError(EBADF, "not open for writing")
            if len(f["wbuf"]) + len(data) > SNARF_MAX:
                raise OSError(EFBIG, "snarf")
            f["wbuf"] += data
            return 119, struct.pack("<I", len(data))
        if t == 120:  # Tclunk fid
            fid = r.u32()
            f = self.fid(fid)
            del self.fids[fid]
            if "wbuf" in f:
                text = f["wbuf"].decode("utf-8", "replace").encode("utf-8")
                self.state["snarf"] = text
                self.log("chan %d snarf set %d bytes %r" % (self.chan, len(text), text.decode("utf-8")))
            return 121, b""
        if t == 24:  # Tgetattr fid request_mask
            fid, _mask = r.u32(), r.u64()
            node = self.fid(fid)["node"]
            uid = self.uid or 0
            body = struct.pack("<Q", 0x3FFF) + qid(node) + struct.pack(
                "<IIIQQQQQ", NODES[node][1], uid, uid, 1, 0, 0, 4096, 0) + b"\0" * (8 * 10)
            return 25, body
        if t == 40:  # Treaddir fid offset count
            fid, off, count = r.u32(), r.u64(), r.u32()
            f = self.fid(fid)
            kids = NODES[f["node"]][2]
            if kids is None or f["mode"] is None:
                raise OSError(ENOTDIR if kids is None else EBADF, "readdir")
            out, i = b"", 0
            for i, (name, node) in enumerate(sorted(kids.items()), start=1):
                if i <= off:
                    continue
                ent = qid(node) + struct.pack("<QB", i, 4 if NODES[node][0] == QTDIR else 8) + s9(name)
                if len(out) + len(ent) > count:
                    break
                out += ent
            return 41, struct.pack("<I", len(out)) + out
        if t == 26:  # Tsetattr fid valid mode uid gid size ...
            fid, valid = r.u32(), r.u32()
            r.u32(), r.u32(), r.u32()
            size = r.u64()
            node = self.fid(fid)["node"]
            if valid & (0x2 | 0x4):
                raise OSError(EPERM, "ownership")
            if valid & 0x8 and not (node == SNARF and size == 0):
                raise OSError(EINVAL, "size")
            return 27, b""
        if t == 108:  # Tflush oldtag
            r.u16()
            return 109, b""
        if t == 8:  # Tstatfs fid
            self.fid(r.u32())
            return 9, struct.pack("<IIQQQQQQI", 0x01021997, 4096, 0, 0, 0, 0, 0, 0, 255)
        self.log("chan %d unanswered T%d" % (self.chan, t))
        raise OSError(EOPNOTSUPP, "op")

    def name(self, node):
        return {ROOT: "/", SNARF: "snarf", FOLDERS: "folders"}[node]


class Line:
    """The app's end of the line: frames in, sessions per channel, frames out."""

    def __init__(self, sock, log, state):
        self.sock, self.log, self.state = sock, log, state
        self.buf = b""
        self.peer_max = None
        self.chans = {}  # chan -> Session, or "sent" once this end's CLOSE went

    def send(self, b):
        self.sock.sendall(b)

    def data(self, chan, b):
        room = self.peer_max - 9
        for i in range(0, len(b), room):
            self.send(frame(DATA, chan, b[i:i + room]))

    def frames(self):
        while len(self.buf) >= 4:
            size = struct.unpack("<I", self.buf[:4])[0]
            if size < 9 or size > MAXFRAME:
                raise Malformed("size %d" % size)
            if len(self.buf) >= 5 and self.buf[4] > RESET:
                raise Malformed("type %d" % self.buf[4])
            if len(self.buf) < size:
                return
            f, self.buf = self.buf[:size], self.buf[size:]
            yield f[4], struct.unpack("<I", f[5:9])[0], f[9:]

    def run(self):
        while True:
            b = self.sock.recv(65536)
            if not b:
                self.log("line closed by QEMU")
                return
            self.buf += b
            for kind, chan, data in list(self.frames()):
                self.frame(kind, chan, data)

    def frame(self, kind, chan, data):
        if self.peer_max is None:
            if kind != HELLO:
                self.log("discarded %d on chan %d before the driver's HELLO" % (kind, chan))
                return
            if chan != 0 or len(data) < 2:
                raise Malformed("hello")
            n = struct.unpack("<H", data[:2])[0]
            if len(data) != 2 + n + 4 or data[2:2 + n] != VERSION:
                raise Malformed("hello version %r" % data[2:2 + n])
            m = struct.unpack("<I", data[2 + n:])[0]
            if m < 4096:
                raise Malformed("hello maxframe %d" % m)
            self.peer_max = m
            self.log("hello maxframe=%d" % m)
            self.send(hello())
            return
        if kind == HELLO:
            raise Malformed("second hello")
        if kind == RESET:
            if chan != 0 or data:
                raise Malformed("reset on chan %d" % chan)
            self.log("reset by the driver")
            raise Malformed("reset")
        if kind == OPEN:
            if chan == 0 or data or chan in self.chans:
                raise Malformed("open %d" % chan)
            self.chans[chan] = Session(chan, self.log, self.state)
            self.log("open chan %d" % chan)
            return
        s = self.chans.get(chan)
        if kind == DATA:
            if s is None:
                raise Malformed("data on chan %d not open" % chan)
            if s == "sent":
                return
            reply = s.feed(data)
            if reply:
                self.data(chan, reply)
            return
        if kind == CLOSE:
            if s is None or data:
                raise Malformed("close %d" % chan)
            del self.chans[chan]
            if s != "sent":
                self.send(frame(CLOSE, chan))
            self.log("close chan %d" % chan)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--snarf", default="")
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()
    log = Log(a.log)
    state = {"snarf": a.snarf.encode("utf-8")}
    log("start pid=%d socket=%s" % (os.getpid(), a.socket))
    while True:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(a.socket)
        except OSError:
            sock.close()
            time.sleep(0.2)
            continue
        log("connected")
        try:
            Line(sock, log, state).run()
        except Malformed as e:
            log("line malformed: %s" % e)
        except OSError as e:
            log("line error: %s" % e)
        finally:
            sock.close()
        if a.once:
            return 0


if __name__ == "__main__":
    sys.exit(main())
