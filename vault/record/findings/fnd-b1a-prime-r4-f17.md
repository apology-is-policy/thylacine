---
id: fnd-b1a-prime-r4-f17
type: fnd
title: "An EL0 abort that is neither a translation, an access-flag nor a permission fault -- an alignment fault or a synchronous external abort on a mapped page -- is fed to the demand pager and answered as handled, so the instruction re-faults forever where the Proc owes a snare:bus death"
round: adt-b1a-prime-r4
severity: P1
status: fixed
surface: [sub-kernel-fault]
threatens: [inv-i32]
fixed-by: chg-2026-09-23-b1a-prime-close-r4
regression: "demand_page.alignment_abort_is_bus_not_handled"
created: 2026-09-23
---
## Prosecution

`fault_info_decode` sets `is_translation` for FSC 0x04-0x07, `is_access_flag`
for 0x09-0x0B and `is_permission` for 0x0D-0x0F, and nothing consumes a
fourth class. An alignment fault is FSC 0x21 (an exclusive, an ordered load
or store, or a Device access at an address the instruction cannot take --
ARM ARM D5.10.3, regardless of `SCTLR.A`), a synchronous external abort 0x10
or 0x14-0x17: all three flags false. `arch_fault_handle` dispatches an EL0
abort on `from_user` alone, and `demand_page_locked` never reads the class.
Chain: a program executes `ldxr w0, [x1]` with x1 misaligned on a page it
has already touched (a valid leaf that admits the access). EC 0x24, DFSC
0x21 -> `arch_fault_handle` -> `userland_demand_page` -> `as->lock` -> the
VMA found, the prot admits -> step 2b: the leaf admits the access ->
`FAULT_HANDLED` -> ERET to the SAME instruction -> the same abort. Before
round 3 the same fault reached step 5 and the idempotent install returned 1
for the identical leaf -> the same loop. The Proc spins at one CPU's worth of
exception round-trips, taking and releasing `as->lock` every iteration (its
sibling threads' faults queue behind it), with no diagnostic and no exit;
preemptible and killable only because the EL0 return tail delivers notes.
Linux delivers SIGBUS for both classes (`do_alignment_fault`, `do_sea`); the
kernel-mode uaccess entry (`exception.c`) already admits only the three
classes into the pager, the EL0 path had no such gate. Pre-existing since
P3-Dc; I-32's "fail clean" (a fault the kernel cannot resolve terminates the
Proc) is violated by a livelock.

## Fix

`fault_info` gains `is_alignment` (FSC 0x21) and `is_external` (0x10,
0x14-0x17), decoded beside the three classes, and the top of
`userland_demand_page` -- before `as->lock` and any lookup -- returns
`FAULT_USER_BUS` when none of `is_translation` / `is_access_flag` /
`is_permission` holds: the class decides, not the page. The EL0 dispatcher's
`FAULT_USER_BUS` arm posts `snare:bus`. Witnesses:
`demand_page.alignment_abort_is_bus_not_handled` (a synthetic 0x21 and a
0x10 on a mapped, admitting page -> `FAULT_USER_BUS`; CONTROL: a translation
fault on the same VA -> `FAULT_HANDLED`) and `/bus-probe-child` from EL0
(joey reaps it with the expect-fault census: the marker and a non-zero
status). That child's first draft did `ldar` from `va + 1` and SURVIVED on
the Apple core under HVF: FEAT_LSE2 with `SCTLR_EL1.nAA = 0` (the kernel never
sets it) permits a misaligned ordered access inside one 16-byte quantity and
faults only across a boundary; exclusives keep their natural alignment under
every rule, so the witness is an `ldxr` at offset 14, both misaligned and
crossing, and it dies as owed. RED `nobusgate` (the gate removed) reddens
the unit test and would hang the boot at joey.
