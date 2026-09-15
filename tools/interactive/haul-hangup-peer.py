#!/usr/bin/env python3
"""A host-side TCP peer that hangs up on haul at a chosen moment.

Used by the LS-CI scenarios proving that a server which hangs up makes haul
FAIL rather than hang: tools/interactive/haul-hangup.exp, and the encrypted
leg of haul-npxf.exp.

  --mode accept-close
      Accept one connection and hang up on it at once. Plain 9P reaches the
      attach with nothing in the way, so this is the whole reproduction.

  --mode relay-handshake --upstream PORT
      Accept one connection, relay npxf's three handshake flights byte-exactly
      to 127.0.0.1:PORT (40 bytes up, 64 down, 32 up), then hang up on both
      sides. haul's handshake therefore SUCCEEDS against the real server, and
      the connection dies before the first reply can come back.

HANGING UP MEANS A FIN, NOT A RESET. Closing a socket that still holds unread
bytes sends RST on both Linux and Darwin, and in relay mode haul's first record
may already be queued. So the peer half-closes (SHUT_WR, which sends the FIN),
drains whatever arrives until the other side closes too or the drain window
expires, and only then closes. The defect under test is a server that hangs
up; a reset takes a different path through netd and would need its own leg.

Every event is one flushed line in --log, so a scenario asserts what this peer
DID rather than what it was asked to do. A relay that never reached the server
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
import sys
import time

# (bytes, name, direction): npxf's msg1, msg2, msg3.
FLIGHTS = ((40, "flight 1", "up"), (64, "flight 2", "down"), (32, "flight 3", "up"))


class ParentGone(Exception):
    pass


def main():
    ap = argparse.ArgumentParser(description="Hang up on haul at a chosen moment.")
    ap.add_argument("--port", type=int, required=True,
                    help="0 lets the kernel pick a free port; the port bound is logged")
    ap.add_argument("--log", required=True)
    ap.add_argument("--mode", choices=("accept-close", "relay-handshake"), required=True)
    ap.add_argument("--upstream", type=int)
    ap.add_argument("--deadline", type=float, default=900.0,
                    help="seconds before giving up on the whole exchange")
    ap.add_argument("--drain", type=float, default=10.0,
                    help="seconds to wait for the far side to close after our FIN")
    args = ap.parse_args()
    if args.mode == "relay-handshake" and args.upstream is None:
        ap.error("--mode relay-handshake needs --upstream PORT")

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

    def hang_up(sock, name):
        sock.shutdown(socket.SHUT_WR)
        say("%s: sent FIN" % name)
        more = 0
        until = min(deadline, time.monotonic() + args.drain)
        try:
            while True:
                wait_readable(sock, until)
                chunk = sock.recv(65536)
                if not chunk:
                    say("%s: the other side closed (%d bytes arrived after our FIN)"
                        % (name, more))
                    break
                more += len(chunk)
        except ConnectionResetError:
            say("%s: the other side reset (%d bytes arrived after our FIN)" % (name, more))
        except TimeoutError:
            # Not an error for this peer: it is exactly what a haul stuck in its
            # attach looks like from here, so it is logged as a witness.
            say("%s: STILL OPEN %.0fs after our FIN (%d bytes arrived)"
                % (name, args.drain, more))
        sock.close()

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
