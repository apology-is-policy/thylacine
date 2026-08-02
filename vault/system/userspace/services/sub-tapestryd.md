---
id: sub-tapestryd
type: sub
title: "tapestryd — the compositor: the weave lifecycle, the present engine, and the retire ordering"
parent: moc-userspace
code: [usr/tapestryd/src/server.rs, usr/tapestryd/src/gpu.rs, usr/tapestryd/src/pane.rs, usr/tapestryd/src/input.rs, usr/tapestryd/src/main.rs, usr/tapestryd/src/chords.rs, usr/tapestryd/src/keymap.rs]
audit: hard
guarded-by: [inv-i40, inv-i5, inv-i34, inv-i1]
validated-by: [spec-tapestry-present, prose, gate-smp]
locks: []
hazards: [haz-driver-panic-dos]
abis: []
design: ["docs/TAPESTRY.md", "docs/AURORA-CONFIG.md"]
created: 2026-08-02
updated: 2026-08-02
---
## Purpose

The compositor: it owns both graphics-path PCI functions, serves
`/dev/tapestry`, and holds the server half of [[inv-i40]]. Clients hand
it pixels through a shared page (a *weave*) and a 32-byte present
descriptor; it transfers, flushes, and either scans a client's resource
out directly or composes several into its own screen buffer.

The warden binds it to `virtio-pci:16` (GPU) **and** `virtio-pci:18`
(keyboard) through the manifest's `gather` mode — one grant, one Proc,
an I-34 allowance narrowed to exactly those functions. Both ride PCI
because the six populated virtio-mmio slots share one page whose
lifetime belongs to stratumd, so a second persistent MMIO claimant is
structurally impossible.

## Contract

**The tree** is `/dev/tapestry`: a global `ctl`, `surface/` (with `new`
as the mint file, then per-surface `ctl`/`weave`/`present`/`event`/
`geometry`), `layout`, and `pane/<id>/`. Surface qids carry bit 40, pane
qids bit 41 — the same template ptyfs and netd use.

**The client protocol**, in the order a client uses it: open
`surface/new` (the mint rebinds the fid onto the new surface's ctl),
write `create W H`, `Tweft` the weave fid for a share id, `SYS_WEFT_MAP`
it, then write 32-byte present descriptors and read the event stream.

**The present descriptor** is version-pinned, names a slot and a damage
rect, and carries additional rects inline when `rect_count ≥ 2` — the
payload already lives in the client's registered buffer, so a separate
slice reference would be redundant indirection.

**The isolation contract (F2):** a surface resolves **only** for the
connection that minted it. Every client attaches its own session — open=
connect on `/srv/tapestry` mints a fresh SrvConn and dev9p session per
opener — so connection *is* client session. Procs that deliberately
*share* a session (fd inheritance, or ops through the shared boot mount)
share its surfaces: the Plan 9 shared-mount semantic, capability-coherent
because the session **is** the capability.

**The pane and layout tree is deliberately connection-global.** F2 gates
surfaces; it never gates the shared tree, because the tree is the window
manager's, not any one client's.

## Mechanism

The weave lifecycle, which is the spec's state machine as built
([[spec-tapestry-present]]):

| spec | code | what happens |
|---|---|---|
| `WeaveFirst` | `create W H` → `alloc_weave` | `t_dma_create_weave` (the kernel-minted share-admissible subtype) + map + **zero** + `RESOURCE_CREATE_2D` + whole-weave `ATTACH_BACKING` |
| `Reweave` | `resize W H <serial>` → `resize_ack` | mint the new generation, displace the old |
| `Map` | kernel-side | the client's `SYS_WEFT_MAP` claims the share **consume-once**; tapestryd never observes it |
| `Submit`/`Complete` | `present` | validate → `TRANSFER_TO_HOST_2D` → `RESOURCE_FLUSH` → `Rwrite`, all inside one dispatch |
| `RetireDisplaced` | `present`'s tail | the first post-fence present drops the displaced generation |
| `Destroy`/`ServerRelease` | `retire` / `release_gen` | the five-step teardown below |

`armed` becomes real **lazily**, at the first `Tweft` (`weft_ensure` —
the netd precedent): the share registers once and the stored id echoes
thereafter. The spec's Map guard is indifferent to *when* registration
happens; it cares only that retire disarms it.

The weave is **zeroed at allocation** — a DMA chunk must never leak a
prior occupant's bytes into a client mapping. It is triple-buffered: one
weave carries three page-aligned slots, and a present names the slot.

**The retire ordering** — `retire(n)` and its per-generation twin
`release_gen`, five steps whose order is the whole point:

1. **Quiesce.** Empty by construction (see Concurrency).
2. **`SYS_WEFT_UNSHARE`** — registry removal *before* page free, so a
   claim racing the retire finds nothing and fails closed. This
   discharges `NoStaleMap`.
3. **Scanout release** — forced explicitly, because two reconcile arms
   can leave scanout still naming the surface.
4. **`DETACH_BACKING` + `RESOURCE_UNREF`** — the resource dies before
   its backing.
5. **Unmap + close the weave** — `serverRef` drops. The pages survive
   until the client's mapping ref drops too, or until the kernel reaper
   force-reclaims an orphaned mapping after the compositor dies.

`retire` also clears a `last_focus` naming the slot (a stale one would
suppress the focus-gained event for a *future* surface minted there) and
closes the hosting pane before reconciling.

**The generation fence.** `resize_ack`'s `Rwrite` completes only *after*
the new generation is allocated, and the connection's frame stream is
FIFO — so every present sent after reading that ack validates and blits
against the new geometry. No per-present serial tagging is needed; the
reply ordering carries it. The new generation is minted **first**, so an
allocation failure leaves the current one untouched and the offer
standing for a retry. The displaced generation drains *passively* —
never read again, its last content still displayed, so tearing-freedom
holds — and retires at the first post-fence present. At most one drains;
a second reweave returns `E_AGAIN`.

## Data structures

`Surface`: the current `Weave` (handle, VA, size, optional share id),
its resource id, `w`/`h`/`slot_stride`, an optional `old_weave` for the
draining generation, `owner_conn` + `gen`, a bounded event queue, and
the state (`Minted` → `Woven` → `Live`).

Bounds — F9: 8 surfaces globally, **4 per connection**, dimensions ≤ the
display (the weave is tapestryd's own DMA allocation, so the client's
page budget does not bound it), 8 connections, 32 fids, a 128-entry
per-surface event queue, 64 rects per present.

`Comp` holds the surfaces, the pane `Layout`, the `Gpu`, the scanout
mode (`Off` / `Direct(n)` / `Composed`), and the bump-allocated weave VA
window.

The `tevent` record is 24 bytes, version-pinned; pointer MOVE packs
surface-**relative** coordinates, never absolute screen ones.

## Concurrency

Single-threaded, like its siblings. The interesting property is not a
lock but a *shape*:

**Every present is handled synchronously** — validate, transfer, flush,
reply, all inside one 9P dispatch — because `gpu.rs` submits a
two-descriptor chain and waits. So the in-flight present set is
**provably empty at every retire decision point**, and the spec's
`ServerRelease` guard (`intransfer = 0`) holds by construction rather
than by a drain.

**This is the single most important thing to know before touching this
subsystem.** A pipelined controlq — the obvious performance lift — does
not make the guard *false*; it makes it **unimplemented**, silently,
with the model still green. Any move that way must land a real drain
first.

Deferred event reads use the ptyfs shape: park a `PendingRead`, deliver
from `poll_events` at the loop top, with four cancel sites (conn death,
clunk, `Tversion`, `Tflush`).

## Invariants enforced

[[inv-i40]] — the retire ordering above (`NoStaleMap`, `NoTornScanout`),
the reweave fence (`ReweaveOrdered`, `DisplayedBacked`), and the
completion-not-submit recycle gate (`RecycleGate`).

[[inv-i5]] and [[inv-i34]] — the gather grant confers exactly the
matched nodes' own bdfs and INTIDs; the allowance is never fabricated
per axis.

[[inv-i1]] — the F2 owner + generation gate at every surface-qid
consumer.

**The untrusted edge** is `present`, and it validates the version word,
`rect_count ≤ 64`, the **exact** payload length for the declared count,
the slot index, and **every rect before any pixel work**, in `u64`
arithmetic so `x + w > surface_w` cannot wrap. Validate-all-then-act is
the same discipline as ptyfs's ctl grammar, for the same reason.

**The global-ctl authority gate is default-deny**: `is_ungated_ctl` is a
denylist of exactly the determinism verbs, so every *future* global verb
is gated by construction. An allowlist would silently ungate a verb
added without touching the gate line. That inversion is the design and
must survive any edit.

## Error paths

Any `gpu` submit failure latches `dead` — fail fast, never the
zeroed-response cascade (below). Present errors surface as `EIO`;
resize-ack "not now" verdicts as `EAGAIN` (stale serial, or a prior
reweave still draining), which the client answers by draining events or
presenting a frame and re-acking.

`alloc_weave` rolls back fully on every failure path — the DMA close,
the unmap, and the resource unref each unwind what preceded them.

**The never-drop set (R2-F4).** `FRAME` coalesces globally — at most one
queued per surface, its tick refreshed in place (a back-of-queue-only
check let interleaved KEY/FRAME streams accumulate, the G-3 F3 fix).
Relative pointer motion coalesces by **summation**, not replacement,
because replacing deltas loses motion. `CONFIGURE` coalesces wholesale —
only the latest serial matters. On overflow with a non-droppable event
pending, one coalescible entry is evicted; if there is nothing
coalescible the client is dead or stalled and the surface **wedges**
(force-retire and close). It never blocks and never drops a control
event for a live client.

## Performance

Direct scanout is the zero-copy fullscreen path: the client's own
resource is scanned out and a present is transfer + flush on it.
Composed mode blits the damage into tapestryd's screen buffer at the
pane's content rect, which copies — so the screen resource references no
client weave, which is exactly what makes the post-fence retire safe.
Triple buffering keeps the client a frame ahead. Damage is per-rect;
`rect_count == 0` means full-surface.

The synchronous command engine is the cost side of the quiesce
construction: one IRQ wait per GPU command.

## Prosecution

- The **retire order** is the invariant: unshare before any backing
  free, resource before backing, scanout off before the resource dies. A
  reordering reopens `NoStaleMap` or `NoTornScanout`.
- **Every** surface-qid consumer must carry the owner + generation gate
  — walk, readdir, open, read, write, `Tweft`. A missed gate is a
  cross-client screen scrape.
- Present validation stays validate-all-before-any-pixel-work, in `u64`.
- The gather grant: every conferred bdf and INTID must be some *matched*
  node's own.
- The global-ctl gate stays a **denylist**.
- **The completion authority is the used ring, never the ISR bit**
  (#31). VIRTIO orders the device's `used.idx` write before its
  notification, but a wake proves only that *some* notification-ish
  event arrived: irqfwd collapses INTx edges, and a level re-fire or
  config event can latch a stale pending event — routine under a live
  display backend. The pre-fix shape (break on the first `ISR_QUEUE`
  wake, read `used.idx` once, fail if behind) turned that benign timing
  into a *permanent* engine desync: `seq` diverged from the device's
  avail consumption, and every later command re-published a consumed
  avail index and read its own zeroed response buffer as `resp_type =
  0`. The wait must never break on the ISR alone; the dead latch must
  stay one-way.

## Seams

- **`h_walk` accepts `P9_NOFID` as a newfid and silently rebinds a
  newfid that is already in use.** Its two siblings reject both — netd
  because they *are* its `net-4d` F2 fix, ptyfs by inheritance. Bounded
  and self-inflicted here (tapestryd's fids carry no refcount, so a
  clobbered binding leaks nothing), but it is the guard that carries
  ptyfs's `HupAtMostOnce` argument. Task #47.
- The weave-mapping VA window is bump-allocated and freed VAs are not
  reused — bounded by the surface caps per generation against a 47-bit
  space. A free list is a v1.x seam.
- A session peer can close or steal focus from another client's pane.
  The v1.0 trust boundary is the per-territory `/srv`: `/srv/tapestry`
  lives in the driver's territory and only the trusted boot chain
  connects. A per-client ACL is the Halcyon-era seam.
- A tapestryd crash resets its virtio devices — scanout blanks until a
  restart re-inits ([[haz-driver-panic-dos]]).

## Caveats

- **`h_version` replies `9P2000.L` to any proposal** and sets
  `version_done` unconditionally, where ptyfs replies `unknown` for an
  unsupported version. Inert — the only client proposes `9P2000.L` —
  and the same shape as the `h_walk` seam above: a template guard that
  did not come across.
- `parse_dec` accepts leading zeros, so `surface/007` and `surface/7`
  name the same surface; readdir emits only the canonical form. (ptyfs's
  twin rejects leading zeros for exactly this reason.)
- A pane qid carries only the low 24 bits of the pane id while the
  `layout` file parses the full `u32`. They agree for the first 2^24
  allocations; past that the paths diverge into a miss, never a
  cross-pane alias. Bits 8..40 are free below the pane flag, so widening
  is cheap if it ever matters.
- The hold flag (deferred device-visible push, for deterministic tests)
  is stripped to `E_OPNOTSUPP` in production builds. Pixel work still
  happens in-dispatch even when held, so tearing-freedom is unaffected
  by which build is running.

## Provenance

The Tapestry G-arc: the kernel weave share (G-2), the compositor and the
orphaned-weave reaper (G-3), the console renderer role (G-4), the pane
tree + resize generations + the interaction layer (G-6), and the display
config surface (cfg-3). Swept into the vault by
[[chg-2026-08-02-server-sweeps]], which mints [[inv-i40]] and
[[spec-tapestry-present]].

## Tests

In-guest: the per-boot pattern gate drives the full path with a liveness
double-dump; `ls-gfx` drives QMP-typed input through the whole loop
asserted on the serial tee; `ls-gfx-live` covers the VNC live-display
leg; `ls-gfx-panes` (22 legs) covers the G-6 pane tree; `ls-gfx-mode`
covers the display-mode verb and its authority gate. The `test-mode`
cargo feature strips the determinism surface to `E_OPNOTSUPP` for
production, and both variants compile.

## Referenced by

[[spec-tapestry-present]] · [[inv-i40]] · [[sub-ptyfs]] ·
[[sub-kernel-weft]] · [[moc-userspace]].
