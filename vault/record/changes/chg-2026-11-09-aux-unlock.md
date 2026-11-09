---
id: chg-2026-11-09-aux-unlock
type: chg
title: "An unlock that never left for want of memory is sent again: the aux join's round 3 F2, bounded where a kill cannot reach"
date: 2026-11-09
arc: arc-phase6-pouch
commits: ["43c1916d9", "25321863c", "54341523b"]
touched:
  - sub-kernel-ninep-client
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-attach
  - sub-kernel-dev
  - sub-kernel-filelock
  - sub-substrate-gates
established: []
closed: []
opened: []
mirrors-checked:
  - "kernel/include/thylacine/9p_client.h: p9_client_unlock gains max_resends, P9_E_UNSENT is kernel-internal; 9P and syscall wire unchanged"
depth: skeletal
no-dossier-change: "the note changes no code; the touched dossiers were co-staged with the code in 43c1916d9, 25321863c and 54341523b"
created: 2026-11-09
---
The aux join's audit round 3 F2 ([[chg-2026-11-06-aux-join2]]): a Tlock UNLCK whose reply buffer (`kmalloc(recv_cap)`) or spill could not be allocated never left, and on a live session the range stayed held, for the Proc's life in the post-grant re-check's case while the call answered EBADF. Branch aux-3-unlock, off aux-3-tidy2.

**The fix (43c1916d9).** `client_run_ex` reports `*unsent` at its two take-back sites, and `p9_client_unlock` sends such a frame again until the server answers, synchronously. The design recorded in OPEN-BUGS (an owed range kept on the lock owner, paid later) was rejected: paying later is the trap, since a program told EBADF may take the range again at once. The rejection is in the commit body and [[sub-kernel-filelock]].

**Round 1 (Fable 5.1, 0/0/0/3, plus the author's two P2s) and its fixes (25321863c).** SF1: an exit's close waited for memory only its own reap frees, so it now sends once (`lock_unlock_resends`, keyed on `exit_close_active`) and leaves the rest owed, for a closer or the last reference's clunk. SF2 + F2: the closer's job resent without bound, silently, so it is now bounded at the pool's own rule (`P9_RESEND_CLOSER`, 21 sends, 1023 ms) and gives up loudly (`9p: close: unlock of fid N failed rc R`, `unlocks_lost`, which `tools/test.sh` fails on). The reviewer proposed unbounded-and-reported; the pool's rule is the precedent. F1: the identity learn before a close's whole-file unlock resends too (`p9_client_getattr_patient`, `tsleep_noteintr`). F3: an abandon latch for the legs.

**Round 2 (Fable 5.1, 0/0/0/4, clean) and its fixes (54341523b).** F1: "never left" was `-P9_E_AGAIN`, which a server's Rlerror(11) also produces; now `P9_E_UNSENT`, outside every ecode, and a flushed unlock is told by the call's mark (`wb_note_cancelled`). F2: the closer reported a server's refusal as a loss, and Stratum's ESTALE for a file unlinked under the lock would have failed a gate; a refusal is now an answer everywhere. F3: the new verdict arm got its red (U13). F4: the Dev contract names the closer arm.

**Leases.** runs-1 (43c1916d9): green 2426, U01-U05 red, regreen == green. runs-2 (25321863c, 2026-11-09 05:22-05:49Z): green 2429 plus guest legs, U01-U11 each red at its leg, regreen == green; U12 did not run (its anchor was edited mid-lease). Runs-3 (54341523b, 06:49-07:02Z) green 2431 plus guest legs with the nine unlock legs, U12-U16 each red at its leg (U13 at tools/test.sh's new verdict line, its leg passing), regreen == green (c15ef522807edc30). U15's kernel printed the line; test.sh judges it only on a passing suite, so U13 is that arm's red.

**Not run:** the full SMP matrix (held since 2026-11-02), the interactive fleet. No spec models the lock path.
