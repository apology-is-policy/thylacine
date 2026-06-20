# NET-DESIGN.md — the Thylacine network charter (#68)

Binding scripture for the Phase-8 network arc. This document is the design
pass the HOLOTYPE RW-13 vote (ROADMAP §2.2 D1, 2026-06-11) called for: the
`/net`-via-`netd` shape was already committed; this charter fills the eleven
undesigned holes inside it (the `#68` register cluster) and binds the three
load-bearing decisions taken in the 2026-06-15 design conversation.

It is **scripture before code**: no network code is written against a hole
this charter has not closed. The Phase-8 impl sub-chunks cite this document's
section numbers the way the Loom impl cited `LOOM.md`.

---

## 1. What was already settled, and what this charter decides

### 1.1 The committed shape (do not re-litigate)

Three pieces of recent, voted scripture fixed the architecture before this
charter:

- **ROADMAP §2.2 D1** (HOLOTYPE RW-13, 2026-06-11): *"Networking is IN v1.0
  (Phase 8). The Plan 9 `/net`-via-`netd` shape stands."*
- **ARCHITECTURE.md §10.1**: *"The network is 9P too… a userspace `/net` 9P
  server (`netd`, the stratumd-as-driver precedent — a `CAP_HW_CREATE` Proc
  owning virtio-net)… network I/O rides Loom with no socket opcodes… the
  native remote-access story is `import`/`exportfs` over authenticated 9P."*
- **NOVEL.md Angle #1**: *"The network is a 9P filesystem"* — the last big
  surface to totalize.

So the network is **file/namespace-shaped** (`/net/tcp/clone` + `ctl`/`data`,
the pure Plan 9 model), served by a **userspace** stack, riding **Loom** for
async I/O, with **no socket syscalls in the kernel**. The SOTA survey confirms
this is the right fork: of the capability-microkernel field, **Genode**
(`socket_fs` — sockets as directories) and **Hurd** (`pfinet` translator on
`/servers/socket/N`) independently chose the file/namespace shape and are the
two closest precedents to a Plan 9 `/net`. Fuchsia (BSD-socket FIDL) and seL4
(link-the-library) took the other fork, which our scripture already declined.

### 1.2 The three decisions this charter binds (2026-06-15 votes)

| # | Decision | Choice | Rationale | Rejected alternative |
|---|---|---|---|---|
| **D-net-1** | netd locality | **Shared `netd` + namespace-narrowed views** | One netd Proc owns virtio-net and serves `/net`; per-Proc isolation comes from narrowing each territory's `/net` view (the Plan 9 model: no `/net` = no network, narrowed `/net` = restricted reach). One stack to harden; isolation without N×memory; matches the stratumd-as-driver precedent + the single `import`/`exportfs` story. | Per-territory stack instances (Genode/LionsOS) — maximal fault isolation but N copies of TCP state + a `nic_router`-style mux; heavier, less aligned with single-`/net`. |
| **D-net-2** | BSD-socket compat locality | **Pouch-userspace translation** | A pouch boundary-line patch maps `socket()`/`connect()`/`bind()`/`listen()`/`accept()` onto `/net/tcp/clone` file ops (Genode's `socket_fs`-in-libc precedent). Keeps the kernel socket-syscall-free (ARCH §11.5 / NOVEL #1 intact); the Plan-9-heritage answer; rides the existing audit-bearing pouch discipline. | Kernel socket-shim syscalls (Fuchsia fdio→zxio) — reintroduces the socket ABI the "network is 9P" commitment deliberately excluded; reopens a settled scripture decision. |
| **D-net-3** | v1.0 firewall scope | **Namespace-restriction only** | A Proc's firewall IS its `/net` view; zero new filter mechanism; the isolation is structural and composes with territory/capability scoping. Sufficient for the container/confinement story; the explicit L3/L4 packet filter is a clean v1.x add. | A basic stateful filter in netd at v1.0 — real per-port/per-address policy now, but a new config + observability + audit surface and more Phase-8 scope. |

### 1.3 Relationship to other scripture

- **ARCH §10.1** is the one-paragraph architectural statement; this charter is
  its expansion. Where they overlap, this charter is authoritative for the
  as-designed Phase-8 detail.
- **ROADMAP §9** carries the Phase-8 deliverable list + exit criteria; this
  charter refines them (W4-F8: the criteria gain server-side + soak).
- **LOOM.md** owns the ring transport; this charter consumes it (network I/O
  rides Loom; the listen-multishot accept loop is `loom_multishot.tla`'s
  generality realized, no new Loom core).
- **POUCH-DESIGN.md §11** owns the boundary-line discipline; the socket-compat
  patch (§7) is a new pouch surface under that discipline (P-1..P-4 hold).
- **STALK-DESIGN.md** owns path resolution; the `/net` view narrowing (§8) is
  exactly the per-territory mount/bind machinery, no new mechanism.

---

## 2. Architecture: one `netd`, many narrowed views

`netd` is a userspace Proc, spawned by joey at boot with `CAP_HW_CREATE`,
exactly as stratumd is spawned to own the disk. It:

1. **Owns the NIC.** It creates `KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` handles
   over the virtio-net device (the existing `usr/virtio-net-*` probe transport
   generalized to RX+TX). By **I-5** these handles are non-transferable, so no
   other Proc can touch the live device.
2. **Runs the stack.** It embeds the smoltcp TCP/IP stack (§14) in its own
   address space — the single shared stack instance (D-net-1).
3. **Serves `/net` as a 9P server.** It posts itself into the boot namespace's
   `/srv` and is mounted at `/net` (the joey `joey_mount_static_dev` idiom,
   the same one that mounts `/proc`, `/ctl`, `/dev`). Every read/write/walk on
   `/net/...` routes through the kernel 9P client (`dev9p`) to netd's request
   handlers — netd is *just another 9P server*, indistinguishable to a client
   from stratumd.

**Isolation = view narrowing, not stack duplication.** A Proc's only access to
the network is the `/net` subtree in *its* territory. The kernel grants or
withholds that subtree with the existing mount/bind machinery:

- **No `/net` in the namespace → no network at all.** Not a capability bit, not
  a filter rule — the resource is simply unnamed, so it cannot be opened. This
  is the Plan 9 §4.4 property and it falls out of **I-1** (territory isolation)
  + **I-28** (path-resolution containment) with *zero new mechanism*.
- **A narrowed `/net` → a restricted network.** A confined Proc can be given a
  territory whose `/net` contains only `/net/tcp` (no UDP), or only a bind of a
  single pre-opened connection directory, or a `/net` imported over 9P from a
  filtering gateway Proc. The set of things it can dial is exactly the set of
  `clone` files it can walk to (§8).

**The remote story.** `import host /net` 9P-mounts another machine's `/net`
into the local namespace; thereafter opening `/net/tcp/clone` locally opens a
connection *originating on `host`*. Because `/net` is files, the standard 9P
import IS remote network access — governed by the same handle/rights/corvus-auth
path as importing any other tree (the Plan 9 `cpu(1)` model; ARCH §10.1).

---

## 3. The `/net` schema and the netd fid state machine (closes W4-F7)

### 3.1 The directory tree

```
/net/
    cs                      # connection server — name→address (§5)
    dns                     # DNS resolver       (§5)
    ndb                     # network database (read view; §5)
    ipifc/                  # IP interface config (§6)
        0/ctl  0/status  0/local  ...
        clone               # mint a new logical interface
    tcp/                    # TCP protocol directory
        clone               # the connection factory
        stats               # protocol-wide counters (§11)
        0/  1/  2/ ... N/    # one directory per live connection
    udp/                    # UDP — same clone+N/ shape
        clone  stats  0/ ...
    icmp/                   # for ping (§16)
        clone  0/ ...
```

Adding a protocol adds a directory with the same `clone` + numbered-connection
shape; it adds no API. (IL — the Plan 9 9P-carrier datagram protocol — is **not**
ported: Thylacine carries 9P over Loom/SrvConn/Unix-socket transports already,
so IL has no role. Recorded as a deliberate non-port.)

### 3.2 The connection directory `/net/tcp/N/`

Each live connection is a directory of **ASCII** files (ASCII deliberately —
byte-order-free and remotely accessible, the §1.1 import property):

| File | Mode | Semantics |
|---|---|---|
| `ctl` | rw | Read once → the connection number `N` (ASCII). Write command strings to drive the state machine (§3.3). |
| `data` | rw | The byte stream. Reading/writing `data` *is* recv/send. Opening `data` after a `connect` write is what establishes the connection. |
| `listen` | r | Server accept. `open()` **blocks** until an inbound call arrives, then returns a fd onto the **`ctl` of a fresh connection directory** for that call. |
| `local` | r | Local endpoint, ASCII `ip!port`. |
| `remote` | r | Remote endpoint, ASCII `ip!port`. |
| `status` | r | Human-readable TCP state (`Established`, `Listen`, window) — `cat`-able introspection (§11). |
| `err` | r | The out-of-band reason a read/write failed or the stream hit EOF. |

### 3.3 The `ctl` verb vocabulary (TCP)

The Plan 9 `ip(3)` set, parsed by netd's `ctl`-write handler:

- `connect ip!port[!r] [local]` — active open; `!r` requests a restricted
  (<1024) local port; trailing `local` pins the local address.
- `announce ip!port` (or `*!port`, `*!service`) — bind a local port to listen.
- `bind ip!port` — set only the local address (BSD `bind()` separability, §7).
- `hangup` — active close.
- `keepalive [ms]`, `ttl n`, `tos n` — per-connection options.

### 3.4 The netd-internal fid state machine

This is the part W4-F7 named as undesigned, and the part that is **distinct
from smoltcp** (smoltcp is the wire/protocol state machine; this is the 9P-fid
lifecycle netd maintains *on top of* a smoltcp socket). Each `clone`-minted
connection fid moves through:

```
            open(clone)
   ─────────────────────────►  ALLOCATED   (smoltcp socket reserved; N assigned)
   write ctl "connect a!p"  ►  CONNECTING   (smoltcp active-open issued)
   smoltcp established       ►  ESTABLISHED  (data I/O live)
   write ctl "announce ..."  ►  ANNOUNCED    (smoltcp listen socket bound)
   open(listen) + inbound    ►  (mints a NEW fid in ESTABLISHED; the listen
                                  fid stays ANNOUNCED for the next call)
   hangup / peer FIN / err   ►  CLOSING ──► CLOSED  (fid clunk frees N + the
                                                     smoltcp socket)
```

Invariants the impl must uphold (the audit prosecutes these):
- **One fid ↔ one smoltcp socket** for its open lifetime (I-11 fid identity,
  generalized to the netd session). A `clunk` is the *only* path that frees the
  smoltcp socket; a half-open `data` read after `CLOSED` returns the `err`
  reason, never a stale or reused socket's bytes.
- **`N` reuse is gated on clunk** (I-10-style: the connection number is not
  reusable until its directory is fully torn down), so a late reply on an old
  `N` is never mis-attributed.
- **A connection fid is per-session state**; multi-thread-Proc sharing of a
  `/net` fd is governed by the existing handle-table lock (#844) — netd sees
  one 9P session per attach and serializes its own connection table.

---

## 4. The connection lifecycle as 9P operations

**Client** (the four Plan 9 steps, realized over `dev9p`):

```
fd  = open("/net/tcp/clone")        # → ALLOCATED, fd onto ctl
N   = read(fd)                      # ASCII connection number
write(fd, "connect 1.1.1.1!443")    # → CONNECTING
d   = open("/net/tcp/N/data")       # blocks to ESTABLISHED; d is the stream
read(d)/write(d)                    # recv/send
```

**Server**:

```
a   = open("/net/tcp/clone"); N = read(a)
write(a, "announce *!80")           # → ANNOUNCED
l   = open("/net/tcp/N/listen")     # blocks for a call → fd onto a NEW conn's ctl
M   = read(l)
d   = open("/net/tcp/M/data")       # the accepted stream
```

**Native libthyla-rs** wraps this as a thin `net::TcpStream` / `net::TcpListener`
(string-format + file-op, no privileged syscall underneath — the Plan 9 `dial(2)`
library is thin by construction). **Pouch/Linux** binaries reach the same files
through the socket-compat boundary-line (§7). **Async** native clients drive the
lifecycle through Loom (§12) instead of blocking opens.

---

## 5. Naming: `cs`, `dns`, `ndb` (closes W4-F3)

- **`/net/cs`** — the connection server. Write a dial string
  (`tcp!host!port`, `net!host!service`), read back one line per reachable path
  in preference order, each `clonefile message` (the path of the `clone` to
  open and the string to write to its `ctl`). `cs` collapses host-name
  resolution, service-name resolution, and network/route selection into one
  file read. The native `dial()` library calls `cs` then performs §4.
- **`/net/dns`** — the recursive resolver as a file. Write `name type`
  (`example.com a`), read back one line per resource record. `cs` delegates
  Internet domain names to `dns`; `dns` performs the recursive query and caches.
- **`ndb`** — the network database (static config): host/network/service
  definitions, indented `attr=value` entries (the Plan 9 `ndb(6)` format).
  Location: **`/lib/ndb/local`** + `/lib/ndb/global`, read live (no caching
  daemon — single source of truth, the Plan 9 consistency property).

**Locality decision (v1.0):** `cs` and `dns` are served **by netd itself**,
under `/net/cs` and `/net/dns`, rather than as separate daemons. Rationale: one
Proc, no extra IPC, and netd already holds the smoltcp UDP socket `dns` needs.
**v1.x seam:** splitting `cs`/`dns` into independent daemons (the Plan 9 purity
— so a confined Proc can be given a `dns`-less `/net`, or a custom resolver
bound over `/net/dns`) is a clean later refactor; the file interface is
identical whether netd or a separate daemon backs it, so nothing downstream
changes. Recorded in §18.

**ndb source at v1.0 — the netd-confinement refinement (user-voted 2026-06-18).**
netd is a Menagerie warden-bound *leaf driver* (I-34), spawned pre-pivot, so it
inherits the warden's devramfs Territory and **cannot reach `/lib/ndb`** (which
lives in the post-pivot Stratum FS) — and widening netd's namespace to read
`/lib` would fight its I-28/I-34 confinement (the deliberate "drivers are
leaves" posture, MENAGERIE §13.2). The two §5 statements above ("cs/dns served
by netd" + "ndb read live from /lib/ndb") are therefore jointly unsatisfiable
*as written* at v1.0. Resolution: at v1.0 netd serves `cs`/`dns` from a
**compiled-in ndb** — the static `localhost` host + a builtin services table —
**plus the DHCP-learned entries read live** from the lease (the resolver, the
default router, the interface address: never stale). This is the
capability-microkernel idiom (config-at-construction; Fuchsia/Genode), which
*preserves* netd's confinement, rather than the Plan 9 full-namespace-service
idiom (a `cs`/`dns` process with `/lib` mounted) that a confined leaf driver
cannot adopt. The canonical **`/lib/ndb/local` file is baked into the post-pivot
FS** (user-readable, real ndb(6) format) — it is the source the v1.x cs/dns
daemon split reads live; only the live-from-`/lib` read is deferred, the file
and its format are real at v1.0. The dynamic half (resolver/router/my-address)
is genuinely live; only the static host/service half is compiled-in.

---

## 6. IP configuration (closes W4-F10)

Interface configuration is `ctl` writes on `/net/ipifc/N/`, the Plan 9 idiom:

- `add ip mask [gw]` — assign a static address.
- `bind ether <devpath>` — bind the interface to the NIC.
- `unbind`, `remove ip` — teardown.

Two config sources, both native libthyla-rs tools:

- **`ipconfig`** — applies static config (from a config file or argv) by
  writing `/net/ipifc/N/ctl`. The boot path runs it for any static address.
- **DHCP client** — smoltcp ships a DHCPv4 client; netd drives it on an
  interface marked dynamic, writing the leased address into `ipifc` and the
  resolver into `ndb`/`dns`. A `dhcpctl`-style native tool starts/stops it.

QEMU user-mode networking (`slirp`, the default dev loop) hands out a lease via
DHCP, so the DHCP path is the primary dev-loop config; static is the
bridged/production path. (W4-F10's "v1.1 board can't hardcode slirp" is closed:
config is data-driven, not slirp-assumed.)

---

## 7. BSD-socket compatibility (binds D-net-2; closes W4-F2)

The kernel has **no** socket syscalls, by commitment (ARCH §11.5 lists zero;
NOVEL #1; P-1 "no foreign syscall number ever enters the kernel"). Linux/pouch
binaries that call `socket()`/`connect()`/`bind()`/`listen()`/`accept()`/
`send()`/`recv()`/`getsockname()`/`setsockopt()` are served by a **pouch
boundary-line patch** that translates each to `/net` file operations — the
Genode `socket_fs`-in-libc model:

| BSD call | `/net` realization |
|---|---|
| `socket(AF_INET, SOCK_STREAM)` | `open("/net/tcp/clone")`; remember the fd + `N`. |
| `connect(fd, a)` | write `connect a!p` to `ctl`; `open` `data`; dup `data` onto the socket fd. |
| `bind(fd, a)` | write `bind a!p` to `ctl`. |
| `listen(fd, backlog)` | write `announce` to `ctl`; the backlog is netd's accept queue depth. |
| `accept(fd)` | `open` `listen` (blocking) or Loom-multishot (non-blocking); return the new conn's `data` fd. |
| `send`/`recv` | `write`/`read` on `data`. |
| `getsockname`/`getpeername` | read `local`/`remote`. |
| `setsockopt(SO_*, TCP_*)` | write the matching `ctl` verb (`keepalive`, `ttl`, …); unsupported options return a documented errno (P-3 "no silently-wrong surface"). |

**This reconciles W4-F2** (ARCH §11.5/§16 vs ROADMAP §9.1): the socket calls
are **libc-level translations, not kernel syscalls**. ARCH §11.5 is correct to
list zero socket syscalls; the "socket shim" is the pouch boundary-line above,
not a kernel surface. The patch is audit-bearing under the existing pouch
discipline (a new `usr/lib/pouch/patches/00NN-pouch-net-sockets.patch`).

**Native libthyla-rs** programs do **not** go through this — they use `/net`
directly via `net::TcpStream`/`net::TcpListener` (§4). The split is the
standard native-vs-ported boundary (CLAUDE.md "Native vs ported userspace").

**`AF_UNIX` is unaffected**: the existing `/srv` SrvConn path (the
`0006-pouch-sockets.patch`) already serves Unix-domain sockets and is a
*different* surface from this network-socket patch — both can coexist (a binary
that opens an `AF_UNIX` socket and an `AF_INET` socket hits the two patches
independently).

---

## 8. Firewall: namespace restriction is the network sandbox (binds D-net-3; closes W4-F1)

There is **no explicit packet filter at v1.0**. A Proc's firewall IS its `/net`
view, enforced by the existing path-resolution machinery (no new code):

- **Deny-all**: a territory with no `/net` mount. The Proc cannot name any
  network resource. This is the container/confinement default.
- **Allow-scoped**: a territory whose `/net` is narrowed — only `/net/tcp`, or
  only a bind of specific pre-dialed connection directories, or a `/net`
  imported from a gateway Proc that itself filters. The reachable dial set =
  the walkable `clone` set.
- **Per-component X-search** (I-28) governs every `/net` walk: a Proc that
  lacks search permission on a `/net` subtree cannot traverse into it.

This is sufficient for the v1.0 container story (a container is a territory;
its network reach is the `/net` its territory grants — exactly the "containers
are territories" model, ROADMAP §9.1). It composes with capability scoping with
zero net-specific authority surface, so it adds **no new §28 invariant** (it is
I-1 + I-28 + I-23 specialized to `/net`).

**v1.x seam (§18):** an explicit stateful packet filter — per-interface /
per-connection allow/deny rules served as a `/net/filter` ctl surface (the
Genode `nic_router` precedent) — layers *on top of* namespace restriction
without changing it. Recorded as deferred; the namespace firewall is the
primary mechanism, the packet filter is the fine-grained add.

**Realized incrementally by the mandate arc (`docs/MANDATE-DESIGN.md` §5):** the
*per-principal* form of this seam -- netd enforcing a connecting principal's
allow-list (`connect tcp *!80` etc.) in the connect/announce verb handler, driven
by that user's mandate -- is the network granularity the post-net Imperium/
Authority arc delivers (the user's "grant a citizen standing TCP:80"). Keep this
seam reserved + netd `srv_peer`-principal-aware through net-3..net-8 so it slots
in without a netd rewrite; the full *stateful* packet filter remains the v1.x add.

---

## 9. TLS and the root-cert bundle (closes W4-F4)

- **Native clients** use **rustls** (pure-Rust TLS, `alloc`-based — libthyla-rs
  provides `alloc`). It is the native `https`/`wget`-as-fetcher path.
- **Pouch/Linux clients** (curl, wget, openssl) use their bundled TLS (OpenSSL/
  mbedTLS as ported), reading the **system root-cert bundle** at the canonical
  path **`/etc/ssl/certs/ca-certificates.crt`** (+ the hashed `/etc/ssl/certs/`
  dir for OpenSSL). The bundle is baked into the image at build time (the
  host-bake idiom, like the pool corpus) and is a normal Stratum-FS file.
- TLS is a **userspace library concern**, not a kernel or netd surface — netd
  serves raw TCP `data`; the TLS handshake runs in the client over that stream.
  This keeps the cert-trust policy in userspace where it belongs.

W4-F4 is closed: the root bundle has a designed home (a baked file at a canonical
path), not just an exit-criterion mention.

### 9.1 As-built: the native rustls feasibility (net-7c-1)

The feasibility spike proved native rustls TLS runs on Thylacine. The as-built
recipe (`usr/lib/tls`):

- **rustls 0.23** `default-features = false`, feature `custom-provider` (no
  built-in aws-lc-rs/ring -- those compile C/asm and do not build bare-metal).
- **`rustls-rustcrypto`** as the pure-Rust `CryptoProvider` (aes-gcm /
  chacha20poly1305 / p256+p384 ecdsa / rsa / x25519 / sha2), `default-features =
  false` (its default pulls `std`, which cascades `getrandom/std`).
- **`getrandom` 0.2 + `custom`** with `register_custom_getrandom!` routed to the
  kernel CSPRNG (`SYS_GETRANDOM`). The provider draws the client random + the
  ECDHE ephemeral key share through getrandom, which has no bare-metal backend.
- A **`TimeProvider`** reading LS-K `CLOCK_REALTIME` -- no_std rustls has no
  built-in clock, and webpki needs the wall time to bound a server cert's
  validity. This is why TLS follows net-7a (the clock work): a native TLS client
  cannot validate a certificate without a real wall clock.
- The **unbuffered connection API** (`UnbufferedClientConnection`) -- the no_std
  rustls connection type, driven over a `/net` `TcpStream` (the transport glue
  lands with its loopback E2E in net-7c-2).

Two as-built constraints the spike surfaced:

- **`CAP_CSPRNG_READ` is required.** A native TLS tool must be spawned WITH
  `CAP_CSPRNG_READ` (the spawning authority -- the warden / login / joey --
  confers it, exactly as joey does for corvus). Without it the handshake cannot
  generate its key share. This is a real conferral requirement, not an option.
- **Binary size sits at the `SYS_SPAWN_BLOB_MAX` (1 MiB) ceiling.** The TLS-1.3
  client smoke is ~1.0 MiB (TLS 1.2 + RSA-kx pushes it ~60 KiB OVER, so v1.0 is
  **TLS 1.3-only** for the native client -- a sound modern posture; the
  pouch/curl path still does 1.2 via its own TLS). The full https tool
  (net-7c-2: + the transport + the PEM root loader + the GET) will exceed 1 MiB.
  The recorded size strategy is to **strip the TLS binaries** (no userspace tool
  consumes their symbols -- only the kernel's own addr2line uses the kernel
  symtab; a build-time strip keeps an unstripped copy under `build/` for manual
  fault-PC resolution). This is pure build config -- no kernel change. A
  `SYS_SPAWN_BLOB_MAX` bump is the fallback if stripping proves insufficient.

The roots-source decision: the native client reads the baked
`/etc/ssl/certs/ca-certificates.crt` at runtime (one source of truth with the
pouch/curl path), NOT a compiled-in `webpki-roots` -- which would be a second
copy of ~150 certs and add hundreds of KiB toward the cap. net-7c-2 lands the
PEM loader + the baked bundle.

### 9.2 As-built: the full client + the bundle (net-7c-2)

net-7c-2 completes the native TLS path. The detail lives in
`docs/reference/123-tls.md`; the design-relevant facts:

- **The transport.** `tls::TlsStream<S>` drives the unbuffered rustls state
  machine over any blocking `Read + Write` stream (a `/net` `TcpStream`),
  validating the server cert against the config's trust anchors + the LS-K wall
  clock at `connect`, then carrying plaintext through the record layer. A shared
  `drive_state<D>` helper handles every connection state (the per-role
  `process_tls_records` is the only role-specific call).
- **The bundle.** `load_roots_pem` parses the baked
  `/etc/ssl/certs/ca-certificates.crt` into a `RootCertStore`. The bundle is a
  real Mozilla CA root set (193 roots), baked into the pool at build time (the
  host-bake idiom, like `/lib/ndb/local`); the committed source is
  `usr/https/ca-certificates.crt`. A refresh/update mechanism is a v1.x seam.
- **The client tool.** `/bin/https <host> [path]` is the native curl-over-pouch
  analog: resolve via `/net/cs`, handshake, GET, print the status line.
- **The deterministic in-guest proofs** (the net-3d loopback pattern, since the
  blocking `TlsStream` cannot be tested single-threaded against an in-memory
  peer): `tls::loopback_roundtrip` runs a full client<->server handshake + an
  app-data round-trip in memory using a baked self-signed test cert -- exercising
  the **real WebPki certificate verification** (chain + validity window vs the
  wall clock + the SAN). A **negative** test (a client trusting no roots must
  REJECT the handshake) proves the verifier is not a permissive accept-all -- the
  cert-validation regression a real TLS client must never have. Both gate the
  boot via `tls-smoke`.
- **The size strategy (resolved: strip).** The TLS binaries exceed
  `SYS_SPAWN_BLOB_MAX` (1 MiB) unstripped (~1.1 MiB), so the build strips them
  (`llvm-strip`, ~520 KiB each) into the cpio root; the unstripped artifacts stay
  under `build/` for fault-PC resolution. Pure build config, no kernel change.
  This is the v1.0 expedient: REVENANT's file-backed demand-paged exec (task
  #231, post-net) retires the slurp cap entirely, after which the strip becomes
  optional.

The live `https <host>` socket transport is shell-runnable + best-effort (it
needs a reachable server + a trusted chain); the deterministic in-guest E2E of
that path (a netd loopback interface) is owed to net-8. The formal focused audit
of the cert-validation surface is net-7d.

---

## 10. NTP / clock synchronization (closes W4-F11)

- An **SNTP client** (native libthyla-rs tool, `ntpdate`-shaped) queries an NTP
  server over UDP (`/net/udp`) and computes the offset against the LS-K
  monotonic clock.
- **Stepping the wall clock** requires a runtime-settable `CLOCK_REALTIME`.
  LS-K shipped `timer_set_wallclock_anchor()` as **write-once-before-`smp_init`**
  and recorded settability as a v1.x seam. The charter's resolution: **pull a
  minimal `SYS_CLOCK_SETTIME` forward into the net arc** (the depth-first
  dependency default, CLAUDE.md "pull dependencies forward") — a
  `CAP_HOSTOWNER`-gated syscall that re-anchors `epoch_anchor_ns` (re-anchor
  only; MONOTONIC is untouched). This is a small, bounded ABI addition.
  - **ABI-confirm at impl:** a new syscall is an ABI surface; the impl sub-chunk
    surfaces `SYS_CLOCK_SETTIME` for confirmation before landing (the standing
    "syscall interface changes" escalation). If the user prefers, the fallback
    is an SNTP client that *reports* offset only (display/`date`-check), with
    stepping deferred to v1.x — but the recommended path is the small settable
    clock, so NTP actually synchronizes.
  - **CONFIRMED (net-7a, 2026-06-19 vote):** the recommended path — the
    `SYS_CLOCK_SETTIME` syscall (`= 79`), `CAP_HOSTOWNER`-gated, re-anchoring the
    single wall-clock offset (`g_wallclock_offset_ns`) at full-nanosecond
    granularity via one atomic `u64` store; `CLOCK_MONOTONIC` untouched. The
    report-only fallback was declined. Landed net-7a-1 (the ABI surface +
    bindings + joey's elevated round-trip proof).
  - **net-7a-2 LANDED — the native SNTP client** (`usr/sntp`, RFC 4330): a
    `libthyla_rs::net::UdpSocket` (new — the connected-UDP analog of
    `TcpStream`, over `/net/udp`) carries an NTPv4 request; the reply's Transmit
    timestamp converts 1900→Unix (`− 2_208_988_800`); the offset is computed
    against the LS-K MONOTONIC clock (for the round-trip) and applied via
    `time::set_realtime`. `sntp` (no args) is the deterministic boot gate (the
    build/parse/validate + offset battery + the **EACCES clock-step gate proof**
    — spawned unelevated, `set_realtime` must be denied); `sntp <a.b.c.d>` is the
    best-effort live query (bounded-poll via the `ready` QTPOLL sibling, net-6b).
    No new kernel surface (open/read/write/close over `/net` + the landed
    `SYS_CLOCK_SETTIME` + `SYS_POLL`); composes I-1/I-5/I-23/I-28, no new
    invariant. The in-guest live round-trip over a loopback UDP responder is
    owed to net-8. See `docs/reference/122-net.md`.

---

## 11. Observability (closes W4-F5)

Network introspection is **9P-native and mostly free** (the Plan 9 property —
`netstat` is a walk of `/net`):

- **Per-connection** `/net/tcp/N/status` (+ `local`/`remote`) — the live TCP
  state, `cat`-able. This exists by §3.2 at no extra cost.
- **Per-protocol** `/net/tcp/stats`, `/net/udp/stats`, `/net/icmp/stats` —
  counters (`active`/`opened`).
- **Per-interface** `/net/ipifc/N/status` — address, link state, packet/byte
  counters.
- **A `/net/summary` rollup** — a read-only aggregate (interfaces + per-protocol
  stats + the live connection table) for one-shot `cat /net/summary`. It is
  **visibility, not authority** (read-only, world-readable, mints nothing) and
  reports only what the reader's `/net` view already exposes — it is *in* `/net`,
  so per-territory by construction. **netd serves it** (the data — the connection
  table — is netd-held; the kernel holds no network state), modelled on the `/ctl`
  introspection precedent (#57a) but located where its data lives rather than
  under the kernel's devctl mount. (Reconciled from the charter's original
  `/ctl/net` path, user-voted 2026-06-19: a kernel `/ctl/net` would have to reach
  up into a userspace service from a devctl read — a layering inversion — for
  state the kernel does not own; `/net/summary` is the Plan-9-clean home and needs
  no kernel change.)
- A native **`netstat`** tool is a thin walk of `/net` (it reads the per-protocol
  `stats`, enumerates the live `/net/<proto>/N` connection dirs, and reads each
  one's `status`/`local`/`remote`) — the one-shot aggregate as a client-side walk,
  independent of `/net/summary`.

W4-F5 is closed: observability has a designed surface (per-connection status +
protocol/interface stats + the `/net/summary` rollup + the native `netstat`),
not just a charter mention.

---

## 12. Poll-readiness over 9P, and the Loom path (closes W4-F6)

This is the subtlest hole. `dev9p` has **no `.poll` slot** today, and a NULL
poll slot degenerates to "always ready" — wrong for a network fd. Two readiness
paths, by client kind:

### 12.1 Async native — Loom multishot (the committed path)

Network I/O **rides Loom with no socket opcodes** (ARCH §10.1):
- A `LOOM_OP_READ` on `/net/tcp/N/data` *is* a recv; its completion CQE carries
  the bytes. A **multishot** read streams recv CQEs as data arrives.
- A **multishot read on `/net/tcp/N/listen`** *is* an async accept loop — each
  inbound call posts a CQE naming the new connection's `N`. This is exactly
  `loom_multishot.tla`'s generality (one SQE → many CQEs, exactly-one-terminal,
  the I-30 pin held across shots) realized for networking, with **zero new Loom
  core** and no `LOOM_OP_ACCEPT`/`SEND`.

This is the primary native readiness mechanism and needs no new kernel surface.

**Throughput + the Weft dataplane (`docs/NET-THROUGHPUT.md`, I-37).** The committed
"net rides Loom" path above is the *latency*/readiness mechanism. The *throughput* arc
extends it: a per-flow capability-scoped zero-copy shared-page dataplane — the bytes of a
flow travel through a Burrow shared guest↔netd, set up once at grant, netd out of the
per-packet loop — the Snap/Arrakis-grounded NOVEL that pushes throughput from 16 MiB/s
toward hundreds of MiB/s and closes the RX-wake floor on the *same* readiness ring.
Committed 2026-06-20 as the post-net **Weft** arc; design in `docs/NET-THROUGHPUT.md`.

### 12.2 Synchronous `poll()`/`select()` — the `dev9p.poll` bridge

Pouch/Linux binaries call `poll()`/`select()`/`pselect6` over `/net` fds. The
mechanism is **BOUND** (user-voted 2026-06-18, the kernel-bridge — below — over a
pouch-thread-per-fd shim and over deferring `poll()` to v1.x; the system-best
real readiness primitive). The design:

- **`dev9p` gains a real `.poll` slot.** `dev9p` has none today (a NULL slot
  degenerates to "always ready" — wrong for a network fd); this is the one new
  kernel surface in the net arc.
- **Readiness rides the existing 9P read completion — no new 9P wire message, no
  new userspace→kernel signal syscall, no new authority surface.** netd serves a
  **non-consuming per-connection readiness file** (a `ready` file; the accept
  case reuses the already-deferred `listen`); a `read` on it **blocks** (the
  net-3a/4b deferred-reply machinery) until smoltcp reports the socket
  readable/writable per the requested events, then returns the readiness bitmap
  **without consuming data**. `dev9p.poll` registers a kernel poll-hook (the
  `poll_waiter_list` machinery) and ensures an outstanding readiness read is in
  flight; the kernel's #841 **elected reader**, on that readiness `Rread`, marks
  the hook ready and walks the `poll_waiter_list` **in process context** (never
  from an interrupt — the LS-8a `console_mgr` deferred-wake discipline). The
  whole path rides the existing 9P read authority (the client already holds the
  connection's fids).
- The pouch `poll()`/`select()` translation drives this `dev9p.poll` per fd.

**The I-9 hazard, and the spec (spec-first RE-ENABLED for this surface).** The
readiness wake is a register-then-observe surface: a `poll()` caller must not
miss a readiness edge that lands between its readiness-check and its sleep — the
outstanding readiness read must be in flight *before* the caller observes
not-ready, else an edge between is lost. This is **I-9 generalized** (exactly the
`cons_poll.tla` / `poll.tla` shape). The vote confirms the wake crosses the
userspace→kernel boundary, so per the per-surface spec-first re-enablement
precedent (SMP/ASID/death-wake/Loom/cons-poll), **`specs/net_poll.tla`** (clean +
a `BUGGY_LOST_READY` counterexample = the edge dropped when the probe is not
outstanding before the observe) is written + TLC-green **before** the
`dev9p.poll` impl. `dev9p.poll` (the new vtable slot) **and** the 9P-client
reply-dispatch change (waking the hook on the readiness `Rread`) join the ARCH
§25.4 + CLAUDE.md audit-trigger table at the net-6b impl. The rest of the net arc
stays prose-validated per the 2026-05-23 broadening.

**As-built refinement (net-6b-2b): the readiness-file marker `QTPOLL` (user-voted
2026-06-18).** `dev9p` is generic — it backs *every* 9P mount (Stratum, corvus,
netd), not just `/net` — and a native `SYS_POLL` on a regular dev9p file is
reachable (`libthyla_rs::poll::Poller` exists). A `poll()` on a regular file is
POSIX **always-ready** (it never reads the file); only a netd readiness file
should issue the blocking readiness `Tread` *probe*. So `dev9p.poll` must
distinguish the two, and dev9p has no a-priori knowledge of which 9P files speak
the readiness protocol (Plan 9 has no `poll()` over 9P; Linux v9fs treats 9P
files as always-ready — there is no prior art to inherit; the distinguisher is
Thylacine-novel). The signal is carried by a **reserved 9P `qid.type` bit
`QTPOLL` (`0x01`, unused in 9P2000.L)**: netd sets it on the `ready` file's qid;
`dev9p.poll` probes **only** a Spoor whose cached `qid.type` has `QTPOLL`, and
returns POSIX always-ready (`events & POLL_REQUESTABLE` — the exact prior
NULL-slot behavior) otherwise. The design **fails safe**: an unmarked file (any
non-netd server, or a netd→dev9p plumbing slip) degenerates to always-ready —
never to an unsound probe of a regular file (which would misread the file's bytes
as a poll bitmap). The file *declares its nature*, exactly as `qid.type` already
signals `QTDIR`/`QTSYMLINK`; this is an **additive** 9P semantic (one unused bit,
no new message, no version bump), squarely within the bound "readiness rides the
existing 9P read completion" mechanism. Chosen over a kernel poll-events bit
(which fails *unsafe* — a caller that omits it busy-loops on a never-ready
socket) and over always-probe (unsound for the reachable regular-file-poll path).

**As-built realization (net-6b-2b): the global poll-pump kthread.** A `poll()`
caller *parks* (it does not block-read), so no synchronous reader drives the 9P
elected reader for the outstanding readiness `Tread`. A boot-spawned **global
poll-pump kthread** (the `cons_poll` `console_mgr` + Loom-4 SQPOLL analog) drives
it: the readiness op is an async `p9_client_submit_async` (a `Tread` whose offset
carries the event mask); the kthread *borrows* the netd client(s) from the live
ops via `spoor_ref` (the Loom `loom_first_inflight_client` borrow-guard — it never
owns the client lifetime) and pumps the elected reader of **every distinct client**
with a non-terminal op each cycle (`p9_client_reader_pump_once_deadline`; the
net-6b-4 multi-client fairness fix — pumping only the head client would starve a
second QTPOLL client's pending reply, a hang latent under the v1.x per-user-netd
config, v1.0-safe with the single netd → single `dev9p` client);
the op's `on_complete` (firing under `c->lock` in `demux_frame_locked` — the
seam contract: no sleep, no re-enter, atomics only) records the bitmap into the
Spoor's per-poll-state `cached_revents` + flags a deferred poll-wake + wakes the
kthread, which walks that Spoor's `poll_waiter_list` **in process context**
(`KthreadWalk`) and reaps the terminal op (releasing the `spoor_ref`). The per-op
mask = the polling caller's `events`; a broader concurrent poller widens it by
abandoning + resubmitting the union (`p9_client_abandon_async`, #845 Tflush); a
completion that satisfies one poller's events but not another's is a benign
spurious wake the second poller re-arms on its next `poll()` (no busy loop — the
non-satisfying poller's next call re-defers). Lock order: `dev9p_poll`
poll-state-lock → (submit) `c->lock`; `on_complete` is `c->lock` → atomics-only
(no poll-state-lock → no cycle); the kthread takes `c->lock` (pump) and the
poll-state-lock (walk/reap) **separately**, never nested. The spec
action↔impl map (`PollerRegister` / `NetdReplyDemux` / `KthreadWalk`) is in
`specs/SPEC-TO-CODE.md`.

**As-built (net-6b-3): the pouch `poll()` translation.** `0018-pouch-net-poll.patch`
makes pouch/Linux `poll()`/`select()`/`pselect`/`ppoll` work over an `AF_INET`
`/net` socket. The hazard: a tagged socket fd's `kernel_fd` is the
`/net/<proto>/N/data` fd — a *regular* dev9p file, which `dev9p.poll` treats as
always-ready, so polling it reports a socket ready unconditionally. The fix is a
new slot helper `pouch_sock_poll_fd` that resolves the **poll target**: a
`FAM_INET` socket polls its `/net/<proto>/N/ready` sibling (the `QTPOLL`-marked
readiness file), opened lazily on the first `poll()` and held for the socket's
lifetime; a `FAM_UNIX` socket keeps polling its `/srv`-stream `kernel_fd`
(unchanged); the events bits pass straight through (the kernel threads them as the
readiness `Tread` offset). **No new kernel surface** — only a `SYS_open` of the
readiness file. Proven by `/pouch-hello-net`: a connected UDP socket reads
`POLLOUT`-ready but `poll(POLLIN)` times out (the load-bearing assertion — `poll()`
*waited* on the `ready` file, not the always-ready data fd). Reference:
`docs/reference/78-pouch.md` (the AF_INET backend).

**v1.0 seam — listener-poll (#220).** netd's `check_ready` reports `can_recv()`,
false for a TCP listener, so `poll(listener, POLLIN)` does not wake on a pending
accept — a `select()`-based server multiplexing accept + connections would block.
The blocking accept via `open(listen)` (§12.1's net-3a path) works, and no v1.0
in-VM consumer polls a listener. Candidate fix: `check_ready` reports
`accept_ready` for an `ANNOUNCED` TCP slot, proven via a netd loopback E2E; weighed
at the net-6b-4 audit.

---

## 13. Driver model: the userspace virtio-net NIC (closes W4-F15)

netd owns the NIC via the userspace-driver model already proven for virtio-blk
and exercised by the `usr/virtio-net-*` probes:

- **Transport**: the virtio-net device's RX + TX virtqueues. Since the pci
  sub-arc (`docs/VIRTIO-PCI-DESIGN.md`) net is a virtio-**PCI** device — its
  per-function, page-aligned BAR is claimed + mapped via `KObj_PCI`
  (`SYS_PCI_MAP_BAR`), with `KObj_IRQ` (the swizzled INTx INTID) for RX/TX
  completion and `KObj_DMA` for the frame buffers — all held by netd,
  non-transferable (I-5). The existing probes proved TX + the IRQ-completion
  handshake (over the mmio transport); net-1 added the RX path + the reusable
  driver; the pci sub-arc re-homes the transport on a per-device BAR (the
  field-wide convergence is a shared-memory ring + a signal — the virtio
  virtqueue is exactly that).
- **The probes generalize, the scripted device retires.** W4-F15 noted the
  `usr/virtio-net-*` probes built but the scripted `/dev/ether0` never landed,
  its blocker (no FS namespace) dissolved at stalk. Resolution: **netd owns the
  NIC and serves `/net`**; there is no separate `/dev/ether0` device — the raw
  frame interface, if ever exposed, is a `/net/ether0` file under netd (the
  Plan 9 `#l` shape), not a kernel char device. ROADMAP:375's `/dev/ether0`
  note is amended accordingly.

---

## 14. The TCP/IP stack: smoltcp

- **smoltcp** (Rust, `no_std`, `alloc`-based) is the stack, embedded in netd.
  It is the native libthyla-rs fit (no musl, no pouch) and was already named in
  ROADMAP §9.1. It provides Ethernet/ARP/IPv4/IPv6/ICMP/TCP/UDP + a DHCPv4
  client.
- netd's fid state machine (§3.4) wraps smoltcp sockets; smoltcp owns the wire
  protocol correctness (its authors spec'd it — ROADMAP §9.3's "protocol
  correctness is smoltcp's responsibility" stands).
- **Fallback** (ROADMAP §9.5 risk): if smoltcp can't cover a needed surface, a
  Plan 9 IP-stack port is the recorded fallback; the fid state machine (§3.4) is
  stack-agnostic by design (it wraps *a* socket abstraction), so the fallback
  does not change `/net`.

---

## 15. Invariants and audit-trigger surfaces

### 15.1 No new §28 invariant

The network arc adds **no new numbered §28 invariant** (the A-5 precedent — a
capstone integration that composes existing invariants). Network soundness is
governed by:

| Invariant | How the network arc is bound by it |
|---|---|
| **I-1** | A Proc's network reach is bounded by its `/net` territory view (the firewall, §8). |
| **I-5** | netd's NIC handles (`KObj_MMIO`/`IRQ`/`DMA`) are non-transferable — the device can't leak. |
| **I-9** | The `dev9p.poll`→netd readiness wake loses no edge (§12.2; reserved `net_poll.tla`). |
| **I-10/I-11** | netd's connection-`N` reuse + fid identity (§3.4) — no stale/reused connection. |
| **I-23** | netd's authority is bounded by its endowed NIC capability (the service-FS-authority property — netd owns exactly its NIC, nothing more). |
| **I-28** | `/net` navigation is path-contained + per-component X-searched (the firewall mechanism, §8). |
| **I-29/I-30** | Network I/O riding Loom inherits Loom's completion integrity + submit-time capability pin (§12.1). |

### 15.2 Reserved audit-trigger surfaces (enumerate at impl)

Per the Tapestry §28 / Loom precedent (reserve now, enumerate in §25.4 +
CLAUDE.md at impl), the net arc reserves these audit-bearing surfaces; each
joins the ARCH §25.4 + CLAUDE.md audit-trigger table in the sub-chunk that
lands it:

- **`netd`** — the stack server: the fid state machine, the smoltcp embedding,
  the connection table SMP-safety, the NIC ownership.
- **`dev9p.poll`** — the new readiness bridge (the I-9 surface).
- **the socket-compat pouch boundary-line** (`00NN-pouch-net-sockets.patch`) —
  the BSD→`/net` translation (the existing pouch audit discipline).
- **the virtio-net userspace driver** (RX+TX) — the DMA/IRQ/ring memory safety
  (the virtio-blk audit class).
- **`cs`/`dns`/`ndb`** — the resolver (hostile-response bounds, cache safety).
- **`ipconfig`/DHCP** — the lease-handling + `ipifc` ctl surface.
- **`SYS_CLOCK_SETTIME`** (if pulled forward, §10) — the settable-clock ABI.

---

## 16. Exit criteria (refines ROADMAP §9.2; closes W4-F8)

W4-F8: the existing criteria were 100% client-side, never proving the
listen/accept/soak shape that defines the network workload. The refined set:

**Client (existing, kept):**
- [ ] `ping 1.1.1.1` works (ICMP via `/net/icmp`).
- [ ] `curl`/`wget https://example.com` fetches a URL (TLS via the root bundle, §9).
- [ ] `ssh` from Thylacine to an external host works (bridged QEMU networking).
- [ ] A native libthyla-rs client (`net::TcpStream`) connects + round-trips.

**Server (NEW, W4-F8):**
- [ ] A native libthyla-rs **echo server** (`net::TcpListener`, the §4 server
      lifecycle) accepts ≥2 concurrent connections and echoes correctly.
- [ ] The **Loom-multishot accept loop** (§12.1) accepts a stream of inbound
      connections (the async server path proven, not just blocking accept).
- [ ] A ported server (sshd-server or a Pouch echo server) accepts a connection
      (the socket-compat `listen`/`accept` path, §7).

**Soak (NEW, W4-F8):**
- [ ] A sustained-throughput soak: N connections × M seconds of bidirectional
      traffic, asserting **no fd/connection/Burrow leak** (the connection table
      returns to baseline) and no corruption — run under the SMP gate
      (default+UBSan × smp4/smp8).

**Posture:**
- [ ] No regressions in Utopia.
- [ ] No P0/P1 audit findings on the reserved surfaces (§15.2).

---

## 17. Sub-chunk phasing

The net arc splits into focused, independently-landable sub-chunks (each with
its own status row + audit where audit-bearing):

| Sub-chunk | Scope |
|---|---|
| **net-0** | This charter (scripture; no code). |
| **net-1** | virtio-net RX path + the frame transport (generalize the probes to RX+TX under netd's NIC ownership). |
| **net-2** | netd skeleton: embed smoltcp, serve `/net`, the `/net/tcp` `clone`/`connect`/`data` client path + the §3.4 fid state machine. **Rides the virtio-PCI transport** (preempted by the pci sub-arc below; resumes on the PCI NIC). |
| **net-3** | server side: `announce`/`listen`/`accept` + `/net/udp` + `/net/icmp` (ping). |
| **net-4** | naming + config: `cs`/`dns`/`ndb` (§5) + `ipconfig`/DHCP (§6). |
| **net-5** | the socket-compat pouch boundary-line (§7) — Linux/pouch binaries reach `/net`. |
| **net-6a** | blocking sockets: netd deferred-reply data reads + the pouch `shutdown`/`sendto`/`recvfrom` completion (closes the net-5 audit seam #209) + the Loom-multishot async path (§12.1, zero new Loom core) + a native echo server (§16). Userspace + the already-built kernel Loom — **no new ABI** (autonomy). |
| **net-6b** | the synchronous `poll()`/`select()` bridge (§12.2): the kernel `dev9p.poll` slot + the netd readiness file + the I-9 wake. **ABI surface — spec-first `net_poll.tla`, then impl, then focused audit** (user-voted 2026-06-18: the kernel-bridge over the pouch-thread-per-fd shim). |
| **net-7** | TLS root bundle (§9) + SNTP + `SYS_CLOCK_SETTIME` (§10) + observability (`/net/summary`, `netstat`, §11). |
| **net-8** | exit criteria: the server + soak proofs (§16) + one focused audit over the reserved surfaces. |

**Preempt — the virtio-PCI transport (pci-0..pci-3, `docs/VIRTIO-PCI-DESIGN.md`).**
net-1 surfaced **#140**: virtio-net (slot 30) and the Stratum-pool virtio-blk
(slot 31) share one 4 KiB virtio-mmio page, and the page-exclusive `KObj_MMIO`
claim (over a hard 4 KiB MMU granule) cannot give two *persistent* userspace
drivers (`netd` + `stratumd`) sound, isolated co-residency. Per the
depth-first-dependencies principle (user-voted 2026-06-15), net-2 is **preempted**
to build the future-proof virtio-PCI transport — net moves to its own
page-aligned PCI BAR, dissolving the contention by construction. **net-only**
scope (blk → PCI is a v1.x seam); **INTx** interrupts (MSI-X needs GIC ITS/v2m →
seam). net-2 resumes on the PCI NIC once pci-3 closes.

Sequencing note: the net arc (now: pci-0..pci-3 → net-2..net-8) precedes the
container runner (#70) and the on-system toolchain (#67) in Phase 8
(ROADMAP §2.2). pci-0..pci-3 + net-2..net-3 are the critical path (a working
client); net-4..net-8 complete the surface.

---

## 18. Recorded v1.x seams (deferred, by design)

- **Explicit stateful packet filter** (§8) — per-interface/per-connection
  allow/deny rules (the `nic_router` precedent), layered on namespace
  restriction. The v1.0 firewall is namespace-restriction (D-net-3).
- **`cs`/`dns` as independent daemons + the live `/lib/ndb` read** (§5) — the
  Plan 9 purity; v1.0 folds them into netd, which (as a confined warden-bound
  leaf driver, I-34) sources a **compiled-in ndb + the live DHCP entries**, not
  a live `/lib/ndb` read (it cannot reach `/lib`). The v1.x split into a
  post-pivot daemon WITH a namespace is what reads the baked `/lib/ndb/local`
  live (the file already exists at v1.0, the real ndb(6) format). The file
  interface is identical, so the split is a clean refactor.
- **Per-territory stack instances** (D-net-1 rejected alternative) — if a future
  workload needs per-component stack fault-isolation (the Genode model), the
  shared-netd design does not preclude adding a per-territory mode behind a
  `nic_router`-style mux. Not v1.0.
- **IPv6 hardening, IL, raw `/net/ether0`** — smoltcp provides IPv6; the net arc
  proves IPv4 first. IL is a deliberate non-port (§3.1). A raw frame file is a
  netd `/net/ether0` (§13), exposed only if a use case appears.
- **`epoll`-shaped readiness** — out at v1.0 (ARCH §11.5); `poll`/`select`
  (§12.2) + Loom (§12.1) are the v1.0 readiness surfaces.

---

## 19. The #68 hole-resolution ledger

Every RW-13 `#68` register finding, and where this charter closes it:

| Finding | Hole | Closed by |
|---|---|---|
| W4-F1 | packet filter designed nowhere | §8 (namespace-restriction; explicit filter → v1.x §18) — **D-net-3** |
| W4-F2 | socket-shim claimed vs ARCH §11.5 zero socket syscalls | §7 (pouch-userspace translation; reconciled — not kernel syscalls) — **D-net-2** |
| W4-F3 | DNS/`cs` mechanism designed by nothing | §5 (`cs`/`dns`/`ndb`, netd-served) |
| W4-F4 | TLS/root-cert-bundle only in an exit criterion | §9 (rustls native + baked root bundle at a canonical path) |
| W4-F5 | network observability undesigned | §11 (per-conn status + stats + `/net/summary` + `netstat`) |
| W4-F6 | poll-readiness over 9P undesigned (`dev9p` no `.poll`) | §12 (Loom-multishot async + `dev9p.poll` bridge + reserved `net_poll.tla`) |
| W4-F7 | `/net` schema + fid state machine pre-design; §9.3 waiver mis-scoped | §3 (the schema + the netd fid state machine, distinct from smoltcp) |
| W4-F8 | exit criteria 100% client-side | §16 (server + soak criteria added) |
| W4-F10 | IP config (DHCP/static) undesigned | §6 (`ipconfig` static + smoltcp DHCP; data-driven, not slirp-assumed) |
| W4-F11 | NTP/clock-sync designed nowhere | §10 (SNTP client + `SYS_CLOCK_SETTIME` pull-forward) |
| W4-F15 | virtio-net probes built; `/dev/ether0` never landed | §13 (netd owns the NIC; no `/dev/ether0`; ROADMAP:375 amended) |

(W4-F9 → #64 reconciliation, already closed; W4-F12 → wait_pid_for, separate;
W4-F13 daemon-logging → record-only; W4-F14 import/exportfs → acceptable depth,
named in §2.)

---

## 20. Status

- **net-0 (this charter): LANDED** — the design conversation (2026-06-15, three
  user votes) is bound; the eleven `#68` holes are closed at design level; the
  v1.x seams are recorded.
- **net-1: LANDED** — the reusable virtio-net frame-transport driver
  (`usr/lib/netdev`); the virtio-PCI transport sub-arc (pci-0..pci-3) re-homed it
  on a per-device BAR (`VirtioNetPci`).
- **net-2a: LANDED** — `netd` (`usr/netd`) embeds smoltcp on the PCI NIC (the
  Menagerie warden binds `virtio-pci:1` narrowed — the live I-34-on-PCI proof,
  evolved from the 6b-3 ARP demo) and acquires a DHCP lease (`10.0.2.15/24`),
  proving the whole lower stack (Ethernet/ARP/UDP/DHCP) end-to-end. The NIC owner
  *is* the stack Proc (I-5 non-transferable handles). See
  `docs/reference/121-netd.md`.
- **net-2b-1: LANDED** — `netd` is a *persistent* service: the libdriver
  `Lifecycle` manifest field + the warden's leave-running-on-`READY` policy keep
  it resident past the bind phase (the transient `netdev-driver` MMIO demo still
  exercises the `DeviceRemoved` teardown).
- **net-2b-2: LANDED** — the 9P `/net` server. `netd` posts `/srv/net` (9P-mode)
  and serves the §3.1 directory skeleton (`tcp/udp/icmp` + a read-only `stats`
  file each) over a combined accept/stack event loop; joey mounts it at `/net`
  (post-pivot, so every login inherits it). Posting requires
  `MAY_POST_SERVICE`, conferred joey→warden→netd (gated on the persistent
  lifecycle). The `clone`→socket fid machine (§3.4) + live counters are net-2c.
  See `docs/reference/121-netd.md`.
- **net-2c-1: LANDED** — the `/net/tcp` `clone` fid state machine (§3.4): the
  dynamic qid-encoded tree, the clone-mints-`N` Plan 9 idiom (the kernel dev9p
  client accepts the rebound-fid `Rlopen` qid), the refcounted connection slots
  (the *last* clunk frees `N` — the only free path, the I-10/I-11 invariant), and
  the `libthyla_rs::ninep` `Treaddir` codec (`/net/tcp` lists its live `N/`
  directories). A slot is "N assigned" — no smoltcp socket yet (`MAX_SLOTS = 16`,
  a #65 DoS floor). Boot proof: `clone`→0, `/net/tcp/0/ctl`→0, `Treaddir` grows
  with the live entry, the clunk frees + reuses 0. Pure userspace — kernel
  byte-unchanged. See `docs/reference/121-netd.md`.
- **net-2c-2: LANDED** — the live TCP data path. The `socket-tcp` smoltcp
  feature; the `Net` table now owns the `Interface` + `SocketSet` (moved in
  post-DHCP) so the 9P dispatch reaches the stack; `clone` reserves a real
  `tcp::Socket` (freed at the last clunk — the §3.4 `ALLOCATED` state). The `ctl`
  verb parser drives `connect a.b.c.d!port` (active-open: `socket.connect`, an
  ephemeral local port since smoltcp requires a non-zero one) and `hangup`;
  `announce`/options are honestly `EOPNOTSUPP` (net-3+). `status`/`local`/`remote`/
  `err` report the live socket; `data` read/write is `recv_slice`/`send_slice`
  (non-blocking — blocking/readiness is the net-6 dev9p.poll leg). Boot proof
  (deterministic, peer-independent): `connect 10.0.2.2!9` → `remote 10.0.2.2!9` +
  `local 10.0.2.15!…` + the multi-fid clunk frees + reuses `N`. The NIC-IRQ poll
  fd is deferred (a pollable IRQ fd is a kernel ABI surface; the `poll_delay`-
  clamped timeout poll is correct, ≤ 50 ms under load). Pure userspace — kernel
  byte-unchanged. See `docs/reference/121-netd.md`.
- **net-2d: LANDED** — the focused audit over the netd surface (the central
  audit-bearing surface, §15.2): an Opus-4.8-max prosecutor + a concurrent
  self-audit, **CLEAN 0 P0 / 0 P1 / 1 P2 / 4 P3, NOT dirty**. Fixed F1 [P2] (the
  `h_readdir` budget omitted the 11-byte Rreaddir frame overhead → a populated
  dir read by a small-msize client overran its msize), F2 [P3] (the `P9_NOFID`
  sentinel accepted as a live fid), F3 [P3] (a rejected re-`connect` burned an
  ephemeral port + a rolled-back clone over-counted `opened`); F4/F5 + the
  cross-session liveness closed-justified. Kernel byte-unchanged.
- **net-3a: LANDED** — the server side: `announce` + the blocking `listen`/accept.
  `announce *!port` puts a connection's socket into LISTEN; the blocking
  `open(listen)` is a **deferred 9P reply** — netd *holds* the Rlopen (registering
  a pending accept) and keeps polling, since a single-threaded server cannot block
  in the handler (it must keep polling the NIC to receive the very SYN that would
  unblock it). When the listener establishes, a socket-swap mints the accepted
  connection `M` (taking the established socket) + re-arms the listener, and the
  held `Rlopen(tag, M/ctl)` is sent, unblocking the client's open. This is the
  committed-blocking realization of §3.4 over the existing dev9p client (match-by-
  tag, no per-op deadline, death-interruptible) — **no kernel surface** (§12's
  readiness multiplexing via `dev9p.poll`/Loom is the separate net-6 leg). The
  client-death path is handled: a `Tflush(oldtag)` cancels the pending accept +
  replies `Rflush` (also closing a pre-existing net-2c-2 outstanding-tag leak; the
  only shared-crate change is the `ninep` Tflush/Rflush codec). Boot proof
  (deterministic): `announce *!7777 → Listen` + the `listen` file in the readdir +
  the not-announced gate. The full inbound-accept E2E is owed to net-3d (a
  deterministic in-guest inbound path — a netd loopback interface). Pure userspace
  — kernel byte-unchanged. See `docs/reference/121-netd.md`.
- **net-3b: LANDED** — UDP: `/net/udp` clone/connect/data (datagrams). The shared
  `Slot` carries a `proto` (TCP/UDP — the `get::<tcp::Socket>` vs `get::<udp::
  Socket>` discriminator; every socket touch is dispatched on it, since a mismatch
  panics in smoltcp). `/net/udp/clone` mints a `udp::Socket` (a `PacketBuffer` of
  whole datagrams with per-packet sender metadata); `connect ip!port` binds a local
  port + records the remote; `data` is `send_slice`/`recv_slice` of datagrams;
  there is no `listen` file (UDP has no accept). The slot pool is shared, so
  `walk_child`/`for_each_child` filter the numeric children of each protocol dir.
  Boot proof: joey's deterministic `/net/udp` machinery probe (clone → connect →
  endpoint readback → `Open` → readdir → no-listen → free+reuse) + netd's
  best-effort DNS round-trip demo through the live data path (logged, not a gate —
  slirp forwards DNS to the host resolver). The `socket-udp` Cargo feature is the
  only new dependency; pure userspace — kernel byte-unchanged. The deterministic
  UDP-via-9P data round-trip E2E is owed to net-3d (the loopback interface). See
  `docs/reference/121-netd.md`.
- **net-3c: LANDED** — ICMP: `/net/icmp` clone/connect/data (ping). The third
  `proto` along the `Slot` discriminator. `/net/icmp/clone` mints an `icmp::Socket`
  bound to a rotated Echo identifier (smoltcp routes EchoReplies back by ident);
  `connect` is portless — a bare IPv4 — and only records the ping target; a `data`
  write wraps the payload into an `EchoRequest` (smoltcp's own encoder), a `data`
  read returns the matching `EchoReply` payload; there is no `listen` file, and
  `hangup` is a no-op (connectionless). Boot proof: joey's deterministic
  `/net/icmp` machinery probe (clone → portless connect → bare-address readback →
  `Open` → readdir → no-listen → free+reuse) + netd's best-effort gateway-ping demo
  through the live data path (logged, not a gate — whether slirp answers a guest
  echo internally vs proxying it to a host ping is host-dependent; on the dev host
  it did not answer, vindicating the best-effort framing). The `socket-icmp` Cargo
  feature is the only new dependency; pure userspace — kernel byte-unchanged. The
  deterministic in-guest ICMP round-trip E2E is owed to net-3d (the loopback iface
  auto-replies to an echo to its own IP). See `docs/reference/121-netd.md`.
- **net-3d: LANDED** — the focused net-3 audit (one Opus-4.8-max prosecutor + a
  concurrent self-audit) + the deterministic in-guest loopback E2E. The audit
  found **F1 [P1]**: a half-open deferred-`listen` fid stranded a generation-less
  `PendingAccept`, so a clunk + cross-proto slot re-mint drove a wrong-proto
  `get::<tcp::Socket>` panic (a whole-network DoS) — fixed by a per-slot mint
  generation + the `poll_accepts` proto+gen guard + a `cancel_accept_fid`-on-clunk
  + marking the listen fid `opened` (every strand facet closed). **F2 [P2]** folds
  into the guard; **F3/F4** are P3 doc caveats. The **loopback E2E** (an isolated
  `127.0.0.1` stack driving the real `Net` methods) delivers the three owed
  deterministic in-guest round-trips (TCP inbound-accept, UDP datagram, ICMP echo)
  — the TCP leg is the runtime regression for the F1 fix. The isolation is
  load-bearing: a loopback iface sharing the live NIC socket set mis-routes (the
  NIC default route steals the `127.0.0.1` egress, verified in the smoltcp source).
  Pure userspace; the kernel is byte-unchanged. `memory/audit_net3_closed_list.md`.
- **net-4a: LANDED** — `/net/cs` (the connection server: dial → clonefile line) +
  the compiled-in ndb (§5). cs resolves a numeric IPv4, an ndb static host
  (`localhost`), and a numeric/named service (`http` → 80); a non-numeric non-ndb
  name yields an empty response (DNS delegation is net-4b). Per the **net-4
  ndb-source decision** (user-voted 2026-06-18; §5/§18 refinement): netd, a confined
  leaf driver (I-34), cannot read `/lib`, so it compiles in `ndb/local` and serves
  cs from it (the config-at-construction idiom) — the byte-identical
  `/lib/ndb/local` is baked into the post-pivot FS (user-readable; the v1.x cs/dns
  daemon split's live source); the resolver/router are read live from the DHCP
  lease. Pure userspace; the kernel is byte-unchanged. Boot proof: `net-4a PROBE OK`
  + `/lib/ndb/local readable` + 930/930 + SMP gate clean. See `docs/reference/121-netd.md`.
- **net-4b: LANDED** — `/net/dns` (the resolver) + cs→dns delegation. One shared
  `dns::Socket` seeded from the DHCP resolver multiplexes every `/net/dns` + cs→dns
  query; the net-4a `CsSession` generalizes into the deferred-capable per-fid
  `Query`. A name resolves **numeric → ndb → DNS**: numeric/ndb fill synchronously;
  a DNS name starts a query and the read **defers** (the net-3a held-Rread
  mechanism, completed by `poll_dns`). The central hazard — smoltcp's
  `get_query_result` frees the slot on a result + panics on a free slot — is closed
  by keeping the handle in one place (`Query.query`) nulled on every result, never
  double-polled (a netd panic = a whole-network DoS). v1.0 is IPv4-A only. Pure
  userspace (the only new dependency is `socket-dns`); the kernel is byte-unchanged.
  Boot proof: `net-4b PROBE OK (dns 10.0.2.2 → 10.0.2.2; localhost ip → 127.0.0.1;
  aaaa → empty)` + the best-effort live `net-4b DNS live query OK` + 930/930 + SMP
  gate clean. The deterministic in-guest E2E of the 9P deferred-read plumbing
  landed at net-4d (a loopback DNS responder). See `docs/reference/121-netd.md`.
- **net-4c: LANDED** — `/net/ipifc/0` (the interface-config tree) + `/net/ndb` (the
  live dynamic database) + the native `ipconfig` tool (§6). The DHCP lease folds
  into an `IfConfig` snapshot at bring-up (the dynamic path) and is surfaced
  read-only through `status`/`local`/`ndb`; `ipconfig add IP MASK [GW]`/`remove`
  applies static config (the bridged path) onto both the live iface and the
  snapshot. The resolver socket (net-4b) is seeded from `ifc.dns`, so cs→dns and the
  ndb `dns=` line are one source of truth. Pure userspace; the kernel is
  byte-unchanged. Boot proof: `net-4c ipifc E2E PASS` (the in-guest add/remove/
  status/ndb selftest) + `net-4c PROBE OK (ipifc status addr=10.0.2.15 dhcp
  gw=10.0.2.2; ndb ip=10.0.2.15 dns; local addr; ctl rejects malformed)` + 930/930 +
  SMP gate clean. See `docs/reference/121-netd.md`.
- **net-4d: LANDED** — the focused net-4 audit + close (Opus-4.8-max prosecutor +
  self-audit; **0 P0 / 0 P1 / 1 P2 / 3 P3, NOT dirty**, + a precautionary round-2 on
  the deferred-reply fix, CLEAN). **F1 [P2]** closed a cs/dns held-`Rread` loss (a
  2nd concurrent read / a re-write while a read is deferred could drop the held tag,
  I-9) with two minimal guards. Three deterministic in-guest proofs landed:
  `proto_selftest` (the cs/dns/ndb/mask parser battery — the OWED-since-net-2d
  host-test coverage, in-guest), `dns_defer_guard_selftest` (the F1 regression), and
  `dns_loopback_e2e` (the OWED net-4b deferred-read E2E — a mock DNS responder on an
  isolated `127.0.0.1` stack). Pure userspace; the kernel is byte-unchanged. Boot
  proof: `net-4d proto selftest PASS` + `net-4d dns defer-guard PASS` + `net-4d dns
  loopback E2E PASS` + 930/930 + SMP gate clean. **The net-4 arc is COMPLETE.** See
  `docs/reference/121-netd.md`.
- **net-5: LANDED** — the BSD-socket compatibility boundary-line (§7). A new
  pouch patch `usr/lib/pouch/patches/0016-pouch-net-sockets.patch` translates
  AF_INET `socket()`/`connect()`/`bind()`/`listen()`/`accept()`/`send()`/`recv()`/
  `getsockname()`/`getpeername()`/`setsockopt()`/`getsockopt()` into operations on
  netd's `/net` 9P files — the Genode `socket_fs`-in-libc model. It **stacks on**
  `0006-pouch-sockets.patch` (the AF_UNIX-over-`/srv` layer): a slot carries
  `family` FAM_UNIX (the 0006 path) or FAM_INET (`/net`); each call gains an
  AF_INET arm. **NO new kernel surface** (only `SYS_open`/`read`/`write`/`close`,
  already seamed — reconciling W4-F2). The **blocking** subset only; non-blocking
  + `poll`/`select` need the `dev9p.poll` readiness bridge (net-6), so
  `SOCK_NONBLOCK` → `EOPNOTSUPP` (P-3). Proving binary `/pouch-hello-net` (the
  first POSIX `socket()` program Thylacine runs): the deterministic control-surface
  AF_INET dance (socket / family+proto reject / setsockopt / bind /
  listen→announce / getsockname / close) against `/net`, plus a best-effort logged
  live `connect`+round-trip. Boot proof: `net-5 PROBE OK` + `pouch-hello-net:
  control surface OK` + 930/930 + boot OK + 0 EXT + the net-2c/3/4 probes
  unregressed. The live `recv` returns 0 (netd's data read is non-blocking — the
  net-2c-2 documented behavior; blocking-until-data is the net-6 readiness leg).
  Pure userspace — kernel byte-unchanged. The full deterministic in-guest data
  round-trip + the ported-server/soak E2E are net-8's exit criteria (§16).
  See `docs/reference/78-pouch.md` (the AF_INET socket backend).
- **net-6: IN PROGRESS (design bound 2026-06-18 user vote).** The §12.2
  synchronous-readiness fork was surfaced with prior-art attached (Plan 9's
  one-proc-per-fd + blocking-read idiom; Linux v9fs has *no* real 9P `.poll`;
  Fuchsia/Genode converge on server-driven readiness the poller waits on) and the
  user **voted the kernel `dev9p.poll` bridge** over a pouch-thread-per-fd shim
  and over deferring `poll()` to v1.x — the system-best real readiness primitive
  (the charter's committed §12.2 direction, realized 9P-natively via the existing
  read completion). Decomposed: **net-6a** (autonomy, no new ABI), split into:
  **net-6a-1 — LANDED** (netd blocking `data` reads — closes the net-5 F2
  `recv`-returns-0 seam: `data_recv_outcome` Data/WouldBlock/Eof + `h_read` park +
  `poll_data` deliver, the net-3a/4b deferred-reply pattern; the
  `recv_blocking_e2e` in-guest proof; a pouch `recv()` blocks automatically);
  **net-6a-2 — LANDED** (the pouch `shutdown`/`sendto`/`recvfrom` completion,
  closes net-5 F1: `0017-pouch-net-datacalls.patch` makes the three tag-aware --
  shutdown(SHUT_WR) -> netd `hangup`, sendto's dest re-points an AF_INET UDP
  datagram, recvfrom blocks [net-6a-1] + fills src; `sendmsg`/`recvmsg`/
  `socketpair` stay a fail-closed ENOSYS seam; the `/pouch-hello-net` control
  surface proves it; PURE userspace, kernel byte-unchanged); **net-6a-3 —
  LANDED** (the native `net::TcpStream`/`TcpListener` API +
  `usr/net-echo` + the Loom async composition; §16; PURE userspace, kernel
  byte-unchanged). The native `net` module (`libthyla_rs::net`) is the
  Plan 9-shaped client of `/net` (the libthyla-rs analog of the pouch AF_INET
  boundary-line: `clone`→`ctl`→`connect`/`announce`, `data`, `listen`-accept).
  A native echo server (`net-echo serve`) is the §16 server-lifecycle artifact.
  The deterministic in-guest proofs: netd's isolated-loopback `echo_e2e`
  (≥2-concurrent accept + bidirectional echo — the server LOGIC, the net-3d
  pattern) + the `/net-echo` boot probe (the native API's parse/bind/announce/
  local over the live `/net`, + the **Loom async witness**: a Loom READ on a
  `/net` fid completes via the kernel dev9p async client, proving network I/O
  rides Loom with **zero new Loom core** — §12.1). The composition verified:
  a Loom READ on `/net/tcp/N/data` composes with net-6a-1's `poll_data` (the
  recv stream), and a Loom LOPEN on `/net/tcp/N/listen` composes with net-3a's
  `PendingAccept` (the async accept) — both ride the existing deferred-reply
  mechanism. **Seams (net-8, which owns the in-guest peer):** the live
  cross-Proc native-API ≥2-concurrent round-trip (netd's live stack is NIC-only
  — no loopback route — so an in-guest peer can't reach a native server) and the
  literal §12.1 multishot-*read*-on-`listen` accept loop (which wants a netd
  listen-read-defer; net-3a defers the *open*). The netd listener backlog is 1
  (net-3d) — a backlog > 1 is also a net-8 seam.
  Then **net-6b** (the `dev9p.poll` bridge — spec-first `net_poll.tla` + impl +
  focused audit; the one ABI surface). §12.2 + §17 refined.
- **net-7: complete** (TLS + SNTP + `SYS_CLOCK_SETTIME` + observability; audited +
  gated at net-7d).
- **net-8: in progress.**
  - **net-8a (landed): the resident loopback interface** — netd carries a real
    `lo` stack (127.0.0.1/8 on its own `Loopback` device + `SocketSet`, polled
    alongside the NIC; a 127.x dial/announce migrates the connection's socket to
    it). This is the in-guest peer mechanism the owed E2Es need: the NIC-only live
    stack could not give a deterministic in-guest peer (slirp does not loop a
    guest-to-own-IP packet). A loopback socket cannot share the NIC iface+set (the
    net-3d default-route mis-route), so it is a second isolated stack — opt-in
    (`enable_loopback`), so the audited single-stack E2E selftests are untouched.
    Proven by the boot-asserted `resident_lo_selftest` (127.x migrate + accept +
    data + no-leak over the dual-poll). See `docs/reference/121-netd.md`.
  - **net-8b (landed): the in-guest over-`/net` TCP echo round-trip.** `net-echo`'s
    `loopback_e2e` dials 127.0.0.1 and accepts + echoes over the *real* `/net`
    mount — the first live exercise of the full Plan 9 dial+accept+data path
    (net-3a deferred accept + net-6a-1 blocking read + the net-8a lo stack). It is
    peer-independent (one Proc is both ends; the blocking accept/recv defer inside
    netd, so no self-deadlock). This surfaced + fixed **#239**: netd served the
    `listen` file `FILE_RO`, but the accept opens it `ORDWR` (its fid becomes the
    accepted ctl), so the A-3 dev9p `perm_check` denied the open before the
    Tlopen — latent because the deferred accept had only ever been driven by
    `echo_e2e`'s direct methods (no perm_check). Boot: `net-8b loopback E2E PASS`.
  - **net-8c-1 (landed): the §16 soak / leak-baseline.** `net-echo`'s `soak_e2e`
    runs the net-8b live over-`/net` echo round-trip 8x and requires netd's live
    TCP `active` count (`/net/tcp/stats`) to return to its pre-soak baseline — a
    per-cycle slot leak (each cycle mints a listener + connector + accepted M)
    would grow it. The I-10/I-11 slot-lifetime discipline proven under repeated
    live use. Boot: `net-8c-1 soak PASS`.
  - **net-8c-2 (landed): the `TlsStream`-over-`/net` live socket** (SA-2) — a real
    TLS 1.3 handshake + app-data echo over the live `/net` loopback. The `tls`
    crate gains a symmetric server transport `TlsServerStream<S>` (the server
    `accept`), factored with the existing client `TlsStream<S>` over one shared
    private `TlsTransport<S, C>` driver (zero record-layer duplication). `net-echo`
    spawns a server Thread (the server-side TLS state machine must pump
    concurrently with the client's — netd only moves TCP bytes, it does not run
    TLS) bound on the lo stack; the main Thread is the client; they interleave
    through netd. The cert is verified against a baked self-signed trust anchor +
    the LS-K wall clock. `net-echo` gains `CAP_CSPRNG_READ` (the handshake's
    `SYS_GETRANDOM`) + joins the TLS-bin strip (it exceeds `SYS_SPAWN_BLOB_MAX`
    unstripped). Boot: `net-8c-2 TLS-over-/net E2E PASS`. (The UDP/ICMP *protocol*
    logic over `/net` is proven at net-3b/3c/3d on the isolated lo stack; the
    live-mount dev9p plumbing — perm_check, deferred replies, blocking reads — is
    proto-agnostic and exercised over the real mount by net-8b/8c-2 for TCP, so a
    live-mount UDP/ICMP round-trip is a net-8d audit consideration, not a named
    §15.2 exit criterion.)
  - **net-8d: the whole-arc focused audit (§15.2) + the close.**

The thylacine is real. So is its network — and it is, of course, a filesystem.
