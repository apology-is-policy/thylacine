# Native `/net` applications (aux track)

User-facing network tools authored natively on `libthyla_rs::net` (the Plan 9
`/net` client: `TcpStream`/`TcpListener`/`UdpSocket`/`IcmpSocket` + `resolve` over
`/net/cs`). No socket syscalls, no pouch -- a buggy client corrupts only its own
state (the kernel + netd validate every op), so each composes I-1/I-5/I-23/I-28
and adds no invariant.

**Why now:** the main track is optimizing `/net` throughput. The existing bench
(`netperf`) is loopback-only *by design* -- it isolates our stack's per-op CPU
cost and deliberately excludes the NIC RX-wake floor. These tools add the missing
half: a **load generator** (`nc`/`httpd`/`tcpproxy`) and an **over-the-wire
yardstick** (`nettest`, and `weft-bench` for the zero-copy dataplane), so the
main agent can *see* an optimization land end-to-end on the real NIC.

**Design language:** these match the coreutils -- the Bonfire palette on by
default (`--color[=WHEN]`, `never` for a clean pipe) and `--help` with worked
examples on every tool. The discipline holds: color is on PRESENTATION and
DIAGNOSTICS (status banners, errors, reports), never on a PAYLOAD a pipe consumes
-- `nc`'s pumped bytes on stdout stay byte-clean.

**Verification ceiling (aux):** `cargo build` only -- the aux track never boots
QEMU. Each tool is compile- and clippy-clean here; the **functional + throughput
tests below are the in-VM backlog** for the main track to run on merge. The
unlock that makes a guest-side server measurable: even though netd's live stack
is NIC-only (no in-guest peer can reach it), **the host *is* a peer over the
NIC** -- so host-paired tests (host drives load <-> guest serves/sinks) measure
exactly the real-NIC path the throughput work targets.

Build all native bins: `tools/build.sh userspace` (or, per tool,
`cargo build -p coreutils --bin <name> --release` from `usr/`).

## Slate

| App | Kind | Status | Purpose |
|---|---|---|---|
| `nc` | coreutils bin | **built** | connect/listen byte-pump (TCP) + `-u` UDP; the universal tool + bulk load generator |
| `dial` | coreutils bin | **built** | `/net/cs` reachability tester (Plan 9 `dial`) + RTT |
| `con` | coreutils bin | **built** | interactive Plan 9 dial-string client (service-name aware) |
| `tcpproxy` | coreutils bin | **built** | listen/accept/dial-upstream splice; a throughput torture + port-forwarder |
| `httpd` | crate | **built** | tiny static HTTP/1.1 server; host-curl a big file -> real-protocol throughput |
| `nettest` | crate | **built** | host-paired sustained-bandwidth bench (the number `netperf` can't produce) |
| `weft-bench` | crate | **staged** | zero-copy yardstick; skeleton built (copy-tax baseline), zero-copy path awaits Weft-6c |
| `tftp` | -- | deferred | blocked by a UdpSocket `recvfrom` gap (the TID-port switch) -- see DOC-GAPs; `nc -u` covers UDP bulk |

The shared pump + cs-resolution live in `coreutils::netpump` (`stdio_pump`,
`udp_pump`, `splice`, `cs_resolve`) -- one correct implementation of the
non-blocking, POLLOUT-backpressured pump for all four tools.

## A tour: `net-demo.ut`

`net-demo.ut` (baked 0755 into the cpio by `tools/build.sh`; run it as
`net-demo.ut` from any `ut` prompt) is a guided tour. It runs only what works
in-guest with no host peer -- `ipconfig` (the DHCP lease), `ping 127.0.0.1` (the
net-8a loopback auto-answers, deterministic), `weft-bench` (the copy-tax), and a
best-effort bounded `dial 1.1.1.1 443` (out through slirp) -- then prints the
toolbox + the host-paired throughput commands. No backgrounded servers, no
unbounded connects: it never hangs, and `dial` degrades gracefully to
"unreachable" if there is no outbound route. (aux can't boot, so the script is
authored to the proven `fun.ut` dialect but is unrun -- the main track's first
in-VM boot is its test.)

---

## `nc` -- netcat

`nc HOST PORT` (TCP connect), `nc -l PORT` (TCP listen, accept one), or
`nc -u HOST PORT` (UDP connect). `-v` prints connection progress; the payload on
stdout is uncolored.

**Implementation note (the load-bearing bit):** netd's data write is
*non-blocking* -- a full send window returns a 0-count write, not a deferred
reply (`net.rs`). So `nc` polls the connection's readiness sibling
(`/net/tcp/N/ready`, net-6b) for **POLLOUT** before retrying a send, and pumps
half-duplex-buffered. A naive `write_all` would spuriously fail the instant the
first window fills -- exactly the bulk-transfer case. On stdin EOF the TCP send
side half-closes (a FIN, like `nc -N`); UDP has no FIN, so `nc -u` drains replies
briefly after EOF then exits.

**Build:** `cargo build -p coreutils --bin nc --release` -- clean.

### Test plan (in-VM)

**T1 -- functional, in-guest loopback (deterministic).** netd has a resident
127.0.0.1 loopback (net-8a) that auto-routes. In `ut`:
```
nc -l 9999 > /tmp/got &        # background sink
echo -n hello | nc 127.0.0.1 9999
wait; cat /tmp/got             # expect: hello
```
If cross-Proc loopback routing is not yet wired (the `loopback_e2e` proof is
single-Proc), fall back to T2/T3.

**T2 -- request/response against a live service (host-paired).** With a host
`nc -l 8080`: `printf 'GET / HTTP/1.0\r\n\r\n' | nc -v <host> 8080` -- the request
is sent, the FIN terminates it, the response prints.

**T3 -- TCP throughput sink (the real-NIC number).** Boot QEMU with an inbound
forward, e.g. `-netdev user,hostfwd=tcp::5555-:9999`. Guest: `nc -l 9999 >
/dev/null`. Host: `time head -c 100000000 /dev/zero | nc localhost 5555`. Record
wall time -> MB/s -- the real-NIC sustained receive rate (incl. the RX-wake
floor) that `netperf`'s loopback M2 cannot produce. Re-run after a throughput
change to confirm the win on the wire.

**T4 -- UDP datagram pump.** Guest `echo -n hi | nc -u 10.0.2.2 9` (a discard/echo
service). UDP throughput: guest `nc -u <host> 9999 < big.bin` (source) <-> host
`nc -ul 9999 > /dev/null` (sink) -- the guest exercises the UDP datapath
(net-3b). UDP is connect-only here (guest is always the source; see the gap).

**Edge cases:** unresolvable host -> exit 1 + stderr diagnostic, never a hang;
`nc -lu` -> usage error (UDP listen unsupported); port 0 -> usage error; `--help`
-> usage on stdout, exit 0.

---

## `dial` -- Plan 9 connection tester

`dial HOST PORT` or `dial [tcp!]HOST!SERVICE` -- resolve through `/net/cs` and
open a bounded TCP connection, reporting the resolved address and the RTT to
ESTABLISHED. Showcases `/net/cs` resolving host AND named service in one query.
Colored report by default.

**Build:** clean. **Test (in-VM):**
```
dial 127.0.0.1 7          # -> resolves, connects, prints RTT (sub-ms loopback)
dial 10.0.2.2 79          # the slirp gateway: refused or reachable, bounded
dial tcp!example.com!http # host + service resolution via /net/cs (host-dep)
dial nope.invalid 80      # -> "cannot resolve", exit 1, no hang
```

---

## `con` -- interactive connect

`con HOST PORT` / `con [tcp!]HOST!SERVICE` -- the interactive client: resolve via
`/net/cs`, bridge the console to the connection so you can talk to a line
protocol by hand. Ctrl-D half-closes; the peer's close ends it. Shares `nc`'s
pump.

**Build:** clean. **Test (in-VM, interactive):**
```
con 127.0.0.1 7                     # type lines; see them echoed; Ctrl-D closes
con tcp!example.com!http            # then: GET / HTTP/1.0 <enter><enter>
```
(Interactive -- the expect/PTY harness drives it; not a pure-stdin pipe.)

---

## `tcpproxy` -- forwarding splice

`tcpproxy LPORT HOST PORT` -- announce `*!LPORT`, accept a connection, dial
HOST:PORT, splice both directions until both close; loop to the next call. A
useful port-forwarder AND a deliberate throughput TORTURE: every byte crosses
netd's stack twice. The bidirectional `splice` (two independent half-duplex legs,
POLLOUT-backpressured, FIN-propagating) is in `coreutils::netpump`.

**Build:** clean. **Test (in-VM):**
```
# functional: proxy in front of the in-guest echo
tcpproxy 8080 127.0.0.1 7 &
echo -n hi | nc 127.0.0.1 8080        # expect: hi (echoed through the proxy)

# throughput torture: front a sink, measure the doubled-stack rate
tcpproxy 9000 127.0.0.1 9999 &        # forwards :9000 -> a local sink on :9999
# then drive :9000 host-paired as in nc T3; compare MB/s vs the direct sink.
```

---

## DOC-GAPs surfaced (for the main track)

**G-NET-1: `UdpSocket` has no `recvfrom` / `sendto`.** `net.rs`'s `UdpSocket` is
connected-peer only: `recv` returns bytes without the source address, and `send`
goes only to the connect-time remote. This **blocks a native TFTP client**: RFC
1350 requires the server to reply from a fresh ephemeral TID port and the client
to direct subsequent packets there -- which needs the source address (`recvfrom`)
and an arbitrary-destination send (`sendto`). It also blocks any UDP request/multi-
peer server. netd's `/net/udp` `ctl` already *re-points* the remote on every
`connect` write (net-3b), so the kernel/netd side is close; the gap is the
userspace API surface. Suggested: `UdpSocket::recv_from(&mut self, buf) ->
(usize, SocketAddrV4)` + `send_to(&mut self, buf, SocketAddrV4)`. (`nc -u` covers
the connected-peer UDP bulk case in the meantime.)

**G-NET-2: no UDP bind / announce.** `UdpSocket::connect` binds an *ephemeral*
local port; there is no "bind to a fixed local port" (a UDP `announce`), so a UDP
*server* cannot listen on a known port -- hence `nc -lu` is rejected and the
guest is always the UDP source. Suggested: a `UdpSocket::bind(SocketAddrV4)` over
netd's `/net/udp` announce path.

**G-NET-3: `--color=auto` can't probe the TTY** (pre-existing, tracked).
Standalone native programs can't tell whether stdout is a console (both console
and pipe are path-less `KOBJ_SPOOR`), so `--color=auto` parks color-on via a
stub. Closed by the proposed `SYS_FD_DEVCLASS` (main's `docs/SYS-FD-DEVCLASS-
SPEC.md`). Until then these tools default to `Always` and honor `--color=never`.

---

## `httpd` -- static HTTP/1.1 server

`httpd [-p PORT] [DIR]` -- serve files under DIR (default `/`) on PORT (default
8080); GET + HEAD, one connection at a time, `Connection: close`. The response
body is STREAMED (chunked file reads + `send_all` backpressure), so it serves
files far larger than the userspace heap -- the throughput case. A colored access
log prints per request.

**Build:** clean (clippy-clean). **Test plan (in-VM):**

**T1 -- functional.** Put a file in the namespace and GET it (host with
`-netdev user,hostfwd=tcp::8080-:8080`):
```
echo hello > /tmp/index.html
httpd -p 8080 /tmp &
# host:
curl http://localhost:8080/                  # -> hello   (/ maps to /index.html)
curl -I http://localhost:8080/index.html     # HEAD: headers only
curl http://localhost:8080/nope              # -> 404 Not Found
curl http://localhost:8080/../etc            # -> 400 (traversal rejected)
```

**T2 -- throughput (the real-NIC, real-protocol number).** Serve a big file,
host-curl it, measure:
```
# create a big file in the guest namespace, then:
httpd -p 9999 /srv/www &
# host:
curl -s -o /dev/null -w '%{speed_download} B/s\n' http://localhost:9999/big.bin
```
Re-run after a throughput change to confirm the win end to end -- the streamed
`send_all` path exercises the same POLLOUT backpressure as the bulk sender.

**Security:** the namespace is the sandbox (httpd reaches only what its territory
grants and its identity may read, I-1/I-22/I-23/I-28); `..` traversal is rejected
on top of that.

## `nettest` -- wire throughput bench

`nettest -s [-p PORT]` (sink: accept, drain to EOF, report received MB/s) or
`nettest -c HOST [-p PORT] [-n MB]` (source: send MB megabytes, report sent
MB/s). The over-the-wire number `netperf`'s loopback design omits -- it includes
the NIC RX-wake floor. Rate math is integer-only (the no_std float path is not
linked). Host-paired: a host load tool drives the other end.

**Build:** clean (clippy-clean). **Test plan (in-VM, host-paired):**

**T1 -- guest sink, host source (NIC-in):**
```
# guest:
nettest -s                              # sink on :5555
# host (with -netdev user,hostfwd=tcp::5555-:5555):
head -c 200000000 /dev/zero | nc localhost 5555
# guest prints: received 200000000 bytes in T s = X MB/s
```

**T2 -- guest source, host sink (NIC-out):**
```
# host:  nc -l 5555 > /dev/null         (or iperf -s)
# guest:
nettest -c <host> -n 128                # send 128 MB; prints MB/s
```

Compare against `netperf`'s loopback M2 (which excludes the NIC) to see the wire
overhead; re-run after a throughput change to confirm the win on the NIC. A
wire-RTT percentile mode (p50/p99) needs a paired echo peer -- a follow-on.

## `weft-bench` -- zero-copy dataplane yardstick (staged)

The yardstick for the Weft NOVEL (the per-flow capability-scoped zero-copy `/net`
path, `docs/NET-THROUGHPUT.md`). The native Weft push/pop/wait API lands at
Weft-6c; this is the aux Phase-B skeleton -- it compiles and runs the BASELINE
now, the zero-copy path clearly staged:

- **Baseline (now):** `weft-bench [-n MB]` measures the memcpy **copy-tax** -- the
  userspace per-byte copy bandwidth, the ceiling on any copy-based dataplane and
  the cost Weft's zero-copy path removes. A real, motivating number.
- **Zero-copy (Weft-6c):** register a flow ring (anon Burrow), push payload
  descriptors (no copy), wait on the readiness ring; report ops/s + the speedup
  vs the copy-tax. Stubbed with a note today.

**Build:** clean (clippy-clean). **Test (in-VM, now):** `weft-bench` prints the
copy-tax MB/s + the staged note; `weft-bench -n 1024` runs over 1 GiB. **When
Weft-6c lands:** fill in the zero-copy path (the API surface is sketched in the
source + NET-THROUGHPUT.md section 6) and the bench reports the win directly.
