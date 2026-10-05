#!/usr/bin/env python3
"""A host-side TCP peer that misbehaves at a chosen moment: it hangs up on
haul, or breaks the 9P session's msize.

Used by the LS-CI scenarios proving that a server which hangs up, or sends a
reply larger than the session allows, makes haul FAIL rather than hang:
tools/interactive/haul-hangup.exp, haul-post.exp, and the encrypted leg of
haul-npxf.exp.

  --mode accept-close
      Accept one connection and hang up on it at once. Plain 9P reaches the
      attach with nothing in the way, so this is the whole reproduction.

  --mode relay-handshake --upstream PORT
      Accept one connection, relay npxf's three handshake flights byte-exactly
      to 127.0.0.1:PORT (40 bytes up, 64 down, 32 up), then hang up on both
      sides. haul's handshake therefore SUCCEEDS against the real server, and
      the connection dies before the first reply can come back.

  --mode oversize-reply --reply N [--rversion-msize M]
      Speak just enough plain 9P2000.L to get a mount up -- an Rversion whose
      msize is M (default 4096), an Rattach naming a directory -- then answer
      the next request with an N-byte frame and keep the connection open. M is
      sent as given, even above the guest's proposal: a server may not do that,
      and the kernel keeps its own msize when one does, so a leg can leave the
      Tversion's msize as the only bound. No FIN: what is wrong is the server's
      frame, not its liveness, so the verdict is whether haul closes ITS side
      once it has refused the frame.

HANGING UP MEANS A FIN, NOT A RESET. Closing a socket that still holds unread
bytes sends RST on both Linux and Darwin, and in relay mode haul's first record
may already be queued. So the peer half-closes (SHUT_WR, which sends the FIN),
drains whatever arrives until the other side closes too or the drain window
expires, and only then closes. The defect under test is a server that hangs
up; a reset takes a different path through netd and would need its own leg.

Every event is one flushed line in --log, so a scenario asserts what this peer
DID rather than what it was asked to do. The last line about the guest is the
verdict on haul's side of the connection: "the other side closed" or "reset",
or "STILL OPEN" when the drain window passed with haul still connected. A relay that never reached the server
makes haul fail differently, and the log says which.

Pass --port 0 and read the port back from the `listening` line. A port the
scenario picks for itself is check-then-use: LS-CI runs scenarios in parallel,
two checks can pass for one port, and the second bind then fails.

It exits after one connection, when --deadline passes, or as soon as its parent
dies. The last matters because lc_fail exits the expect process without running
any scenario cleanup, and a leftover peer would hold its port.
"""

import argparse
import os
import select
import socket
import struct
import sys
import time

# (bytes, name, direction): npxf's msg1, msg2, msg3.
FLIGHTS = ((40, "flight 1", "up"), (64, "flight 2", "down"), (32, "flight 3", "up"))

# version(5) and the 9P2000.L attach. haul relays at most 64 KiB in a frame.
TVERSION, RVERSION, TATTACH, RATTACH = 100, 101, 104, 105
QTDIR = 0x80
MSG_MIN, MSG_MAX = 7, 64 * 1024


class ParentGone(Exception):
    pass


def main():
    ap = argparse.ArgumentParser(description="Hang up on haul at a chosen moment.")
    ap.add_argument("--port", type=int, required=True,
                    help="0 lets the kernel pick a free port; the port bound is logged")
    ap.add_argument("--log", required=True)
    ap.add_argument("--mode", choices=("accept-close", "relay-handshake", "oversize-reply"),
                    required=True)
    ap.add_argument("--upstream", type=int)
    ap.add_argument("--reply", type=int,
                    help="oversize-reply: the size of the frame that answers the first request")
    ap.add_argument("--rversion-msize", type=int, default=4096,
                    help="oversize-reply: the msize the Rversion carries, sent as given")
    ap.add_argument("--deadline", type=float, default=900.0,
                    help="seconds before giving up on the whole exchange")
    ap.add_argument("--drain", type=float, default=10.0,
                    help="seconds to wait for the far side to close after our FIN")
    args = ap.parse_args()
    if args.mode == "relay-handshake" and args.upstream is None:
        ap.error("--mode relay-handshake needs --upstream PORT")
    if args.mode == "oversize-reply" and not (args.reply and MSG_MIN <= args.reply <= MSG_MAX):
        ap.error("--mode oversize-reply needs --reply N, %d..%d" % (MSG_MIN, MSG_MAX))

    parent = os.getppid()
    deadline = time.monotonic() + args.deadline
    log = open(args.log, "w")

    def say(msg):
        log.write(msg + "\n")
        log.flush()

    def wait_readable(sock, until):
        while True:
            if os.getppid() != parent:
                raise ParentGone("the scenario exited")
            left = until - time.monotonic()
            if left <= 0:
                raise TimeoutError("deadline passed")
            ready, _, _ = select.select([sock], [], [], min(1.0, left))
            if ready:
                return

    def recv_exact(sock, n):
        buf = b""
        while len(buf) < n:
            wait_readable(sock, deadline)
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("closed after %d of %d bytes" % (len(buf), n))
            buf += chunk
        return buf

    def recv_frame(sock):
        hdr = recv_exact(sock, 4)
        size = struct.unpack("<I", hdr)[0]
        if not MSG_MIN <= size <= MSG_MAX:
            raise EOFError("a frame claiming %d bytes" % size)
        frame = hdr + recv_exact(sock, size - 4)
        return frame[4], struct.unpack("<H", frame[5:7])[0], frame

    def send_frame(sock, kind, tag, body):
        sock.sendall(struct.pack("<IBH", MSG_MIN + len(body), kind, tag) + body)

    def wait_close(sock, name, since):
        """Drain until the other side closes or the window passes; log which."""
        more = 0
        until = min(deadline, time.monotonic() + args.drain)
        try:
            while True:
                wait_readable(sock, until)
                chunk = sock.recv(65536)
                if not chunk:
                    say("%s: the other side closed (%d bytes arrived after %s)"
                        % (name, more, since))
                    break
                more += len(chunk)
        except ConnectionResetError:
            say("%s: the other side reset (%d bytes arrived after %s)" % (name, more, since))
        except TimeoutError:
            # Not an error for this peer: it is exactly what a stuck haul looks
            # like from here, so it is logged as a witness.
            say("%s: STILL OPEN %.0fs after %s (%d bytes arrived)"
                % (name, args.drain, since, more))
        sock.close()

    def hang_up(sock, name):
        sock.shutdown(socket.SHUT_WR)
        say("%s: sent FIN" % name)
        wait_close(sock, name, "our FIN")

    def oversize_reply(sock):
        kind, tag, frame = recv_frame(sock)
        if kind != TVERSION or len(frame) < 11:
            raise EOFError("the first frame is type %d, not a Tversion" % kind)
        proposed = struct.unpack("<I", frame[7:11])[0]
        version = b"9P2000.L"
        send_frame(sock, RVERSION, tag,
                   struct.pack("<IH", args.rversion_msize, len(version)) + version)
        say("served Tversion: the guest proposed %d, we answered %d"
            % (proposed, args.rversion_msize))
        kind, tag, _ = recv_frame(sock)
        if kind != TATTACH:
            raise EOFError("the second frame is type %d, not a Tattach" % kind)
        send_frame(sock, RATTACH, tag, struct.pack("<BIQ", QTDIR, 0, 1))
        say("served Tattach")
        kind, tag, _ = recv_frame(sock)
        send_frame(sock, kind + 1, tag, bytes(args.reply - MSG_MIN))
        say("sent a %d-byte reply to request type %d" % (args.reply, kind))
        wait_close(sock, "guest", "the oversized reply")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(1)
    say("listening 127.0.0.1:%d mode=%s" % (listener.getsockname()[1], args.mode))

    try:
        wait_readable(listener, deadline)
        conn, peer = listener.accept()
        listener.close()
        say("accepted %s:%d" % peer)
        if args.mode == "accept-close":
            hang_up(conn, "guest")
        elif args.mode == "oversize-reply":
            oversize_reply(conn)
        else:
            up = socket.create_connection(("127.0.0.1", args.upstream), timeout=10)
            up.settimeout(None)
            say("upstream connected 127.0.0.1:%d" % args.upstream)
            for n, name, direction in FLIGHTS:
                src, dst = (conn, up) if direction == "up" else (up, conn)
                data = recv_exact(src, n)
                dst.sendall(data)
                say("relayed %s %s: %d bytes" % (name, direction, len(data)))
            hang_up(conn, "guest")
            hang_up(up, "upstream")
    except (ParentGone, TimeoutError, EOFError, OSError) as e:
        say("ERROR %s: %s" % (type(e).__name__, e))
        return 1
    say("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
