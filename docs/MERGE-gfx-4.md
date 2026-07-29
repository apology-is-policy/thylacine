# Merge handoff: `gfx-4` -> `main`

## Round 1: DONE (2026-07-27, by the main agent)

`gfx-4` merged into local `main` at **`15edb01e`**, plus a follow-up fix
`de451566` ("pouch 0030: restore the O_APPEND seek-to-END the gfx-4 merge
dropped"). The pouch series collision this doc warned about (§2 of the original)
was real and was resolved; the O_APPEND restore is exactly the residue that
section flagged.

The merge took `gfx-4` at **`11ebf755`**.

**Not yet pushed**: `origin/main` is still `b0bf63f2`. Local `main` is clean.

---

## Round 2: DONE (2026-07-27, by the main agent)

`gfx-4` @ **`7b917e55`** merged into local `main` at **`db566f28`** ("Merge gfx-4
into main: VIVARIUM V-4a + the aux-resolved collisions"). The whole VIVARIUM V-4a
arc (V-4a-0 `Proc.exe_path`, V-4a-0b `srv_peer_info.pid`, V-4a the diorama) is in.

**Not yet pushed**: `origin/main` is still `da049000`. Local `main` is clean.

**Verified in main's tree** (checked, not assumed) — all four aux-side resolutions
carried through:

| Resolution | State in main |
|---|---|
| `sizeof(struct Proc) == 392` | ✅ |
| prowl-1 `name[]` @352 | ✅ |
| V-4a-0 `exe_path` @384 | ✅ |
| `PQS_SCHED=12` / `PQS_EXE=13` | ✅ |
| `usr/diorama` + `usr/diorama-probe` | ✅ present |

The two collisions and why they resolved the way they did are recorded in the
`struct Proc` field comments and at the `PQS_*` enum, so the reasoning travels
with the code rather than only with this file.

---

## The working pattern (both rounds)

1. Aux works on `gfx-4`, pushing to both mirrors.
2. Aux periodically merges `origin/main` **into** `gfx-4` to keep its base current
   — a merge, never a rebase: `gfx-4` is pushed, so a rebase would need a
   force-push, and its earlier history is already in `main` via prior merge
   commits.
3. Main periodically merges `gfx-4` **into** `main`.

Collisions caught by a `_Static_assert` (the `struct Proc` offset clash both
rounds) are the good failure mode — loud at build time rather than silent. Do not
loosen one to make a merge pass.

## What NOT to do

- **Do not merge across a dirty worktree** (this bit round 1's planning; both trees
  are clean now).
- **Do not "fix" the `struct Proc` size assert by loosening it.** If it fires, the
  merge dropped or duplicated a field — that is the assert doing its job.

---

## Round 3: DONE (2026-07-29, by the main agent)

`gfx-4` @ **`df9de8d4`** merged into `main` at **`93593a6f`**. All 26 outstanding
commits are in: VIVARIUM **V-4b** (1..6 + close), **V-4c** (1, 2a/2b/2c, 3), and
**#76**. The aux notes below were accurate — the overlap was exactly the twelve
files they name, and re-measuring at the current tips (main had moved on by
W1u-a/W1u-b, which touch none of them) reproduced the same list.

**Three conflicts, all resolved by keeping both sides.** One correction to the
notes: CLAUDE.md and `docs/ARCHITECTURE.md` carry **different text** for the same
LS-8 row (ARCH is the authoritative prosecution copy — 12445 chars vs CLAUDE's
10483), so they were merged **separately**. Copying one result into both would
have silently truncated the ARCH row.

**What the auto-merges needed, and this is the part worth carrying forward.** The
notes predicted conflicts in `tools/interactive/*`; git produced **none** — it
merged them silently. That is the more dangerous outcome, not the safer one, so
each was checked in both directions:

| Check | Result |
|---|---|
| Six harness files vs main's pre-merge versions | **byte-identical** — no union, no duplicated logic |
| Aux-only lines in any of the six | **zero** — preferring main drops nothing (the notes' claim, verified rather than taken) |
| All three cons TX tests registered in `test.c` **and running** | ✅ all three PASS |
| Suite arithmetic | base 1208 + main's 1 + aux's 9 = **1218**, matches observed |
| `_Static_assert(sizeof(struct Proc) == 392)` | holds |
| #80's `kernel/proc.c` hunk | every added line re-checked present |
| `docs/reference/111-cons.md` | both sides' sections survive |
| pouch series numbering | no duplicate numbers; `0031` in `series`, 31/31 parity |

**Gates on the merged tree**: default suite **1218/1218** + boot OK + 0
EXTINCTION; SMP gate **40/40 PASS, 0 corruption**; LS-CI **31/32 + 1 expected
SKIP** (`ls-gfx-mp`, a missing optional host artifact — build it with
`tools/build.sh quake-host` if you want 32/32).

**V-1b is now unblocked**: both halves of its stated condition are met.

---

## Round 3: the aux-side notes (written 2026-07-29, before the merge)

`gfx-4` is at **`15617895`**, pushed to both mirrors. The last `gfx-4` commit
already in `main` is **`7b917e55`** (round 2), so **25 commits are outstanding**:
the whole of VIVARIUM **V-4b** (1..6 + close), **V-4c** (1, 2a/2b/2c, 3), and
**#76** (which is not VIVARIUM -- it rode the same branch).

`gfx-4`'s base is **17 commits stale** w.r.t. `origin/main`. Per the working
pattern above, aux owes a merge of `origin/main` into `gfx-4` first; that has NOT
been done yet, so whoever moves first should expect to resolve the conflicts
below either way.

### The overlap, measured (not guessed)

Twelve files were touched on both sides since the merge base. Three groups:

**1. The console TX surface -- the one that needs real attention.**
`kernel/test/test_cons.c` + `docs/reference/111-cons.md`.

Main's **`7daf61e5`** ("#75 audit F2: the owed room-wait + #67 deadline test for
the cons TX ring") and aux's **`15617895`** (#76, SYS_PUTS joins the writer role)
are *both* P1-F console work, from opposite ends. They **compose in meaning** --
different mechanisms on one surface -- but **conflict textually**, because both
append a test to the same file and both rewrite sections of the same reference
doc.

Resolve by **keeping both**, not by choosing:
  - Both tests must survive. They assert different properties: main's is the
    room-wait / #67 deadline behaviour of the ring; aux's
    (`cons.sys_puts_uses_shared_console_path`) is that SYS_PUTS routes through
    `cons_output_write` at all.
  - Both doc sections must survive in `111-cons.md`. Aux replaced the old
    "SYS_PUTS bypasses the ring + role" bullet with a struck-through CLOSED entry
    plus a new "SYS_PUTS joins the shared path (#76)" section; main's F2 work adds
    its own coverage text. Neither supersedes the other.
  - **Post-merge semantic check**: the suite count should be main's + aux's new
    tests, and BOTH cons tests must appear in the run. If either vanished, the
    merge dropped a test -- that is the failure mode to look for here, and it is
    silent (a dropped test does not fail, it just stops existing).

**2. `kernel/proc.c`** -- main's `1060a75d` (#80, the EXITKILL reaper line) vs
aux's V-4c-3 `proc_set_exe_path` locking fix. Different functions; expected to
merge cleanly. The tripwire is the same as both prior rounds: the
`sizeof(struct Proc)` `_Static_assert`. If it fires, a field was dropped or
duplicated -- **do not loosen it**.

**3. `tools/interactive/*` + `tools/test-interactive.sh` -- the duplication
trap.** Aux's **`9b5d5b15`** is a *manual re-application* of main's own seven
LS-CI harness fixes (648 insertions across 6 files), so the same logical changes
exist on both sides with **no shared ancestry** -- git will show conflicts that
look alarming but are largely the same content arriving twice. Main is also
**newer** here (`cfe18c65`, #89, landed after the aux port).

  - **Prefer main's side on these six files**, and *verify* rather than union the
    hunks. Unioning duplicates logic.
  - Checked: `454921bf` (the aux-side `reap_qemu` tree-scoping fix) is already in
    **both** branches, so there is no aux-only harness fix at risk in this group.

Also touched by both, low risk: `CLAUDE.md` and `docs/ARCHITECTURE.md` -- both
sides append to different `section 25.4` rows (aux added a `#76 ADDENDUM` to the
LS-8 row's P1-F addendum). Take both additions.

### Why this round matters beyond hygiene

**V-1b is gated on it.** The aux roadmap records V-1b as "merge-ordered, not
blocked-forever": it wants `kernel/exec.c` + `kernel/syscall.c`, which CL-4 also
touched, and the stated unblock condition is "land `clade-cl4-wip` -> `main` ->
then `gfx-4` -> `main`". `clade-cl4-wip` is now **fully contained in `main`** (0
commits ahead), so **round 3 is the only remaining half of that condition.**

### State of the aux gates at `15617895`

Everything below was run on the aux side at the tip, so a clean merge should
reproduce it: default suite **1217/1217** + boot OK + 0 EXTINCTION; SMP gate
**40/40 PASS, 0 corruption**; LS-CI **32/32 with zero retries**; spec `cons_poll`
clean + liveness clean + `cons_poll_buggy_lost_wake` counterexample.
