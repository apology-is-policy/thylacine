# 121 — netd: the network daemon (NET-DESIGN.md, the #68 charter)

## Purpose

`netd` (`usr/netd`) owns the NIC and runs the TCP/IP stack. It is the network
arc's central Proc: the Menagerie [warden](119-warden.md) binds it **narrowed**
to the `virtio-pci:1` function's I-34 allowance (the PCI `(bus,dev,fn)` + the
wired INTID + a DMA pool — nothing else), and netd claims the device, drives the
[virtio-net-pci transport](114-netdev.md), and embeds [smoltcp](#the-smoltcp-stack)
as its stack. From net-2b it also serves `/net` as a 9P server (the Plan 9
`/net`-via-`netd` model — NET-DESIGN.md §2).

**Why netd is the NIC owner, not a separate stack Proc.** The NIC handles
(`KObj_PCI`/`KObj_IRQ`/`KObj_DMA`) are non-transferable (**I-5**), so the Proc
that *claims* the device is the Proc that must *run* the stack — a driver cannot
hand its device to a peer. This reconciles the charter's "joey spawns netd" with
the Menagerie warden-binds-narrowed model: netd is simply `netdev-pci-driver`
(the 6b-3 ARP-proof demo) evolved into the daemon, conferred exactly the NIC and
nothing more. The narrowing *is* the I-34 allowance; netd holds the live
I-34-on-PCI proof.

## The warden bind

The warden's compiled-in manifest (`usr/warden/src/main.rs::BUILTIN_MANIFESTS`):

```
driver "netd" {
    abi   = 1
    binds = ["virtio-pci:1"]      # the virtio-net PCI function (devpci-discovered)
    needs {
        pci = "node"              # the (bus,dev,fn) — claimed via SYS_PCI_CLAIM
        irq = "node:interrupts"   # the swizzled INTx INTID
        dma = "pool: 64 KiB"      # the ring + frame pools (4 KiB ring + 2×32 KiB)
    }
    serves    = "/net"            # the 9P /net tree (served from net-2b-2)
    restart   = on-crash
    lifecycle = persistent        # a standing service -- the warden leaves it running
}
```

There is **no `mmio` axis**: a PCI function's registers live in its BARs, mapped
through `SYS_PCI_MAP_BAR` off the claimed `KObj_PCI`, not through an MMIO
allowance window. The warden confers this allowance + `CAP_HW_CREATE` + a grant
descriptor (argv[1]); netd's `probe` re-checks the conferred identity is exactly
`virtio-pci:1` and fails closed on a mis-bind before touching hardware.

## The driver shape (`impl libdriver::Driver`)

netd is a [libdriver](118-libdriver.md) `Driver`, so it slots into the warden's
spawn/grant/readiness protocol identically to every other bound driver:

- **`probe(grant)`** — verify `DeviceId::parse(grant.compatible) == VirtioPci(1)`,
  then `VirtioNetPci::open()` (claim the function, map BARs, run the VIRTIO 1.2
  modern-PCI init, arm the device). A non-net grant or an open failure → `Err`.
- **`serve(self, grant)`** — own the device and run the stack. It brings the link
  up (the DHCP proof, below), then — since net-2b-1 — signals `READY` and stays
  resident in a stack-poll loop (the warden's `lifecycle = persistent` leaves it
  running). net-2b-2 turns that loop into the 9P `/net` serve loop.

Diagnostics go to the **console** (`t_putstr`): a warden-spawned driver's stderr
is `/dev/null`. netd is a **persistent** service, so it signals bring-up success
with a `READY` line on stdout (the warden's readiness pipe) and then stays
resident — the long-lived-service contract, as opposed to a one-shot that signals
completion by **exiting**.

## The smoltcp stack

[smoltcp](https://docs.rs/smoltcp) `0.12.0` is the embedded TCP/IP stack
(NET-DESIGN.md §14) — `no_std` + `alloc`, the native libthyla-rs fit (no musl, no
pouch). The `netd` Cargo.toml pins it `default-features = false` with the minimal
feature set (`alloc`, `medium-ethernet`, `proto-ipv4`, `socket-dhcpv4` at
net-2a; `socket-tcp` etc. join as the protocol surface grows). libthyla-rs
provides everything smoltcp needs: the global allocator (`alloc::ThylaAlloc`),
the monotonic clock (`time::Instant`, mapped to `smoltcp::time::Instant`), and
the CSPRNG (`rand::fill_bytes`, seeding the interface's transaction/ISN source).

### The phy::Device over `VirtioNetPci`

`NicDevice` wraps the [netdev](114-netdev.md) `VirtioNetPci` as a smoltcp
`phy::Device`. The token pattern avoids aliasing (smoltcp's `receive` hands back
*both* an RxToken and a TxToken from one `&mut self`):

- **`NicRxToken`** OWNS its received bytes (a `Vec<u8>` copied out of the RX
  ring in `receive`) — it holds no device borrow.
- **`NicTxToken`** holds the single `&'a mut VirtioNetPci` borrow; its
  `consume(len, f)` fills a frame buffer and calls `nic.send` (back-pressure
  tolerant — `send` self-drains the TX ring and drops only if still full, which
  smoltcp recovers from by retransmit).

`capabilities()` reports `Medium::Ethernet` and `max_transmission_unit =
netdev::MAX_FRAME` (the largest L2 frame `nic.send` accepts, so smoltcp never
builds a frame the NIC drops; the derived IP MTU is `MAX_FRAME − 14 = 1500`).

## net-2a: the DHCP-lease proof

`serve` brings the link up by acquiring a DHCP lease from QEMU's slirp DHCP
server, exercising the whole lower stack end-to-end (Ethernet TX/RX over the
BAR-mapped virtqueues → ARP → UDP → the DHCP client state machine):

1. Build a smoltcp `Interface` with `Config::new(Ethernet(mac))` (the NIC MAC)
   and a CSPRNG `random_seed`.
2. Add a `dhcpv4::Socket` to a `SocketSet`.
3. A bounded **sleep-poll** loop (10 ms cadence, ~5 s cap): `iface.poll(now,
   device, sockets)` drains RX + drives egress; check `dhcp.poll()` for
   `Event::Configured`. On a lease, install the address + default route, print
   it, and **exit 0**. The loop is self-bounding (it cannot hang — the right
   shape for a one-shot boot proof; net-2b's persistent serve uses an IRQ-driven
   event loop instead).

A lease prints (the boot proof):

```
netd: up mac=[52, 54, 00, 12, 34, 57] link=true mtu=1500 -- bringing the link up (DHCP)
netd: DHCP lease addr=10.0.2.15/24 router=Some(10.0.2.2) dns=1 ip-mtu=1500
netd: PASS -- smoltcp brought the link up via the PCI NIC (DHCP)
```

On DHCP failure netd exits non-zero → the warden restarts it (bounded) then gives
up SOFT — the boot still completes. **On success** netd no longer exits: since
net-2b-1 it signals `READY` and stays resident (below).

## net-2b-1: the persistent lifecycle

net-2a's `serve` exited after the lease (a one-shot); net-2b-1 makes netd a
**resident service**. After the DHCP lease it announces success on the console,
signals `READY` on stdout, and enters a resident stack-poll loop that never
returns — keeping the link up (DHCP renew, ARP) until the Proc dies:

```
netd: PASS -- smoltcp brought the link up via the PCI NIC (DHCP)
netd: serving (persistent; /net 9P server lands at net-2b-2)
warden: netd pid=… up (READY) -> serving (persistent; left running)
```

The warden's `lifecycle = persistent` manifest field (libdriver `Lifecycle`,
[118-libdriver.md](118-libdriver.md); MENAGERIE.md §5) is what makes the warden
*leave netd running* on `READY` instead of tearing it down via `DeviceRemoved`
(the transient path the `netdev-driver` MMIO demo still exercises). The warden
drops netd's `Child` un-waited (no kill-on-drop), so netd reparents to the
orphan-adopter when the warden exits; its I-34 allowance is bound to its own Proc
(confer-at-spawn, #160), so the warden's exit neither revokes the NIC nor reaps
the daemon.

The resident loop honors smoltcp's `poll_delay` hint, clamped to `[50 ms, 1 s]`:
a floor that forecloses a 0 ms busy-spin (the #108 idle-spin class), a ceiling
that keeps netd responsive and bounds the idle-wakeup rate. With no active
sockets the hint is the DHCP renew deadline, so an idle netd wakes ~once/second.
net-2b-2 folds the 9P accept into this loop.

## net-2b-2: the 9P /net server

net-2b-2 stands the `/net` server up. After the lease, netd **posts `/srv/net`**
(9P-mode) and then runs a **combined event loop** that multiplexes the static
`/net` 9P tree (`server.rs`) with the smoltcp stack — `READY` is signalled
*after* the post, so the warden's "left running" also means "the service is up".

**The directory skeleton** (NET-DESIGN.md §3.1, the read-only subset): `/net`
root → `tcp/`, `udp/`, `icmp/` directories, each with a `stats` file (honestly
zeroed counters at net-2b-2). It is served by a static node table (`server.rs`),
where the array index is the qid path. The `clone`→socket fid machine (§3.4) and
live counters are net-2c — serving a `clone` with no socket behind it would be a
half-built fid machine, so the skeleton deliberately stops at walkable dirs + a
readable file (the proof the mount path works), and net-2c grows it.

**Posting the service requires `PROC_FLAG_MAY_POST_SERVICE`** (the kernel
`devsrv_post_listener` gate), which a warden-spawned driver does not inherit. The
warden confers it — gated on `lifecycle = persistent` (a persistent service is
exactly the one that serves a namespace) — and the warden may confer it because
joey grants the warden the bit at spawn (`t_spawn_with_perms`; joey is
console-attached *and* holds the bit, so the kernel's per-bit grant gate
[console-attached OR already-holds] passes). This is the #827b one-hop delegation
extended one hop: **joey → warden → netd**. A transient driver serves no
namespace and is conferred nothing.

**The combined loop**: `t_poll` over `[listener] + connections` with the
poll-delay hint as the timeout. A 9P request wakes the loop immediately; on an
idle timeout the stack is still serviced (DHCP renew, ARP) at least that often.
Each accepted connection is a `server::Conn` (a fid table + framing buffers); a
read drains every complete frame and dispatches it (Tversion / Tattach / Twalk /
Tlopen / Tread / Tgetattr / Tclunk). **`Tgetattr` is load-bearing**: it reports
the security trio (mode `0555`/`0444`, uid/gid `SYSTEM`) the kernel's A-3 dev9p
per-component X-search reads — an unfilled trio fails closed and denies the walk,
and the world-`r-x` mode keeps `/net` usable by every logged-in session.

**joey mounts it**: post-pivot (after the `/srv` re-graft, before logins inherit
the namespace), `t_open("/srv/net", OREAD)` (9P-mode open=connect → a dev9p root)
+ `t_mount("/net", …, MREPL)`. `t_mount` bumps the root's refcount, so the 9P
session to netd stays alive after joey closes its connect fd. A missing
`/srv/net` (netd gave up / no NIC) is non-fatal — `/net` is simply absent.

```
netd: serving /net (9P over /srv/net)
warden: netd pid=… up (READY) -> serving (persistent; left running)
joey: net-2b-2 /net mounted (netd 9P server)
joey: net-2b-2 PROBE /net/tcp/stats OK (43 bytes)
```

## net-2c-1: the `/net/tcp` clone fid state machine

net-2c-1 grows the net-2b-2 static skeleton into the **live TCP fid state
machine** (NET-DESIGN.md §3.4), the `clone`-minted numbered-connection half of
the tree, plus the `Treaddir` codec a dynamic directory needs. It is still pure
userspace — the kernel is byte-unchanged.

**The dynamic tree.** The static node table is replaced by a qid encoding with
two disjoint ranges: the static skeleton occupies small fixed paths (`/net`=0,
`tcp`=1, …, `tcp/clone`=4, `tcp/stats`=5, …), and a live connection `N` under a
protocol encodes as `CONN_FLAG(1<<40) | (proto<<32) | (N<<8) | filekind`, so the
connection's directory and each of its files (`ctl`/`data`/`local`/`remote`/
`status`/`err`) is one stable qid. A walk resolves a connection node **only while
its slot is live**, so a stale or forged connection qid is unreachable.

**The clone idiom.** Opening `/net/tcp/clone` (`Tlopen`) **mints** a connection:
it claims a free slot `N`, then *rebinds the opened fid onto that connection's
`ctl`* and returns the `ctl` qid — the Plan 9 clone idiom. The kernel dev9p
client accepts an `Rlopen` qid that differs from the walked qid (verified:
`kernel/9p_session.c` stores the open qid without comparing it to the walk qid),
so the rebind is legal. Reading the clone-opened fid therefore yields `N`
(ASCII). A second fid can reach the same connection by walking `/net/tcp/N/…`.

**The refcount (the I-10/I-11 invariant).** A connection is reference-counted by
the fids that name its subtree. Every fid bound into `/net/tcp/N/` refs slot `N`;
a clunk (or a walk that moves the fid out, or a session teardown, or a Tversion
reset) unrefs it. The **last** unref frees `N` — the *only* free path — so `N` is
not reusable until the directory is fully torn down (a late reply on an old `N`
is never mis-attributed). `fid_set` refs the new connection *before* unreffing
the old, so a within-connection rebind never transits refs==0 and frees the slot
out from under the fid. Minting is bounded at `MAX_SLOTS = 16` (a DoS floor,
#65); past it, `clone` open returns `ENOMEM`.

**`Treaddir`.** `libthyla_rs::ninep` gains the readdir codec (`parse_treaddir` +
`build_rreaddir` + `pack_dirent`; dirent = `qid(13)│next_offset(8)│type(1)│
name`). netd serves it: a directory's children are enumerated in a stable order
(static names, then live slots ascending), each dirent's `next_offset` is a
strictly-increasing ordinal cookie (never 0), and the page is bounded by the
Treaddir `count` and msize. So `/net/tcp` lists `clone`, `stats`, and every live
`N/` directory — and the kernel's dev9p readdir issues `Treaddir`, not a legacy
`Tread` stream, so this codec is required for `ls /net` to list at all.

At net-2c-1 a slot carried *no smoltcp socket* — it was "N assigned"
(`ALLOCATED`-without-wire), so `data`/`local`/`remote`/`err` read empty and
`status` read `Closed`. **net-2c-2 made the connection live** (next section): the
slot grows a real `SocketHandle`, the `ctl` verb parser drives `connect`, and the
endpoint/status/data files report the live socket.

**netd is single-threaded** (one Proc, one `serve()` loop): every 9P frame across
every session is processed sequentially, so the global connection table (`Net`)
needs no lock — the refcount is single-threaded-safe by construction (§3.4 "netd
serializes its own connection table").

```
joey: net-2c-1 /net mounted (netd 9P server)
joey: net-2c-1 PROBE OK (clone->0, 0/ctl->0, readdir grew, clunk frees+reuses 0)
```

## net-2c-2: the live TCP data path

net-2c-2 makes the net-2c-1 connection *live*. It is still pure userspace — the
kernel is byte-unchanged; the only build change is the `socket-tcp` smoltcp
feature.

**The `Net` table owns the stack.** After the DHCP bring-up, `serve()` moves the
smoltcp `Interface` + `SocketSet` into the `Net` table (which the 9P dispatch
already holds `&mut`), so a `ctl`/`data` handler reaches both as disjoint fields.
The `device` (the `phy::Device` over `VirtioNetPci`) stays a `serve()` local —
`Net::poll(&mut device)` borrows it per call. The serve loop drives the stack
through `net.poll` at the top of each iteration *and* again after dispatching a
batch of 9P requests, so a just-enqueued SYN/data egresses that tick instead of
waiting for the poll timeout.

**The socket reserved at clone.** `clone` now reserves a real `tcp::Socket` (rx/tx
`SocketBuffer`s, 4 KiB each) and stores its `SocketHandle` in the slot — the
faithful §3.4 `ALLOCATED` state (one socket bound to the connection for its open
lifetime). The last clunk (`slot_unref` → refs 0) `SocketSet::remove`s it, the
*sole* free path, so the socket lifetime is exactly the connection's.

**The `ctl` verb parser (§3.3).** Writing `ctl` parses one command:
- `connect a.b.c.d!port` — active-open: `socket.connect(iface.context(), remote,
  local)`. The local endpoint uses a rotating ephemeral port (49152..=65535;
  smoltcp requires a non-zero local port — it auto-selects only the local
  *address*). `connect()` sets the tuple + `SynSent` **synchronously**, so netd
  records the resolved `local`/`remote` in the slot immediately (peer-independent).
- `hangup` — active-close (`socket.close()`).
- `announce`/`bind`/`keepalive`/`ttl`/`tos` — the server side + per-connection
  options, rejected honestly (`EOPNOTSUPP`) not silently accepted; they land net-3+.

**The connection files.** `status` reports the live `socket.state()` (`Syn-Sent`/
`Established`/…); `local`/`remote` report the recorded `ip!port`; `err` reports a
recorded failure reason. `data` is the byte stream: a write is `send_slice`, a
read is `recv_slice` of available bytes. `data` I/O is **non-blocking** at
net-2c-2 (a 0-length read is ambiguous between "no data yet" and EOF); blocking /
readiness is the dev9p.poll leg (net-6).

**Boot proof** (deterministic + peer-independent — it does not depend on slirp
replying): the joey probe `clone`s, writes `connect 10.0.2.2!9` on the ctl fid
(the write returns the full count → smoltcp accepted the active-open), then reads
back `remote == 10.0.2.2!9` (the dialed target) and `local == 10.0.2.15!…` (the
source address `connect()` selected via the live iface + the ephemeral port),
proving the whole ctl→socket→iface→endpoint path. Each endpoint fid refs+unrefs
`N`, and the final clunk frees + reuses `N` (the multi-fid refcount).

```
joey: net-2c-2 PROBE OK (clone->0, connect 10.0.2.2!9, local 10.0.2.15!, frees+reuses 0)
```

## Data structures

| Type | Role |
|---|---|
| `NetD { nic: VirtioNetPci }` | the driver; owns the claimed device |
| `NicDevice { nic: VirtioNetPci }` | the smoltcp `phy::Device` adapter |
| `NicRxToken { frame: Vec<u8> }` | owns one received frame (no device borrow) |
| `NicTxToken<'a> { nic: &'a mut VirtioNetPci }` | the single `&mut nic` TX borrow |
| `server::Conn` | one accepted 9P connection: fid table + `in_buf`/`out_buf` |
| `server::Net` | the global connection table (`Slot[MAX_SLOTS]`) + the smoltcp `iface`/`sockets` (folded in post-DHCP) + the ephemeral-port cursor + live stats; shared `&mut` across all sessions |
| `server::Slot { used, refs, socket, local, remote, err }` | one `/net/tcp/N/` connection: the refcount key + its reserved `SocketHandle` + the recorded endpoints / err reason |

## Tests

- **Boot proof** (the live path): `tools/test.sh` → the warden binds netd → the
  `netd: PASS … (DHCP)` line + lease `10.0.2.15/24` + `netd: serving /net` +
  `4 bound, 3 up` + `joey: net-2c-2 PROBE OK (clone->0, connect 10.0.2.2!9, local
  10.0.2.15!, frees+reuses 0)` — the full §3.4 fid machine + the live data path
  exercised through the live dev9p 9P session: `clone` mints `N` (reserving a
  smoltcp socket), the `ctl` `connect` verb drives `socket.connect` (the write
  returns the full count), `remote`/`local` read back the recorded endpoints, and
  the multi-fid clunk frees + reuses `N` — plus 930/930 + boot OK + 0 EXTINCTION.
- **SMP gate**: `tools/ci-smp-gate.sh` (default+UBSan × smp4/smp8, N=10) — netd is
  single-threaded; the boot probe exercises the (already-gated) kernel dev9p
  client + the 9P clone/connect/readdir/clunk traffic + netd's live-socket path
  under SMP. PASS, 0 corruption; every "timing"-classified boot is ground-truthed
  guest-clean (boot OK + the net-2c-2 probe + 930/930 + 0 EXTINCTION, at login —
  the harness post-marker exit artifact, not a guest defect).
- The fid machine + the live connect path are validated by the boot probe as an
  **integration test** — netd builds the wire, the kernel dev9p client parses it,
  joey reads it back (two independent implementations cross-checking end-to-end),
  the same discipline as net-2b-2 + corvus (libthyla-rs is no_std with no host
  test harness). A `data` round-trip needs a real peer (deferred — see caveats).

## Error paths

- `probe` mis-bind (identity ≠ `virtio-pci:1`) → `Err(NoMatch)` (fail-closed).
- `VirtioNetPci::open()` failure (no device / already-claimed / BAR map / no
  INTID / feature reject) → `Err(Hardware)`.
- DHCP no-lease within the poll bound → `Err(Hardware)` → warden restart → SOFT
  give-up (boot survives).

## Status

- **net-2a (LANDED)**: smoltcp embedded + the `phy::Device` over `VirtioNetPci` +
  the DHCP-lease boot proof; netd is the warden-bound `virtio-pci:1` driver
  (retiring the `netdev-pci-driver` ARP demo it subsumes). The kernel is
  byte-unchanged (pure userspace).
- **net-2b-1 (LANDED)**: the persistent lifecycle — the libdriver `Lifecycle`
  manifest field + the warden's leave-running-on-`READY` policy; netd signals
  `READY` and stays resident in the stack-poll loop. The kernel is byte-unchanged.
- **net-2b-2 (LANDED)**: the 9P `/net` server (`server.rs`, the corvus precedent +
  `libthyla_rs::ninep`) — posts `/srv/net`, serves the NET-DESIGN.md §3.1 directory
  skeleton (`tcp/udp/icmp` + `stats`), joey mounts it at `/net`; the resident loop
  multiplexes the 9P accept with the stack. The MAY_POST_SERVICE conferral
  (joey→warden→netd, gated on the persistent lifecycle) is the only new privilege
  surface; the kernel is byte-unchanged (pure userspace). Boot proof: `/net`
  mounted + `/net/tcp/stats` walked+read (43 bytes).
- **net-2c-1 (LANDED)**: the `/net/tcp` `clone` fid state machine (§3.4) — the
  dynamic qid-encoded tree, the clone-mints-`N` Plan 9 idiom, the refcounted
  connection slots (last clunk frees `N`, the only free path), and the ninep
  `Treaddir` codec (`/net/tcp` lists its live `N/` directories). A slot is
  "N assigned" — no smoltcp socket yet. The kernel is byte-unchanged.
- **net-2c-2 (LANDED)**: the live TCP data path — the `socket-tcp` feature, the
  smoltcp socket reserved at clone (freed at the last clunk), the `Net` table
  owning the iface + socket set, the `ctl` verb parser (`connect`/`hangup` live;
  `announce`/options → `EOPNOTSUPP`, net-3+), `data` recv/send (non-blocking), and
  `status`/`local`/`remote`/`err`. The boot probe drives a live `connect` +
  endpoint readback. The kernel is byte-unchanged. (The NIC-IRQ poll fd is
  deferred — see the caveats.)
- **net-2d**: the focused audit over the netd surface (the fid machine +
  connection-table SMP-safety + NIC ownership) + the ARCH §25.4 / CLAUDE.md
  audit-trigger enumeration + the SMP gate + close.

## Known caveats / seams

- **`readdir` lands at net-2c-1**: `/net` directories now list via `Treaddir`
  (the ninep readdir codec). A `Tread` on a directory still returns `EISDIR` (the
  9P2000.L convention — directory enumeration is `Treaddir`, and the kernel dev9p
  client issues exactly that).
- **udp/icmp `clone` is deferred** (net-2c-1): the `clone`-minted machine is TCP
  only; `udp/`/`icmp/` keep just their `stats` file. The qid encoding reserves a
  `proto` field so udp/icmp connections slot in without a re-encode, but their
  sockets land with the protocol surface (post-net-2c).
- **The poll is timeout-driven, not NIC-IRQ-woken**: the combined loop's `t_poll`
  timeout is smoltcp's `poll_delay` (clamped `[50 ms, 1 s]`). With an active TCP
  socket smoltcp's hint is short, so the clamp floor governs (≤ 50 ms RX latency
  under load). A *pollable* NIC-IRQ fd would need a kernel ABI surface
  (`SYS_IRQ_WAIT` blocks; it is not pollable) — deferred; the timeout poll is
  correct, just not minimal, and the post-dispatch `net.poll` flush keeps TX
  prompt.
- **`data` I/O is non-blocking** (net-2c-2): a `data` read returns the available
  rx bytes, and a 0-length read is ambiguous between "no data yet" and EOF;
  blocking/readiness is the dev9p.poll leg (net-6). A `data` round-trip needs a
  real peer (QEMU slirp has no general TCP listener) — the net-2c-2 boot proof
  covers the deterministic connect + endpoint readback; a round-trip is a manual
  hostfwd test (or net-3's announce/listen loopback).
- **The ephemeral local-port allocator is a rotating cursor** (49152..=65535), not
  liveness-checked — at `MAX_SLOTS = 16` connections a collision after wrap is
  astronomically unlikely; a checked allocator is a v1.x refinement.
- **The connection table is bounded at `MAX_CONNS = 8`** and effectively
  single-session at v1.0 (joey's one `/net` mount drives one kernel dev9p-client
  session multiplexing all callers). Excess connections pend in the kernel; the
  only connectors are trusted (the kernel dev9p client + joey's mount). net-2d
  prosecutes the connection-table + fid-machine SMP-safety.
- **smoltcp owns wire-protocol correctness** (its authors spec'd it); netd's
  state machine (net-2c) wraps *a* socket abstraction, so the recorded fallback
  (a Plan 9 IP-stack port, NET-DESIGN.md §14) would not change `/net`.

## References

- `docs/NET-DESIGN.md` (the #68 charter) — §2 (one netd, narrowed views), §13
  (the userspace virtio-net driver), §14 (smoltcp), §17 (sub-chunk phasing).
- `docs/reference/114-netdev.md` (`VirtioNetPci`), `119-warden.md` (the bind),
  `118-libdriver.md` (the `Driver` trait + the allowance).
- ARCH §10.1 (the network is 9P), §28 (no new net invariant — composes
  I-1/I-5/I-9/I-10/I-11/I-23/I-28).
