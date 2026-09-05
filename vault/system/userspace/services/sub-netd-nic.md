---
id: sub-netd-nic
type: sub
title: "netd/nic — the driver, the stack bring-up, and the serve loop"
parent: moc-userspace-netd
code: [usr/netd/src/main.rs, usr/netd/Cargo.toml]
audit: hard
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: [haz-driver-panic-dos]
abis: []
design: ["docs/NET-DESIGN.md", "docs/NET-THROUGHPUT.md"]
created: 2026-07-31
updated: 2026-08-04
---
## Purpose

The driver half of netd: the warden-bound `libdriver::Driver` that
claims the `virtio-pci:1` NIC, wraps it as a smoltcp `phy::Device`,
brings the link up by DHCP, runs the boot selftest battery, posts
`/srv/net`, and then runs the resident serve loop that multiplexes the
smoltcp stack with the `/net` 9P server for the life of the box. The 9P
protocol machine itself is [[sub-netd-server]]; this dossier owns
everything that decides WHEN it runs — the poll cadence, the delivery
passes, and the accept/service/teardown choreography.

## Contract

- **`NetD::probe(res)`** — fail-closed identity gate: the grant's
  `compatible` must parse to exactly `DeviceId::VirtioPci(1)`; any other
  bind is a manifest error refused BEFORE touching hardware
  (`Error::NoMatch`). Then `VirtioNetPci::open()` (claim + BAR map +
  VIRTIO init — [[sub-netdev]], audited at net-1).
- **`NetD::serve(self, res)`** — never returns on success. Ordering
  contract (each step gates the next): DHCP lease (bounded; FAIL exits
  non-zero → the warden's bounded restart, then soft give-up — the box
  boots without `/net`) → `Net::new` + `enable_loopback` → the selftest
  battery (asserted PASS/FAIL boot lines; a FAIL is currently non-fatal
  — [[seam-242-selftest-nonfatal]]) → `post_srv_net` (fail-closed: a
  post failure means netd never signals READY) → the `READY` line on
  stdout LAST (so READY also means "`/srv/net` is up" — joey's
  post-warden mount is guaranteed to find it) → the resident loop.
- **Diagnostics** go to the console (`t_putstr` via the `say!` macro): a
  warden-spawned driver's stderr is `/dev/null`. Exactly one `READY`
  line ever reaches stdout (the warden's readiness pipe).

## Mechanism

**The phy::Device (`NicDevice`).** The classic no-alias token pattern:
`NicRxToken` OWNS its received bytes (a `Vec` copied out of the RX ring
inside `receive`, no device borrow) while `NicTxToken` holds the single
`&mut VirtioNetPci`; `receive` can therefore hand back both tokens from
one `&mut self`. `capabilities()` reports `Medium::Ethernet` and
`max_transmission_unit = netdev::MAX_FRAME` — the largest L2 frame
`nic.send` accepts, so smoltcp never builds a frame the NIC would drop
and the derived IP MTU (`MAX_FRAME − 14`) is exact. `TxToken::consume`
clamps `len` to `MAX_FRAME`; `nic.send` is back-pressure-tolerant (it
self-drains the TX ring and drops the frame only if still full —
acceptable because smoltcp retransmits).

**DHCP bring-up.** A bounded sleep-poll loop (`POLL_MS`=10 ×
`MAX_POLLS`=500 ≈ 5 s fail-closed backstop; slirp answers in a few
polls): `iface.poll` + drain the `dhcpv4::Socket` for
`Event::Configured`, then install the address + default route and fold
the lease into the `IfConfig` snapshot (address/prefix/gateway/primary
resolver + `dynamic=true`) — the net-4c "lease into ipifc/ndb" path. The
interface seed (`config.random_seed`, the DHCP xid + TCP ISN source)
comes from the kernel CSPRNG.

**DHCP re-apply (the resident twin — #293-adjacent, landed with it).**
The DHCP socket stays in the set, and smoltcp drives the RENEW/REBIND
exchange inside `iface.poll` — but the resulting `Configured`/
`Deconfigured` EVENTS are only delivered via the socket's `poll()`, and
the bring-up apply ran once. `Net::poll_dhcp` (called every loop tick)
drains + re-applies them: a renewed lease rewrites the address/route/
snapshot and reseeds the resolver iff the DNS server changed
(`reseed_resolver` keeps the in-flight per-fid query table intact); a
lost lease (expiry/NAK) clears the address + route and marks the link
down until smoltcp re-DISCOVERs. On slirp the lease silently
auto-renews (no event — a no-op there), but a real DHCP server's expiry
would otherwise kill the link at T1. `dhcp_renew` (the `ipifc` `renew`
ctl verb → `ipconfig renew`) forces a fresh DISCOVER by resetting the
client; the next `poll_dhcp` re-applies the result.

**The resident serve loop** (the order is load-bearing — it IS the
no-lost-wakeup argument, see Concurrency):

1. `net.poll(device)` — TX flush + RX drain on BOTH stacks (NIC + the
   net-8a resident loopback).
2. `net.poll_dhcp()` — the lease re-apply above (delta logged).
3. `net.sweep_stale_connects()` — the #293 ARP-storm bound: any TCP slot
   still handshaking past its connect deadline is DROPPED (socket
   REMOVED, not aborted — [[sub-netd-server]] owns the mechanism).
4. `net.poll_accepts()` — completed deferred accepts, routed to the
   issuing `Conn` by handle; a vanished issuer's mint is discarded
   (`discard_accept`); a delivery write-failure tears that Conn down.
5. Per-Conn delivery passes, iterated BACKWARD so a teardown-remove
   cannot shift an unvisited index, with `||` short-circuit so no pass
   runs on a condemned Conn: `poll_dns` → `poll_data` → `poll_weftio` →
   `poll_ready` → `poll_connects` (each delivers held replies whose
   condition landed this tick; any write failure condemns the Conn).
6. The poll-delay decision (below), then `t_poll` over
   `[listener] + conns` with that timeout; `rc < 0` → a
   full-idle-period backoff sleep (a persistent poll error cannot
   busy-spin — the #108 discipline); `rc == 0` → loop (re-service the
   stack).
7. Accept at most one new 9P connection per tick (the listener re-fires
   next iteration; `MAX_CONNS` bounds the table).
8. Service readable Conns BACKWARD (`Conn::service` — reads + dispatches
   complete frames); a `false` return or `POLLHUP` tears the Conn down
   (`teardown` + `t_close` + remove).
9. `net.poll(device)` again — flush anything the dispatch just enqueued
   so a SYN/data egresses THIS tick.

**The poll-cadence policy (#221 trim / the in-code #291 constant).**
The `t_poll` timeout is smoltcp's `poll_delay` hint clamped into one of
two bands: with NO pending probe, `[IDLE_POLL_MIN_MS=50,
IDLE_POLL_MAX_MS=1000]` (idle netd wakes ~1/s; ≤50 ms RX latency under
load); with ANY pending probe (a deferred read/readiness/accept/
connect/weftio on any Conn), `[1, ACTIVE_POLL_MAX_MS=2]`. The active
band exists because the event that completes a held reply (an inbound
SYN, RX data, a DNS answer, a TCP window-update) arrives on the NIC —
not on any pollable fd — so only a timeout-driven `net.poll` can
observe it; on loopback the window-update that unblocks a parked bulk
sender is entirely `net.poll`-driven and smoltcp exposes no prompt
timer for it, so the old flat 50 ms floor capped bulk loopback at
~2.4 MiB/s (the #290 "readiness stall"); the 2 ms cap lifted it ~6× to
~14 MiB/s. The 1 ms floor forecloses a `Some(0)` hint becoming a 0 ms
`t_poll` busy-spin. Tradeoff: a long-lived idle blocked-reader
connection re-polls at up to ~500 Hz (bounded by `MAX_CONNS`; benign in
the v1.0 workload); the loopback-vs-NIC-aware cap is the recorded v1.x
refinement (only the lo stack needs the fast re-poll).

**The boot selftest battery** (all in [[sub-netd-server]]; wired +
asserted here — each prints a `netd: <name> PASS/FAIL` line): the
deterministic set is `loopback_e2e` (net-3d: TCP-accept + UDP + ICMP
over an isolated 127/8 stack), `resident_lo_selftest` (net-8a: the
dual-stack migrate/accept/data/no-leak proof — the primary stack is
10.0.0.1/24 so 127.x MUST migrate), `recv_blocking_e2e` (net-6a:
WouldBlock/Data/Eof), `echo_e2e` (net-6a-3: ≥2-concurrent accept +
bidirectional echo), `ready_e2e` (net-6b: POLLOUT/POLLIN/EOF-readable),
`ipifc_e2e` (net-4c), `connect_sweep_selftest` (#293 disposal),
`proto_selftest` + `dns_defer_guard_selftest` + `dns_loopback_e2e`
(net-4d). The host-coupled BEST-EFFORT probes (`udp_dns_probe`,
`icmp_ping_probe`, `dns_live_probe`) are logged, never asserted — slirp
forwards DNS to the host resolver and may not answer a guest echo, so a
round-trip is not a sound boot gate.

## Data structures

- `NetD { nic: VirtioNetPci }` — the driver.
- `NicDevice { nic }`, `NicRxToken { frame: Vec<u8> }`,
  `NicTxToken<'a> { nic: &'a mut VirtioNetPci }` — the phy tokens.
- The loop locals: `conns: Vec<server::Conn>` (the accepted 9P
  connections) + `pollfds: [TPollFd; 1 + MAX_CONNS]` (listener first).
- Constants: `VIRTIO_ID_NET=1`, `ETHERNET_HEADER=14`, `POLL_MS=10`,
  `MAX_POLLS=500`, `IDLE_POLL_MIN_MS=50`, `IDLE_POLL_MAX_MS=1000`,
  `ACTIVE_POLL_MAX_MS=2`.

## Concurrency

netd is SINGLE-THREADED (verified: no `thread_spawn` in `usr/netd/`).
The serve loop is the whole concurrency story:

- **The I-9-analog ordering.** `net.poll` (observe every edge delivered
  this tick) runs BEFORE the `poll_*` delivery passes and BEFORE any
  dispatch that could park a new pending — so between a handler's
  empty-observe (which parks a pending) and the next tick's re-observe,
  no edge can be lost: the loop is sequential, and the park is
  re-checked against fresh state every iteration. This is the
  register-then-observe of [[spec-net-poll]] realized by LOOP ORDER
  rather than by a lock — it holds only while netd stays
  single-threaded (the [[haz-driver-panic-dos]] sibling obligation; a
  concurrency lift must add real synchronization).
- **Backward iteration vs removal.** Both the delivery passes and the
  service pass iterate `conns` backward so `remove(i)` never shifts an
  unvisited element; the accept appends only, leaving the serviced
  `[0, nc)` range stable within a pass.
- **`device` aliasing.** `device` stays owned by the serve loop; only
  `net.poll(&mut device)` borrows it, and the resident lo stack owns
  its OWN `Loopback` device inside `Net` (disjoint fields — no alias).

## Invariants enforced

- **The probe identity gate** (I-34/I-5 composition): a grant carrying
  any identity other than `virtio-pci:1` is refused before hardware is
  touched; the claimer runs the stack (handles non-transferable).
- **READY-last**: every console diagnostic and the `/srv/net` post
  precede the single READY line, so the warden's "left running" implies
  the service is reachable.
- **Fail-closed post**: a `post_srv_net` failure (most likely a missing
  `MAY_POST_SERVICE`) means no READY — the warden logs gave-up and the
  box boots without `/net`, never with a half-up netd.
- **Bounded bring-up**: the DHCP loop cannot hang (wall-bounded); a
  no-lease boot exits non-zero (restart-then-soft-give-up).

## Error paths

- `probe`: mis-bind → `Error::NoMatch`; NIC open failure →
  `Error::Hardware` (both logged).
- `serve`: no DHCP lease after the bound → `Error::Hardware`;
  `post_srv_net` Err → `Error::Hardware` (no READY).
- Loop: `t_poll` rc<0 → backoff sleep + continue (never exit, never
  spin); a Conn service/delivery failure → that Conn torn down +
  closed + removed (never fatal to netd); selftest FAIL → logged line,
  loop still entered ([[seam-242-selftest-nonfatal]]).

## Performance

- Bulk loopback throughput ~14 MiB/s (M2 byte-copy 2370 →
  ~14300 KiB/s; MW weft 2436 → ~14000–15000 KiB/s) after the #221
  cadence trim — the serve-loop poll floor was ~95% of the stall, not
  the kernel pump or the weft mechanism (the #290 correction).
- Idle wakeup ~1/s (the 1000 ms idle clamp; post-lease the hint is the
  DHCP renew deadline). While any probe is pending: up to ~500 Hz (the
  2 ms cap) — the documented tradeoff.
- The NIC path's RX latency is bounded by the 50 ms idle floor under
  load; a pollable NIC-IRQ fd (a kernel ABI surface — `SYS_IRQ_WAIT`
  blocks, it is not pollable) is the deferred alternative.

## Prosecution

On any change, prosecute:

- The serve-loop ORDER: `net.poll` must precede the delivery passes and
  the park-capable dispatch (the I-9-analog rests on it); the
  post-dispatch flush `net.poll` must remain (else a SYN/data waits a
  full timeout).
- The poll-delay bands: the active band must keep a nonzero floor (a
  0 ms `t_poll` is a busy-spin); the `pending` predicate must cover
  EVERY pending-kind (a missed kind stalls its completion at the idle
  cadence — the pre-#221 shape).
- The backward-iteration discipline at every `conns.remove` site.
- probe's fail-closed identity gate; READY-last; post-before-READY.
- The DHCP re-apply's coherence: `poll_dhcp` must mutate the iface,
  the route, the snapshot, AND the resolver together (a partial apply
  desyncs the ipifc/ndb views from the live stack).
- Every selftest stays deterministic (no host coupling in an ASSERTED
  test — the no-host-load line the best-effort probes exist to
  respect).

## Seams

- [[seam-242-selftest-nonfatal]] — a selftest FAIL logs + proceeds;
  redundant with the net-echo boot gate today, fail-closed at its own
  layer is the v1.x posture (#242).
- The loopback-vs-NIC-aware active poll cap (v1.x; the #221/#291
  tradeoff note).
- A pollable NIC-IRQ fd for RX-driven wakeups (a kernel ABI surface;
  deferred — the timeout poll is correct, just not minimal).
- The `*`-announce does not span the loopback stack (recorded at
  [[sub-netd-server]]; surfaced here because the serve loop is where a
  wildcard listener's calls arrive).

## Caveats

- **A stale in-code comment**: the bring-up comment above `Net::new`
  ("the resident loop does NOT re-drain the dhcp Configured event …
  a DHCP renewal re-application is a v1.x seam") predates the #293-era
  `poll_dhcp` re-apply pass and is now wrong in both claims; the loop
  comment at the `poll_dhcp` call site is the accurate one. Fix on the
  next main-track netd touch.
- The commit that landed the cadence trim is titled `netd #221` while
  the in-code constant comment says `#291` — two task numbers for the
  same facet family (the kernel pump half stayed #221 and remains open:
  [[seam-221-idle-pump-wake]]).
- `resident_lo_selftest` mutates a THROWAWAY `Net`, never the live
  config (as do all the battery tests) — the live opt-in is the single
  `net.enable_loopback()` call in `serve`.
- **netd never stops its device, and is right not to — but not for the
  reason its transport documents.** [[sub-netdev]]'s `quiesce()` is
  documented on both transports as an obligation on any long-lived
  driver, since the warden's teardown is a forced group-terminate that
  skips `Drop` and would otherwise leave a live device writing into
  pages the reap frees. netd is exactly that driver and never calls it.
  It is safe because releasing the `KObj_PCI` handle clears the
  function's bus-master bit before releasing anything else — a kernel
  fence written to protect the BAR ranges, which stops device transfers
  as a side effect, and which lands before the ring pages are freed only
  because handles are released in ascending slot order and `open()`
  claims the function before allocating its three DMA regions. Nothing
  states that dependency at either end; see [[sub-netdev]]'s caveats
  before rearranging `probe`.

## Provenance

Landed across [[chg-2026-06-17-net2-netd-birth]] (probe/DHCP/persistent
lifecycle/serve loop), [[chg-2026-06-18-net6a-blocking-reads]] +
[[chg-2026-06-18-net4-cs-dns-ipifc]] + [[chg-2026-06-17-net3-server-side]]
(the battery accretion), [[chg-2026-06-19-net8-resident-lo]] (the
dual-stack + enable_loopback), [[chg-2026-06-21-netd-221-poll-cadence]]
(the active band), [[chg-2026-06-21-netd-293-connect-sweep]] (the sweep
pass + poll_dhcp + renew). Audited by [[adt-net2d-r1]] (the loop +
probes in scope), [[adt-net3d-r1]]/[[adt-net3d-r2]],
[[adt-net4d-r1]]/[[adt-net4d-r2]], [[adt-net8d-r1]] (the dual-poll +
battery). The gate-smp witness: every SMP-gate boot runs the full
battery + the DHCP bring-up.

## Tests

No host tests (netd is a `no_std` aarch64 bin — the named
[[seam-netd-host-tests]]). The in-guest roster, asserted as boot lines
every boot: `net-3d loopback E2E` · `net-8a resident lo E2E` ·
`net-6a recv-blocking E2E` · `net-6a-3 echo E2E` · `net-6b ready E2E` ·
`net-4c ipifc E2E` · `#293 connect-sweep selftest` · `net-4d proto
selftest` · `net-4d dns defer-guard` · `net-4d dns loopback E2E`; plus
the logged best-effort `net-3b` UDP/DNS, `net-3c` ICMP, `net-4b` live
DNS probes; plus joey's boot-fatal `/net` mount + per-chunk PROBE lines
(net-2c-1 … net-4a) and the net-echo/go-net over-the-mount E2Es on the
consumer side.
