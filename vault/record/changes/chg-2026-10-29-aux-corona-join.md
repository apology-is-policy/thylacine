---
id: chg-2026-10-29-aux-corona-join
type: chg
title: "The corona join: aux-3 carries env-a and env-c, and every deferring lock is taken through its helper"
date: 2026-10-29
arc: arc-phase6-pouch
commits: ["f7c4b5e14", "69dcfcdd6", "a68fc0eca", "f3f5b4b7b", "842c0c908", "009d20767"]
touched:
  - sub-kernel-poll
  - sub-kernel-ninep-client
  - sub-kernel-proc
  - sub-kernel-pts
  - sub-kernel-loom
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-02
---
aux-3 merged corona/async-memory at its pushed tip c38f1cb1f, so it now carries the spawn environment tail
([[chg-2026-10-28-spawn-env]]: `SYS_SPAWN_FULL_ARGV` ext bit 2, `SPAWN_EXT_ENV`; pouch 0064, under which
`posix_spawn` with a NULL envp gives an empty environment). Thirty conflicts, each read. `struct Proc` is 504
bytes; one `SPAWN_PERM_ALL` popcount assert survives; the pouch series runs 0050, 0064, 0065, 0066. Every
`posix_spawn` in the merged tree passes `environ` or the test's envp, so 0064's NULL rule changes nothing in tree.

The textual merge hid two semantic seams, both of the poll chunk's deferring locks
([[chg-2026-10-28-poll-hook-walks]]): fourteen raw `c->lock` acquires and releases in corona's private-progress
paths, one inside `submit_async`, which the poll chunk had moved to `client_lock` -- a raw release there leaves
`defer_depth` raised, and nothing fails until `thread_free` extincts ([[sub-kernel-ninep-client]]) -- and four raw
`g_proc_table_lock` sites ([[sub-kernel-proc]]). All take the helpers, and the new `tools/check-lock-helpers.py`,
run by `build.sh` after its own self-test, fails any raw site ([[sub-kernel-poll]]). The C7 and poll test hooks
moved into their headers' `KERNEL_TESTS` blocks under corona's declaration rule. The first compile found a
third seam: Astra's protocol index embeds 64 `struct poll_waiter` and asserted it fits one page; the poll chunk's
`prev` link made it 4176 bytes. It is now allocated at its size and charged by its buddy footprint, two pages
([[sub-kernel-loom]]).

Audit round 1 (Fable 5.1 reviewing Opus 5.5, start == end): 0 P0 / 0 P1 / 0 P2 / 2 P3, both fixed. F1: corona's
pts interaction woke binding pollers under `g_proc_table_lock`; the wakes now follow each holder's release
([[sub-kernel-pts]]). F2: the checker missed multi-line calls, alias pointers, the `_raw` variants and client
locks outside `9p_client.c`; its self-test now plants each. Round 2 (Fable 5.1, start == end) on those fixes and
the index charge: 0 / 0 / 0 / 4, all fixed -- the F1 test's fixture leaked on a failing assert; the checker still
missed a client lock through a header member, parenthesised, cast or in a header, and exempted two shapes that
write the table lock's word; and nothing pinned the index charge to kmalloc's real order, which
`loom.metadata_charge_matches_kmalloc_order` now does.

Gates. On 842c0c908: the suite and its regreen each 2150 registered tests with 0 fails and identical ELFs; the C7
debug-probe and pouch mask legs and corona's env witnesses pass; SJ1 and SJ2 (a raw client unlock in the
private-progress paths) each extinct `poll_deferred_await inside a deferring lock` in
`loom.private_native_operations`, so those paths have a runtime witness beside the static gate; SJ4 (the index
charged one page) red on `loom.private_slot_owner`; `specs/check-poll.sh` all as claimed; SMP gate default-smp1/4/8 +
ubsan-smp4/8 each 10/10, 0 corruption. On 009d20767: suite 2151/0 with every marker; check-decls and check-production
PASS; the production kernel ELF byte-identical to 842c0c908's (`1cba396bb13fcd39`), so the SMP result covers the
production code exactly -- the test kernel's new test and its reworked one had one suite and no SMP run; SJ3a and
SJ3b (the wake back under the table lock, one path each) red on the F1 test's own assertion for that path; SJ5
(kmalloc over-allocating two-page requests) red on the new charge test; regreen ELF identical. Owed: the F1 test's
new cleanup on an early assert has not run ([[fnd-aux-corona-join-r2-f1]]).
