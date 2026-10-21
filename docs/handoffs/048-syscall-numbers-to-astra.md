# Handoff 048 -- main has taken native syscalls 127 and 128 (to Astra)

**From**: main, 2026-10-13. **To**: Astra. **Why you**: `codex/astra`
(`5bb9f1772`) numbers five native syscalls 127-131 and moves
`VIV_NATIVE_CEILING` to 131. Main already uses 127 and is taking 128, so all
five of yours renumber when you next merge main, and the collision arguments
you wrote for 128-131 move to the numbers you land on. One of those arguments
also conflicts with an operator vote.

## What main has

- **127 = `SYS_JIT_CREATE_SEALED`** (B-2b, on main since 2026-10-07). Your
  `SYS_SRV_REGISTRY_NEW = 127` collides with it today; `codex/astra` does not
  contain it yet.
- **128 = `SYS_ICACHE_SYNC_SPACE`** (B-2e2, landing now): no arguments;
  interrupts every other CPU running a thread of the caller's address space
  and returns once each has acknowledged. Operator vote 2026-10-13
  (`dec-2026-10-13-icache-sync-space`); design in `docs/JIT-ON-WX-DESIGN.md`,
  B-2 item 4. Main's ceiling becomes 128, and `restart_syscall`'s collision
  check (Linux 128) becomes per-number: its `ENOSYS` row stays mandatory.
- Main also changes `arch/arm64/gic.c` in B-2e2: the barrier IPI is
  acknowledged by ticket, and `gic_synchronize_cpu` keeps its signature.

## What your merge needs

1. Renumber your five calls above main's top at the time you merge (today
   that is 129 onward), in append order. The `SYS__NATIVE_TOP` sentinel
   assert in `kernel/vivarium.c` fails until `VIV_NATIVE_CEILING` matches the
   new top, as designed.
2. Each new number at or below the new ceiling owes a per-number collision
   argument against the Linux row at that number. From 129 on, those are
   `kill` (129), `tkill` (130), `tgkill` (131), `sigaltstack` (132) and
   `rt_sigsuspend` (133), and above that the rest of the signal family
   (`vivarium.h`). Your current asserts pin `VIV_LINUX_KILL ==
   SYS_HANDLE_FLAGS_SET` and the like; they fail on the renumber and must be
   re-derived, not just re-pointed.
3. **Vote 6 (operator, 2026-10-13): the Linux phenotype gets `kill`, `tkill`
   and `tgkill` rows**, each reusing the cross-Proc gate Pouch's `kill`
   already passes, never a third gate (`vivarium.c`'s signal-family comment
   says the same). Your branch makes those three rows mandatory `VIV_ENOSYS`
   with the comment "no signal authority is implemented". The rows stay
   consumed before native dispatch either way, so your collision argument
   survives, but an assert or comment that pins the rows' **kind** to ENOSYS
   will be wrong when main's signal chunk lands them as Tier-2 rows. Pin that
   each row is present and consumed before the native switch, not that it
   is ENOSYS.

Reply in a handoff or on yip if a number is contested; whichever branch
lands first keeps the number, which is the append-only rule the space runs
on.
