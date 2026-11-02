---
id: chg-2026-11-02-usr-notes
type: chg
title: "SIGUSR1 and SIGUSR2 become notes, and every family word outgrows its byte"
date: 2026-11-02
arc: arc-phase6-pouch
commits: ["ad23e3315", "451a39eaf", "b429cb8e6", "9f77a7394", "33fbf2198", "a37734c18", "f5546b2be"]
touched:
  - sub-kernel-notes
  - sub-kernel-proc
  - sub-kernel-thread
  - sub-kernel-death
  - sub-kernel-vivarium
  - sub-pouch-signal
  - sub-pouch-thread
  - sub-libthyla-rs
  - sub-utopia-eval
  - abi-note-names
established: []
closed: []
opened: []
mirrors-checked: [usr/lib/pouch/patches/0007-pouch-signals.patch, usr/lib/pouch/patches/0021-pouch-pty.patch, usr/lib/pouch/patches/0011-pouch-abort.patch, usr/lib/pouch/patches/0049-pouch-sigterm-term-note.patch, kernel/vivarium.c, usr/lib/pouch/patches/0050-pouch-self-fault-signals-errnos.patch, usr/lib/pouch/patches/0067-pouch-thread-directed-kill.patch, usr/lib/pouch/patches/0068-pouch-usr-notes.patch]
depth: skeletal
created: 2026-11-02
---
SIGUSR1 and SIGUSR2 had no note. They now have one each on bits 8 and 9, catchable and default-terminating, each with its own latch and its own recorded death signal ([[dec-2026-11-02-usr-notes-chunk]]). `term` was the template. Bits past 7 do not fit the words that carry a family, so `note_caught` goes to u32 and `note_claim`, `note_target` and `note_direct` to u16, in existing padding: `sizeof(struct Proc)` stays 504 and `sizeof(struct Thread)` 1776. `NOTE_RING_SPARE` goes to 10, and the phenotype's `viv_signote` rows are appended, so `USR2` is `COUNT - 1`. Pouch 0068 and libthyla-rs carry the mirrors ([[sub-pouch-signal]], [[sub-libthyla-rs]]), utopia's `mask note` knows the names ([[sub-utopia-eval]]).

Audit: [[adt-thread-usr-r1]] (Fable 5.1, 0/1/0/2, before the first boot). Not dirty. Its P1 was a test expecting a move to coalesce with interrupts that name a sender.

The leases found what reading did not. Lease 1 stopped on a stale exact pin of what block-all covers. Lease 2 left K12, K15 and G3 green. K15 was a real gap at every family: nothing at runtime asserted that a designation closes its family, and `rendez.caught_note_designation_closes_the_family` now does. K12 and K37 turned out to be a hand-off in an unwinding wait winning its own designation straight back, since the walk prefers the calling thread; who unwinds cannot show that, so `rendez.caught_note_designee_keeps` counts walks. The first G3 could never touch L345, because a default death has two arms. G7 showed L345 had never had a sleeper: the kill beat the child's first entry, and L345 now settles first.

Gates on f5546b2be: suite 2175/0, green and regreen with identical ELFs (57b66ede4b4f850c), every guest leg, K12 and K37 red on the walk count, G7 red on L345 (`sig=10 ws=-1`). On a37734c18: K15, K38, G3 (`sig=12 ws=15`) red on their own lines, P1 dying at kill-usr, and the SMP subset `default-smp4` and `ubsan-smp4` each 10/10, 0 corruption. On 33fbf2198: K01-K36 bar K12 and K15, B1-B7 and R1 on their own guards, G1, G2, G4, G5, G6, P2, P3, P4 on the right legs. From a37734c18 to f5546b2be only a counter under KERNEL_TESTS, a test and a probe's nap change, so the SMP subset was not repeated.

The SMP subset here is aux's reading of the hold of 2026-11-02 as a narrow gate, not a vote: vote 17 named C4. NOT RUN: the full SMP matrix, under that hold; `tools/test-fault.sh`; the interactive fleet; `tools/check-production.sh`, beyond the production-shape build `tools/test.sh` does first. The merge to main owes them.
