---
id: fnd-pouchb0-r2-f1
type: fnd
title: "ualarm() still returns uninitialised stack -- a sixth member of the class 0037 says it swept, and the closed list states the opposite"
round: adt-pouchb0-r2
severity: P2
status: fixed
surface: [sub-pouch-seam]
threatens: []
fixed-by: chg-2026-09-21-pouch-b0-libc
regression: "pouch-hello-malloc: errno pre-loaded, ualarm(1000, 0) == (useconds_t)-1 && errno == ENOSYS"
created: 2026-09-21
---
## Prosecution

**File**: upstream `src/unistd/ualarm.c:7-12`; `src/signal/setitimer.c`; `bits/syscall.h.in` (`__NR_setitimer 0xFFFF`)
**Invariant**: Pouch P-3; 0037's own header ("five libc functions stop reporting uninitialised stack")
**Prosecution**:
1. `}, it_old;` / `setitimer(ITIMER_REAL, &it, &it_old);` / `return it_old.it_value.tv_sec*1000000 + it_old.it_value.tv_usec;` -- `it_old` has no initialiser.
2. `setitimer` is the sentinel: -ENOSYS before the trap, `old` never written.
3. `ualarm()` returns stack residue as "microseconds left on the previous alarm" and arms nothing. `alarm()` is saved only because upstream wrote `old = { 0 }`.
4. The dossier's prescribed half-(b) sweep finds it in one grep; run over the whole patched `src/` (a named-wrapper pass and a generic pass over 213 always-failing wrappers) the only out-struct readers are `ualarm.c` and `alarm.c`.
5. The closed list said "`alarm/ualarm` return 0 and arm nothing" -- false for `ualarm`.
**Suggested fix**: `it_old = {0}` and report the failure.

## Disposition

Fixed in 0037: zero-initialised, and `(unsigned)-1` with the wrapper's ENOSYS when the timer cannot be set (glibc's answer). The closed list's sentence corrected.
