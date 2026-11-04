---
id: dec-2026-11-04-dormant-answers-estale
type: dec
title: "A dormant present and HIN1's BadHandle answer ESTALE"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-tapestryd, sub-libhalcyon, abi-errno]
created: 2026-11-04
---
## Fork

Vote 16 of 2026-10-21 has the 9P client read a server's `EBADF` as `EIO` (`map_error`, `kernel/9p_client.c`). On corona's side of the integration two things read that value. `tapestryd`'s present on a dormant pixel generation answered `EBADF`, and `kaua-term-probe`'s storage checks asserted -9, so `tapestry-storage.exp` would go red on every boot. HIN1's `BadHandle` was `EBADF`, so on the join `libhalcyon` would never see it. Merge audit r3 raised it as F1, and `docs/ERRORS.md` said no code read a server's `EBADF`, which the join made false.

## Research

The audit's census of flat -1 and numeric errnos across `usr/` found `kaua-term-probe/src/storage.rs` the only reader of the value on the present's route. A second census for this change found no other literal -9 reading a server's answer: `viv-pheno-probe`, `alloc-smoke` and `loom-client`'s simulator read the kernel's handle gate. `ESTALE` (116, vote 28) is already "the file a fid named has died", crosses by value, and is read in the kernel only by the file-lock paths, which a present and a HIN1 record do not take.

## Options

Move both to `ESTALE`, keeping vote 16's fold. Or accept `EIO`: the probe expects -5, and `libhalcyon`'s `BadHandle` arm is dead, so a client cannot tell a refused fid from a lost link.

## The call

Vote 66 of 2026-11-04, the recommended option: move both to `ESTALE`. Vote 16's fold stays. `tapestryd`'s other `EBADF` answers stay and read `EIO`.

## Rationale

The client keeps the distinction it was written around. This changes an ABI table, HIN1's error values, but private activation is off by default and nothing outside this tree speaks HIN1 yet.
