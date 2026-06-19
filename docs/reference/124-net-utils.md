# 124 — net-utils: the native network CLI tools

The standard network command-line tools, authored as native libthyla-rs
programs (the Plan 9 split — native, not pouch-ported; ARCH §3.5). All are
**clients of netd's `/net` tree**: they touch no hardware (netd owns the NIC,
I-5) and reach only the `/net` (and `/etc` for TLS roots) their territory grants
(I-1/I-23/I-28). The kernel is byte-unchanged by every tool here.

| Tool | Crate | /net surface | One-line |
|---|---|---|---|
| `nslookup` | `coreutils` | `/net/cs` | resolve a name → IPv4 |
| `ping` | `coreutils` | `/net/icmp` | ICMP echo round-trip + RTT |
| `curl` | `curl` | `/net/{cs,tcp}` + TLS | HTTP/HTTPS GET → stdout |
| `wget` | `curl` (bin) | `/net/{cs,tcp}` + TLS | HTTP/HTTPS download → file |

`nslookup`/`ping` ride only existing syscalls + the `coreutils` deps, so they
live in the `coreutils` crate. `curl`/`wget` link the rustls `tls` crate (heavy)
and so live in their own `curl` crate — `curl` and `wget` are two bins over one
shared engine lib (`usr/curl/src/lib.rs`), so the URL parsing + HTTP exchange
exist in exactly one place.

## Resolution: `net::resolve` (the shared front door)

Every tool resolves a host through `libthyla_rs::net::resolve(host, port)`
(`usr/lib/libthyla-rs/src/net.rs`) — the one path all `/net` clients share:

- A dotted-quad resolves locally (no round-trip).
- A name is dialed at `/net/cs` as `tcp!<host>!<port>`; the reply's
  `<clone-file> <ip>!<port>` second field gives the address (numeric →
  ndb-static → DNS, NET-DESIGN §5).
- `NotFound` on an empty/unresolvable reply; `InvalidArgument` on a malformed
  one.

**Footgun (load-bearing):** `/net/cs` rejects a `0` service (it falls through to
an ndb lookup of `"0"` and misses), so `resolve` must be called with a **valid
non-zero port** even when the caller only wants the IP. `nslookup` and `ping`
pass `80` (a valid numeric service; the port is immaterial — nslookup prints
only the address, ICMP is portless).

## `nslookup HOST`

Resolves `HOST` and prints `Name:` + `Address:`. v1.0 reports the single A-record
the resolver returns (cs/dns is first-match); MX/AAAA/PTR are a v1.x `dig`
refinement.

```
$ nslookup localhost
Name:    localhost
Address: 127.0.0.1
```

## `ping [-c COUNT] HOST`

Sends ICMP echo requests over `/net/icmp` (a `net::IcmpSocket`: clone →
`connect <bare-ip>` [ICMP is portless] → `data` write [netd wraps an EchoRequest
at the socket's bound Echo ident] → `data` read [the matching EchoReply]). Each
request waits up to **one second** for the reply (poll the QTPOLL `ready`
sibling, then `recv` only when POLLIN fired — a never-answering host times out,
it does not hang). RTT is the LS-K monotonic clock; `-c` sets the count
(default 4).

```
$ ping -c 1 127.0.0.1
PING 127.0.0.1 (127.0.0.1): 56 data bytes
56 bytes from 127.0.0.1: icmp_seq=0 time=0.898 ms
--- 127.0.0.1 ping statistics ---
1 packets transmitted, 1 received, 0% packet loss
```

A 127.x target auto-answers in-guest (netd's resident loopback stack, net-8a) —
deterministic. An external ping is host-dependent under slirp (slirp may answer
an echo internally or proxy it to a host ping socket the host may forbid) —
best-effort, never gated.

**Seam #256:** `recv` assumes the first readiness edge is the EchoReply. An ICMP
*error* response (Destination Unreachable) that quotes our ident also makes the
socket readable; `recv` consumes it, finds it is not an EchoReply, and waits
(interruptibly — the read is #811 death-interruptible, so Ctrl-C frees it) for a
reply that may not come. The silent-host case is bounded by the 1 s poll; only
error-but-no-echo waits. A per-`recv` deadline (composing net-8d F2) is the v1.x
fix.

## `curl [-I] [-s] [-o FILE] URL`

Parses `[scheme://]host[:port][/path]` (http default), resolves, dials a `/net`
`TcpStream` (http) or wraps it in the `tls` crate's `TlsStream` (https,
validating the server cert against the baked CA bundle + the LS-K wall clock),
issues an HTTP/1.0 GET (`Connection: close` — the peer EOF ends the body; no
chunked/keep-alive at v1.0), and writes the body to stdout.

- `-I` / `--head` — HEAD request; print the response headers.
- `-o FILE` / `--output FILE` — write the body to `FILE`.
- `-s` / `--silent` — suppress error messages.

An https fetch needs `CAP_CSPRNG_READ` (the handshake's client random + ECDHE key
share draw on the kernel CSPRNG); login confers it down the session
(`LOGIN_CAPS`), so an interactive `curl https://...` works. A plain http fetch
needs no capability.

## `wget [-q] [-O FILE] URL`

The same engine as curl, but **saves the body to a file**: by default a file
named after the URL's last path segment (`index.html` for a path-less URL), or
an explicit `-O FILE` (`-O -` writes to stdout). `-q` suppresses the progress
line.

```
$ wget http://10.0.2.2:8000/file.bin     # -> ./file.bin
$ wget -O page.html https://host/         # explicit output
```

## Transport, trust, and what is proven where

The transport itself — a `TcpStream` over `/net`, a `TlsStream` over it — is
**net-8b / net-7c-audited**; the net-utils add no kernel or netd surface. A
*live* fetch (curl/wget against a real server) is host-dependent under slirp, so
it is best-effort, never a boot gate.

The **deterministic in-guest proofs** (joey's post-net `net-util PROBE`s, each
gating the boot):

- `nslookup localhost -> 127.0.0.1` — cs resolution (slirp-independent).
- `ping 127.0.0.1 -> EchoReply` — the full ICMP echo path over the resident
  loopback (clone → connect → EchoRequest → ready-poll → EchoReply).
- `curl --selftest` / `wget --selftest` — a pure URL-parse + request-build +
  response-parse battery (+ wget's basename derivation); no network. Proves all
  the new pure logic in-guest.

## Status

- nslookup — net-util 1/4 (`6450350`).
- ping — net-util 2/4 (`e01da9f`); `net::IcmpSocket` added to `net.rs`.
- curl — net-util 3/4 (`dd0069d`); the `curl` crate (lib + bin).
- wget — net-util 4/4; the second `curl`-crate bin + the shared engine lib.

## Known caveats / seams

- **No `traceroute`.** It needs per-packet TTL control + ICMP Time-Exceeded from
  intermediate routers — slirp is a single NAT hop with no router chain, so
  there is nothing to trace at v1.0. (A real-NIC bridged setup is the v1.x path.)
- **`ping` ICMP-error recv** — seam #256 (above).
- **HTTP/1.0 only**, `Connection: close` framing (no chunked transfer-encoding,
  no keep-alive, no redirects). Sufficient for a GET against a cooperative
  server; a fuller HTTP client is a v1.x refinement.
- **Live fetch is best-effort** under slirp (needs a reachable server, and for
  https a trusted chain). The in-guest gate is the parser self-test + the
  loopback ICMP/echo proofs.
