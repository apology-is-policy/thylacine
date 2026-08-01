---
id: seam-70-tcg-watchpoint
type: seam
title: "QEMU TCG never raises EC 0x34, and a watched access wedges the vCPU"
status: open
surface: [sub-substrate-machine, sub-substrate-gates]
opened-by: chg-2026-08-01-substrate-sweep
tracker: "#70"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Nothing in Thylacine. This is an emulator gap, and it is recorded so the
workaround's polarity is never quietly inverted.

## The gap

QEMU TCG programs `DBGWVR`/`DBGWCR` but never raises EC 0x34. Worse than
silence: a guest thread that touches a watched page then spins inside the
emulator's retry of that ONE instruction. It takes no timer IRQ, so it never
reaches the EL0-return tail and therefore **cannot be killed** — under TCG's
round-robin that wedged vCPU starves the guest and the boot never finishes.

The kernel is correct here; the same encoding fires on real silicon and
under HVF. So the only safe move is to not arm a watchpoint on that
substrate at all.

## The workaround, and the property that must not be lost

`run-vm.sh` appends `thylacine.nowatchpoint` to `/chosen/bootargs` when and
only when accel is TCG; the guest reads it back through the `/hw` FDT mount,
so no kernel cmdline parser is required. `debug-probe` bounds its wait and
prints a SKIP there instead of hanging.

**The polarity is the design.** ABSENCE of the token means "watchpoints
work", so every substrate that is not TCG keeps the hard assertion — and
`test.sh` enforces the fire on any accel that can deliver, failing the boot
if `debug-probe: hwwatch ok` is missing. A real regression on the I-39 debug
surface therefore cannot hide behind the emulator's gap, and a NEW substrate
cannot silently inherit the exemption by default.

## Risk while open

TCG runs (which is LS-CI's default accel) get no watchpoint coverage. HVF
runs do, and that is where `test.sh` asserts it.
