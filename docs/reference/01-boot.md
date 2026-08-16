# 01 — Boot path [ABSORBED INTO THE VAULT]

This document was absorbed at the boot sweep (`chg-2026-08-02-boot-sweep`).
Its content now lives, code-verified and current, in the dossiers:

    vault/system/kernel/boot/sub-kernel-boot-entry.md
    vault/system/kernel/boot/sub-kernel-boot-sequence.md

(the Linux image header and why it is load-bearing rather than cosmetic, the
EL2 to EL1 drop, the `SPSel`-before-`mov sp` bank discipline, the BSS clear
and the per-CPU register initialization, PAC key derivation and cross-CPU
uniformity, the stack re-anchor and the long branch, `_torpor`, the linker
layout and its five build-time assertions, and — in the second dossier — the
roughly forty-step initialization order with the five positions where a move
is silently wrong rather than immediately fatal).

**What this file got WRONG by the time it was absorbed** (the reason the
dossiers are written from the code): it names the fatal-output ABI as
`PANIC:`. The actual prefix is `EXTINCTION:` and has been since the thematic
rename. The same sentence states that the format "does not change without
coordinated updates to `tools/run-vm.sh`, `tools/agent-protocol.md`,
`CLAUDE.md`, and `TOOLING.md`" — a list this document is not on. It changed,
and the document that stated the coordination requirement is the one that fell
out of it. The ABI's home is now
`vault/system/boundary/registries/abi-boot-banner.md`.

**Two corrections to the paragraph above, both mine, both found on
2026-08-16.** It said "the four listed places were updated" — two of the four
could not have been. `tools/agent-protocol.md` has never existed (planned in
Phase 1, never written; retired from the scripture on the user's vote at
main#244), and `tools/run-vm.sh` matches neither string, so there was nothing
in it to update. And it vouched for the successor — "which has it right" — a
claim about a *different file*, asserted while writing the redirect and never
re-checked. The registry carried the same phantom until the sweep that found
this. An absorption stub that certifies its destination is making the one kind
of claim it is worst placed to make.

Also stale: the framing throughout is P1-A ("no scheduler, no MMU, no devices
beyond the polled UART; the kernel halts in a `wfi` loop after the banner"),
which describes neither the sequence nor its ending — boot hands off to an
init process that never exits, and the success line is printed on that
process's explicit signal rather than at the end of `boot_main`. The
`_torpor` entry in the symbol table cites a stale line number.

**What was NOT absorbed, and is therefore owed** (found at the ledger
reconciliation, `chg-2026-08-02-absorption-reconciliation`): this document also
held the only account of **the PL011 driver itself** — `arch/arm64/uart.c`, 473
lines: the discovery of its register base from the device tree, the hardcoded
fallback covering the window before the tree is parsed, the register
programming, the receive interrupt, and the line-break detection the trusted
path's attention key rests on. The console dossier covers how the console
*uses* the UART — the rings, the full-FIFO back-pressure, the writer role — but
not the driver beneath it, and no other note does. Tracked as task #32.

The invariants live at `vault/invariants/inv-i16.md` (randomized, never-zero
kernel base) and `inv-i21.md` (uniform EL1h, established here at the first
stack write); I-12 and I-15 are cross-referenced from the dossiers. The open
debt is `seam-kaslr-link-va-unchecked` (task #24). Design scripture is
unchanged: `ARCHITECTURE.md section 5`, `TOOLING.md section 10`.
