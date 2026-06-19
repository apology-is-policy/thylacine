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
read is `recv_slice` of available bytes. `data` I/O was **non-blocking** at
net-2c-2 (a 0-length read was ambiguous between "no data yet" and EOF);
**net-6a makes the read blocking** (a parked-and-completed deferred reply — see
below). The synchronous `poll()`/`select()` *readiness* bridge — distinct from a
blocking read — is the `dev9p.poll` leg (net-6b).

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

## net-3a: the server side (announce + the blocking `listen` via a deferred 9P reply)

net-3a adds the server half of §3.4 — `announce` + `listen`/accept — over the
net-2c-1 fid machine (which reserved the `ANNOUNCED` state + the `listen` file).
Still pure userspace; the kernel is byte-unchanged. The only shared-crate change
is the `libthyla_rs::ninep` **Tflush/Rflush** codec (the cancellation path needs
it), mirroring net-2c-1's readdir-codec addition.

**`announce` puts the socket into LISTEN.** Writing `announce *!port` (or
`announce a.b.c.d!port`) on a connection's `ctl` calls `socket.listen(ep)` (smoltcp)
and records the endpoint in the slot (`listen_ep`). The connection is now
`ANNOUNCED`; `status` reads `Listen`. `*` is the any-address listen.

**The blocking `listen` open is a *deferred 9P reply* — the load-bearing
mechanism.** netd is a single-threaded 9P server: if `h_lopen` on a `listen` file
*blocked* waiting for an inbound call, the whole serve loop would stall and never
poll the NIC to *receive* the very SYN that would unblock it (a self-deadlock).
The fix is the 9P-native one — netd **holds the Rlopen**:

1. `open(/net/tcp/N/listen)` arrives as a `Tlopen`. `h_lopen` registers a
   `PendingAccept { conn_id, tag, fid, listening_n }` in `Net` and returns the
   `Disp::Deferred` sentinel: dispatch emits **no reply** for this frame. The
   client's `open()` syscall stays blocked on the outstanding tag (the kernel
   dev9p client matches replies purely by tag, with no per-op deadline, and the
   sleep is #811-death-interruptible — `kernel/9p_client.c`).
2. The serve loop, after `net.poll`, calls `Net::poll_accepts()`: for each pending
   whose listener's socket left LISTEN for an established connection
   (`accept_ready` = `is_active() && state ≠ SynReceived`), it **swaps** —
   `accept_swap` mints a NEW connection `M` that takes `N`'s now-established
   `SocketHandle`, and re-arms `N` with a *fresh* listening socket on `listen_ep`
   (so `N` stays `ANNOUNCED` for the next call). It emits an `AcceptDone`.
3. The serve loop finds the issuing `Conn` by `conn_id`, calls `complete_accept`:
   rebind the blocked listen fid onto `M`'s `ctl` (the refcount moves from `N` to
   `M`; `N` stays alive via the announce fid), build the held `Rlopen(tag, M/ctl
   qid)`, and write it — unblocking the client's `open()`, which returns a fd onto
   `M`'s ctl (the Plan 9 `listen` contract: open(listen) → a fresh connection's
   ctl). If the issuing session already closed, the unowned mint is freed.

This is the **committed-blocking** realization of §3.4. It is distinct from §12's
*readiness multiplexing* (`poll()`/`select()` via `dev9p.poll`, and async via
Loom) — both are net-6. A `listen` open *commits* to one accept (deferred reply
handles it natively over the existing client); `poll()` multiplexes N fds without
committing (needs the readiness bridge). They are complementary, not the same
surface — so net-3a needs **no kernel surface** and no `net_poll.tla`.

**Tflush cancels a dead deferred accept.** A `listen` Rlopen can be held for a
long time, so a client that dies on its blocked open is reachable: the kernel
client sends `Tflush(oldtag=T)` (`client_run` on a death-interrupt). `h_flush`
cancels the pending accept for `T` (no late Rlopen, no connection minted for the
dead op) and replies `Rflush` — which is what frees the kernel's `awaiting_flush`
reservation on `T` (per 9P, `oldtag` is reusable only after the Rflush). This
also closes a **pre-existing net-2c-2 latent**: an ignored Tflush leaked the
kernel's outstanding tag; net-3a is the first deferred op that makes the window
wide, and it owns the fix. A Tversion session reset + a Conn teardown likewise
cancel any pending accepts the session held.

**Accept latency.** The inbound SYN arrives on the NIC (not a pollable fd), so
only a timeout-driven `net.poll` catches it; with a pending accept the serve loop
clamps the idle-poll delay to the floor (`IDLE_POLL_MIN_MS = 50 ms`).

**Backlog-of-1.** Between an accept and the swap re-arming `N`, a second inbound
SYN finds `N` mid-establish (no listener) → RST. A proper listen backlog (several
listening sockets per announce) is a documented v1.x refinement.

**Boot proof** (deterministic + self-contained — no inbound peer needed): the joey
probe `clone`s A, writes `announce *!7777` (→ `status` reads `Listen`), confirms
the `listen` file appears in the connection readdir, and confirms that opening
`listen` on a **non-announced** connection is rejected immediately (`E_INVAL`,
proving the gate without blocking).

```
joey: net-3a PROBE OK (announce *!7777 -> Listen; listen file + readdir; listen gated on announce)
```

**The full inbound-accept E2E is owed to net-3d.** A real call → a held Rlopen → a
fresh connection needs a deterministic in-guest inbound path (QEMU slirp does not
loop a guest→self connection back, and there is no host actor at boot). net-3d
adds one — the recommended path is a netd **loopback interface** (`127.0.0.0/8`, a
real feature every stack has), giving a deterministic in-guest accept proof
without host-injection timing fragility; the deferred-reply + swap + Tflush
mechanism is reasoned + audited there.

## net-3b: UDP — `/net/udp` clone/connect/data (datagrams)

net-3b adds the datagram protocol. `/net/udp` grows a `clone` file mirroring
`/net/tcp/clone`: opening it mints a connection backed by a smoltcp `udp::Socket`
(a `PacketBuffer` of whole datagrams *with* per-packet metadata — the sender
endpoint — unlike TCP's byte stream). The connection tree, the refcounted fid
machine, the qid encoding, and the Treaddir listing are all the §3.4 machinery,
now generalized over a `proto` axis.

**The shared slot carries a `proto`.** `Slot` gains a `proto` field (`PROTO_TCP`
/ `PROTO_UDP`); it is the discriminator for the type-recovering socket access
(`sockets.get::<tcp::Socket>` vs `get::<udp::Socket>` — a mismatch *panics* in
smoltcp, so every socket touch is dispatched on `slot_proto(n)`). The slot pool is
shared across protocols, so a TCP conn `N` and a UDP conn `N` cannot coexist (one
slot index); `walk_child`/`for_each_child` filter the numeric children of
`/net/tcp` and `/net/udp` to slots of the matching protocol.

**`connect` is the datagram setup, not a handshake.** `ctl_connect` dispatches:
TCP active-opens (CONNECTING); UDP `bind`s a local ephemeral port (smoltcp
requires a bound socket before send/recv) and records the remote, so subsequent
`data` writes default to it. `status` reads `Open` (bound) / `Closed`. `data`
write `send_slice(data, remote)` (one datagram, all-or-nothing); `data` read
`recv_slice → (n, meta)` dequeues one datagram (the sender endpoint is dropped at
net-3b — the connected client knows its remote). There is **no `listen` file**
under a UDP conn dir (datagrams have no accept) — `walk_child` rejects `listen`
for a UDP conn, so opening it fails.

**The boot demo is a self-contained DNS round-trip** through the live `/net/udp`
data path (`udp_clone → connect → data_send → poll → data_recv`), bounded-polled
inside netd like the DHCP bring-up so it is deterministic in its own loop. It is
**best-effort, logged, never a boot gate**: slirp *forwards* DNS to the host
resolver (unlike its internal DHCP server), so a response is environment-dependent.

```
netd: net-3b UDP round-trip OK (DNS resp 61 bytes, ancount=2)
joey: net-3b PROBE OK (udp clone->0, connect 10.0.2.3!53, remote readback, no listen, frees+reuses 0)
```

The deterministic, asserted proof is joey's `/net/udp` machinery probe (clone →
connect → endpoint readback → `status` Open → readdir → no-listen → free + reuse).
**The deterministic UDP-via-9P data round-trip E2E is owed to net-3d** — the same
loopback interface that proves the TCP accept also round-trips a UDP datagram to
`127.0.0.1` in-guest, with no host-resolver coupling.

## net-3c: ICMP — `/net/icmp` ping

net-3c adds the third protocol along the same `proto` axis. `/net/icmp` grows a
`clone` file: opening it mints a connection backed by a smoltcp `icmp::Socket`
bound to a unique Echo **identifier** (rotated per clone from `ICMP_IDENT_BASE`),
so smoltcp routes EchoReplies for this connection's pings back to it (its
`accepts` filters incoming ICMP by the bound ident). `PROTO_ICMP` joins the slot
discriminator; the connection tree / refcounted fid machine / Treaddir listing
are the §3.4 machinery, unchanged.

**`connect` is portless.** ICMP has no ports and no handshake (the bind happened
at clone), so `ctl_connect` dispatches to `icmp_connect`, which only records the
ping target. The `ctl` verb parser accepts a **bare IPv4** for an ICMP slot
(`connect 10.0.2.2`; any `!suffix` is ignored), where TCP/UDP require the full
`ip!port`. `remote`/`local` report just the dotted-quad address (no `!port` —
`Content::push_ip`). `status` reads `Open` (the bound echo socket) / `Closed`.

**`data` is the Plan 9 ping shape.** A `data` write is an ICMP echo-request
*payload*: netd wraps it into an `Icmpv4Repr::EchoRequest { ident, seq_no, data }`
(smoltcp's own encoder; the iface recomputes the checksum on egress) and sends it
to the recorded target. A `data` read dequeues a whole ICMP packet smoltcp already
filtered to the bound ident, parses it, and returns the EchoReply's payload (a
non-reply or parse error → 0). There is **no `listen` file** under an ICMP conn
dir, and `hangup` is a no-op (a connectionless socket has nothing to close — the
fid clunk frees the slot + removes the socket).

**The boot demo is a self-contained gateway ping** through the live `/net/icmp`
data path (`icmp_clone → connect → data_send → poll → data_recv`), bounded-polled
inside netd. It is **best-effort, logged, never a boot gate**: whether QEMU slirp
answers a guest echo to the gateway *internally* (vs proxying it to a host ping
socket the host may not permit) is host-dependent — so a round-trip is not a sound
boot gate. (On the dev host slirp did *not* answer it — which is exactly why the
round-trip is best-effort and the machinery probe is the proof.)

```
netd: net-3c ICMP round-trip best-effort: no echo reply (slirp host-ping?) -- /net/icmp machinery proven via joey
joey: net-3c PROBE OK (icmp clone->0, connect 10.0.2.2, remote readback, no listen, frees+reuses 0)
```

The deterministic, asserted proof is joey's `/net/icmp` machinery probe (clone →
portless connect → bare-address readback → `status` Open → readdir → no-listen →
free + reuse). **The deterministic in-guest ICMP round-trip E2E is owed to
net-3d** — the loopback interface auto-replies to an echo request addressed to its
own IP, so a `127.0.0.1` ping round-trips in-guest with no slirp/host coupling
(joining the owed TCP-accept + UDP-datagram E2Es).

## net-3d: the focused net-3 audit + the loopback E2E + the deferred-accept hardening

net-3d closes net-3 with the focused audit (one Opus-4.8-max prosecutor + a
concurrent self-audit) and delivers the three owed deterministic in-guest E2Es.

**The audit found one real soundness hole (F1, P1):** a deferred-`listen` fid was
left **half-open** (`opened == false`) with a committed `PendingAccept`. A native
`/net` client could **clunk** it (clunk does not gate on `opened`) without
removing the pending; the slot then frees and `clone` re-mints that index
**cross-proto**, leaving a stranded `PendingAccept{listening_n=N}` with no
generation guard → `poll_accepts` does `get::<tcp::Socket>` on a now-UDP/ICMP
handle → a smoltcp **type-mismatch panic** → netd (the sole NIC owner, I-5
non-transferable) aborts → a whole-network DoS (plus walk-from / double-defer /
connection-hijack facets). Latent in-VM (the trusted kernel dev9p client abandons
only via Tflush, which `h_flush` handles), but reachable from the open=connect
native client the `MAX_CONNS` headroom anticipates — so, soundness-bar P1.

**The fix is four complementary layers:**

1. **A per-slot mint generation.** `Slot.gen` is stamped from a monotonic
   `Net.mint_seq` (`next_gen()`, never 0) at every mint (`tcp_clone`/`udp_clone`/
   `icmp_clone`, and `accept_swap` for the new `M`; the listener `N` keeps its gen
   on re-arm — it is the same listener). `PendingAccept.listening_gen` records the
   listener's gen at `register_accept`.
2. **The `poll_accepts` guard.** Before the typed `get::<tcp::Socket>`, drop the
   pending unless `slot_proto(listening_n) == Some(PROTO_TCP) && slots[listening_n]
   .gen == pa.listening_gen`. A cross-proto re-mint fails the proto check; a
   same-proto re-mint fails the gen check. Either way the panic becomes a harmless
   drop, making the typed recovery **locally sound** regardless of the fid-pinning
   invariant.
3. **`cancel_accept_fid` on clunk.** `fid_clunk` cancels any pending the clunked
   fid held, so clunking the half-open listen fid cannot strand its pending.
4. **The listen fid is marked `opened = true`.** This blocks walk-from (`h_walk`)
   and re-open / double-defer (`h_lopen`) via the existing `opened` gates;
   `complete_accept`'s `fid_set` rebind ignores `opened`, so the legitimate
   completion still works.

**The loopback E2E (`server::loopback_e2e`)** delivers the three owed
deterministic in-guest round-trips and serves as the **runtime regression** for
the F1 fix. It builds an **isolated** loopback stack — a smoltcp `Loopback` device
(TX queue → own RX) + an `Interface` (`127.0.0.1/8`) + its **own** `SocketSet` —
and drives the **real** `Net` methods over it. The isolation is load-bearing: a
loopback iface sharing the live NIC `SocketSet` mis-routes, because the NIC's
default route steals the `127.0.0.1` egress (smoltcp's `tcp::Socket::dispatch`
builds the IP repr from the socket's own tuple with no iface-address-ownership
gate, then `route()` falls to the default route — verified in the smoltcp 0.12
source). The three legs:

- **TCP inbound-accept**: announce a listener + connect a client to
  `127.0.0.1:port`; the SYN loops back, the listener establishes, and a synthetic
  `register_accept` resolves through the real `poll_accepts` → `accept_swap` (the
  F1-fixed path).
- **UDP datagram**: a udp socket re-points its remote at its own bound port and
  sends — a self-addressed datagram loops back to the same socket.
- **ICMP echo**: a single icmp socket pings `127.0.0.1`; the iface auto-replies to
  an echo to its own address and routes the reply back by ident.

With no host coupling the E2E is **asserted** (a PASS/FAIL boot line), unlike the
host-coupled gateway/DNS probes. `Net::poll` was made generic over the device so
the lo self-test can poll a `Loopback`; the live NIC path is unchanged.

```
netd: net-3d loopback E2E PASS (tcp-accept + udp + icmp, in-guest 127.0.0.1)
```

## net-4a: `/net/cs` + the compiled-in ndb (the dial front door)

`/net/cs` is the **connection server** (Plan 9 `cs(8)`): a client opens it, writes
a dial string, and reads back one `<clone-file> <ctl-arg>` line per reachable path.
A native `dial()` opens the clone-file, reads the connection number, opens its
`ctl`, and writes `connect <ctl-arg>` — exactly the arg netd's `connect` verb
already parses (`<ip>!<port>`). cs collapses host-name + service-name resolution
into one file read.

**Resolution (net-4a):**

- **proto** — `tcp` / `udp` / `net` (the `proto!host!service` first field). `net`
  maps to tcp (the v1.0 default; the tcp+udp fan-out is a v1.x refinement).
- **host** — a numeric IPv4 is used verbatim; otherwise the static ndb
  (`sys=`/`dom=` → `ip=`, e.g. `localhost` → `127.0.0.1`). A non-numeric non-ndb
  name yields no line at v1.0 (DNS delegation is **net-4b**).
- **service** — a numeric port is used verbatim; otherwise the ndb services table
  (`service=` → `port=`, e.g. `http` → `80`).
- **unresolvable / malformed** → an **empty** response (cs's "no reachable path"
  signal; the reader sees 0 bytes, never an error frame).

`/net/cs` is a request/response file: the write IS the query (`cs_set` resolves it
via `cs_resolve` + stashes the per-fid response), the read drains that response via
a per-fid cursor (offset-agnostic, like the `data` stream). The session is dropped
on clunk / teardown / Tversion, so it is bounded by the connection's fid table (no
unbounded growth — a re-write replaces the same fid's session). cs is `FILE_RW`
(opened `ORDWR`); an opened cs fid cannot be walked-from, so a stale session cannot
be rebound under a different path.

**The ndb (`/lib/ndb/local`) — the netd-confinement refinement (NET-DESIGN §5).**
netd is a confined warden-bound leaf driver (I-34): it cannot read `/lib`, and
widening its namespace would fight its I-28/I-34 confinement. So at v1.0 netd
serves cs from a **compiled-in ndb** (`include_bytes!("../ndb/local")`, parsed by
`ndb.rs` — the static `localhost` host + a services table) plus the DHCP-learned
dynamic entries read live (net-4b/4c). The **byte-identical `/lib/ndb/local`** file
is baked into the post-pivot FS (user-readable; the real ndb(6) format; the source
the v1.x cs/dns daemon split reads live — NET-DESIGN §18). This is the
capability-microkernel config-at-construction idiom (Fuchsia/Genode), not the Plan
9 full-namespace read a confined driver cannot adopt. The `ndb.rs` parser is a
line-based ndb subset: each non-comment line is space-separated `attr=value`
tokens; a host record carries `ip=` + `sys=`/`dom=`, a service record `service=` +
`port=`; a malformed entry resolves to nothing (never a crash), bounded by the
embedded file's size with no heap.

```
joey: net-4a PROBE OK (cs tcp!127.0.0.1!80 + net!localhost!http -> /net/tcp/clone 127.0.0.1!80; unresolved -> empty; /lib/ndb/local readable)
```

## net-4b: `/net/dns` (the resolver) + cs→dns delegation

`/net/dns` is the **DNS resolver**: a client writes `name [type]`, reads back the
resolved address(es). It is backed by one shared `smoltcp::socket::dns::Socket`
seeded from the DHCP-learned resolver (`Net::new(.., dns_servers)`); if the lease
carried no resolver the socket is absent (`Net.dns == None`) and every name fails
closed to the empty answer (never a hung read).

**The unified request/response session (net-4a + net-4b).** `/net/cs` and
`/net/dns` are the same shape — write a query, read the answer — so net-4b
generalizes net-4a's `CsSession` into one per-fid `server::Query`:

- The **write** begins the resolution (`query_begin`). The host resolves
  **numeric → static ndb → DNS**: a numeric or ndb host fills the answer
  *synchronously* (`resp` set, no query); a name that needs DNS starts a query
  (`query = Some(handle)`, `resp = None`). A malformed request or a resolver-less
  DNS name fills the empty answer. The write always returns `Rwrite` immediately.
- The **read** (`query_read`) drains `resp` via a per-fid cursor if the answer is
  ready. If a DNS query is still in flight it **defers**: the held `Rread` is sent
  later by `poll_dns` (the serve-loop analog of `poll_accepts`), exactly the
  net-3a deferred-reply mechanism (the client's `read()` blocks on the outstanding
  tag — the dev9p client matches by tag, no per-op deadline, #811-death-
  interruptible). This is the committed-blocking realization of a blocking
  resolver over a single-threaded server (it cannot block in `h_read` — it must
  keep polling the NIC to receive the very DNS reply that unblocks it).
- **cs→dns delegation** is automatic: a cs dial whose host is not numeric and not
  in the ndb is exactly the "delegate to DNS" branch of `query_begin` with
  `kind = Cs` — the resolved address is formatted into the `<clone-file>
  <ip>!<port>` line instead of a bare address.

**The load-bearing query-lifetime invariant.** smoltcp's `get_query_result`
**frees the query slot on a result** (Resolved/Failed) and **panics on a free
slot** — so a handle must be polled at-most-once-to-completion and never reused.
The handle lives in **exactly one place** (the issuing fid's `Query.query`), which
is nulled the instant `dns_poll` returns a result, so it is never double-polled
(a panic in netd, the sole NIC owner, would be a whole-network DoS). `dns_cancel`
(on clunk / teardown / Tversion / Tflush / rewrite) is called only while
`query == Some` — i.e. on a still-occupied slot — so it never panics. Every
in-flight query is cancelled on session drop, so no smoltcp query slot leaks.

The v1.0 resolution is IPv4-A-record only (`proto-ipv4`): a `name aaaa` (or any
non-`ip`/`a`/`ipv4` type) yields the empty answer; `/net/dns` returns the bare
dotted-quad (a v1.x refinement returns the full ndb-style attribute line).

```
joey: net-4b PROBE OK (dns 10.0.2.2 -> 10.0.2.2; localhost ip -> 127.0.0.1; aaaa -> empty)
netd: net-4b DNS live query OK (example.com -> 172.66.147.243)   # best-effort, logged, not a gate
```

The deterministic joey probe asserts only the **synchronous** fast paths (numeric
+ ndb); a live DNS query is host-coupled (slirp forwards DNS to the host
resolver), so the live resolver path is netd's **best-effort, logged** probe
(`dns_live_probe`, which exercises the Net-level `dns_query_start`/`dns_poll`
lifecycle end-to-end on the real NIC) — never a boot gate ([[feedback-no-host-load]]).
The deterministic in-guest E2E of the **9P deferred-read plumbing**
(`query_read`-defer → `poll_dns` → the held `Rread`) is **owed to net-4d** (a
loopback DNS responder, the net-3d loopback analog).

## net-4c: `/net/ipifc` + `/net/ndb` (interface config + the dynamic database)

net-4c closes the IP-configuration half of NET-DESIGN §6: the interface-config
tree, the live dynamic network database, and the `ipconfig` tool — all pure
userspace over the existing 9P (no kernel surface).

**The `/net/ipifc/0` tree** is the per-interface config view (the Plan 9 `ipifc`
idiom). There is one interface at v1.0 (the NIC), so the tree is fixed:

```
/net/ipifc/        (dir)
/net/ipifc/0/      (dir — interface 0, the NIC)
  ctl              (RW)  write config verbs; read returns "0" (the if number)
  status           (RO)  the live config, one `key value` line per fact
  local            (RO)  the address as `addr/prefix` (or `none`)
/net/ndb           (RO)  the live dynamic network database, ndb(6) format
```

`status` renders the live `IfConfig`:

```
device ether0
mac 52:54:00:12:34:56
mtu 1500
addr 10.0.2.15/24
gw 10.0.2.2
dns 10.0.2.3
mode dhcp
```

`/net/ndb` renders the same facts in ndb(6) format — the **dynamic half** of the
network database (NET-DESIGN §5: "the resolver, the default router, the interface
address: never stale"), complementing the compiled-in static half (`/net/cs`'s
`localhost`/services):

```
ip=10.0.2.15 ipmask=255.255.255.0 ipgw=10.0.2.2
	dns=10.0.2.3
```

**DHCP-lease-into-ipifc (the dynamic path).** The lease facts (address/mask,
router, resolver) are captured at bring-up in `main.rs` and folded into
`IfConfig` (`addr`/`prefix`/`gw`/`dns`/`up`/`dynamic=true`); `mac`/`mtu` are the
fixed NIC facts. `Net::new` seeds the `/net/dns` resolver socket (net-4b) from
`ifc.dns` — the same lease resolver `/net/ndb` reports, so cs→dns and the ndb
`dns=` line are one source of truth.

**`ipconfig add`/`remove` (the static path).** A `ctl` write applies static
config (NET-DESIGN §6's bridged/production path) onto **both** the live smoltcp
iface and the `IfConfig` snapshot, so a reader never sees an address the iface is
not using:

- `add <ip> <mask> [gw]` — `mask` is a dotted-quad (`255.255.255.0`), a `/N`, or a
  bare prefix length; a non-contiguous mask, an out-of-range octet, or an
  oversized prefix is rejected (`EINVAL`). Replaces the current address; clears +
  re-adds the default route (a gateway-less `add` leaves no stale route).
  Marks `dynamic = false` (`mode static`); the resolver is retained.
- `remove` (alias `unbind`) — clears the address + default route (`mode down`);
  the ndb dynamic half empties.
- `bind ether <dev>` is a **v1.x seam** (the single NIC is bound at probe), so it
  is rejected honestly rather than silently accepted.

**`ipconfig`** (`usr/coreutils/src/bin/ipconfig.rs`) is the native client: no
args prints `/net/ipifc/0/status` + `/net/ndb` (the `ip addr` equivalent);
`ipconfig add IP MASK [GW]` / `ipconfig remove` write the ctl. It is a thin 9P
client — the `/net/ipifc` tree is the mechanism.

```
netd: net-4c ipifc E2E PASS (add/remove static + status + ndb, in-guest)
joey: net-4c PROBE OK (ipifc status addr=10.0.2.15 dhcp gw=10.0.2.2; ndb ip=10.0.2.15 dns; local addr; ctl rejects malformed)
```

The `ipifc_e2e` (a throwaway `Net`, the loopback-E2E pattern) asserts the static
`add`/`remove` ctl path + the status/ndb/local renders + the malformed-input
rejects deterministically, **without disturbing the live config**. The joey probe
asserts the **read** side of the lease-into-ipifc fold (status/ndb/local reflect
the live slirp lease) plus the **9P ctl-write** path (a malformed write is
rejected — a guaranteed no-op — so the live config stays intact). The combination
proves the whole surface: the ctl logic (e2e), the read render through 9P (joey),
and the write path through 9P (joey reject).

## net-4d: the focused net-4 audit + the deferred-read guard + the loopback DNS E2E

net-4d is the focused audit over the cumulative net-4 surface (net-4a `/net/cs` +
ndb, net-4b `/net/dns` + cs→dns, net-4c `/net/ipifc` + `/net/ndb`) — one
Opus-4.8-max prosecutor + a concurrent self-audit — and the close. It is pure
userspace; the kernel is byte-unchanged.

**Close: 0 P0 / 0 P1 / 1 P2 / 3 P3, NOT dirty** (then a precautionary round-2 on
the fix — the deferred-reply lineage is the tree's most bug-prone family — CLEAN).
The `get_query_result` single-completion (a double-poll of a freed slot panics →
a whole-network DoS, I-5), the no-query-slot-leak across every abandonment path,
the cs/dns/ndb/mask parsers' fail-closed bounds, the iface↔snapshot coherence, the
single-threadedness, and the isolation all held (verified against the real smoltcp
0.12 source).

- **F1 [P2] — a held deferred cs/dns read tag could be lost (I-9).** `Query.deferred`
  is a single `(tag, cap)` slot; a *second concurrent read* on one fid (legal for a
  multi-thread Proc — the kernel dev9p client multiplexes by tag, so two `Tread`s on
  one fid are both outstanding) overwrote it, and a *re-write while a read was
  deferred* (`query_begin` → `query_drop`) dropped it — either way the first held
  `Rread` was never delivered (the reader hangs until the Proc dies; no crash, no
  cross-Proc effect, death-recoverable → P2, not P1). **Fixed with two minimal
  guards** (not a wait/wake restructure): `query_read` returns an empty `Rread` for a
  second read when one is already deferred (the first keeps its real answer); and
  `h_write` rejects (`E_PROTO`) a re-write while `fid_has_deferred(fid)`. The
  *bare-clunk-while-deferred* facet is mitigated on the trusted mount (the kernel
  abandons via `Tflush` → `cancel_dns_flush`) and is a client protocol violation
  otherwise — dispositioned consistently with the net-3 listen-clunk close.
- **F2 [P3]** the `Content` buffer was `[u8; 128]` (the net-4c commit message wrongly
  claimed 256); the worst-case status render is ~120 B (safe — `push` min-clamps, no
  OOB) but tight → bumped to `[u8; 256]`.
- **F3 [P3]** the shared dns socket's `queries` `Vec` is a bounded, reused
  high-water (≤ `MAX_CONNS × MAX_FIDS`), not a leak → documented (caveats).
- **SA-3 [P3, doc]** clarified the DHCP-renewal comment (the iface + `ifc` are both
  pinned at bring-up → coherent; renewal re-application is the v1.x seam).

Three new **deterministic in-guest proofs** (asserted PASS lines, the
`loopback_e2e`/`ipifc_e2e` pattern):

- `proto_selftest` — a battery over the pure `cs_parse`/`dns_parse`/`parse_mask`/
  `mask_octets`/`ndb` parsers (valid + every malformed-rejects-closed case). This
  is the parser COVERAGE the OWED-since-net-2d host-test module would give; netd is
  a `no_std` + aarch64 **bin** crate (unlike the netdev **lib** that
  `cargo test`s its pure `ring`), so the true host-test module needs a
  bin → feature-gated refactor that would risk the green device build — carried as a
  named seam, with the in-guest battery as the rigor floor.
- `dns_defer_guard_selftest` — the F1 regression: a throwaway `Net` whose query
  stays `Pending` (never polled) defers a read, then a second concurrent read must
  not clobber the held tag (fails on the pre-fix code by construction).
- `dns_loopback_e2e` — closes the net-4b OWED deferred-read E2E: an isolated
  `127.0.0.1/8` stack whose resolver IS a mock DNS responder bound to `:53` (a udp
  socket answering a fixed A query via `build_dns_response`), so the shared
  `dns::Socket` resolves a name **in-guest, deterministically** through the real
  `dns_query_start`→poll→`dns_poll` methods (the net-3d loopback analog).

```
netd: net-4d proto selftest PASS (cs/dns/ndb/mask parsers)
netd: net-4d dns defer-guard PASS (no lost held-Rread on a 2nd read)
netd: net-4d dns loopback E2E PASS (deferred resolve in-guest 127.0.0.1)
```

## net-6a: blocking `data` reads (the deferred-read completion)

net-6a makes the `data` recv **blocking** (POSIX socket semantics): a `read` on
`/net/<proto>/N/data` that finds the socket's rx buffer empty no longer returns 0
— it *blocks* until bytes arrive or the stream ends. This closes the net-5 audit
F2 seam (`recv()` returning 0 on transient no-data). Pure userspace — the kernel
is byte-unchanged. A pouch `recv()` → `read()` blocks **automatically**; no shim
change is needed for the read path (net-6a-2 is only the *other* BSD calls).

**The EOF-vs-no-data distinction.** The older `data_recv` returned a single
`usize` (bytes, 0 if none), collapsing "no data yet, connection open" and "end of
stream" — the ambiguity that made a blocking read impossible. net-6a adds
`data_recv_outcome` returning `RecvOutcome::{Data(n), WouldBlock, Eof}`, grounded
in smoltcp 0.12's `recv_error_check` (tcp.rs:1212):

| smoltcp `recv_slice` result | `RecvOutcome` | meaning |
|---|---|---|
| TCP `Ok(k>0)` | `Data(k)` | bytes dequeued |
| TCP `Ok(0)` | `WouldBlock` | established/half-open, rx empty |
| TCP `Err(Finished)` | `Eof` | peer FIN received + rx drained |
| TCP `Err(InvalidState)` + `is_active()` | `WouldBlock` | still connecting (SynSent/SynReceived) |
| TCP `Err(InvalidState)` + `!is_active()` | `Eof` | closed / aborted |
| UDP/ICMP `Err(Exhausted)` | `WouldBlock` | no datagram / packet queued |
| UDP `Ok((k,_))` | `Data(k)` | one datagram (k may be 0) |

UDP/ICMP never `Eof` (connectionless — a blocked datagram/ping read waits for a
packet or its fid's clunk/flush). The old `data_recv` is retained as a thin
wrapper (`Data(k) → k`, else `0`) for the best-effort net-3 demos + the loopback
E2Es, which want the non-blocking shape.

**The deferral.** When `h_read(FK_DATA)` sees `WouldBlock`, it parks a
`PendingRead { fid, slot_n, tag, cap }` and returns `Disp::Deferred` (no reply
emitted; the client's `read()` stays blocked on the outstanding 9P tag — the
kernel dev9p client matches by tag, no per-op deadline, #811-death-interruptible).
The serve loop's `poll_data` (the analog of `poll_dns`/`poll_accepts`, run
per-Conn after `net.poll()`) re-attempts the dequeue: `Data` sends the held
`Rread` with the bytes; `Eof` sends a 0-byte `Rread`; `WouldBlock` keeps waiting.
This is the **exact** deferred-reply mechanism net-3a (accept) and net-4b (dns)
use, applied to the data stream — the most bug-prone lineage in netd, so the same
lifecycle discipline holds: a parked read is cancelled on its fid's **clunk**, the
connection's **teardown**, a **Tversion** reset, and a **Tflush** on its tag.
Multiple parked reads on one fd (a multi-thread Proc reading a shared fd) are
FIFO-delivered; `pending_reads` is bounded by `MAX_FIDS` per connection (a client
cannot pin unbounded reads — the #65 DoS floor; an over-cap read is `E_PROTO`).

**I-9 (no lost wake).** netd is single-threaded, and `net.poll()` (which delivers
RX) runs in the serve loop *before* `poll_data`, so a data edge between
`h_read`'s empty dequeue and the park cannot be lost — the next `poll_data`
observes it (the `poll_dns`/`poll_accepts` reasoning). The poll-delay clamp folds
`has_pending_reads()` in (alongside `has_pending_accepts` / `has_pending_dns`) so
RX-driven completion stays prompt — RX arrives on the NIC, not a pollable 9P fd,
so only a timeout-driven `net.poll` catches it.

**Boot proof** (`recv_blocking_e2e`, deterministic in-guest, asserted): an
isolated 127.0.0.1 loopback stack (the net-3d pattern) establishes a TCP
connection through the real net-3a accept path, then drives the three states
through `data_recv_outcome` — empty → `WouldBlock`, client-sent `"hi"` →
`Data(2)` with the exact bytes (via the non-consuming `slot_can_recv` to wait),
client-close → `Eof`. No host coupling.

```
netd: net-6a recv-blocking E2E PASS (WouldBlock/Data/Eof, in-guest 127.0.0.1)
```

The full 9P-level blocking round-trip (a kernel dev9p client reading a `/net`
data fd that blocks then completes) is structurally identical to the
net-3a/net-4b-audited deferred-reply path; its deterministic end-to-end proof
lands at net-8 (the inbound-accept + soak exit criteria, which own a live
in-guest peer). The synchronous `poll()`/`select()` *readiness* multiplex over
`/net` — distinct from a blocking read — is the `dev9p.poll` bridge (net-6b).

## net-6b (2a): the `ready` readiness file (the dev9p.poll netd half)

net-6b adds a non-consuming per-connection **`/net/<proto>/N/ready`** file — the
netd half of the kernel `dev9p.poll` readiness bridge (NET-DESIGN §12.2; the
kernel `.poll` slot + the poll-pump kthread are net-6b-2b). A `read` on it
carries the requested poll event-mask in its **Tread offset** (`POLLIN | POLLOUT`
— the requestable bits; their values mirror the kernel `poll.h` ABI, since
`dev9p.poll` is the only client) and returns the satisfied `revents` as a **u32
LE**, *without consuming socket data*:

- `check_ready(n, mask)` computes `revents`: `POLLIN` iff `mask & POLLIN` and the
  socket is readable (`slot_poll_readable` — data buffered OR end-of-stream, so a
  read would not block; non-consuming, computed from `can_recv` + the TCP state
  machine, since `peek_slice` cannot see EOF); `POLLOUT` iff `mask & POLLOUT` and
  writable (`slot_can_send`); `POLLERR`/`POLLHUP` always (the output-only bits).
- A **non-zero** `revents` replies the 4-byte bitmap at once. A **zero** `revents`
  **DEFERS** — `h_read` parks a `PendingReady { fid, slot_n, tag, mask }` and
  `poll_ready` (the serve-loop pass, after `net.poll()`) re-checks each tick,
  sending the held Rread when the socket becomes ready *for that mask*. So a
  `poll(POLLIN)` on a writable-but-empty socket waits for **data**, never
  busy-loops — the offset names the specific events to wait for (the net-6b
  motivation). Bounded by `MAX_FIDS` per connection (#65 floor); cancelled on the
  fid's clunk / a Tflush on its tag / teardown / Tversion (the net-6a
  PendingRead discipline).

```
netd: net-6b ready E2E PASS (POLLOUT/POLLIN/EOF-readable, in-guest 127.0.0.1)
```

The `ready_e2e` selftest asserts `check_ready` deterministically over an isolated
127.0.0.1 loopback connection (the net-3d pattern): established → `POLLOUT` set /
`POLLIN` clear; peer sends → `POLLIN` set; peer closes + drains → `POLLIN` set
(EOF reads as readable). The full `poll()`/`select()` round-trip (a kernel
`dev9p.poll` registering a hook, eliciting the readiness read, and waking on the
held Rread) lands at net-6b-2b (the kernel ABI) + net-6b-3 (the pouch
translation).

## net-6b (2b): the kernel `dev9p.poll` consumer of the `ready` file

The kernel half of the readiness bridge is **`kernel/dev9p_poll.c`** (the
audit-bearing surface; the full design is NET-DESIGN §12.2 + that file's header +
the ARCH §25.4 / CLAUDE.md audit-trigger row). From netd's side, three contracts
matter:

- **The `ready` file carries qid.type `P9_QTPOLL` (`0x01`).** `qid_of` sets it on
  every `FK_READY` qid (walk / lopen / getattr), so the kernel's cached qid has
  the bit. `dev9p.poll` probes **only** a Spoor whose qid carries `QTPOLL`; a
  regular dev9p file (Stratum, corvus) is POSIX always-ready and never probed
  (the fail-safe distinguisher — netd is the only server that sets the bit).
- **The probe is a `Tread` whose offset is the poll event-mask, count 4.** This
  is exactly the net-6b-2a `ready`-file read: `check_ready(mask)` replies the
  satisfied `revents` (u32 LE) immediately, or **DEFERS** (parks a `PendingReady`)
  until the socket is ready for that mask. The kernel issues it as an *async* op
  driven by a poll-pump kthread (a `poll()` caller parks, so nothing else pumps
  the shared `/net` 9P reader); the held Rread wakes the parked poller.
- **A re-mask abandons via `Tflush`.** When a broader poller widens the mask, the
  kernel `Tflush`es the in-flight readiness read (the net-3a/4b `cancel_dns_flush`
  / `h_flush` path) and resubmits the union — so netd's `PendingReady` for the
  narrower mask is cancelled, never stranded. A timed-out poll likewise `Tflush`es
  its stranded readiness read (the GC), which netd's `h_flush` cancels.

The deterministic proof is the joey `net-6b` boot probe: a bound UDP socket is
polled `POLLOUT` (deterministically ready → the full submit → kthread-pump →
`check_ready(POLLOUT)` → completion → wake path) then `POLLIN` (no datagram →
netd defers → the poll parks then times out → the kthread GCs the stranded read).

## net-7b: observability (`/net/summary` + the native `netstat`)

net-7b closes NET-DESIGN §11 (W4-F5). Network introspection is 9P-native and
mostly free: the per-connection `status`/`local`/`remote`, the per-protocol
`stats` (`active`/`opened`), and the per-interface `/net/ipifc/0/status` all
already exist by net-2c…net-4c. net-7b adds the two designed *aggregate*
surfaces — a server-side rollup and a client-side tool.

**`/net/summary` — the server-side rollup (`P_SUMMARY = 18`).** A read-only,
world-readable static node under `/net`. A read renders `render_summary()` (a
one-shot aggregate, server-side, in `Net::render_summary`): the interface view
(`push_ifc_status`), the three per-protocol stats blocks (`file_content(P_*_STATS)`
reused verbatim), and a `conn` table — one line per live `Slot` (`proto N local
remote status`), reusing the same `slot_endpoint`/`state_str` renderers a
per-connection read returns. `cat /net/summary` is the one-shot view designed in
§11. It is **visibility, not authority**: it mints nothing, reports only this
netd's `/net`, and so is per-territory by construction (a Proc that cannot name
this `/net` cannot read its summary).

Two mechanics distinguish it from the other static files:
- **It renders into a `Vec`, not the fixed `Content`.** A multi-connection table
  can exceed the 256-byte `Content` cap, so `h_read` special-cases `P_SUMMARY`
  (like the `cs`/`dns` reads): build the `Vec` fresh per Tread and offset-slice
  it. A `cat` fits the whole rollup in one msize frame (coherent); a paginated
  read sees a stable byte stream as long as no slot frees mid-read — the same
  property the per-protocol `stats` carry (each is re-rendered per read).
- **`h_getattr` reports `render_summary().len()`** for `P_SUMMARY` (not the empty
  `file_content`), so a `stat` is accurate; a reader looping to EOF is unaffected
  either way. Mode is `FILE_RO` (`0444`, SYSTEM), `QTFILE` (not `QTPOLL`).

**Known caveat (net-7d audit F2): paginated reads are not cross-Tread coherent.**
Because the `Vec` is rebuilt per Tread, a summary larger than one msize frame
that is read across multiple Treads can shift between reads if another `/net`
client connects/clunks a slot in the window (a `cat` of a large rollup could see
a duplicated/dropped/shifted line). The slice access stays in bounds (no memory
bug — purely a coherency artifact), and it matches the existing per-protocol
`stats` behaviour. The v1.x fix snapshots the render into the per-fid state at
the first (offset-0) Tread and serves that snapshot until EOF.

netd holds the connection table; the kernel holds no network state — which is why
the rollup lives here in `/net` rather than at a kernel `/ctl/net` (the charter's
original path, reconciled to `/net/summary` 2026-06-19: a kernel devctl read
cannot honour the per-territory "reader's `/net` view" constraint without a
layering inversion). See NET-DESIGN §11.

**`netstat` — the client-side walk (`usr/coreutils/src/bin/netstat.rs`).** The
Plan 9 `netstat`, a thin walk of `/net` (a native libthyla-rs coreutil, sibling
to `ipconfig`; no kernel surface, just `SYS_OPEN`/`SYS_READ`/`SYS_READDIR`). It
`cat`s `/net/ipifc/0/status` and the three `/net/<proto>/stats`, then for each
protocol `read_dir`s `/net/<proto>`, filters the numeric (live) connection slots,
and reads each one's `local`/`remote`/`status` into a printed table. It is the
one-shot aggregate *as a client walk* — independent of `/net/summary`, exercising
netd's readdir + per-connection reads end-to-end, and reflecting exactly the
reader's own `/net` view. A missing `/net` (netd down / not mounted) is reported
once (the stats `cat`); an empty connection table prints just the header.

**Boot proof** (the joey `net-7b` probe): spawn `/bin/netstat` (a clean exit
proves the readdir walk runs over the live `/net`), then read `/net/summary` and
assert it carries the DHCP-leased addr (`10.0.2.15`, the ipifc section) and
`opened` (the stats section). The populated-connection-table case is structurally
identical (`render_summary`/`netstat` both iterate live slots whether zero or
many); its live in-guest round-trip is owed to net-8 (the in-guest peer).

```
joey: net-7b PROBE OK (netstat walk + /net/summary rollup)
```

No new kernel surface, no new §28 invariant — net-7b composes I-1 (the per-Proc
`/net` view) and the existing read-only introspection. The focused audit is
net-7d.

## Data structures

| Type | Role |
|---|---|
| `NetD { nic: VirtioNetPci }` | the driver; owns the claimed device |
| `NicDevice { nic: VirtioNetPci }` | the smoltcp `phy::Device` adapter |
| `NicRxToken { frame: Vec<u8> }` | owns one received frame (no device borrow) |
| `NicTxToken<'a> { nic: &'a mut VirtioNetPci }` | the single `&mut nic` TX borrow |
| `server::Conn` | one accepted 9P connection: fid table + `in_buf`/`out_buf` |
| `server::Net` | the global connection table (`Slot[MAX_SLOTS]`) + the smoltcp `iface`/`sockets` (folded in post-DHCP) + the ephemeral-port cursor + live stats; shared `&mut` across all sessions |
| `server::Slot { used, refs, proto, socket, local, remote, err, listen_ep, icmp_ident, gen }` | one `/net/<proto>/N/` connection: the refcount key + its protocol (TCP/UDP/ICMP — the `get::<T>()` discriminator) + its reserved `SocketHandle` + the recorded endpoints / err reason + the announce endpoint (TCP-only; re-arm key) + the bound Echo ident (ICMP-only) + the monotonic mint `gen` (net-3d; the deferred-accept slot-reuse guard) |
| `server::PendingAccept { conn_id, tag, fid, listening_n, listening_gen }` | a held `listen` Rlopen awaiting an inbound call (the deferred-reply state); bounded by `MAX_PENDING_ACCEPTS = 16`; `listening_gen` (net-3d) pins the listener slot's mint generation so a freed+re-minted index can never type-confuse `poll_accepts` |
| `server::AcceptDone { conn_id, tag, fid, new_n, ctl_qid_path }` | a completed accept the serve loop delivers (opaque to `main.rs`, routed by `conn_id()`) |
| `server::PendingRead { fid, slot_n, tag, cap }` | a blocking `data` read holding its Rread (net-6a); parked on an empty-but-open rx, completed by `poll_data` when bytes arrive (or 0 on EOF); a flat `Vec` (FIFO over the slot), bounded by `MAX_FIDS`; cancelled on the fid's clunk / connection teardown / Tversion / Tflush |
| `server::PendingReady { fid, slot_n, tag, mask }` | a deferred `ready` readiness probe holding its Rread (net-6b); parked when `check_ready(mask) == 0`, completed by `poll_ready` when the socket becomes ready for `mask` (the dev9p.poll netd half; non-consuming); a flat `Vec`, bounded by `MAX_FIDS`; cancelled on the fid's clunk / teardown / Tversion / Tflush |
| `enum server::RecvOutcome { Data(usize), WouldBlock, Eof }` | the result of a non-blocking dequeue (net-6a): bytes / no-data-yet-defer / end-of-stream — the distinction `data_recv_outcome` adds over the older `data_recv`'s collapsed `usize` |
| `enum Disp { Reply(usize), Deferred, Fatal }` | the per-request dispatch outcome — `Deferred` holds the reply (a blocking listen) |
| `enum DnsProbe { Ok{resp_len,ancount}, NoResponse, MintFailed }` | the net-3b best-effort UDP-round-trip demo outcome (logged, never a boot gate) |
| `enum PingProbe { Ok{reply_len}, NoResponse, MintFailed }` | the net-3c best-effort ICMP-ping demo outcome (logged, never a boot gate) |
| `server::LoopbackResult { icmp, udp, tcp }` | the net-3d in-guest loopback E2E outcome (each leg deterministic, ASSERTED via the PASS/FAIL boot line) |
| `server::Query { fid, kind, query, resp, cursor, deferred }` | a per-fid `/net/cs` + `/net/dns` request/response session (net-4a/4b); `kind` (`Cs{clone_tcp,port}` / `Dns`) selects the answer format; `query` is the in-flight `dns::QueryHandle` (nulled the instant it resolves — the single-completion discipline); `resp`/`cursor` drain the ready answer; `deferred = (tag, cap)` holds a blocked read — a *single* slot, so net-4d's F1 guards (a 2nd concurrent read gets an empty `Rread`; a re-write while deferred is `E_PROTO`) keep the held tag from being clobbered/dropped (I-9); dropped (cancelling any in-flight query) on clunk/teardown/Tversion |
| `enum server::QueryKind { Cs{clone_tcp,port}, Dns }` | how a resolved address is formatted: `Cs` → `<clone-file> <ip>!<port>\n`; `Dns` → `<ip>\n` |
| `enum DnsPoll { Pending, Resolved([u8;4]), Failed }` | the state of a started DNS query (net-4b); `Resolved`/`Failed` free the smoltcp slot |
| `enum DnsProbeResult { Ok([u8;4]), NoResponse, NoServer }` | the net-4b best-effort live-query demo outcome (logged, never a boot gate) |
| `Net.dns: Option<SocketHandle>` | the shared DNS resolver socket (net-4b); seeded from `ifc.dns` (the DHCP resolver); `None` if the lease carried none (queries fail closed fast) |
| `server::IfConfig { mac, mtu, addr, prefix, gw, dns, up, dynamic }` | the live interface-config snapshot (net-4c); `mac`/`mtu` are fixed NIC facts, the rest are the DHCP-lease (`dynamic`) or static `ipconfig add` facts; surfaced read-only through `/net/ipifc/0` + `/net/ndb`; kept coherent with the smoltcp iface by `ifc_set_static`/`ifc_clear` |
| `Net.ifc: IfConfig` | the one interface's config snapshot (net-4c); mutated only in the single-threaded serve loop (DHCP at bring-up; `ipifc_ctl` on a ctl write) |
| `ndb` module (`NDB_LOCAL` + `lookup_host` + `lookup_service`) | the compiled-in network database (NET-DESIGN §5, the static half); `include_bytes!("../ndb/local")` — the byte-identical copy baked to `/lib/ndb/local`; a stateless ndb(6)-subset parser, no heap. The **dynamic half** (resolver/router/address) is `Net.ifc`, rendered live at `/net/ndb` (net-4c) |

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
- **net-2d (LANDED)**: the focused audit over the netd surface (Opus-4.8-max
  prosecutor + concurrent self-audit) — **CLEAN: 0 P0 / 0 P1 / 1 P2 / 4 P3, NOT
  dirty**. The fid-machine refcount, the socket reservation/free balance, the
  disjoint borrow, the parser bounds, the fail-closed non-live ops, the
  single-threadedness, the I-5 probe gate, the MAY_POST_SERVICE persistent gate,
  malformed-frame safety, and the Tgetattr trio all held. Fixed: **F1 [P2]**
  `h_readdir`'s budget omitted the 11-byte Rreaddir frame overhead (`P9_HDR_LEN +
  4`) that `h_read` reserves — a populated directory read by a small-msize client
  overran its negotiated msize — fixed by a `rreaddir_budget` parity helper;
  **F2 [P3]** `h_attach`/`h_walk` accepted the `P9_NOFID` sentinel as a live fid
  → fail-closed reject; **F3 [P3]** a rejected re-`connect` burned an ephemeral
  port + a rolled-back clone over-counted `opened` → peek-then-commit port +
  `tcp_clone_rollback`. F4/F5 + the cross-session liveness closed-justified (see
  caveats). The deterministic small-msize/NOFID/failed-connect regressions are
  architecturally unreachable in-VM (the only /net client is the trusted
  large-msize kernel dev9p mount; /net is 9P-mode; netd has no host-test harness)
  → owed to a netd pure-protocol host-test module (net-3+). The kernel is
  byte-unchanged. (`memory/audit_net2d_closed_list.md`.)
- **net-3a (LANDED)**: the server side — `announce` + the blocking `listen` via a
  **deferred 9P reply** (netd holds the Rlopen until an inbound call lands; the
  socket-swap mints the accepted connection + re-arms the listener) + the
  **Tflush/Rflush** cancellation path (also closing the pre-existing net-2c-2
  outstanding-tag leak). Pure userspace; the kernel is byte-unchanged; the only
  shared-crate change is the `ninep` Tflush/Rflush codec. Boot proof: `announce
  *!7777 → Listen` + the `listen` file + the not-announced gate (deterministic).
  The full inbound-accept E2E is owed to net-3d (a deterministic in-guest inbound
  path — a netd loopback interface). The focused audit over the net-3 surface is
  net-3d.
- **net-3b (LANDED)**: UDP — `/net/udp` clone/connect/data. The shared `Slot`
  carries a `proto` (the `get::<tcp::Socket>` vs `get::<udp::Socket>`
  discriminator; every socket touch dispatched on it); `/net/udp/clone` mints a
  `udp::Socket`; `connect` binds a local port + records the remote; `data` is
  `send_slice`/`recv_slice` of whole datagrams; no `listen` file (UDP has no
  accept). The `socket-udp` Cargo feature is the only new dependency; the kernel
  is byte-unchanged (pure userspace). Boot proof: joey's `/net/udp` machinery
  probe (deterministic) + netd's best-effort DNS round-trip demo (logged). The
  deterministic UDP-via-9P data round-trip E2E is owed to net-3d (the loopback
  interface, alongside the TCP accept). The focused audit is net-3d.
- **net-3c (LANDED)**: ICMP — `/net/icmp` clone/connect/data (ping). The third
  `proto` along the slot discriminator; `/net/icmp/clone` mints an `icmp::Socket`
  bound to a rotated Echo ident; `connect` is **portless** (a bare IPv4) and only
  records the target; `data` write wraps the payload into an `EchoRequest`, `data`
  read returns the matching `EchoReply` payload; no `listen` file; `hangup` is a
  no-op (connectionless). The `socket-icmp` Cargo feature is the only new
  dependency; the kernel is byte-unchanged (pure userspace). Boot proof: joey's
  `/net/icmp` machinery probe (deterministic) + netd's best-effort gateway-ping
  demo (logged — slirp did not answer it on the dev host, vindicating the
  best-effort framing). The deterministic in-guest ICMP round-trip E2E is owed to
  net-3d (the loopback interface auto-replies to an echo to its own IP). The
  focused audit over the whole net-3 surface is net-3d.
- **net-3d (LANDED)**: the focused net-3 audit (Opus-4.8-max prosecutor +
  concurrent self-audit) + the deterministic in-guest loopback E2E. The audit
  found **F1 [P1]** — a half-open deferred-`listen` fid stranded a
  generation-less `PendingAccept`, so a clunk + cross-proto slot re-mint drove a
  wrong-proto `get::<tcp::Socket>` panic (a whole-network DoS) — fixed by a
  per-slot mint generation + the `poll_accepts` proto+gen guard + a
  `cancel_accept_fid`-on-clunk + marking the listen fid `opened` (the panic and
  every strand facet — walk-from / double-defer / hijack — closed). **F2 [P2]**
  (poll_accepts gated on liveness not proto) folds into the same guard; **F3/F4**
  + SA-2/SA-3 are P3 doc caveats below. The **loopback E2E** (an isolated
  `127.0.0.1` stack driving the real `Net` methods) delivers the three owed
  deterministic in-guest round-trips (TCP inbound-accept, UDP datagram, ICMP echo)
  — the TCP leg is the runtime regression for the F1 fix. The kernel is
  byte-unchanged (pure userspace). Boot proof: `net-3d loopback E2E PASS` + 930/930
  + boot OK + 0 EXTINCTION; SMP gate clean. (`memory/audit_net3_closed_list.md`.)
- **net-4a (LANDED)**: `/net/cs` (the connection server — dial → clonefile line) +
  the compiled-in ndb (NET-DESIGN §5 — `ndb.rs` parses the embedded `ndb/local`;
  the byte-identical `/lib/ndb/local` baked into the post-pivot FS, user-readable).
  cs resolves a numeric IPv4, an ndb static host (`localhost`), and a numeric or
  named service (`http` → 80); a non-numeric non-ndb name yields an empty response
  (DNS delegation is net-4b). A confined leaf driver cannot read `/lib`, so the
  live-`/lib/ndb` read is the v1.x cs/dns daemon split (§18); the dynamic
  resolver/router are read live from the DHCP lease (net-4b/4c). The kernel is
  byte-unchanged (pure userspace). Boot proof: `net-4a PROBE OK (cs … →
  /net/tcp/clone 127.0.0.1!80; unresolved → empty; /lib/ndb/local readable)` +
  930/930 + boot OK + 0 EXTINCTION; SMP gate clean (0 corruption across 60 boots,
  every "timing" boot ground-truthed guest-clean). The focused net-4 audit is
  net-4d.
- **net-4b (LANDED)**: `/net/dns` (the resolver) + cs→dns delegation. One shared
  `dns::Socket` seeded from the DHCP resolver multiplexes every `/net/dns` +
  cs→dns query; the net-4a `CsSession` generalizes into the deferred-capable
  per-fid `Query`. A name resolves **numeric → ndb → DNS**: numeric/ndb fill
  synchronously; a DNS name starts a query and the read **defers** (the net-3a
  held-Rread mechanism, completed by `poll_dns`). The smoltcp query-lifetime
  hazard (`get_query_result` frees on a result + panics on a free slot) is closed
  by keeping the handle in one place (`Query.query`) nulled on every result, so it
  is never double-polled (a netd panic = a whole-network DoS); `dns_cancel` runs
  only on an occupied slot. v1.0 is IPv4-A-record only. The kernel is byte-unchanged
  (pure userspace; the only new dependency is the `socket-dns` smoltcp feature).
  Boot proof: `net-4b PROBE OK (dns 10.0.2.2 → 10.0.2.2; localhost ip → 127.0.0.1;
  aaaa → empty)` (the deterministic synchronous paths) + `net-4b DNS live query OK`
  (the best-effort live resolver, logged) + 930/930 + boot OK + 0 EXTINCTION; SMP
  gate clean. The deterministic in-guest E2E of the **9P deferred-read plumbing** is
  owed to net-4d (a loopback DNS responder). The focused net-4 audit is net-4d.
- **net-4c (LANDED)**: `/net/ipifc/0` (the interface-config tree) + `/net/ndb` (the
  live dynamic database) + the native `ipconfig` tool. The DHCP lease is folded into
  an `IfConfig` snapshot at bring-up (the dynamic path) and surfaced read-only
  through `status`/`local`/`ndb`; an `ipconfig add IP MASK [GW]`/`remove` ctl write
  applies static config (the bridged/production path) onto both the live iface and
  the snapshot. The resolver socket (net-4b) is seeded from `ifc.dns`, so cs→dns and
  the ndb `dns=` line are one source of truth. The kernel is byte-unchanged (pure
  userspace; no new dependency). Boot proof: `net-4c ipifc E2E PASS` (the in-guest
  add/remove/status/ndb selftest on a throwaway Net) + `net-4c PROBE OK (ipifc status
  addr=10.0.2.15 dhcp gw=10.0.2.2; ndb ip=10.0.2.15 dns; local addr; ctl rejects
  malformed)` + 930/930 + boot OK + 0 EXTINCTION; SMP gate clean. The focused net-4
  audit is net-4d.
- **net-4d (LANDED)**: the focused net-4 audit + close — **0 P0 / 0 P1 / 1 P2 /
  3 P3, NOT dirty** (+ a precautionary round-2 on the deferred-reply fix, CLEAN).
  **F1 [P2]** closed the cs/dns held-`Rread` loss (a 2nd concurrent read / a re-write
  while deferred could drop the held tag, I-9) with two minimal guards (`query_read`
  empty-reply + `h_write` `E_PROTO` reject); **F2 [P3]** bumped `Content` 128→256;
  **F3 [P3]** documented the dns `queries` `Vec` high-water; **SA-3 [P3]** clarified
  the DHCP-renewal comment. Added three deterministic in-guest proofs:
  `proto_selftest` (the cs/dns/ndb/mask parser battery — the OWED-since-net-2d
  host-test coverage, delivered in-guest), `dns_defer_guard_selftest` (the F1
  regression), and `dns_loopback_e2e` (the OWED net-4b deferred-read E2E — a mock
  DNS responder on an isolated `127.0.0.1` stack). The kernel is byte-unchanged.
  Boot proof: `net-4d proto selftest PASS` + `net-4d dns defer-guard PASS` + `net-4d
  dns loopback E2E PASS` + 930/930 + boot OK + 0 EXTINCTION; SMP gate clean. **The
  net-4 arc is COMPLETE.**
- **net-6a (LANDED)**: blocking `data` reads (closes the net-5 audit F2 seam —
  `recv()` returning 0 on transient no-data). `data_recv_outcome` distinguishes
  `Data`/`WouldBlock`/`Eof` (smoltcp-grounded); `h_read(FK_DATA)` parks a
  `PendingRead` on `WouldBlock` (returns `Disp::Deferred`); `poll_data` delivers
  the held `Rread` on data/EOF — the net-3a/net-4b deferred-reply pattern applied
  to the data stream, with the same clunk/teardown/Tversion/Tflush cancel
  discipline + a `MAX_FIDS` bound. A pouch `recv()` → `read()` blocks
  automatically (no shim change). A self-audit caught + fixed a `count==0`-read
  park (a 0-length dequeue is `Ok(0)` even on a readable socket → would have
  blocked forever; guarded to return 0). Kernel byte-unchanged. Boot proof:
  `net-6a recv-blocking E2E PASS` (the in-guest WouldBlock/Data/Eof loopback
  selftest) + 930/930 + boot OK + 0 EXTINCTION; SMP gate clean. The synchronous
  `poll()`/`select()` readiness bridge is the separate net-6b (`dev9p.poll`).

## Known caveats / seams

- **The `listen` backlog is 1** (net-3a): between an accept and the swap re-arming
  the listener, a second inbound SYN finds the announce connection mid-establish
  (no listener) → RST. A proper backlog (several listening sockets per announce,
  the Plan 9 listen queue) is a v1.x refinement.
- **`check_ready` does not report a listener POLLIN-ready on a pending accept**
  (net-6b-3; #220): `slot_poll_readable` reports `can_recv()`, which is false for a
  TCP listener, so a `poll(listener, POLLIN)` (the pouch/`select()`-server
  multiplex pattern) never wakes when an inbound call lands — the blocking accept
  via `open(listen)` (net-3a) is the working path, and no v1.0 in-VM consumer polls
  a listener. Candidate fix: `check_ready` reports `accept_ready(listener_socket)`
  (server.rs `accept_ready`) for an `ANNOUNCED` TCP slot, proven via a loopback
  E2E (the `loopback_e2e` pattern); weighed at the net-6b-4 audit.
- **The inbound-accept E2E landed at net-3d** (was owed by net-3a): the
  deterministic in-guest inbound path is the netd loopback interface
  (`127.0.0.1/8`) — the `loopback_e2e` TCP leg announces + connects + resolves a
  synthetic deferred accept through the real `poll_accepts`/`accept_swap`. The lo
  stack is **isolated** (its own `SocketSet`) because a loopback iface sharing the
  live NIC socket set mis-routes (the NIC default route steals the `127.0.0.1`
  egress — verified in the smoltcp source).
- **The ICMP Echo-ident allocator is liveness-unchecked** (net-3c; net-3d F3): a
  collision needs 65536 `icmp_clone`s to wrap onto a live slot's ident
  (`MAX_SLOTS = 16`), and is benign if hit (a ping reply mis-delivered to the wrong
  `/net/icmp` conn — smoltcp's `bind(Ident)` does not reject a duplicate — never a
  panic/UAF). A liveness-checked allocator is the v1.x refinement, parallel to the
  ephemeral-port one.
- **A full-table deferred accept buffers the inbound call** (net-3d F4): when an
  accept lands but the slot table is full (`MAX_SLOTS = 16` live connections),
  `poll_accepts` leaves the pending and retries — the established call sits in the
  listener's socket until a slot frees. Liveness, not safety (the pending is
  bounded by `MAX_PENDING_ACCEPTS`; no leak/panic). Tracked with the #65
  resource-floor seam.
- **An oversize ICMP `data` payload fails closed** (net-3c; net-3d SA-2): a write
  whose payload + 8-byte header exceeds `ICMP_TX_BUF` returns 0 (`send_slice` Err),
  never a panic/OOB — a ping payload is normally tiny.
- **The announce/ctl fid keeps the listener alive** (net-3d SA-3): a completed
  accept rebinds the `listen` fid onto the accepted connection, so a client that
  clunked its announce/ctl fid (holding only the listen fid) frees the re-armed
  listener on accept. The Plan 9 idiom — keep the announce fd open to keep
  listening.
- **DNS is IPv4-A-record only** (net-4b): `/net/dns` + cs→dns resolve only A
  records (`proto-ipv4`); a `name aaaa` / IPv6 query yields the empty answer. AAAA
  + the full ndb-style attribute-line response are v1.x refinements.
- **The DHCP lease is snapshotted at bring-up, not re-applied on renewal** (net-4c;
  pre-existing since net-2a): `IfConfig` (and the iface addresses) are set once when
  the lease is `Configured`; the serve loop keeps the lease *alive* (smoltcp renews
  inside `iface.poll`) but does not re-handle a `Configured` event, so a renewal that
  *changed* the address/router/resolver would not update `/net/ipifc` + `/net/ndb`.
  Dormant for slirp (the lease is stable); the renewal-event re-application (update
  iface + `IfConfig` + the dns socket on every `Configured`) is a v1.x refinement.
  It is untestable in-guest with slirp (the lease never changes), so it stays a
  documented seam rather than unverified code.
- **`ipconfig add` does not stop the DHCP client** (net-4c): a static `add` over a
  DHCP-leased interface overrides the address but leaves the dhcpv4 socket running
  in the background (it would try to renew the prior lease). The DHCP-vs-static
  arbitration (mutually-exclusive per interface, the Plan 9 model) is a v1.x
  refinement; at v1.0 the boot path is DHCP and `add` is the bridged/production path,
  so the two never collide in the tested configuration.
- **`/net/ipifc` is single-interface; `bind ether`/`clone` are v1.x** (net-4c): the
  NIC is bound at probe, so there is exactly one interface (`0`) and no `ipifc/clone`
  (multi-NIC minting) at v1.0. `add`/`remove` cover the address axis; `bind ether
  <dev>` / `unbind`-the-device are rejected honestly (`unbind` is accepted as the
  `remove` alias). Multi-interface + `bind ether` land with the multi-NIC seam.
- **The 9P deferred-resolve E2E landed at net-4d** (was owed by net-4b): the
  synchronous `/net/dns` paths are asserted (joey) and the Net-level dns query
  lifecycle is proven live (best-effort); net-4d's `dns_loopback_e2e` adds the
  deterministic in-guest proof — a mock DNS responder bound to `127.0.0.1:53` on an
  isolated stack (the net-3d loopback analog) answers a fixed A query, so the shared
  `dns::Socket` resolves a name in-guest through the real `dns_query_start`→poll→
  `dns_poll` methods.
- **A bare `Tclunk` of a deferred cs/dns read drops the held tag** (net-4d F1
  facet 3): the two F1 guards close the *second-concurrent-read* + *re-write-while-
  deferred* facets, but a hostile client that clunks a fid mid-deferred-read without
  a prior `Tflush` abandons that read's tag (an I-9 loss for that client only).
  Mitigated on the trusted kernel mount (it abandons a blocked op via `Tflush` →
  `cancel_dns_flush`, never a bare clunk), and a bare clunk-with-outstanding-read is
  a client protocol violation — dispositioned consistently with the net-3 close
  (clunking a blocked `listen` fid likewise drops the held `Rlopen`).
- **The shared dns socket's `queries` Vec is a sticky high-water** (net-4d F3): the
  smoltcp `dns::Socket` grows its query-slot backing `Vec` when every slot is
  in-flight and never shrinks it; the high-water is bounded by total concurrent
  in-flight queries (≤ `MAX_CONNS × MAX_FIDS`) and the slots are *reused* (completed
  slots become free), so it is not a leak — just a non-trivial sticky footprint a
  v1.x per-driver memory cap (the #65 resource-floor analog) would bound.
- **`net!host!service` maps to tcp only** (net-4a): the Plan 9 `net!` (any-proto)
  prefix emits the tcp clone line at v1.0; the tcp+udp fan-out (cs returning both a
  tcp and a udp line for a service registered on both) is a v1.x refinement.
- **netd uses a compiled-in ndb; the live `/lib/ndb` read is a v1.x seam** (net-4a;
  NET-DESIGN §5/§18): a confined warden-bound leaf driver (I-34) cannot reach
  `/lib`, so netd compiles in `ndb/local` and serves cs from it (the
  config-at-construction idiom), with the byte-identical `/lib/ndb/local` baked for
  the user + the v1.x cs/dns daemon split (which *does* have a namespace) to read
  live. Only the static host/service half is compiled-in; the resolver/router are
  read live from the lease.
- **The netd pure-protocol host-test module is still owed** (since net-2d): net-4a
  adds two more deterministically host-testable pure parsers (the cs dial-string
  resolver + the ndb(6) parser). They are validated by the boot probe at v1.0
  (netd is no_std aarch64 with no host harness); the `cfg_attr(not(test), no_std)`
  host-test module that lets `cargo test` drive them with edge cases is carried to
  net-4d (its documented home across the net closed lists).
- **`readdir` lands at net-2c-1**: `/net` directories now list via `Treaddir`
  (the ninep readdir codec). A `Tread` on a directory still returns `EISDIR` (the
  9P2000.L convention — directory enumeration is `Treaddir`, and the kernel dev9p
  client issues exactly that).
- **udp/icmp `clone` landed** (net-3b/net-3c): `/net/udp` and `/net/icmp` now each
  have a live `clone` (a `udp::Socket` at net-3b, an `icmp::Socket` at net-3c)
  along the shared `proto` discriminator — the qid encoding's reserved `proto`
  field slotted them in without a re-encode.
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
  astronomically unlikely; a checked allocator is a v1.x refinement. net-2d (F3)
  made it peek-then-commit, so a rejected `connect` no longer burns a port; a
  post-wrap same-4-tuple collision → smoltcp rejects → `EINVAL` (fail-closed).
- **The connection table is bounded at `MAX_CONNS = 8`** and effectively
  single-session at v1.0 (joey's one `/net` mount drives one kernel dev9p-client
  session multiplexing all callers). Excess connections pend in the kernel; the
  only connectors are trusted (the kernel dev9p client + joey's mount). net-2d
  closed CLEAN on the connection-table + fid-machine SMP-safety (single-threaded
  → the lockless `Net` is sound by construction; no `thread_spawn`).
- **Cross-call `Treaddir` coherency** (net-2d F4): within one `Treaddir` the slot
  array is a single-threaded snapshot (no stale-slot dirent), but a slot freed
  *between* two paginated reads renumbers entries; no UAF / no stale resolution
  (the qid re-validates `slot_live`). Matches the kernel readdir-cookie tolerance;
  a stable per-connection iteration snapshot is a v1.x item.
- **The deterministic netd-protocol regressions are owed** (net-2d): the F1
  small-msize readdir-budget overrun, the F2 `P9_NOFID` reject, and the F3
  failed-connect port are architecturally unreachable in-VM (the only /net client
  is the trusted large-msize kernel dev9p mount; /net is 9P-mode → no native
  small-msize / malformed client; netd has no host-test harness — libthyla-rs is
  no_std + aarch64-asm). The owed vehicle (net-3+, when the server side grows more
  pure logic) is a netd pure-protocol host-test module on the `netdev`
  `cfg_attr(not(test), no_std)` pattern; the fixes meanwhile rest on data-path
  parity (`h_read`) + the ninep `build_rreaddir` length guard + fail-closed
  correctness.
- **smoltcp owns wire-protocol correctness** (its authors spec'd it); netd's
  state machine (net-2c) wraps *a* socket abstraction, so the recorded fallback
  (a Plan 9 IP-stack port, NET-DESIGN.md §14) would not change `/net`.

## The resident loopback interface (net-8a)

`netd` carries a real `lo` interface: a 127.0.0.1/8 stack so an in-guest client
dialing 127.x reaches an in-guest server **over the real `/net` 9P path**. It is
both a genuine OS feature (loopback networking) and the mechanism the net-8 owed
in-guest peer E2Es need (the NIC-only live stack could not give a deterministic
peer -- slirp does not loop a guest-to-own-IP packet, and a self-addressed packet
egresses the wire, not internally).

- **Why a SECOND isolated stack, not the NIC's**: a loopback socket cannot share
  the NIC's `Interface`+`SocketSet`. The NIC default route steals 127.x egress
  (`tcp::Socket::dispatch` has no iface-address-ownership gate -> `route()` falls
  to the default route, verified in smoltcp 0.12 -- the net-3d "isolation is
  load-bearing" finding), and `update_ip_addrs` clears the prior address. So the
  `lo` stack gets its OWN `Interface` + `Loopback` device + `SocketSet`, addressed
  127.0.0.1/8, polled alongside the NIC each `Net::poll` tick. The construction
  mirrors the audited `loopback_e2e`.

- **`Net.lo: Option<LoStack>`** -- the resident netd opts in (`enable_loopback`,
  called once at bring-up after `Net::new`); the isolated single-stack E2E
  selftests leave it `None` (they pass a loopback-configured iface as the PRIMARY
  stack, so a 127.x dial stays on `self.sockets` and the `set_*` routing is
  behavior-identical for them -- the audited E2E paths are untouched).

- **Per-slot routing**: a `Slot` gains a `lo: bool` flag (false at clone -> the
  NIC `SocketSet`). A dial/announce to an explicit 127.x address calls
  `ensure_lo_stack`, which DROPS the fresh NIC-set socket and mints an equivalent
  one on the lo `SocketSet` (the smoltcp `Socket` enum is not re-`add`able -- a
  never-connected socket carries no state worth moving), flipping `slot.lo`. A
  handle is set-specific (a typed `get` on the wrong set panics), so EVERY socket
  access routes through `set_ref(n)`/`set_mut(n)`, which pick the slot's stack.
  `accept_swap` mints M on the listener's stack (a loopback listener accepts
  loopback calls). `tcp_connect` uses the lo iface's `context()` for a migrated
  slot, so smoltcp selects 127.0.0.1 as the source (an on-link route).

- **`*`-announce does NOT span loopback at v1.0**: `announce *!port` (any local
  address) stays on the NIC; a loopback server binds 127.0.0.1 explicitly. The
  wildcard-spans-both refinement is a v1.x seam.

- **Proof**: `resident_lo_selftest` (boot-asserted, `net-8a resident lo E2E PASS`)
  builds the dual-stack with a NON-loopback primary (10.0.0.1/24, no 127 route),
  so a 127.x announce + connect + accept + data round-trip can ONLY succeed by
  migrating to the resident lo stack -- proving `ensure_lo_stack` + the dual-poll +
  a clean teardown (the connection table returns to baseline; no leak). The
  in-guest cross-Proc round-trip over the real `/net` path (a guest tool dialing
  127.x) is net-8b.

## References

- `docs/NET-DESIGN.md` (the #68 charter) — §2 (one netd, narrowed views), §13
  (the userspace virtio-net driver), §14 (smoltcp), §17 (sub-chunk phasing).
- `docs/reference/114-netdev.md` (`VirtioNetPci`), `119-warden.md` (the bind),
  `118-libdriver.md` (the `Driver` trait + the allowance).
- ARCH §10.1 (the network is 9P), §28 (no new net invariant — composes
  I-1/I-5/I-9/I-10/I-11/I-23/I-28).
