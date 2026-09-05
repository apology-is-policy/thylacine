---
id: sub-netd-server
type: sub
title: "netd/server — the /net 9P server: connection table, deferred replies, dial surface"
parent: moc-userspace-netd
code: [usr/netd/src/server.rs, usr/netd/src/ndb.rs, usr/netd/ndb/local]
audit: hard
guarded-by: [inv-i9]
validated-by: [spec-net-poll, prose, gate-smp]
locks: []
hazards: [haz-driver-panic-dos]
abis: []
design: ["docs/NET-DESIGN.md", "docs/NET-THROUGHPUT.md"]
created: 2026-07-31
updated: 2026-08-16
---
## Purpose

The `/net` 9P2000.L server: the Plan 9 dial surface (`clone` → ctl
verbs → `data`) over a refcounted connection table that owns the
smoltcp sockets, plus the resolver front door (`/net/cs`, `/net/dns`,
the compiled-in ndb), interface config (`/net/ipifc`, `/net/ndb`),
observability (`stats`, `/net/summary`), the readiness file (`ready` —
the dev9p.poll bridge's server half), and the weft zero-copy drive
(`Tweft`/`Tweftio`). Its signature mechanism is the **deferred 9P
reply**: a single-threaded server must never block in a handler (it
must keep polling the NIC to receive the very event that would unblock
it — self-deadlock), so every blocking semantic is a HELD reply parked
in a pending table and delivered by a serve-loop pass when the stack
transition lands. [[sub-netd-nic]] owns the loop that drives those
passes; this dossier owns the machine.

## Contract

**The tree.** `/net/{tcp,udp,icmp}/` each carry `clone` + `stats` + the
live numeric connection dirs `N/` (`ctl`,`data`,`local`,`remote`,
`status`,`err`,`ready`, and `listen` on TCP only); `/net/{cs,dns}` are
request/response files (the write IS the query, the read drains the
answer); `/net/ipifc/0/{ctl,status,local}` + `/net/ndb` are the
interface-config views; `/net/summary` is the rollup. Modes: dirs 0555,
control/data files 0666, introspection 0444, all SYSTEM-owned — the
namespace is the firewall (I-1/I-28), not file modes; "anyone who can
name /net can dial" is intended. `FK_LISTEN` reports FILE_RW (**#239**:
the accept opens `listen` ORDWR because its fid is REBOUND onto the
accepted connection's rw ctl; an RO mode fails the kernel's A-3
perm_check before the Tlopen ever arrives — latent until the first
over-the-mount accept because the direct-method E2Es bypass perm_check).

**The clone idiom.** Opening `clone` MINTS connection N and rebinds the
opened fid onto `N/ctl` (the kernel dev9p client accepts the differing
Rlopen qid); reading ctl yields N. A connection is refcounted by the
fids naming its subtree; the LAST clunk frees N and removes its socket
— the only free path.

**ctl verbs** (`ctl_write`): `connect a.b.c.d!port` (ICMP: a bare IPv4;
TCP active-open + the #293 deadline arm; UDP ephemeral-bind + record
remote; ICMP record target) · `hangup` (TCP FIN / UDP unbind / ICMP
no-op) · `announce *!port | ip!port` (TCP only → LISTEN; records
`listen_ep` for re-arm + `local` for the Plan 9 read-local-after-listen
sequence — the go-net fix) · `nonblock [0|1]` (**#52**: an
empty-but-open `data` read answers `E_AGAIN` instead of parking — the
try-and-EAGAIN nonblocking primitive that never touches the readiness
bridge, chosen because the poll-before-read alternative churned the
shared session's tag pool to exhaustion) · anything else `E_OPNOTSUPP`,
honestly.

**The `ready` file** (net-6b): a read carries the requested poll mask
in the Tread OFFSET (POLLIN|POLLOUT requestable; POLLERR|POLLHUP always
reported) and returns the satisfied revents as a u32 LE WITHOUT
consuming socket data; zero revents DEFERS. `qid_of` marks it
`P9_QTPOLL` — the central qid builder (walk/lopen/getattr all route
through it) so the kernel's cached qid always carries the bit; only a
QTPOLL file is ever probed by the kernel bridge.

**The fd that gets polled is not the fd that gets read**, and the Linux
phenotype's poll translator is where that becomes visible. A socket's
descriptor names the `data` file — an *ordinary* remote file, which the
9P device reports as **always ready**: correct for a file, useless for a
socket. So the translation is entirely a descriptor substitution: open
the sibling `ready`, poll **that**, and put the caller's own descriptor
number back in the result.

**The readiness split is not an implementation detail of this server; it
is a fact every consumer has to model.** A layer that polls the obvious
descriptor gets a permanently-ready answer and a busy loop, and nothing
reports an error.

**And the caching decision differs between the two consumers for a reason
that does not transfer.** The ported-libc boundary caches the readiness
descriptor; the kernel translator opens it **per call**. Caching there
would place a descriptor the guest never asked for into **the guest's own
number space** — where the guest can close it, leaving a cached number
naming whatever was allocated next, and where it violates the
lowest-available-descriptor guarantee. In the ported libc that hazard is
absent, because there the readiness descriptor **is** a guest descriptor
its own library opened.

Same optimization, sound in one layer and unsound in the other, and the
discriminator is *who owns the number space* — not anything visible at
the call site.

**Weft** (`Tweft`/`Tweftio` on an opened `data` fid of a live slot,
else fail-closed E_INVAL): `Tweft` lazily allocates the per-flow ring
(`weft_ensure`: burrow-attach → `init_ring` → `SYS_WEFT_SHARE`;
idempotent — one ring per flow, the stored share_id re-returned; any
failure → E_NOMEM and the flow stays byte-copy, never fatal).
`Tweftio(off,len,dir)` re-bounds the window against netd's OWN mapping
(defense in depth — a memory bound, NOT a per-op capability re-check),
then TX reads the payload in place from the ring into `data_send`, and
RX recvs in place into the ring (`weft_recv_into_ring`) with the
WEFT_READY_RX readiness-seq bump on real delivery — deferring like a
byte read when empty.

## Mechanism

**qid encoding.** Static nodes occupy `[0, 19)`; a connection node is
`CONN_FLAG(bit 40) | proto<<32 | N<<8 | filekind` — one stable qid per
node, and a walk resolves a connection qid ONLY while its slot is live
and of the matching proto (`walk_child` filters numeric names per
protocol dir), so a stale or forged connection qid is unreachable.

**The slot pool.** One shared `slots: [Slot; MAX_SLOTS=16]` array
serves all three protocols; `Slot.proto` is the discriminator every
typed smoltcp access (`get::<tcp::Socket>` vs udp vs icmp) dispatches
on — a mismatched typed get PANICS in smoltcp, and a netd panic is a
whole-network DoS ([[haz-driver-panic-dos]]), so proto-dispatch
completeness is the standing memory-safety obligation (all typed-get
sites enumerated + matched at net-3d/net-4d). Mint sites: `tcp_clone` /
`udp_clone` (reserve the socket at clone — the ALLOCATED state) /
`icmp_clone` (bind the rotating Echo ident BEFORE `sockets.add`, so a
bind failure leaks nothing) / `accept_swap`. `clone_rollback` undoes a
mint whose Rlopen build failed (unref + uncount `opened`).

**The mint-generation guard (net-3d F1).** Every mint stamps
`Slot.gen` from a monotonic `next_gen()` (never 0 — 0 marks a free
slot); a deferred accept records its listener's gen, and `poll_accepts`
drops any pending whose slot is no longer the SAME live TCP slot
(proto arm: no typed-get panic on a cross-proto re-mint; gen arm: no
same-proto re-mint confusion). The listener KEEPS its gen across the
`accept_swap` re-arm — required so a sibling pending on the same N is
not falsely dropped. Four complementary layers close the F1 strand
class: the gen+proto guard, `cancel_accept_fid` on clunk, the
FK_LISTEN busy-mark (`opened=true` blocks walk-from/double-defer while
`complete_accept`'s rebind deliberately ignores `opened`), and the
teardown/Tversion `cancel_accepts_for_conn`.

**The five deferred-reply engines** (+ the cs/dns deferred read):

| pending | parked when | delivered by | reply |
|---|---|---|---|
| `PendingAccept` | `open(listen)` on an ANNOUNCED slot | `poll_accepts` → `accept_swap` mints M (taking N's established socket; N re-armed listening) → `complete_accept` rebinds the fid onto M/ctl | held Rlopen |
| `PendingRead` | `data` Tread, rx empty but open (blocking mode) | `poll_data` re-attempts the dequeue | held Rread (bytes; 0 on EOF) |
| `PendingReady` | `ready` Tread, mask unsatisfied | `poll_ready` re-runs `check_ready` | held Rread (revents u32) |
| `PendingConnect` | `data` Tlopen while the TCP handshake is in flight (**#257** — an immediate Rlopen let clients write into a SynSent socket; loopback's ~0 RTT masked it) | `poll_connects`: ESTABLISHED → Rlopen; RST → `E_CONNREFUSED`; deadline → abort + `E_TIMEDOUT` | held Rlopen / Rlerror |
| `PendingWeftRead` | `Tweftio(READ)`, rx empty but open | `poll_weftio` recvs in place into the ring | held Rweftio (count) |
| `Query.deferred` | cs/dns Tread while the DNS query is in flight | `poll_dns` | held Rread (the formatted answer) |

Each engine is bounded (`MAX_PENDING_ACCEPTS=16`; the others by
`MAX_FIDS=32` per Conn — the #65 floor) and carries the SAME
four-site cancel matrix: `fid_clunk` (per-fid retain), `teardown` +
`drop_all_fids`/Tversion (clear-all; accepts also
`cancel_accepts_for_conn`), and `h_flush(oldtag)` (per-tag retain +
`cancel_dns_flush`). Cancellation DROPS the held reply without
emitting a terminal frame — correct for Tflush (the Rflush retires the
kernel tag) but tag-stranding for the clunk/Tversion paths on a live
session ([[seam-56-netd-cancelled-tag]]).

**The cs/dns resolver.** `query_begin` (the write) resolves numeric →
static ndb (`ndb.rs`: the compiled-in ndb(6) subset — netd, a confined
I-34 leaf, cannot read `/lib/ndb/local`; the build bakes a
byte-identical copy there) → DNS (the shared `dns::Socket`
seeded from the lease's resolver; no resolver → the empty answer, fail
closed fast). An unresolvable/malformed dial fills the EMPTY answer —
the Plan 9 "no reachable path" signal, never an error frame. The
smoltcp query lifetime is the central hazard: `get_query_result` FREES
the slot on a result and PANICS on a freed slot, so the handle lives
in exactly one place (`Query.query`), is nulled the instant a result
is observed, and `dns_cancel` runs only on a still-pending handle —
the single-completion discipline (net-4d, verified against smoltcp
source). The net-4d F1 guards keep the single `deferred` slot sound
against concurrent multiplexed reads: a second read on a deferred fid
gets an empty Rread (the first keeps its answer) and a re-write while
deferred is rejected `E_PROTO`.

**The #293 connect bound.** `tcp_connect` arms
`connect_deadline_ms = now + 15 s`; `sweep_stale_connects` (every
tick) DROPS a slot still handshaking past it — `tcp_drop_stuck_connect`
REMOVES the socket from the set rather than `abort()` (an abort makes
smoltcp send a RST, which to an UNREACHABLE peer needs the same
unresolved neighbor, so the socket re-ARPs forever; smoltcp's single
GLOBAL ARP rate-limit then starves every other lookup once its cache
entry expires — the live bug: the M6 boot probe's abandoned dial killed
DNS at the 60 s neighbor expiry). The slot stays allocated while fids
ref it; `err` is set so `check_ready` reports POLLERR (completing a
stranded readiness probe); `slot_unref` later finds `socket == None` —
no double-remove. This bounds EVERY outbound connect, not only the
deferred-open path.

**The resident loopback routing (net-8a).** `Net.lo:
Option<LoStack>` is a second, isolated smoltcp stack (own `Loopback`
device + iface + set, 127.0.0.1/8) — isolation is load-bearing: a lo
address sharing the NIC set mis-routes (the NIC default route steals
127.x egress; proven from smoltcp source at net-3d). A 127.x
dial/announce migrates the FRESH slot socket onto it
(`ensure_lo_stack` drops the NIC-set socket and mints an equivalent —
a never-connected socket carries no state); every subsequent socket
touch routes through `set_ref`/`set_mut` on `Slot.lo` (a handle is
set-specific; a wrong-set typed get panics). A `*` announce stays
NIC-only (the wildcard-spans-both refinement is v1.x); a migrated
UDP/ICMP slot re-dialed to a non-loopback destination silently drops
at lo egress while reporting success ([[seam-240-lo-redial]]). In the
E2E single-stack config (`lo == None`, the primary stack IS loopback)
`ensure_lo_stack` no-ops true, keeping the audited paths byte-identical.

**Frame handling.** `Conn::service` reads once per readable event,
assembles COMPLETE frames (header-checked; size outside
`[P9_HDR_LEN, msize]` or a full buffered msize with no frame = framing
violation → close), and dispatches; `Disp::Deferred` (the `defer` flag
set by a holding handler, cleared by dispatch) emits nothing;
handler build errors re-clear `out_buf` and emit `Rlerror(E_PROTO)`.
Tversion resets ALL session state (fids unref'd, accepts cancelled,
queries cancelled, every pending cleared, msize renegotiated
`min(theirs, SRV_MSIZE=32 KiB)`). `h_attach` rejects `afid != NOFID`
(no auth on the trusted transport) and a NOFID fid (net-2d F2);
`h_walk` rejects walking an opened fid, a NOFID newfid, an in-use
newfid; a partial walk binds nothing; first-component miss → ENOENT.
`h_getattr` fills the mode/uid/gid trio (the kernel's A-3 X-search
fails closed on an unfilled trio → the whole /net walk would be
DENIED) + size (`render_summary().len()` for the summary — its body is
a per-read Vec render, since a multi-connection table exceeds the
256-byte `Content` cap). `h_readdir` uses the ordinal resume cookie
(first entry 1, never 0) under `rreaddir_budget` (count ∧ msize minus
the 11-byte Rreaddir overhead — the net-2d F1 parity with `h_read`).

## Data structures

- `Slot { used, refs, proto, socket: Option<SocketHandle>, local,
  remote, err, listen_ep, icmp_ident, gen, lo, weft:
  Option<WeftFlow>, connect_deadline_ms, nonblock }` × `MAX_SLOTS=16`.
- `WeftFlow { ring_va, ring_size, share_id }` — netd's own mapping of
  the 256 KiB / 64-entry per-flow ring; detached at the last unref
  (the ShareBoundedByFlow netd half).
- `Net { iface, sockets, base, next_local_port, slots, per-proto
  active/opened counters, next_icmp_ident, icmp_seq, mint_seq,
  pending: Vec<PendingAccept>, dns: Option<SocketHandle>, dhcp:
  Option<SocketHandle>, ifc: IfConfig, lo: Option<LoStack> }`.
- `Conn { handle, version_done, msize, fids: [Option<Fid>; MAX_FIDS=32],
  in_buf, out_buf, defer, queries: Vec<Query>, pending_reads,
  pending_ready, pending_connects, pending_weftio }`; `Fid { fid, path,
  opened }`.
- Buffers: `SRV_MSIZE=32 KiB` (Weft-0), TCP rx/tx 64 KiB (the window),
  UDP 8×4 KiB + metadata, ICMP 4×2 KiB, `DATA_CHUNK=32 KiB` recv
  scratch (heap — a 32 KiB stack array would overflow netd's 256 KiB
  stack), `Content[256]` (net-4d F2; `push` min-clamps — truncate,
  never OOB), `DecName[10]`.
- Ephemeral ports rotate 49152..=65535 (peek-then-commit — a rejected
  connect burns no port; liveness-unchecked, the documented v1.x
  refinement, like the ICMP ident rotation).

## Concurrency

Single-threaded; the global `Net` + both socket sets are lockless by
construction. Three obligations a concurrency lift must re-establish
(each carries an in-code INVARIANT note from Weft-7 F4):

- The raw-pointer ring slices in `h_weftio`/`weft_recv_into_ring`: the
  VALUE safety (a vanished/shrunk ring yields Eof/E_INVAL, never OOB)
  holds unconditionally, but the mapping LIVENESS against a concurrent
  `slot_unref → t_burrow_detach` rests on the serve loop's
  synchronousness — a lift needs a per-slot guard keeping the ring
  mapped across the raw access.
- The deferred-reply no-lost-wakeup rests on serve-loop ordering
  ([[sub-netd-nic]] Concurrency; [[inv-i9]]'s userspace analog).
- `fid_set` refs the NEW slot before unref'ing the OLD, so a
  within-connection rebind never transits refs==0 (single-threaded
  today, but the ordering is what makes it lift-safe).

## Invariants enforced

- **[[inv-i9]] (userspace analog).** No held reply is silently lost or
  double-delivered: every park is re-observed each tick after
  `net.poll`; the net-4d F1 guards close the concurrent-read/re-write
  clobber; `Disp::Deferred` guarantees at most one reply per tag
  (either the inline build or the one delivery site).
- **Connection identity (the I-10/I-11 analog).** The last clunk is
  the only free path; a freed N is unnameable (the walk filter) and a
  re-minted N cannot satisfy a stale pending (the gen guard); a held
  fid pins its slot live.
- **Proto-dispatch completeness** — no typed smoltcp access without a
  matching discriminator (the panic = network-DoS class).
- **Fail-closed parsing.** Every dial/announce/mask/ndb/ctl parser is
  bounded + checked; malformed → the empty answer or an honest
  Rlerror, never a silent accept, never a panic (`parse_mask` rejects
  >32 so smoltcp's `Ipv4Cidr` assert is unreachable).
- **The security trio** on every Rgetattr (A-3 X-search compatibility).

## Error paths

- Handlers: unknown fid `E_BADF`; opened-fid walk / unopened read /
  re-open `E_PROTO`; dir Tread `E_ISDIR`; non-dir Treaddir `E_NOTDIR`;
  full tables `E_NOMEM` (clone-mint ENFILE-class, fid table, pending
  tables); unannounced listen `E_INVAL`; malformed ctl/ipifc verbs
  `E_INVAL`; unsupported verbs/ops `E_OPNOTSUPP`/`E_NOSYS`; nonblock
  empty read `E_AGAIN` (#52); deferred-connect failure
  `E_CONNREFUSED`/`E_TIMEDOUT` (#257).
- cs/dns: unresolvable → the EMPTY answer (0-byte read), by design not
  an error.
- A held-reply delivery write failure condemns the whole Conn
  (teardown by the serve loop) — the session died.

## Performance

- One `data` Tread/Twrite moves up to a full 32 KiB msize payload
  (Weft-0 lifted 4→32 KiB payload + 64 KiB TCP window; the ring cap
  binds the ceiling at 32 KiB). The weft path moves large payloads
  in-place through the shared ring (zero 9P-body copy) — data-move ~2×
  fewer ops than byte-copy at equal per-op cost (#290 measurement).
- `/net/summary` re-renders per Tread (O(slots)); the per-protocol
  stats are O(1) counters.
- The ndb lookup is O(file) per query with no heap; DNS answers are
  bounded by smoltcp's ~10 s retransmit timeout (a held cs/dns read is
  bounded, never infinite).

## Prosecution

On any change, prosecute (the standing list, accreted across
net-2d/3d/4d/8d/weft-7):

- **Proto-dispatch completeness**: every `get::<T>`/`get_mut::<T>`
  reached only via a matching `slot_proto` dispatch or TCP-only by
  construction (enumerate; a miss is a remote panic → network DoS).
- **The pending lifecycle**: every engine cancelled at all FOUR sites
  (clunk/teardown/Tversion/Tflush); exactly one reply per held tag; no
  stray frame on the Deferred path; the gen+proto guard intact; the
  listener's gen preserved across re-arm.
- **Refcount balance**: `fid_set` ref-new-before-unref-old;
  `slot_unref` the only free; `free_orphan_mint`/`discard_accept` only
  on refs==0; `clone_rollback` uncounts; the accept moves the count
  N→M without transiting 0.
- **The DNS single-completion**: the handle nulled on every result
  arm; cancel only while pending; the F1 guards (second-read empty,
  re-write reject) intact.
- **The set routing**: every slot-keyed socket touch through
  `set_ref`/`set_mut`; direct `self.sockets` only for the DNS/DHCP
  sockets, the clone mints, and `ensure_lo_stack`'s old-socket removal.
- **Weft windows**: `[off, off+len)` re-bounded against netd's own
  geometry before any raw slice; the single-threadedness INVARIANT
  notes stay with the raw sites.
- **Fail-ordering of the parsers** + the getattr trio + the QTPOLL bit
  riding every qid path (`qid_of` stays the single builder).
- **#293**: the sweep must DROP (remove), never abort; the deadline
  disarms on every resolution arm.

## Seams

- [[seam-56-netd-cancelled-tag]] — cancelled parked reads drop their
  held tag without a terminal reply (clunk/Tversion paths; the Tflush
  path is correct). The fix is netd-side Rlerror-on-cancel.
- [[seam-220-netd-listener-poll]] — `check_ready`'s POLLIN cannot report
  accept-readiness on a LISTEN socket (readable = can_recv or
  terminal-recv states only), so poll(listener) never wakes on a
  pending call; the blocking `open(listen)` is the working path.
- [[seam-240-lo-redial]] — the one-way lo migration mis-routes a
  cross-stack UDP/ICMP re-dial (silent drop reported as success).
- [[seam-netd-host-tests]] — no host `cargo test`; the in-guest
  selftests are the parser/protocol rigor floor.
- Backlog-of-1: a second SYN arriving before the accept re-arms the
  listener is RST'd (documented; a real backlog is a net-8-era note).
- The wildcard announce not spanning loopback; the UDP
  receive-from-any (bind without remote); large-datagram truncation
  (`Err(Truncated)` → an empty read); per-proto ndb services — all
  recorded v1.x refinements.
- Weft #289 (a transient `SYS_WEFT_MAP` failure pins a flow byte-copy
  — the consumed-id idempotence; the kernel-side `SYS_WEFT_UNSHARE` GC
  is the fix) — carried here until the weft sweep mints its own note.

## Caveats

- **Cross-session liveness** (net-2d SF4): any session that can name
  `/net/<proto>/N` holds it live — the Plan 9 shared-namespace model
  bounded by MAX_SLOTS and the I-1 firewall.
- **The announce-fd idiom** (net-3d SA-3): clunking the announce/ctl
  fid keeping only `listen` means a completed accept (which moves the
  listen fid onto M) unrefs N to 0 — the re-armed listener frees. The
  announce fd must stay open to keep listening; semantically the
  Plan 9 contract.
- **Treaddir cross-call coherency** (net-2d F4): a slot freed between
  paginated reads renumbers the ordinals; no stale resolution (the
  walk filter re-validates), a dir-read atomicity snapshot is v1.x.
- **The dns `queries` Vec** is a bounded reused high-water
  (≤ MAX_CONNS×MAX_FIDS), not a leak (net-4d F3).
- **ICMP data is bounded by the socket tx buffer** (oversize →
  send_slice Err → 0, fail-closed); a non-EchoReply consumed while
  waiting reads as WouldBlock (keep waiting).
- An ICMP ident wrap collision (65536 clones/boot) mis-delivers a ping
  reply, never panics (net-3d F3).
- The `h_lopen` open FLAGS are ignored (`let _ = a.flags`) — no
  O_TRUNC/append semantics exist on this tree.

## Provenance

The net-2b-2→2c skeleton-to-live arc
([[chg-2026-06-17-net2-netd-birth]]), the server side + UDP/ICMP
([[chg-2026-06-17-net3-server-side]]), the resolver + config surface
([[chg-2026-06-18-net4-cs-dns-ipifc]]), blocking reads
([[chg-2026-06-18-net6a-blocking-reads]]), the ready file (netd half of
[[chg-2026-06-18-net6b-poll-bridge]]), the summary
([[chg-2026-06-19-net7b-summary]]), the resident lo + #239 + #257
([[chg-2026-06-19-net8-resident-lo]]), the weft drive
([[chg-2026-06-20-weft6b-netd-drive]], [[chg-2026-06-20-weft0-payload-lift]],
[[chg-2026-06-20-weft6c2-readiness-edge]]), #293
([[chg-2026-06-21-netd-293-connect-sweep]]), the go-net local fix
([[chg-2026-06-23-gonet3c-net-over-net]]), nonblock
([[chg-2026-07-22-52-nonblock]]). Prosecuted by [[adt-net2d-r1]],
[[adt-net3d-r1]]→[[adt-net3d-r2]] (the F1 strand class),
[[adt-net4d-r1]]→[[adt-net4d-r2]] (the deferred-overwrite class),
[[adt-net8d-r1]] (the dual-stack routing), [[adt-weft7-r1]] (the ring
sites), [[adt-294-r1]] (the ready-fd clunk verification). The
do-not-re-report preamble: [[view-closed-sub-netd-server]].
[[chg-2026-08-16-seven-small-surfaces]] adds the phenotype poll
translator's readiness substitution and its per-call open.

## Tests

In-guest only ([[seam-netd-host-tests]]): the [[sub-netd-nic]] battery
drives THIS module's real methods (`loopback_e2e`/`echo_e2e`/
`lo_establish_pair` drive clone/announce/connect/`poll_accepts`/
`accept_swap`; `recv_blocking_e2e`/`ready_e2e` drive
`data_recv_outcome`/`check_ready`; `dns_defer_guard_selftest` is the
net-4d F1 regression — fails on pre-fix code by construction;
`dns_loopback_e2e` drives the real resolver methods against a mock :53
responder; `proto_selftest` is the parser battery incl. ndb;
`connect_sweep_selftest` the #293 disposal; `resident_lo_selftest` the
migration). Consumer-side: joey's per-chunk PROBE lines, the net-echo
over-the-mount TCP/TLS/weft E2Es, the go-net Stage-3c listen/dial
round-trip (the regression for the announce-`local` fix).
