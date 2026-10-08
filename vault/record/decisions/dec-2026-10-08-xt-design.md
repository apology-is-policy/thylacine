---
id: dec-2026-10-08-xt-design
type: dec
title: "XT: x86 programs run as ordinary processes -- a native translator, a hosted decode into Vivarium, exact faults, image entitlement, translations as Stratum text (F1-F9)"
date: 2026-10-08
status: standing
decided-by: user-vote
affects: [sub-kernel-exec, sub-kernel-elf, sub-kernel-notes, sub-kernel-exception, sub-kernel-burrow, sub-kernel-addrspace, sub-kernel-vivarium, sub-kernel-syscall-abi, sub-kernel-thread, sub-kernel-proc, sub-kernel-hwcap, sub-kernel-caps]
created: 2026-10-08
---
## Fork

The operator wants x86 software in full: Linux amd64 and i386 programs, and
Windows programs through Wine, running first class the way Rosetta runs on
macOS. Cost does not matter, invasive redesign is allowed, and the result must
keep Thylacine's tenets. `docs/WINE-STUDY.md` found that every route to x86
runs through a translator and a handful of missing kernel primitives. The
design (`docs/X86-TRANSLATION-DESIGN.md`) left nine forks, F1 to F9 in its
section 10.

## Research

- **Heritage.** Plan 9's `$objtype` trees and `bind`, `cpu(1)`, `#!`
  interpreters, shared text, and `notify`/`noted` with a Ureg the handler may
  edit (`notejmp`).
- **SOTA.** Rosetta 2's exec integration, AOT cache and hardware TSO;
  Windows on Arm's `BTCpu*` emulator DLL and XtaCache; FEX's frontends and
  thunks; Box64; Fuchsia's VMEX resource; macOS's image-bound `allow-jit`
  entitlement; Edge's out-of-process JIT.
- **The tree** (at `04df02c9`). The loader accepts only `EM_AARCH64`; every
  EL0 fault terminates the Proc and `NCONT` ignores handler edits; runtime
  mappings live above 4 GiB; B-2's code Burrows are lazy, randomly placed, and
  a code alias counts as `CAP_JIT` in I-39's image join; a fork of an address
  space holding a code region fails whole (`kernel/addrspace.c:204-208`).

## Options

Each fork's options, precedent and cost are in the design's section 10. The
residue the research could not settle was F1 (where the guest's Linux
semantics live), F3 (code-emission authority), F4 (the store) and F9 (spec
posture).

## The call

All nine as recommended (operator, 2026-10-08, AskUserQuestion):

- **F1 A2:** a native translator plus a hosted second decode (`svc #GUEST`)
  into Vivarium's single Linux personality, with partitioned handles and
  memory.
- **F2:** in-thread exact faults: the Ureg is handed to the handler, and
  `noted(NCONT)` resumes at the validated, edited context.
- **F3:** image entitlement (`code-emit`) from a host-owned objtype table,
  counting as `CAP_JIT` in I-39's join and never stamped on a debug-tainted
  load.
- **F4:** translations as Stratum files, per user plus a system store for
  system trees.
- **F5:** per-Proc address-space shapes chosen at image load.
- **F6:** FEX as the engine, Box64 evaluated for ARMv8.0, the kernel
  engine-agnostic.
- **F7:** the SVC immediate as the second-decode selector.
- **F8:** a per-thread memory-model primitive where the hardware has TSO.
- **F9:** spec-first re-enabled for `fault_note.tla` and
  `hosted_decode.tla`.

F10 was voted separately: `dec-2026-10-08-xt-guest-code`.

## Rationale

The kernel never learns x86 (VISION 3.4), and Linux semantics stay
single-homed in Vivarium, while the translator stays a native citizen that can
bridge to native SDL, Venus and OSMesa. Every first-class translator surveyed
is exec integration plus a persistent shared cache plus a hardware-aware
memory model, and all three are OS work, which this design does as Plan 9
would: objtype as a namespace declaration, translated code as shared text,
and faults as notes the handler may edit.
