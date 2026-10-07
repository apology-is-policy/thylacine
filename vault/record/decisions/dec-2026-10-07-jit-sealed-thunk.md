---
id: dec-2026-10-07-jit-sealed-thunk
type: dec
title: "B-2b: the engine's write thunk is born sealed -- SYS_JIT_CREATE_SEALED copies the bytes into an execute-only region no writer ever names"
date: 2026-10-07
status: standing
decided-by: user-vote
affects: [sub-kernel-burrow, sub-kernel-syscall-abi, sub-kernel-fault, sub-kernel-mmu]
created: 2026-10-07
---
## Fork

The operator voted on 2026-09-28 to harden the JIT writer alias inside B-2,
with an execute-only page class for JavaScriptCore's write thunk. The thunk
carries the writer alias's base as `movz`/`movk` immediates, so if its page
cannot be read, no readable memory names the writer. What the vote left open
was the ABI that makes such a page: a code region today always has a writer
alias, and the exec alias is RX.

## Research

- **Apple (the design JSC ships).** The thunk is generated into the first page
  of the executable reservation, then `vm_protect`ed execute-only with
  `set_maximum`, so it can never be made readable again. The writer base exists
  only as immediates in that page (`ExecutableAllocator.cpp:204-277`, the B-2
  re-verification).
- **OpenBSD.** `xonly` text: user mappings that are executable are not
  readable, and the kernel's user copies are unprivileged, so a syscall cannot
  read them either.
- **Linux.** `PROT_EXEC`-only user mappings were added on arm64 and reverted,
  because the kernel could still read them through ordinary user copies under
  PAN; they returned only with EPAN (ARMv8.7).
- **The tree.** `AP[2:1]=10` with `UXN=0` and `PXN=1` is EL0 execute-only on
  ARMv8.0. Every kernel read of a user VA goes through `uaccess.S` (ten
  fault-point instructions), the debugger's `cross_proc_resolve`, or a
  type-gated direct-map path (Loom and Weft accept only ANON). No PAN is in
  use.

## Options

1. **Born sealed.** A new `SYS_JIT_CREATE_SEALED(src, len, out_exec_va)`,
   `CAP_JIT`-gated: the kernel copies the caller's bytes into a fresh code
   region, invalidates the I-cache and maps only an execute-only alias. No
   writer ever exists; no transition on a live region.
2. **Seal an existing region.** `SYS_JIT_SEAL(writer_va)`: unmap the writer
   and turn the exec alias execute-only, one-shot (Apple's write-then-protect
   shape). Adds a transition with a half-sealed failure and a seal racing a
   sibling's write.
3. **Generalise protect.** Let `SYS_BURROW_PROTECT` drop READ on a code alias.
   The most general, and it reopens protect on CODE, which one region's two
   aliases make unsafe.

## The call

Option 1 (operator, 2026-10-07, AskUserQuestion). `SYS_JIT_DESTROY` releases a
sealed region by its exec VA.

## Rationale

A region that never had a writer has no window in which one exists, no
failure that leaves a half-sealed region, and no race between sealing and a
sibling thread's write. The copy-in reads a readable user buffer once, which
the engine zeroes afterwards; the same exposure exists in the other options
(the writer alias is readable memory until the seal). Execute-only only holds
if no kernel path reads the page for EL0, so the same chunk moves the user
copies to `LDTR`/`STTR` and makes the debugger's reader refuse a leaf EL0
cannot read.
