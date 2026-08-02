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
the four listed places were updated, and the document that stated the
coordination requirement is the one that fell out of it. The ABI's home is now
`vault/system/boundary/registries/abi-boot-banner.md`, which has it right.

Also stale: the framing throughout is P1-A ("no scheduler, no MMU, no devices
beyond the polled UART; the kernel halts in a `wfi` loop after the banner"),
which describes neither the sequence nor its ending — boot hands off to an
init process that never exits, and the success line is printed on that
process's explicit signal rather than at the end of `boot_main`. The
`_torpor` entry in the symbol table cites a stale line number.

The invariants live at `vault/invariants/inv-i16.md` (randomized, never-zero
kernel base) and `inv-i21.md` (uniform EL1h, established here at the first
stack write); I-12 and I-15 are cross-referenced from the dossiers. The open
debt is `seam-kaslr-link-va-unchecked` (task #24). Design scripture is
unchanged: `ARCHITECTURE.md section 5`, `TOOLING.md section 10`.
