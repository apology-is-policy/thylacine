---
id: dec-2026-11-14-power-off
type: dec
title: "Power-off is a host-owner kernel control file written by a userspace fshalt, which the power-key driver starts"
date: 2026-11-14
status: standing
decided-by: user-vote
affects: []
created: 2026-11-14
---
## Fork

Quit presses the `virt` machine's power button, and a new guest power-key driver turns the press
into an orderly shutdown (vote M12, `docs/MAC-RUNNER.md` 4.4). Building it needs a way for the
guest to power itself off at the end, and the tree has none: the kernel defines PSCI's
`SYSTEM_OFF` and `SYSTEM_RESET` (`arch/arm64/psci.h`) and calls neither.

## Research

- Plan 9: `fshalt(8)` syncs and halts the file servers, then the machine stops; the console
  device's `reboot` file (cons(3)) restarts it, and only the host owner may write it.
- Linux: `reboot(2)` behind CAP_SYS_BOOT, called by init after it stops the services.
- The tree: only the kernel can make a PSCI call, and stratumd must be synced and halted
  before power goes, or its last writes are lost.

## Options

1. A kernel control file only the host owner can open: `off` -> PSCI `SYSTEM_OFF`, `reboot` ->
   `SYSTEM_RESET`. A userspace `fshalt` stops the services, syncs and halts stratumd, then
   writes `off`. The power-key driver starts fshalt.
2. A power syscall behind a capability.
3. The key driver sends a note to the service supervisor, which shuts down.

## The call

The operator, 2026-11-14: option 1, as recommended (vote M16).

## Rationale

It is Plan 9's shape: the power is a file, its authority is the host owner's, and the orderly
part is a program anyone can read. A shutdown from the guest's shell takes the same path as
Quit. The control file is a kernel surface gated on the host owner, so its audit-trigger row
and its audit round land with it.
