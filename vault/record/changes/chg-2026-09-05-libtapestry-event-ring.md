---
id: chg-2026-09-05-libtapestry-event-ring
type: chg
title: "sub-libtapestry brought current: the shared event ring (H-3c-2), the role constructors (H-3c/H-3d), the surface-leak Drop fix, the host tests"
date: 2026-09-05
arc: arc-vault
commits: ["dbfe341f"]
touched:
  - sub-libtapestry
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The libtapestry deltas main handed off across yip 0037-0041 (the H-3c menu,
the H-3c-2 EVENT SET + its audit close, the H-3d status bar), done as a full
de-stale of [[sub-libtapestry]] rather than an append. The dossier body still
described the pre-H-3c-2 architecture -- "each surface owns its own session",
five descriptors per surface, one ring per surface -- which the H-3c-2 event
set replaced wholesale; leaving that in place while main's own KT-1 append
(the `global_ctl` / `TEV_LAYOUT` section) already referenced `EventRing` was
the both-readings anti-pattern the vault warns against, so the stale sections
were rewritten, not annotated.

## What it covers (verified against the code, not just the handoff notes)

- **H-3c-2 THE EVENT SET (7b9a457d, +1216a465) + its audit close (dfdb6482,
  fixup 557019c2).** `EventRing` = ONE 9P session + ONE Loom ring, shared by
  every surface opened on it (`Rc<RefCell<RingCore>>`, Clone = another handle,
  session closes with the last). The load-bearing reason is a kernel
  constraint, now in the Mechanism section: `loom_wait_for_completions` pumps
  only the FIRST in-flight op's session, so a two-session ring starves the
  second silently -- the exact defect the H-3c lever measured (a tile's
  CONFIGURE at the next RPC, a menu's key never). The slot bookkeeping is the
  new syscall-free `ring.rs` module (added to the dossier's `code:` list --
  it was absent), host-tested: index-stable table + placeholder fid, per-join
  generation belt, retiring-slot lifecycle, EOF/error stream-latch,
  `MAX_RING_SURFACES` = 48, `SLOT_QUEUE_CAP` = 256. Presents became a
  synchronous `t_write` whose Rwrite is the recycle gate (the Loom WRITE it
  replaced bought nothing and cost a handle).
- **H-3c the menu + H-3d the status bar (81515e75/826095b6; e3b5ba1e).** The
  role constructors `menu_on` / `status_on` (plus the H-3b `chrome_on` moved
  onto the shared ring) and the `Mint` enum (`Content` / `Chrome` / `Menu` /
  `Status` / `Claim`). The compositor-side mechanics of those roles are
  [[sub-tapestryd]]'s and land in its own de-stale.
- **The surface-leak Drop fix (H-3b close carried into H-3c-2).** `Drop for
  Surface` now says `destroy` FIRST (so the in-flight read EOFs and frees the
  slot), then leaves the ring, then closes fds -- the H-3b shared-chrome path
  leaked one server-side surface per dropped tag bar, filling the renderer's
  cap after ~17 zooms. The construction path's `fail_created` (H-3c-2 round
  F2) writes `destroy` before closing on any failure past `create`, closing
  the refused-create slot pin.
- **The host tests resolve the "No tests of any kind" caveat.** The crate now
  splits on a default `guest` feature; with `--no-default-features` the wire
  types + `ring.rs` are host-tested (nine tests -- the H-3c-2 audit's F1/F2/
  F3/F4/SA-4 regressions as executable counterexamples). `validated-by` stays
  `[prose]` (the sibling convention -- test detail lives in the body); the
  syscall half behind `guest` remains prose-only.

## What was re-verified, not copied

- The `pixels()` unsafe caveat (task #154) STILL HOLDS: the geometry parser
  validates width/height/row-stride/slot-count but not `slot_stride`, which is
  the field the safety comment names. Read at `usr/lib/libtapestry/src/lib.rs`
  pixels() + the open_on_bound parse.
- The #152 construction-leak caveat is now RESOLVED for surface construction
  (every failure routes through `fail`/`fail_created`; the staging-buffer
  allocation moved to `adopt_flags`, which closes the root on failure), so it
  was dropped rather than carried as a live caveat.
- SQPOLL (`connect_sqpoll` / `poll_fd`, KT-1.5b) recorded as a seam: the
  pollable kthread-driven ring halcyond's unified poll uses.

## Not in scope (owed elsewhere)

The compositor half of these features (Role::Menu/Status server mechanics,
the fid_clunk minted-never-created retire, the W-3c presentable, the W-4
windows) is [[sub-tapestryd]]'s de-stale, tracked separately; this chg is the
CLIENT crate only.
