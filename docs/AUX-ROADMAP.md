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

**NEXT: V-4a** — unblocked, pure userspace, fully specified in `VIVARIUM.md §6`.
Build `usr/diorama` on the ptyfs skeleton (native device-less `/srv` server,
joey-spawned with `MAY_POST_SERVICE`, selftest-before-serve, read-only) serving
Tier-1 `/proc`. **Gate: `/proc/self/exe` reads the running binary's path in-guest.**
It has a consumer today — nothing provides `/proc/self/exe`, which is why Clade's
fork patches LLVM's `getMainExecutable` onto `argv[0]`; pouch programs benefit with
no phenotype at all.

Then: V-4b (per-pid + `sys/kernel`) → V-4c (`/sys` + Linux `/dev` + per-container
mounts + focused audit).

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

- Main is on the **Clade/LLVM** arc (CL-4 complete at `7cfcabce` on
  `clade-cl4-wip`; next CL-5).
- **Merge handoff: `docs/MERGE-gfx-4.md`** — measured conflicts from an aborted
  trial merge, incl. the **pouch patch series number collision** (both tracks
  numbered 0024/0025/0026) and the suspicion that main's
  `0024-pouch-fs-process-wires` **subsumes** the aux `0024-pouch-fopen-create`.
- Ordering: `clade-cl4-wip` → `main` **first**, then `gfx-4` → `main`.
- The aux track has touched kernel files (cfg-3's `srv_peer` stamp, V-1a's
  `Proc.phenotype`); it is not `usr/`-only. Check the main worktree's dirty state
  before editing shared kernel files.
