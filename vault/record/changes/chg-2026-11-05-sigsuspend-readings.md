---
id: chg-2026-11-05-sigsuspend-readings
type: chg
title: "Sigsuspend's readings: a fixture the guard voided, six witnesses that could not fail, and two with none"
date: 2026-11-05
arc: arc-phase6-pouch
commits: [ea0d92b15, 982d66e93]
touched:
  - sub-kernel-notes
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-05
---
The first boot, at `4ddc2f124`, was 2256 of 2257. `notes.restart_settle` failed `the pc` once for each of its three arms that expect a rewind. `087d56d79` had given `notes_call_settle` the guard `notes_tail_refuses_sp`, and the test's zeroed context has an sp of 0, which the guard refuses. The kernel was right. The arms that expect no rewind had been passing for that wrong reason since `087d56d79`. `ea0d92b15` gives the fixture a stack and adds an arm that pins the refusal.

The full run at `ea0d92b15` applied 83 sabotages, and six could not go red on the witness written for them. `982d66e93` changes each:

- K13: the fixture had no caught note, so it never read the caught latch.
- K23: the boot thread's Proc has no table, so `notes_wait_suspend`'s own gate answered EINVAL for the syscall's check.
- K31: the call's end restores whatever its start saved, so the mask read after the call could not tell. The saved slot is read now.
- K35: the own-bit leg's note was also in the queue, so the queue's loop reported it. Only the guest caught this sabotage.
- G16, G17: the tail restores the mask as well, so with no note arriving the end of the no-fds form cannot be seen. Probe leg L372 posts a note only the call's mask holds during the sleep; the handler must run at the return. Each sabotage then read `marker=L372`, `ws=19456`, the child's exit 76.

At `982d66e93` K13, K23, K31 and K35 each fail on their new message, and K25 fails both `notes.restart_settle` and `notewait.suspend_bad_sp_keeps_eintr`.

Two have no witness. K17, the look after an unwind: a post's own list wake ends the wait first, so that branch needs another CPU between a post's enqueue and its wake. G22, an interrupted readiness-file open: netd's reply beat the flush on this machine, as round 2 said it might. L371 stands with the open won, and whether G22 reds is a race. Neither is proved by a run.

Some sabotages went red on a witness other than the one first named, which the lease's lines now state: K20 on `take_sleeps_until_posted`; K38 only on the guest legs, because the kernel fixture sets the flag itself; G06, G07 and G10 in kernel tests, so the guests did not run; P01 on `pouch-hello-threads`' mask leg, before the signals prover.

The specs ran on the final tip. `tail_order`: clean at 46292 and 68418, the four safety cfgs each on their own invariant. Its two temporal cfgs read rc 13 at their pinned 18044 and 26886, but `specs/check-tail-order.sh` prints FAIL for them, because the 2.19 jar on this machine does not name the property in its message. Each cfg has one property, so they were read by hand. `caught_target`: clean at 613245, 807792 and 434763, and the 15 buggy cfgs each violate, at one worker. Their temporal messages name no property on this jar either, so each is read by rc and the cfg. `caught_target_3t` did not run: it has never finished on this host.
