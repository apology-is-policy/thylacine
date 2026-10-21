---
id: chg-2026-10-07-exit-close
type: chg
title: "The at-exit close no longer waits on a server no death can interrupt: the clunk never waits, a kill forces the final close, the closer finishes it; and a Loom registration flushes dev9p's staged run"
date: 2026-10-07
arc: arc-boosty
commits: ["509d9174d", "c34654087", "638656023", "c3c037630", "17de8e28f", "7dfbba97d", "dbfeb561a", "fef091ed0", "e2a63fe76", "d29246829", "4fb87c2f8", "b34ed7c30", "227375707", "f9552bca3", "7b81e2ea1", "d8b177156", "ef64e4b3a"]
touched:
  - sub-kernel-death
  - sub-kernel-notes
  - sub-kernel-proc
  - sub-kernel-thread
  - sub-kernel-loom
  - sub-kernel-ninep-attach
  - sub-kernel-ninep-client
  - sub-kernel-ninep-dev9p
  - sub-kernel-devproc
  - sub-kernel-syscall-dispatch
  - sub-substrate-gates
  - spec-net-poll-teardown
established: []
closed:
  - seam-close-flush-unbounded
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-07
---
The last thread out of a Proc closes its handles under `exit_close_active`,
where no death reaches it, so a dev9p close that waited on its server -- the
write-behind flush, the Tclunk -- held the dying Proc for as long as the
server chose, and a further kill could not break in
([[seam-close-flush-unbounded]]). Any process can serve a mount.

By the operator's vote ([[dec-2026-10-07-exit-close]]: A now, then B with C):

- **A: the clunk never waits** ([[sub-kernel-ninep-client]],
  [[sub-kernel-notes]]). `thread_death_reaches(t)` says whether a death can
  end t's sleeps: not on a kproc thread, not under the final close. On such a
  thread dev9p clunks with `p9_client_clunk_nowait` (`rpc->no_wait`): the tag
  drain and the send flow refuse instead of waiting, and the bound fid goes
  to the closer ([[sub-kernel-ninep-attach]]).
- **A2: a flush a death ends keeps its run** ([[sub-kernel-ninep-dev9p]]).
  The run stays staged and nothing latches, since `write()` already reported
  those bytes; the next flusher sends it. Before, the run was dropped and the
  errno latched (OPEN-BUGS 07:44Z, kill mid-flush).
- **B: a kill forces the final close** ([[sub-kernel-death]],
  [[sub-kernel-proc]]). `proc_group_kill` sets `PROC_FLAG_EXIT_CLOSE_FORCED`
  when it loses the exit-message race or finds `PROC_FLAG_EXIT_CLOSING` set
  (exits() sets it under the proc-table lock). The hold then lets the death
  through, so the closing thread's sleeps end. Only explicit kills force: a
  hangup, EXITKILL or a legate scope's end never does. Loom's SQPOLL join got
  its own `kthread_join_active`, which no kill lifts
  ([[sub-kernel-thread]], [[sub-kernel-loom]]).
- **C: the closer finishes it** ([[sub-kernel-ninep-attach]]). A last close
  that may not wait (`close_may_wait()`: a die-pending thread, or one marked
  `closes_never_wait`, the SQPOLL kthread) hands the staged run and the fid
  to the closer as a `p9_close_job`, which writes the run, then clunks. A
  write refused for want of memory or by the server is retried with backoff
  while the fid is held. A failed hand-off prints `9p: close: flush of fid`,
  which `tools/test.sh` fails on ([[sub-substrate-gates]]).

**What stopping staging means now.** A wstat and a Loom registration stop
staging by clearing the append anchor `wb_known`, never the eligibility flag:
that flag gates the read overlay, fsync's flush, the write ordering of a run a
death kept, and the error latch's report on write and fsync, for another Proc
sharing the fd too. Two audits found the two halves of this: the exit-close
round's F1 (a dying wstat left a kept run unreachable) and the Loom round's F1
(a wstat or registration on a latched priv erased the report).

**The Loom write-behind bypass, closed alongside** (OPEN-BUGS 09:12Z). A Loom
op drives its fid straight to the wire, so a Loom FSYNC reached the server
ahead of bytes still staged. `loom_register_handles` now calls
`dev9p_loom_register` for every new Spoor, on the registering syscall's
thread: it fails on a latched flush error, flushes the run, stops staging, and
drops the file's Larder pages, which the flush installs as own pages and the
ring's writes would leave stale. The Larder half of the Loom bypass (Loom
WRITEs and dirent ops invalidate nothing) remains the L1c/L1d seam.

**The witnesses.** RED on the final tip ef64e4b3a: ten sabotage runs,
each rebuilt and run through the whole suite, the tree restored and checked
clean after each: exit-close R1 (8 red), R2 (7), R3 (8; `tools/test.sh`'s
loss-line matcher fired 5 times), R4 (8), R5 (4), R6 (3; the matcher once),
R7 (1); Loom R1 (3), R2 (3), R3 (3: the dead buffer kept at each stop site,
and a run kept past the anchor no longer anchoring appends). Every predicted
witness, and only those, turned red, each at its predicted assertion but
one: with the closer skipping the job's run,
`p9_closer.close_job_retries_a_refused_write` failed one assertion earlier,
at "the server refused the first write", because no write was sent at all.
Suite 1935/1935 before the first run and after the green rebuild (main
1917 + 18 new tests). The same campaign, without Loom R3, ran first on
d8b177156 with the same result.

**The model.** `net_poll_teardown_buggy_no_closer.cfg` had never been run by
`specs/check-net-poll.sh`; it is now ([[spec-net-poll-teardown]]). On d8b177156 the ten clean cfgs
complete and the eleven buggy cfgs each violate their named property;
`net_poll_teardown_buggy_no_closer` violates `Liveness` (a dying thread's
close cannot send, so its Tclunk is never sent without the closer).

**The audits.** Exit-close round 1 (Fable 5.1): 0 P0 / 0 P1 / 1 P2 / 6 P3,
clean. Loom round 1 (Fable 5.1): 0 / 0 / 1 / 4; F5 (SYS_LOOM_REGISTER reports
every failure as a bare -1) is a syscall-interface change, enqueued for a
vote. Loom round 2 (Fable 5.1, focused on the flag model both rounds
changed): 0 / 0 / 0 / 6, clean. It found the model sound and six P3s, all
fixed: comment drift, two witness controls, an append leg onto a run kept
past the anchor, and the staging buffer and its budget share, which a priv
that stopped staging kept until its last close and now gives back at once.

**Gates.** ci-smp-gate N=10 50/50 PASS (5 rows, no corruption); `check-net-poll.sh` clean; ls-ci NOT RUN (7.7 GiB free disk, a CI bake would not fit; owed)
