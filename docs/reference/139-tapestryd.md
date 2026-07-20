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
  `SUBMIT_DEADLINE_MS` (500 ms of WALL CLOCK, anchored lazily at the
  first stale wake — the G-5 F1 close: the first-cut bound was a wake
  COUNT of 16, which a window-resize config-IRQ train during one
  slow-but-healthy present could exhaust, a false dead-latch whose
  consequence is the permanent console loss #31 exists to prevent; the
  deadline trips only on event-ful non-progress and carries 5+ orders
  of margin over a device's µs-scale retire). The spin-break path
  read-to-clears the ISR once more after the loop (G-5 F2: a
  completion during the spin re-asserts INTx after the wake-path read,
  and leaving that level pending cost the NEXT submit a systematic
  stale wake). The pre-#31 shape — break on the first
  `ISR_QUEUE` wake, read `used.idx` ONCE, hard-fail if behind — turned
  benign display-path timing into a permanent engine desync: `seq`
  diverged from the device's avail consumption, every later command
  re-published a consumed avail idx and read its own freshly-zeroed
  response buffer as `resp_type=0x0` (the cascade the user's cocoa
  session showed: 13 presents then scanout death). Any submit failure
  now LATCHES `Controlq.dead` — later submits fail fast + honestly
  instead of cascading garbage. A device that never interrupts at all
  still blocks in `irq.wait()` (the pre-existing all-virtio-drivers
  posture). **Quiesce provability on the dead paths (G-5 F3)**: the
  deadline-miss and `irq.wait()`-error arms return WITHOUT confirming
  the device finished, so the strict "in-flight set provably empty"
  basis of the I-40 stage-0 quiesce narrows there to
  timeout-≫-device-latency (500 ms vs µs) composed with the #847 dual
  refcount (the client's mapping keeps the backing pages alive until
  its own teardown, seconds later) — no reachable device-side UAF, but
  the honest residual is recorded here; the real controlq drain/reset
  is the standing G-6 obligation. The retire ordering is UNAFFECTED:
  `t_weft_unshare` and the backing free are kernel syscalls, not
  controlq commands, so a dead controlq can never cause a
  free-without-unshare (R2-F5 holds unconditionally). The trigger
  needs a real display backend's thread topology:
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
(version-pinned; HOLD + multi-rect honestly `E_OPNOTSUPP` until G-6c),
validate slot + rect against the geometry (the untrusted-client
boundary; overflow-safe u64 sums), then move the pixels synchronously —
by scanout mode since G-6a (see "The compositor stage 1" below): the
Direct arm TRANSFERs + FLUSHes the client's own resource (the stage-0
path byte-identical), the Composed arm blits into the screen buffer and
TRANSFERs + FLUSHes that; the `Rwrite` becomes the client's CQE — the
D1 recycle gate either way. **The in-flight window opens and closes
inside one dispatch, so the in-flight present set is EMPTY at every
retire decision point: the `tapestry_present.tla` quiesce obligation
(`ServerRelease`'s `intransfer = 0`) holds BY CONSTRUCTION.** A
pipelined controlq must implement a real drain before touching retire —
this sentence is the recorded obligation (G-6a deliberately KEPT the
synchronous engine, so it carries forward).

Scanout follows the spec's `Complete` uniformly: every switch ONTO a
client resource rides a present-COMPLETE (`pending_direct`; the F16
alignment — never before a frame has transferred). The retire
(`Comp::retire` — ctl `destroy`, the owning conn's teardown/Tversion,
or the R2-F4 WEDGE) runs the I-40 ordering: (0) the hosting leaf closes
(G-6a — the layout no longer names the surface); (1) quiesce (empty,
above); (2) `t_weft_unshare` BEFORE any backing free (R2-F5
registry-removal-before-page-free — a Tweft claim racing the retire
fails closed; on an already-claimed share the unshare is a harmless
miss); (3) scanout release — `reconcile()` moves scanout off the dying
surface, and the two arms that legally leave it referenced (a deferred
Direct(survivor) + a degraded screen alloc) are forced away explicitly;
(4) DETACH_BACKING + RESOURCE_UNREF; (5) unmap + close the weave DMA
(serverRef drops; the pages free when the client's mapping ref also
drops, #847 — or at the reaper's force-reclaim).

### Events (TAPESTRY §18.4)

24-byte tevent records; the compositor owns the keymap (`keymap.rs`, US
QWERTY: every KEY carries the raw evdev code AND the resolved rune +
the running modifier mask). FRAME is the synthesized display clock
(60 Hz default, `clock-rate` ctl; base virtio-gpu 2D has no guest
vblank), riding the serve loop's poll timeout — since G-6a it is
delivered to VISIBLE hosted surfaces only (a hidden tab's paced client
naturally suspends). Key events route to the FOCUSED leaf's surface
(G-6a). CONFIGURE `{serial in code, W<<16|H in value}` is emitted
same-size as the REDRAW request OR different-size as the resize offer
(G-6b) and coalesces-by-replacement (only the latest serial matters,
§18.3). TEV_CLOSE is the compositor-initiated exit request delivered on
a pane close (G-6b), distinct from a retired surface's stream-end EOF.
The
bounded per-surface queue (`EVENT_QUEUE_CAP = 128`) implements R2-F4:
FRAME coalesces (at most one queued; refreshed in place) and drops when
full; a NON-droppable event overflowing evicts one coalescible first,
else the surface is WEDGED and force-retired — never blocked, never a
dropped control event for a live client. A retired surface's
stream-end is the event-fid EOF (a parked read is delivered empty; a
later read returns empty); the queued-CLOSE record proper rides the
pane-close notification (G-6b), where compositor-initiated closes need
distinguishing.

## The compositor stage 1 (G-6a): the pane tree + multi-surface composition

`pane.rs` is the i3 container model — ONE structural primitive (a
container whose mode is `splith | splitv | tabbed | stacked`),
nestable, leaves hosting surfaces; the screen is the root container.
Policy encoded there: a split FLATTENS into a same-mode parent (sibling
insert) and NESTS under a different-mode one; a split focuses the NEW
empty leaf (the auto-host target); closing collapses single-child
containers; the root is never removed (an empty root leaf is the blank
screen); pane PUBLIC ids are monotonic and never reused (a stale pane
fid resolves to nothing — the net-3d discipline structurally);
geometry divides equally (remainder to the last child) with a 1px
content inset per leaf iff more than one leaf is visible (the
single-fullscreen root leaf keeps the stage-0 borderless look).

**Hosting**: a surface is hosted at CREATE (`Comp::create` →
`layout.host`) — the focused empty leaf takes it, else the focused
leaf splits (orientation by content aspect). Hosting is once per
surface lifetime; `close` deliberately STRANDS a live surface invisible
(the pane was closed; the client is asked to exit via the queued
TEV_CLOSE — G-6b, below; a stranded client's conn teardown retires it).
A pane-table-exhausted surface stays unhosted (presents complete
without pixels).

**Scanout modes** (`Scanout`): `Boot` (untouched since startup — the
kernel test pattern stays until first content), `Off`, `Direct(n)`,
`Composed`. `reconcile()` — run after every layout/hosting mutation —
recomputes geometry and drives the mode machine: exactly one visible
leaf hosting a display-sized surface wants `Direct(n)` (the zero-copy
fullscreen case — aurora's boot is byte-identical to stage 0); anything
else visible wants `Composed`; nothing wants `Off`. Every entry to
Direct is DEFERRED to the target's next present-COMPLETE
(`pending_direct`, the F16 rule uniformly), and that present's transfer
expands to the full surface when the client resource is stale
(`res_stale` — composed-era presents never touched it).

**The screen buffer** (`Screen`, `SCREEN_RES = 0x40`): a WEAVE-subtype
DMA chunk — the G-2 type discipline puts every `RESOURCE_ATTACH_BACKING`
scanout backing in that class (plain `SYS_DMA_CREATE` is the
virtqueue/command class, capped at `KOBJ_DMA_MAX_SIZE` = 1 MiB; the
first G-6a run measured exactly that reject). Share-admissible by TYPE
but never REGISTERED (`t_weft_share` is never called on it), so no
share_id exists for a client to claim. Allocated lazily at the first
Composed entry; held for the process lifetime.

**The tearing-freedom invariant**: client weave bytes are read ONLY
inside the present dispatch, for the slot the client just presented
(`blit_composed` — clipped both to the damage and the pane content, so
an oversized or CONFIGURE-deaf client shows its top-left crop).
`paint_chrome` never touches client memory; a structural repaint blanks
pane content and panes heal by redraw. Chrome repaints are split by a
visible-geometry signature (`calc_geom_sig`): structural changes
repaint fully; a pure focus move redraws only the 1px frames
(`paint_borders` — idle clients keep their pixels; the focus ring is
`FOCUS_COLOR`).

**The CONFIGURE redraw wire** (the §18.3 emission half, pulled forward
by chunk-completeness): aurora is an ACCUMULATOR client (row-damage
renders — each weave slot is a patchwork; only the resource/screen
accumulates), so after a structural repaint its static rows would never
heal, and a direct-switch full-slot transfer would push patchwork (the
first G-6a runs measured both). Same-size CONFIGURE = the full-repaint
request: emitted to every visible hosted surface at a structural
composed repaint, and to the target at pending-direct ENTRY
(edge-triggered). Aurora's response (`TEV_CONFIGURE`, any size): mark
every row dirty → the next pass repaints the whole grid; a size CHANGE
aurora does NOT ack (its cell grid is bound to the console history) — it
keeps its grid and the compositor crops the top-left (the ignore/crop
posture). The resize ack (`resize W H serial`) is live at G-6b (the
generation fence — see "The resize protocol" below).

**The 9P layer**: the root gains `layout` (read: the tree text —
`epoch`/`focused` header + one pane per line with mode, surface, and
content rect; write: `split <id> h|v`, `close <id>`, `focus <id>`,
`mode <id> <m>`) and `pane/<id>/{ctl,mode,role,tag,surface,geometry}`
(ctl = the same verbs with the fid's pane implicit; mode/role/tag are
direct field reads/writes; geometry is the content rect). Pane qids
carry bit 41 (`PANE_FLAG`); ids never reused. Pane files are GLOBAL
(not F2-gated): the layout is compositor-global state on the same trust
plane as the stage-0 global ctl — the environment-role mutation gate is
the recorded Halcyon-era seam, and the D5 observability caveat (a
client that reads the layout can find its own placement) is part of the
same record.

**The gate**: `tools/interactive/ls-gfx-panes.exp` +
`/bin/tapestry-battery` — one process hosting both synthetic clients
(private sessions) and the layout driver; asserts the views agree
(the battery's in-guest structure asserts [layout text vs the pane
geometry files, disjoint nonzero rects], the resize protocol [G-6b], the
host-side pixel asserts at the printed pane centers via `screendump.sh
-P` + `tools/ppm-sample.py` [solid red/blue, exact], the focus legs
[QMP-typed keys arrive on the focused surface only — leg A via the
layout file, leg B via B's own pane ctl], and the pane-close leg [G-6b])
plus the collapse coda (the battery exits, panes collapse, the console
returns to fullscreen direct scanout, `-c` passes again).

## The resize protocol + pane close (G-6b): weave generations

**Weave generations.** A surface's `weave`/`resource_id` name its
CURRENT generation; a resize acks onto a NEW one. GPU resource ids are
per-generation (`Comp::next_res_id`, minted above `SCREEN_RES` — a fresh
resource never aliases the old one, closing the §317 stale-content
class), so `Comp::alloc_weave` (the shared body of `create` and
`resize_ack`) allocates a full generation: DMA + map + zero + a fresh 2D
resource with the whole weave attached. `Comp::release_gen` tears one
down in the R2-F5 order (unshare → detach → unref → burrow_detach →
close).

**The offer.** `Comp::emit_configure_to(n, w, h)` records `offered =
(serial, w, h)` and pushes the CONFIGURE event. A SAME-size offer is the
redraw request (above); a DIFFERENT-size one is the resize offer. Only
the latest is ackable — a newer emission overwrites `offered` and the
queued (unread) CONFIGURE is coalesced by replacement. The structural
composed repaint fans `emit_configure_to(n, content.w, content.h)` to
every visible hosted surface, so a pane whose content differs from its
surface size is offered its exact fit.

**The generation fence.** `resize W H <serial>` on the surface ctl →
`Comp::resize_ack`. The ack is the fence: `alloc_weave` mints the new
generation FIRST (a failure leaves the current one untouched, the offer
standing for a retry), the surface then swaps
`weave`/`resource_id`/`w`/`h`/`slot_stride` and marks `res_stale` (the
fresh resource has no content), and ONLY THEN does `h_write` send the
`Rwrite` — reply-after-alloc (the R2-F5 precedent). The conn stream is
FIFO, so every present the client sends after reading that Rwrite
validates + blits against the new geometry. Verdicts that do NOT consume
the offer: a stale serial (`< cfg_serial`) → `E_AGAIN` (drain events,
ack the newer offer); an unknown/mismatched echo → `E_INVAL`; a prior
reweave still draining (`old_weave.is_some()`) → `E_AGAIN` (the <=2-gens
bound — present a frame, then re-ack).

**Draining the displaced generation.** The old (weave, resource) moves
to `old_weave` and drains PASSIVELY — its last content stays displayed,
is never read again (tearing-freedom holds), and retires at the FIRST
post-fence present (`Comp::present`'s tail: the display now shows g2
content — a composed blit COPIES into the screen buffer, a direct arm
targets g2's resource — so quiesce holds by construction and
`release_gen` drops g1's server refs; the spec's `RetireDisplaced` +
`ServerRelease`). A surface retire with g1 still draining releases it in
`Comp::retire`'s tail. The client's own g1 mapping drains via its old
weave-fid clunk (`libtapestry::Surface::reweave` maps the new fid BEFORE
clunking the old — the client stays mapped throughout; #847 keeps g1's
pages until the clunk).

**The client.** `Surface::handle_configure` acks the size change
(`reweave`): write `resize W H serial` → open a FRESH weave fid (the old
fid's kernel map binding is pinned to the old generation) → re-read
geometry → `t_weft_map` the new weave → clunk the old fid → `cur_slot`
restarts at 0. `Err(Busy)` (E_AGAIN) means keep draining events; any
other error is fatal for the surface (the caller destroys + recreates).
Aurora (the fbcon) deliberately does NOT ack a size change — its cell
grid is bound to the console history at startup, so it keeps its grid
and the compositor crops the top-left (the ignore/crop client posture).

**Pane close.** Closing a pane (`layout close`) STRANDS its surfaces
invisible and delivers each a queued `TEV_CLOSE` (`Comp::send_close`) —
the exit REQUEST, distinct from the stream END (a retired surface's
event-fid EOF). Close is a request, never a forced retire: the surface
stays live until the client destroys it (it may need to save) or its
conn tears down.

**Seams recorded.** `weave_va_next` is a monotonic bump (no free-list;
each generation consumes VA that is never reclaimed until the process
exits) — bounded (a display-sized weave is ~12 MiB; the 47-bit user VA
holds millions of reweaves), a v1.x free-list closes it. A client error
AFTER a successful ack leaves the server on g2 with the client still on
g1's mapping (a blank frame until recovery; the old mapping frees at the
surface's Drop) — documented as fatal-for-surface.

## The interaction layer (G-6c): chords, focus, strips, move/zoom, multi-rect, determinism

**The Super chord layer** (§14 layer 1; §18.4 "intercepted ABOVE this
stream"). The serve loop's input drain consults `Comp::chord_key` after
the modifier fold and BEFORE `key_event`: while `MOD_SUPER` is in the
mask, every non-modifier key is compositor input — bound chords dispatch
(`chord_action`), unbound ones drop; none reaches a surface. The whole
Super plane is reserved (not just bound combos), so no client can ever
come to depend on a Super combo. Swallow bookkeeping is a 256-bit set
(`Comp.chord_down`): a swallowed press swallows its release and repeats
even if Super lifted first (no stray release reaches a client); a key
pressed BEFORE Super went down keeps flowing (the client that saw its
press sees its release). Modifier keys themselves always flow (clients
see mods — the documented §18.4 behavior). The baked table (compositor
policy, like the keymap; a halcyon.rc concern eventually): Super+arrows
spatial focus, +Shift+arrows directional move, h/v split, f zoom
toggle, t/s tabbed/stacked, e split-orientation toggle, Tab/+Shift tab
cycle, Shift+q close the focused pane.

**TEV_FOCUS** (kind 7; value 1 = gained, 0 = lost; F5 never-drop
class). Emitted from the single point `Comp::focus_sync` at the
reconcile tail — every focus-changing mutation reconciles, so the pair
emits exactly once per change (`last_focus` dedups; `retire` clears a
stale `last_focus` naming its slot so a future surface minted there
still gets its gained event).

**Tab/stack indicator strips** (D7 glyph-free). `Layout::layout_pane`
carves `TAB_STRIP_H = 5` rows from the top of a tabbed container (one
segment row) or `5 * n` from a stacked one (a row per child) — children
lay out below; a too-small rect carves nothing (`strip_h` → 0).
`Comp::paint_strips` fills the segments: the active child's segment is
FOCUS_COLOR when the focused leaf is inside it (`focus_child_of`),
ACTIVE_COLOR otherwise; the rest BORDER_COLOR; tabbed segments carry a
1px BG gap. Strips are chrome — painted with the borders (structural
AND focus-only epochs; the highlight follows focus), never touching
client memory, so the tearing-freedom invariant is untouched.

**Directional focus** (`Layout::focus_dir`). Spatial: among visible
focusable leaves, the nearest one strictly in the direction with
orthogonal overlap (ties → larger overlap). A miss (screen edge;
zoomed — one visible leaf) is a no-op.

**Directional move** (`Layout::move_dir`; D6 re-parenting). Walk up to
the nearest matching-axis ancestor (SplitH/Tabbed for left-right,
SplitV/Stacked for up-down — a horizontal move walks tab order): a
direct child swaps with its neighbor; past the edge it escalates to
the next matching level; nested deeper, the leaf pulls out of its
subtree and inserts beside it (`detach_leaf` — the index stays valid
across dissolution because a dissolving container is REPLACED in place
at its own index); with NO matching level anywhere (a pure cross-axis
move) the root wraps in a fresh axis container; past a matching
level's far edge with nothing outer it is a no-op (`saw_axis_edge`).
The moved leaf keeps focus.

**Zoom** (`Layout::zoomed_id`; §14 pane-zoom, tmux-shaped). A by-id
toggle (`zoom <id>` / Super+f): `recompute` lays out ONLY the zoomed
leaf at full display (the tree untouched; one visible leaf → no inset
→ borderless; a display-sized surface goes DIRECT zero-copy through
the ordinary mode machine). Held by public id, not slot (slots are
reused; ids never) — a stale target self-clears at the next recompute.
Structural mutations (split/close/mode/move/tab/host) and focus-to-
another-pane auto-unzoom first (the tmux rule); focus on the zoomed
pane itself keeps the zoom. The layout header gains `zoomed <id>`.

**Multi-rect presents** (§18.2 D4, as-built). `rect_count k >= 2`
carries rects 1..k INLINE after the 32-byte header — payload
`32 + 16*(k-1)`, `TPRESENT_MAX_RECTS = 64`, length must match exactly,
and EVERY rect validates against the surface geometry before any pixel
work (no partial present). This realizes D4's "compositor case" and
supersedes the provisional `buf_idx_or_off` registered-buffer-slice
sketch: under D3 the present payload already rides the client's
registered buffer, so a separate slice name would be redundant
indirection. `libtapestry` grows `present_rects` (client cap
`MAX_RECTS = 63`, the staging descriptor region below `EV_OFF = 1024`).

**Determinism mode** (§18.6 + F13 + F15; the `test-mode` cargo
feature, default-on for the dev tree — a production build strips with
`--no-default-features` and every verb answers E_OPNOTSUPP, the #880
class). `test-mode on` freezes the FRAME clock: the serve loop skips
wall-clock ticks (already-queued FRAME events drain normally — the F15
transition discipline for a synchronous single-threaded engine) and
re-seats the anchor on unfreeze (no backlog burst); `tick` drives time
one step per ctl write; the ctl read reports `test-mode on|off`.
`TPRESENT_HOLD` (accepted only while the mode is on): the present's
pixel work runs normally INSIDE the dispatch — Direct transfers /
Composed blits — so client weave bytes are still read only inside the
present dispatch (tearing-freedom holds for held presents); ONLY the
device-visible push (Direct flush / Composed screen transfer+flush)
defers into `Surface.held`, released by the `release [<surface>]` ctl
verb (F13; ownership-gated per F2 — only the caller's surfaces).
Multiple holds union (most-recent bytes win where overlapping); a
non-HOLD present or `test-mode off` releases implicitly (no stuck
regions); a hold cannot complete a pending direct SWITCH (E_AGAIN —
the switch is itself scanout composition); a hold staled by a
scanout-mode change drops at release (the structural repaint
superseded it).

**Layout grammar (as-built, complete):** `split <id> h|v`,
`close <id>`, `focus <id>`, `mode <id> <mode>`, `move <id>
left|right|up|down`, `zoom <id>`, and the id-less `focusdir <dir>` +
`tab next|prev` (acting on the focused leaf). Pane ctl accepts the
same per-pane verbs (`move <dir>`, `zoom`, ...).

**Control scoping (the G-6d weighing).** SURFACE qids (weave / present /
event / ctl) are F2-owner-gated at every consumer — walk, open, read,
write, readdir, Tweft — so no client reaches another's pixels or events
(verified sound). The PANE + LAYOUT tree is deliberately conn-GLOBAL:
that is the WM-control model (control is 9P / layout-as-9P; a WM-control
client like `halcyon.rc` drives layout globally — the i3-IPC / tmux
shape), not a per-surface ACL. The `clock-rate` global ctl is the same
same-session-trust family. **G-6d holotype F1 (P2)** weighs the sharpest
consequence: a session peer could `close` another client's pane (for the
console renderer, aurora, that queues `TEV_CLOSE`, which exits it and
darkens the graphical console) or focus-steal its input. The v1.0 trust
boundary that contains this is the per-territory `/srv` (I-1/I-28):
`/srv/tapestry` lives in the driver's own territory, unreachable from a
user session's namespace, so only the trusted boot chain connects — no
untrusted tapestry client exists at v1.0. The per-client
layout-control capability + renderer-role protection + the D5 read ACL
are the Halcyon-era multi-untrusted-client fix (task #42); a partial
owner-scope now would break the global-WM model. `test-mode`/`tick`/
`release`/HOLD are dev-build-only (the `test-mode` cargo feature; #880),
with `release` additionally F2-owner-gated. **F2 (P3):** plain (flowed)
key press-state is not tracked — only swallowed chords — so a focus
change while a plain key is held leaks a half-pair (release routes to
the new surface); benign for aurora (ignores releases), a raw-evdev
client would see a stuck key (task #43). The synthetic-release-on-
focus-change fix is a hardening seam.

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
