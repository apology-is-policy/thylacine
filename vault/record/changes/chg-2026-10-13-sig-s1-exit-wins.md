---
id: chg-2026-10-13-sig-s1-exit-wins
type: chg
title: "The exit wins in the final close; ut reads every wait packed (sig S-1)"
date: 2026-10-13
arc: arc-boosty
commits: ["c0fa40fa1", "7faf67e1d", "eaa842b8d", "a17bf8ab1", "37269d75a"]
touched:
  - sub-kernel-death
  - sub-kernel-proc
  - sub-utopia-eval
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-13
---
S-1 of the signal follow-up to gcpark-b (operator votes 2026-10-13, items 3
and 4): a Proc that has committed to exit keeps its own status when a kill
lands during its final close, on both last-out paths
([[dec-2026-10-13-exit-wins-final-close]]); and ut reads every wait packed,
so `$status` is 128 plus the signal for any death by a note or a fault
([[dec-2026-10-13-ut-status-packed]]). S-2 (the `term` note and the
phenotype's kill family) follows on its own branch.

**What.** [[sub-kernel-death]], [[sub-kernel-proc]]: `thread_exit_self`'s
last-out now reads the group's claim and publishes `PROC_FLAG_EXIT_CLOSING`
in one `g_proc_table_lock` hold before its final close, and takes the
message, code and signal it will record there; `exits_term` already read
the claim before its close and recorded its own exit after. A kill that
lands after that hold forces the close (`PROC_FLAG_EXIT_CLOSE_FORCED`) and
changes nothing else. The same hold closes a gap present since #68: the
mark was stored inside `proc_close_handles_at_exit`, outside the lock, so a
kill between the unlock and that store forced nothing, and a close stuck on
a server could keep the dying Proc forever. The rule is keyed on the commit
to exit, not on the mark: a Proc with no handle table never marks and still
keeps its exit (round-1 F3). [[sub-utopia-eval]]: the eight wait sites that
read a raw status (external commands, passthrough, command substitution,
the degrade path, the WNOHANG reaps, `wait`) pass `T_WAIT_PACKED` and decode
once with `status_of_packed`; the two `WUNTRACED` sites already did.
ERRORS.md drops ut from the raw keepers.

**Witnesses.** Kernel (`test_9p_closer.c`):
`p9_closer.kill_in_final_close_keeps_exit` kills a child inside its final
close, held there by a KERNEL_TESTS hook, on each last-out path, and reads
the child's exit; `p9_closer.kill_at_final_close_start_forces_it` kills it
at the close's start and reads the forced flag. Two REDs on the default
image: reading the claim after the close again fails both tests at "the
kill replaced the exit's status"; dropping the mark from the commit's hold
fails only the second, at "the kill forced the final close", so the old
gap is witnessed both ways. ut (`pty-4`, CI image): a foreground command
interrupted by Ctrl-C reads 130, and a death by `snare:segv` reads 139 in a
`let x = $(...)` and as the exit of a script run by `ut FILE`. Two REDs,
each putting one wait site back to a raw read, fail their own legs (the
substitution reads 1, the script reads 1).

**Audit.** Fable 5.1, round 1: 0 P0, 0 P1, 1 P2 (the new tests used
`NOTE_SIG_KILL` without `notes.h`), 4 P3 (scripture wording on the window
and on the commit-versus-mark key; the status row's witness names; the
window test's premises, hook lifetime and a bounded zombie wait), all fixed
in the close. Closed list: repo `memory/audit_sig1_closed_list.md`.

**Land gates on 37269d75a.** CI image (b2c): build, ls-jsc 59/59, pty-4 PASS (FG 130, CS 139, SX 139). Default image: build, suite 1995/1995, test-fault 8/8, ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8), 0 corruption, check-production --all PASS.
