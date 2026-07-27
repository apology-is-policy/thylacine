# Aux track roadmap

**As of 2026-07-23. Branch `gfx-4` @ `11ebf755`, clean + pushed (both mirrors).**

Supersedes the narrow `usr/apps/AUX-ROADMAP.md` (the old userspace-apps-only aux
agent). This track now owns the graphics arc, the Aurora environment, and — new
today — the VIVARIUM arc.

---

## Where the track stands

| Arc | State |
|---|---|
| **Graphics G-0..G-7** | **COMPLETE.** G-0..G-5 + PTY are already ancestors of `main`; G-6 (compositor) + G-7 (SDL/Quake) are on `gfx-4` awaiting the merge. |
| **Track B — Aurora config (cfg-1..cfg-5)** | **COMPLETE.** OSD + persistence + OSC session push + the apply-authority gate + runtime chords/gaps + baked font sizes, each audited. |
| **VIVARIUM (V-0..V-8)** | **STARTED.** V-0 scripture + V-1a (phenotype ledger + brand hint) landed; V-4 specced build-ready. |
| **Halcyon G-8/G-9** | Not started. The graphics endgame. |

---

## Stream 1 — VIVARIUM (the active arc)

Phase 8's fourth pole: run unmodified Linux binaries. Design + build arc:
`docs/VIVARIUM.md` (all four decisions resolved — fork = **C** hybrid, build-now,
declare-not-infer branding, names adopted). Task **#62**.

**V-4a-0 + V-4a-0b LANDED** — the two kernel prerequisites the build surfaced.
V-4a was specced as pure userspace; ground-truthing the Tier-1 file set against the
tree before writing the crate found that **two of its entries had no native source
at all**:

- **`/proc/self/exe`** — `struct Proc` carried no executable identity whatsoever
  (the Image cache is qid-keyed, the text Burrow anonymous, `format_cmdline` a
  stub). Fixed by `Proc.exe_path` + `/proc/<pid>/exe` (§6.5), pinning the #66 `Path`
  the exec resolver already held.
- **`self` itself** — `srv_peer_info` reported `stripes` (an opaque tag with no
  userspace pid mapping) and **no pid**, so a 9P server could learn which
  *principal* was talking to it but never which *process*. Fixed by
  `srv_peer_info.pid` filling the reserved slot in place (§6.6).

Both were the pull-forward default, not scope creep: §6.2's rule is that the
diorama renders **only** from natively-reachable sources — that is what makes I-43
structural — so a missing source is a *kernel* gap by construction, never a licence
for the diorama to invent or accept an answer. §6.7 records the lesson and flags
`/proc/self/cwd` + `/proc/self/maps` (both V-4b) as the same shape.

**V-4a is DONE** — `usr/diorama` on the ptyfs skeleton, joey-spawned with
`MAY_POST_SERVICE`, selftest-before-serve, read-only, serving Tier-1 `/proc`. It
mounts at `/dio`; joey creates the mount point but deliberately does **not** mount
it, because `self` resolves to the connection's peer — i.e. the *mounter* — so a
shared mount would report joey to every reader. Each client mounts privately,
which is also how V-7 will set up a container.

**V-4b-1 and V-4b-2 are DONE**: `/self/cwd` and `/self/maps`, each with its kernel
source. Both confirmed §6.7's "budget these as kernel + userspace", but neither for
the predicted reason — `cwd` needed no new kernel *state* (the Territory has
carried `dot_path` since LS-4), and `maps` inherited its lock-order argument from
`devproc_mem_walk_cb`, which had already established and audited
`g_proc_table_lock → vma_lock`. The refined lesson (§6.7): **grep for an existing
accessor and an existing lock-order precedent before budgeting either.**

`maps` also forced the first real "which layer speaks Linux" decision, settled in
§6.8: the kernel emits a Thylacine-native table, the diorama translates. Anything
else is phenotype leaking into the kernel.

**V-4b-3 is DONE**: the numeric `/proc/<pid>/…` dirs, the root pid enumeration,
and `sys/kernel/{ostype,osrelease,version,hostname}`. **Pure userspace this time —
kernel byte-unchanged** — and the reason is worth carrying forward: `/self` was
*always* a per-pid render with the pid supplied by the connection's peer rather
than by the path, so the pid had been a parameter from the start and per-pid was a
generalization, not a new mechanism.

Two design findings landed with it:

- **§6.9 — the fourth source.** `sys/kernel/ostype` reformats nothing; the answer
  *is* the phenotype. That is not a §6.2 violation (a constant carries no
  information about the system, so there is nothing to leak), but the distinction
  had to be written down or it becomes the loophole every later file is argued
  through. The rule: *derived from kernel state needs a native source; a constant
  declaring which ABI you are looking at is the phenotype speaking about itself.*
- **§7.1 — the V-7 pid-visibility obligation.** The diorama's pid view matches
  native `/proc`'s exactly (all-pids, Plan 9 posture), so there is no new
  authority — but a contained Proc seeing every host pid is a leak, and that leak
  is in native `/proc` + `/ctl/procs` first. Scoping the diorama alone would be
  theatre. Owed at V-7, against the native surface.

**NEXT: the V-4b remainder** — and it is three different-shaped jobs, not one
(§6.10): `environ` needs a kernel source (`/env` is self-only by construction);
`auxv` needs one too and its value should be weighed first; `fd` is **blocked on
#66c**, the #926 handle-table lifetime restructure, which is a kernel chunk rather
than a Vivarium one. Then V-4c (`/sys` + Linux `/dev` + per-container mounts +
focused audit).

**V-1b is merge-ordered, not blocked-forever**: it wants `kernel/exec.c` +
`kernel/syscall.c`, which CL-4 also touched. Land `clade-cl4-wip` → `main` → then
`gfx-4` → `main`, and V-1b is clear. See `docs/MERGE-gfx-4.md`.

Later: V-2 (the total-and-stateless translation table) · V-3 (the supervisor
channel — **spec-first**, `specs/phenotype.tla`, a new wait/wake on the death
lineage) · V-5 sockets → `/net` (gate: `curl` fetches a URL) · V-6 signals
(audit-bearing) · V-7 `viv` (gate: an Alpine shell) · V-8 audit on I-43.

## Stream 2 — Halcyon (G-8/G-9), the graphics endgame

The last stage of the arc and of Phase 10. Four parts to G-8:

1. **The native TTF rasterizer** (`no_std`, AA + hinting). The natural first
   sub-chunk: self-contained, testable, and it is simultaneously the user's
   "Apple-quality fonts" ask *and* the prerequisite for the Acme tag bar. Scripture
   already calls it "foundational, not a nicety" (`TAPESTRY.md §14`).
2. The transcript pane (Helix-modal, selection-first scrollback).
3. Inline graphical surfaces in the transcript.
4. `halcyon.rc` (the policy layer).

Then **G-9**: Aurora-terminals-as-panes, video player, image display, the Halcyon
audit + `docs/HALCYON.md`.

**Recorded direction — the Acme tag bar** (`TAPESTRY.md §14`, from the user's i3
find): render the pane `tag` as text in the Stacked/Tabbed strip (today glyph-free
colored segments, per D7). The strip becomes a thin **renderer-drawn title surface**
so the compositor never grows a glyph path; the richer end state is Acme's
**executable tag line** — the title bar as a live command surface.

## Stream 3 — polish + debt (small, satisfying, interleavable)

- **cfg-6 — the `letterbox`/zoom-policy OSD row.** A standing user ask; small
  (`Comp::letterbox()` exists, the Display section already shows it info-only —
  needs a config key + a live row like Mode).
- **#39 — the Aurora host-test harness** (the netd-style
  `cfg_attr(not(test), no_std)` refactor) so the dormant vt/render/osd/config
  regressions actually run in CI. Highest leverage of the debt items.
- **#57 — the live-display (cocoa) border under-paint + lingering dead pane.**
  User-observed, no headless repro (the #31 class) — needs eyes on a live window.
- #43 (synthetic key-release on focus change — stuck key) · #44 (4K weave cap +
  multi-point pixel asserts) · #32 (`ls /srv` → "I/O error"; devsrv has no
  `.readdir`) · #13 (per-pts ownership + 0600).

---

## Recommended sequencing

1. **V-4a** now — unblocked, specced, has a real consumer.
2. **The merge** when the main track is ready (`docs/MERGE-gfx-4.md`), which
   unlocks **V-1b**.
3. **cfg-6** as a warm-up whenever a short chunk fits.
4. **G-8's TTF rasterizer** as the next *big* pick after Vivarium's foundation is
   in — or ahead of it if the user prefers the visible win.

Vivarium and Halcyon are both large and both sit in the endgame beside `v1.0-rc.1`
(`ROADMAP §11.5` keeps the fallback: v1.0-rc ships without either if neither
converges). They can proceed in either order; Vivarium is the one currently moving.

---

## Coordination with the main track

**Merge round 1 is DONE** (2026-07-27): `gfx-4` merged into local `main` at
`15edb01e` + the `de451566` pouch O_APPEND restore. The pouch series collision the
handoff predicted was real and was resolved there. **Not pushed** — `origin/main`
is still `b0bf63f2`.

Two things carry forward, both in `docs/MERGE-gfx-4.md` (rewritten as a round-2
handoff):

- **Five `gfx-4` commits landed after the merge point** (`b7df5b21..5af01124`),
  including both VIVARIUM kernel prerequisites. Round 1's analysis said
  `struct Proc` would auto-merge because V-1a's `phenotype` fit the tail pad — that
  is no longer true: V-4a-0 grows it 352 -> 360. The size assert is the drift
  detector, so a bad merge fails the build loudly.
- **Clade CL-4 never landed.** The handoff advised `clade-cl4-wip` -> `main`
  first; in the event `gfx-4` went first, so CL-4's four commits still merge on top
  of the gfx-4 kernel changes. An inconvenience, not a defect — but it means
  `kernel/syscall.c` now has a three-way overlap (main-via-gfx-4, CL-4, and
  V-4a-0b's small `pid` out-param thread-through).

**Consequence for V-1b**: it is *still* best sequenced after CL-4 lands, for the
original reason — CL-4 touches `kernel/elf.c` + `kernel/syscall.c`, which is
exactly where V-1b's syscall-entry phenotype branch goes. V-4a (the diorama crate)
is pure userspace and has no such constraint, which is another reason to do it
first.

- The aux track has touched kernel files (cfg-3's `srv_peer` stamp, V-1a's
  `Proc.phenotype`, V-4a-0's `exe_path`, V-4a-0b's `srv_peer_info.pid`); it is not
  `usr/`-only. Check the main worktree's dirty state before editing shared kernel
  files.
