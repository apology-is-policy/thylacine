---
id: dec-2026-10-13-icache-sync-space
type: dec
title: "B-2e: the address space's CPUs are synchronized by their own syscall, SYS_ICACHE_SYNC_SPACE (128, no arguments), acknowledged by ticket"
date: 2026-10-13
status: standing
decided-by: user-vote
affects: [sub-webkit, sub-kernel-syscall-dispatch]
created: 2026-10-13
supersedes: dec-2026-10-13-wasm-space-sync
---
## Fork

[[dec-2026-10-13-wasm-space-sync]] decided that the kernel context-synchronizes
every CPU running a thread of the caller's address space, as a mode of
`SYS_ICACHE_SYNC`. Designing it (B-2e2) raised three questions that record did
not answer: how a caller selects the mode, which number a new call takes, and
what counts as a CPU's acknowledgement.

## Research

- **The selector.** `SYS_ICACHE_SYNC`'s dispatcher passes only `x0` and `x1`
  (`kernel/syscall.c`), and JavaScriptCore's `cacheFlush` (webkit patch 0001)
  sets only `x0`, `x1` and `x8`, so `x2` is garbage at every existing call: a
  mode bit there would be read from whatever the caller left. Linux made the
  same service its own syscall (`membarrier`).
- **The number.** The native space ends at 127 (`SYS_JIT_CREATE_SEALED`). The
  vivarium's collision argument for every Linux row above 127 is the native
  ceiling (`kernel/vivarium.c`), and a phenotype Proc's numbers are decoded
  before native dispatch (`syscall_dispatch_body`), an unclassified one landing
  on `ENOSYS`. Linux's 128 is `restart_syscall`, which already has a mandatory
  `ENOSYS` row. An unlanded peer branch (`codex/astra`) numbers its own calls
  127-131; its 127 already collides with main's `SYS_JIT_CREATE_SEALED`, so it
  renumbers on merge whichever number this call takes.
- **The acknowledgement.** `gic_synchronize_cpu` (`arch/arm64/gic.c`) reads a
  per-CPU count of barrier interrupts, sends, and returns when the count
  differs. An earlier sender's handler whose increment lands after that read
  satisfies it before the target takes the new interrupt. The ARM ARM's idiom
  for concurrently modified code is: invalidate, `DSB`, publish a flag; the
  other PE observes the flag, then executes a CSE. Linux's GICv3 acknowledge
  executes `DSB SY` after reading `ICC_IAR1_EL1` (`gic_read_iar_common`);
  Thylacine's executed it before only.
- **Heritage and SOTA** are unchanged from the superseded record: Plan 9's
  `procflushseg`, Linux's `membarrier(PRIVATE_EXPEDITED_SYNC_CORE)`; both wait
  without a bound.

## Options

- **A. A new syscall, `SYS_ICACHE_SYNC_SPACE`, no arguments** (the call).
- **B. `SYS_ICACHE_SYNC(0, 0)` as the mode.** No new number; a zero length is
  refused today, so the meaning is free.
- **C. A flags word in `x2`.** Reads garbage from every existing caller.

## The call

A (operator vote, 2026-10-13). The substance of
[[dec-2026-10-13-wasm-space-sync]] stands: option A there, the address space's
CPUs only, replacing JavaScriptCore's suspend-every-thread walk. By design, not
vote: the number is 128, the next native number, with `restart_syscall`'s
collision check made per-number; the acknowledgement is a ticket (the handler
reads its CPU's request count and stores it as done; the sender waits for its
ticket), and `gic_synchronize_cpu` moves to it too; the wait has no timeout and
yields between checks; the call needs no capability. The mechanism and its
ordering argument are `docs/JIT-ON-WX-DESIGN.md`, B-2 item 4.

## Rationale

B overloads an argument check with a meaning, and C reads registers no caller
sets. A ticket is acknowledged only by a handler that read a request made after
the invalidation completed, which is what the ordering argument needs; a count
of interrupts proves only that some interrupt happened. A timeout would turn a
descheduled CPU into a fatal error in JavaScriptCore, so contention would
change the outcome rather than the duration. The call interrupts only CPUs
running the caller's own threads, so it confers nothing a capability would
need to bound.
