# 139 — tapestryd: the compositor (Tapestry G-3, stage 0)

As-built reference for `usr/tapestryd` + its client library
(`usr/lib/libtapestry`), first client (`usr/tapestry-demo`), the warden
`gather` grant mode that binds it, and the kernel R2-F3 orphaned-weave
reaper. Landed on `gfx-1` (G-3a the compositor, G-3b the reaper). The
binding design is `docs/TAPESTRY.md` §18; the spec is
`specs/tapestry_present.tla` with the action↔site map in
`specs/SPEC-TO-CODE.md::tapestry_present.tla` (the G-2 kernel half + the
G-3 server half). ARCH §28 I-40 is COMPLETE as of this chunk (share half
G-2, present half here).

## Purpose

tapestryd is the GPU + keyboard owner and the `/dev/tapestry` 9P server —
the V1 vote realized: the compositor owns scanout from day 0, every pixel
reaches the display through its surface protocol, and Aurora (G-4) arrives
later as an ordinary client of the SAME protocol the demo exercises today.
Stage 0 serves `ctl` + `surface/` (one fullscreen surface class); the
`pane/` + `layout` compositor layer is G-6.

## The process shape

A warden-bound `lifecycle = persistent` libdriver driver (the netd
posture), gather-bound (see below) to BOTH graphics-path PCI functions:
`virtio-pci:16` (GPU) + `virtio-pci:18` (keyboard). `probe` brings both
devices up; `serve` posts `/srv/tapestry` (9P-mode), writes the one
`READY` line, and runs the single-threaded serve loop forever. joey
mounts the tree at `/dev/tapestry` post-pivot (the /net idiom: a missing
`/srv/tapestry` — a no-GPU environment — is soft; a mount error is
boot-fatal), probes the ctl geometry line, and spawns the resident
`tapestry-demo`.

Teardown is the crash contract (TAPESTRY §18.11 F4): at a tapestryd reap
the RW-7 proc-death quiesce resets its virtio devices (the scanout
blanks), `weft_share_release_owner` GCs its unclaimed shares, clients see
the session-dead error on every fid, and the kernel reaper (below)
force-reclaims any orphaned client weave mappings. A post-warden crash
has no restarter (the shared netd posture; the resident-warden lift is
v1.x) — the display stays blank until reboot.

## The device halves

- `gpu.rs` — the absorbed G-1 gpud machinery (PCI transport from the
  audited netdev VirtioNetPci; 2D command layer from the audited P4-L
  probe), generalized: per-surface `RESOURCE_CREATE_2D`, whole-weave
  `ATTACH_BACKING` (one attach serves all slots; per-present slot + rect
  selection rides the `TRANSFER_TO_HOST_2D` offset, rows advancing by the
  resource stride), and the retire pair `DETACH_BACKING` +
  `RESOURCE_UNREF`. **Every command is synchronous** (submit → doorbell →
  INTx wait → verify) — the property the I-40 stage-0 quiesce argument
  stands on (below). `GET_DISPLAY_INFO` adopts scanout 0's enabled rect
  (QEMU-virt default 1280x800; fail-soft to 1024x768; sanity-clamped).
  **The completion authority is the USED RING, never the ISR bit (#31)**:
  a wake proves only that *some* notification-ish event arrived (irqfwd
  collapses INTx edges; a level re-fire or config event latches stale
  pending events; under a live display backend — `-display cocoa` — a
  mis-timed wake is routine), so `submit_and_wait` re-reads `used.idx`
  behind `virtio_rmb` after every wake plus a bounded spin
  (`USED_SPIN_PER_WAKE`, the net for a mid-propagation store with no
  further interrupt coming) until the command retires, bounded by
  `MAX_STALE_WAKES_PER_SUBMIT`. The pre-#31 shape — break on the first
  `ISR_QUEUE` wake, read `used.idx` ONCE, hard-fail if behind — turned
  benign display-path timing into a permanent engine desync: `seq`
  diverged from the device's avail consumption, every later command
  re-published a consumed avail idx and read its own freshly-zeroed
  response buffer as `resp_type=0x0` (the cascade the user's cocoa
  session showed: 13 presents then scanout death). Any submit failure
  now LATCHES `Controlq.dead` — later submits fail fast + honestly
  instead of cascading garbage. A device that never interrupts at all
  still blocks in `irq.wait()` (the pre-existing all-virtio-drivers
  posture). The trigger needs a real display backend's thread topology:
  three escalating headless repro attempts (a 66/s screendump hammer at
  2x cocoa's refresh; a live VNC backend on the gpu0 console at real
  dirty-rect traffic) never fired it — `tools/interactive/ls-gfx-live.exp`
  is the standing live-display coverage leg; the cocoa acceptance test
  is a human at `THYLACINE_DISPLAY=cocoa`.
- `input.rs` — the virtio-keyboard-PCI eventq: the P4-K probe's audited
  populate/drain/recycle discipline over the same PCI transport,
  POLL-MODE (`VIRTQ_AVAIL_F_NO_INTERRUPT`; no IRQ claimed — the
  single-threaded loop cannot also block in `SYS_IRQ_WAIT`). Drained
  every serve-loop pass; worst-case input latency is one FRAME period.
  The keyboard goes PCI for the same measured G-1 co-page rule that moved
  the GPU: the MMIO input slot shares the one page-exclusive slot page
  whose lifetime belongs to stratumd. The MMIO keyboard stays wired for
  the one-shot P4-K kernel-test probe (the gpu0/gpu-mmio0 split).

## The 9P server (`server.rs`)

The ptyfs/netd native-server lineage: the flat `Conn`/`Fid` tables, the
frame extractor, `dispatch`, `send_all`, the `Disp::Deferred` held-reply
mechanism with all four cancel sites (teardown clear / clunk
retain-by-fid / Tversion reset / Tflush retain-by-tag), and the
`h_getattr` security trio (mode/uid/gid — the kernel X-search fails
closed without it). Single-threaded by construction (no locks; the fid
table and surface table have exactly one toucher) — a threading lift must
revisit both, like netd.

The tree (TAPESTRY §18.5, stage 0):

```
/dev/tapestry/
  ctl                  # read: "display W H" + surfaces/clock-rate/tick;
                       # write: clock-rate <hz> (1..240); test-mode -> G-6
  surface/
    new                # open mints a surface in THIS conn + rebinds the
                       #   fid onto its ctl (the netd clone idiom); the
                       #   ctl read yields "<id>"
    <id>/
      ctl              # write: create W H | destroy | title ...; resize -> G-6
      weave            # read: "W H stride slot_stride slots b8g8r8a8";
                       #   the Tweft/SYS_WEFT_MAP map-capability fid
      present          # write: the 32-byte tpresent (LOOM_OP_WRITE)
      event            # read: 24-byte tevent records; parks when empty
      geometry         # read: "0 0 W H 0 0" (placement-transparent)
```

Qids: bit-40 flag + `surface_n << 8` + a low-byte filekind (the
ptyfs/netd template). Every surface-qid consumer re-validates
`owner_conn` + the per-slot generation (the net-3d reuse discipline) —
**F2**: a walk from another session cannot resolve this session's
surfaces. Each client attaches its OWN session (open=connect on
`/srv/tapestry` mints a fresh conn per opener); Procs that deliberately
share a session (fd inheritance, or ops through the shared boot mount)
share its surfaces — the Plan 9 shared-mount semantic, the session being
the capability.

### The surface lifecycle (the I-40 present half)

`create W H` = the spec's `WeaveFirst`: `t_dma_create_weave` (the G-2
kernel-minted share-admissible subtype; F9-bounded to the display dims +
`MAX_SURFACES_PER_CONN`) + map + zero (no prior-occupant leak into a
client mapping) + resource + whole-weave backing. The weave carries
`WEAVE_SLOTS = 3` page-aligned slots (D1 triple buffering); geometry
reports `slot_stride` explicitly.

The Tweft mint (`weft_ensure`) is LAZY + idempotent per surface (the netd
`weft_ensure` precedent): the first Tweft on the weave fid registers via
`t_weft_share` (the spec's `armed`), later Twefts echo the stored id. The
reply's `ring_entries = 0` is the WEAVE-kind contract
`weft_claimed_kind` cross-checks kernel-side.

The present engine (`Conn::present`): parse the 32-byte tpresent
(version-pinned; HOLD + multi-rect honestly `E_OPNOTSUPP` until G-6),
validate slot + rect against the geometry (the untrusted-client
boundary; overflow-safe u64 sums), then TRANSFER + FLUSH synchronously;
the `Rwrite` becomes the client's CQE — the D1 recycle gate. **The
in-flight window opens and closes inside one dispatch, so the in-flight
present set is EMPTY at every retire decision point: the
`tapestry_present.tla` quiesce obligation (`ServerRelease`'s
`intransfer = 0`) holds BY CONSTRUCTION at stage 0.** A pipelined
controlq (G-6+) must implement a real drain before touching retire —
this sentence is the recorded obligation.

Scanout follows the spec's `Complete`: an ownerless scanout is taken at
present-COMPLETE (`scanout_take`; the F16 alignment — never before a
first frame has transferred). The retire (`Comp::retire` — ctl
`destroy`, the owning conn's teardown/Tversion, or the R2-F4 WEDGE)
runs the I-40 ordering: (1) quiesce (empty, above); (2)
`t_weft_unshare` BEFORE any backing free (R2-F5
registry-removal-before-page-free — a Tweft claim racing the retire
fails closed; on an already-claimed share the unshare is a harmless
miss); (3) scanout release; (4) DETACH_BACKING + RESOURCE_UNREF; (5)
unmap + close the weave DMA (serverRef drops; the pages free when the
client's mapping ref also drops, #847 — or at the reaper's
force-reclaim).

### Events (TAPESTRY §18.4)

24-byte tevent records; the compositor owns the keymap (`keymap.rs`, US
QWERTY: every KEY carries the raw evdev code AND the resolved rune +
the running modifier mask). FRAME is the synthesized display clock
(60 Hz default, `clock-rate` ctl; base virtio-gpu 2D has no guest
vblank), riding the serve loop's poll timeout. Key events route to the
focused (scanout-owning) surface. The bounded per-surface queue
(`EVENT_QUEUE_CAP = 128`) implements R2-F4: FRAME coalesces (at most
one queued; refreshed in place) and drops when full; a NON-droppable
event overflowing evicts one coalescible first, else the surface is
WEDGED and force-retired — never blocked, never a dropped control event
for a live client. Stage 0 signals a retired surface's stream-end via
the event-fid EOF (a parked read is delivered empty; a later read
returns empty); the queued-CLOSE record proper rides the pane layer
(G-6), where compositor-initiated closes need distinguishing.

## libtapestry + tapestry-demo

`usr/lib/libtapestry` (`tapestry::Surface`) is the aux-POC client model
cashed onto `libthyla_rs::loom`: private-session connect →
`surface/new` mint → `create` → the weave geometry read →
`t_weft_map` (the kernel issues Tweft on the client's own session — the
F2 property falls out: the mint arrives on the owning conn) →
`present`/`event` fids registered on one Loom ring with a staging
`RegisteredBuffer`. `present()` submits a LOOM_OP_WRITE and waits its
CQE (routing any event CQE reaped meanwhile), then rotates slots.
**Event reads are single-shot, re-armed after each drain** — a
multishot READ re-arms into the same registered slice, so an undrained
shot would be overwritten (droppable for FRAME, a lost KEY otherwise);
the multishot + provided-buffer-pool client lift is a recorded G-6 seam
(the kernel MULTISHOT mechanism itself is Loom-5-proven).

`tapestry-demo` draws the P4-L 4-quadrant pattern (the
`tools/screendump.sh -v` contract: quadrant centers exact) plus an
animated XOR plasma block at the display center — geometrically clear
of the four -v sample points — and presents FRAME-paced partial rects
forever. It IS the end-to-end proof of the whole G-2/G-3 path (and
discharges the G-2-audit F2 Tweft round-trip E2E residue): client →
private 9P session → tapestryd → weave share → cross-Proc zero-copy
mapping → Loom presents → virtio-gpu scanout.

## The gather grant (libdriver + warden)

`gather = all` in a manifest makes the warden COLLECT every matched node
into ONE grant/Proc instead of spawning per node: `resolve_gathered`
folds each extra node's wired INTIDs + bdf into the primary grant
(re-checking bind membership — the I-34 never-fabricated property holds
per axis), `BoundResources.pci_extra` + the `pcix` descriptor key carry
the extra functions across the spawn, and `to_allowance` pushes every
function into the one kernel allowance (`TAllowanceDesc.pci[8]` already
supported it). tapestryd's manifest binds
`["virtio-pci:16", "virtio-pci:18"]` with `dma = "pool: 32 MiB"` (a
1080p triple-buffered weave is ~24.9 MiB; the per-buffer allowance cap
must cover one weave). The driver claims each function by virtio device
id (`PciDev::claim`); the primary/extra ordering is immaterial. 86/86
libdriver host tests including the four gather legs.

## The kernel R2-F3 reaper (`kernel/weft.c`)

The `ServerDeath` leg's kernel half (TAPESTRY §18.12 R2-F3): a dead
compositor's claimed weave leaves the client mapping alive (#847 — no
UAF) but semantically dead; without reclaim a client that never closes
the fd pins the pixel pages UNCHARGED forever. A WEAVE binding registers
with the reaper at the SYS_WEFT_MAP CAS-win (lock-free; the #844 Spoor
ref makes register-vs-close structural) and unregisters at `dev9p_close`
BEFORE the close reads the binding. The kproc reaper kthread parks on an
empty registry (tickless-friendly), ticks at `WEFT_REAP_SWEEP_NS` (1 s)
while bindings exist, and force-reclaims a binding whose serving session
(`p9_attached_is_open` / `p9_client_is_open`) has been dead longer than
`WEFT_REAP_GRACE_NS` (2 s): the cross-Proc unmap runs under
`g_proc_table_lock` (pinning the target ALIVE — the devproc precedent)
+ the target's `vma_lock` with the G-2-audit-F1 identity guard
re-checked; the shared-in budget uncharges inside `burrow_unmap`; the
registration pin drops (deferred outside the reap lock) so the chunk
frees AT RECLAIM. The reaper NULLs `wb->burrow` under
`g_weft_reap_lock`; the close path's unregister under the same lock
guarantees neither side sees a half-reclaimed binding (the disarmed
clunk-unmap cannot match; release NULL-guards). Lock order:
`g_weft_reap_lock -> g_proc_table_lock(irqsave) -> vma_lock -> v->lock
-> buddy`, acyclic. A reclaimed client that touches the stale VA takes
`snare:segv` — it was warned; the live-client path is the F4 reconnect
(re-attach + re-create).

Regressions (`kernel/test/test_weft_share.c`, sweep-driven,
deterministic): `weft.reap_orphan_reclaimed` (stamp → in-grace hold →
reclaim: unmap + uncharge + chunk-freed-at-reclaim + no-op close after),
`weft.reap_live_session_untouched`, `weft.reap_close_unregisters` (the
close racing ahead: an unregistered binding is invisible to any sweep).

## The per-boot gate (evolved, never dropped)

`tools/test.sh` runs `tools/screendump.sh -v` post-banner (bounded
retry): the -v quadrant assert now proves the FULL compositor path, and
the NEW liveness leg (a second dump 0.6 s later must differ — the
plasma animates) proves the present loop live on every boot, including
every ci-smp-gate boot. FAIL diagnostics grep
`tapestryd:|tapestry-demo:|warden:`.

## Known caveats / seams

- **Synchronous presents**: a present blocks the serve loop for the GPU
  round-trip (µs-scale on QEMU). Fine at stage 0 (one client); the
  pipelined controlq + the real retire drain are the paired G-6 lift.
- **No restarter post-warden** (shared with netd): a tapestryd crash
  blanks the display until reboot; the resident-warden supervision is
  the v1.x seam. The kernel reaper + the client reconnect contract are
  in place for when it lands.
- **Event stream-end is EOF, not a CLOSE record** (stage 0; G-6).
- **`test-mode` / `TPRESENT_HOLD` / multi-rect / `resize`** are honest
  `E_OPNOTSUPP` (the §18.6 determinism mode + the reweave protocol land
  with G-6).
- **The weave-VA bump allocator never reuses** freed VAs (bounded by the
  caps × generations; a free-list is a v1.x lift).
- **Input latency ≤ one FRAME period** (poll-mode kbd; fine for stage 0).
- **Multishot event reads client-side** await a Loom provided-buffer
  pool (G-6); the server's deferred-read side is multishot-ready.
