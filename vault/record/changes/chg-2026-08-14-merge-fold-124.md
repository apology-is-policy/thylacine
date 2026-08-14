---
id: chg-2026-08-14-merge-fold-124
type: chg
title: "The 124-commit merge, and what --ours would have thrown away"
date: 2026-08-14
arc: arc-vault
commits: []
touched: [sub-kernel-poll, sub-kernel-ninep-session, sub-kernel-ninep-client, sub-kernel-srvconn, sub-kernel-sched-smp, sub-kernel-pipe]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-14
---
Batch 56: the largest sync the arc has faced — **124 first-parent
commits** of main (the Warp GPU arc, #198/#204/#210/#214/#215, the
gate-hygiene family) — and the first one where task #161's hazard
arrived at a size that could actually hide something.

**SIX CONFLICTS, EVERY ONE ON A DOCUMENT THE VAULT HAD ALREADY
STUBBED.** That is the protection working exactly as designed: the stub
exists so a main-track edit *cannot* land quietly. But the protection
only pays if the resolution folds, and `--ours` — the one-keystroke
answer that produces a clean tree and a green lint — silently discards
whatever main wrote. This merge is the worked example of paying it.

**WHAT MAIN ACTUALLY WROTE: 201 lines.** Folded into six dossiers, each
claim re-verified against the tree rather than inherited from main's
prose:

- **#214, the real-silicon bring-up** (135 lines) → [[sub-kernel-sched-smp]].
  An UNKNOWN-reset `TPIDR_EL1` that QEMU happened to zero and KVM
  deliberately poisons; a recursive EL1h-sync descent that marched
  288-byte frames down past the kstack guard until the L1 page tables
  held `THRD` magic; a bring-up timeout that rode a clock not guaranteed
  to advance and **had never once terminated by timeout in its entire
  history**; and the line-isolated mailbox protocol the old doc's
  caveat 4 had predicted for "bare-metal hardware" and deferred.
- **The demux counter suite** (35) → [[sub-kernel-ninep-client]]. Six
  counters, and a three-way split of "ownerless" — because the word
  conflated one pathology with three by-design flows.
- **The ring conservation law + `/ctl` registry** (21) →
  [[sub-kernel-srvconn]]. `produced == consumed + count` holds *under
  the lock by construction*, so a violation is a memory-safety symptom
  rather than an accounting one.
- **`POLL_MAX_NFDS` is not `PROC_HANDLE_MAX`** (8) → [[sub-kernel-poll]].
- Two one-line constant lifts → [[sub-kernel-ninep-session]] and
  [[sub-kernel-pipe]].

**THE FOLD'S OWN FINDING, AND IT IS THE VAULT'S.** The two constants
main lifted are named all over the tree, and the vault carried
pre-lift values in its own dossiers: poll said `PROC_HANDLE_MAX` is
"now 256", pipe said 64, the session dossier said 256 in three places.
Three values of two constants, live simultaneously. That is the
lifted-constant lesson (a lift voids every proof that named it) landing
inside the notes written to prevent it.

The sharpest instance is not a wrong number, it is a **weakened
argument**. Poll's dossier justified the decoupling with "sizing them
to the fd-table bound would blow the kstack (~14 KiB at 256)". The
arithmetic was right *for 256*. At 1024 the frame is 56 KiB — 32 B per
`poll_waiter` + 24 B per `struct Handle`, the latter
`_Static_assert`-pinned — against **16 KiB** of usable kstack. So the
decoupling changed character without changing text: at 256 it was
prudence (14 of 16 KiB, leaving nothing for the rest of the frame), and
at 1024 it is the only thing between `poll` and a guard-page walk. A
correct sentence had quietly become a four-times-understated one.

**AND THE SWEEP PRESCRIBED TO CATCH THIS HAS THE SAME DISEASE.** #198
ran an explicit stale-constant sweep, for the stated reason that "a
constant that appears as an argument in someone else's safety proof
cannot be lifted without re-running that proof." Its record contains
two errors of its own (task #167): `110-resource.md` names 64 and 1024
as the endpoints of a step that was 256 → 1024, so a true "quadrupled"
reads as a false one; and `phase7-status.md` puts the same 56 KiB frame
against "a 32-KiB kstack" — the TOTAL, half of which is the guard
region that exists to *catch* the overrun. Understating a safety margin
twofold, in the record of a safety sweep. My independent arithmetic hit
the same numerator, so only the denominator diverged, which is what
makes it easy to miss: the hard part was right.

The generalisation: **a stale-constant sweep is the single easiest
place to launder an error, because every line in it already looks like
a correction.** Read what each mention CLAIMS, not just which number it
carries.

**A MEASUREMENT CORRECTION, CAUGHT BEFORE IT BECAME THE HEADLINE.** The
first reading of this merge said main had written **2120** lines into
five stubbed docs — a figure that would have made #161 look four times
more urgent than it is. It diffed `b03cfec0..main`: the vault's own
previous merge commit, whose tree already holds the stubs. So it
measured stub-vs-full and billed the vault's absorption to main.
Against the true merge base (`git merge-base` → `257a3ab7`) it is 201.
Same shape as the recent `git log -15 -- path` reading that returned
"15 of 15" by construction: **an endpoint chosen because it was to
hand rather than because it measures the thing**, and both endpoints
look equally reasonable written out.

**WHAT WENT RIGHT, recorded because it is the load-bearing half.** Lint
found no dangling `code:` path across 124 commits — main renamed and
deleted nothing any dossier cites, which was the real structural risk
at this size. And lint caught *me*: the poll caveat originally cited
`syscall.h:412`, and **R4 rejected the file:line on the Present plane**
— the rule existing precisely because line numbers rot faster than the
claims they anchor. Rewritten to name the symbol.

LEDGER read off the rendered views after the merge, for the seventh
consecutive batch: **370 owned / 67 unowned / 437 files (84%), ~14885
unswept lines.** Owned did not move — this batch folded rather than
swept. Main added 3 files and ~3048 unswept lines, so 85% → 84% is
dilution by a growing denominator, not regression. Staleness rose 45 →
51 dossiers, which is task #160's queue growing under it.

**#161 IS NOW LOUDER, NOT ANSWERED.** Six conflicts against five last
time; 114 reference docs still full, against a main track that writes
to `docs/reference/` on 18 of its last 30 commits. Every one of those
folds was hand-done and none was mechanically checked. The decision is
still the user's.
