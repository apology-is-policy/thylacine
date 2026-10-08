---
id: dec-2026-10-08-pac-keys
type: dec
title: "PAC keys: user keys per address space, a kernel APIA key per thread, APIA swapped on every EL0 crossing, no userspace without entropy (I-49)"
date: 2026-10-08
status: standing
decided-by: user-vote
affects: [sub-kernel-boot-entry, sub-kernel-exception, sub-kernel-addrspace, sub-kernel-thread, sub-kernel-sched, sub-kernel-sched-smp, sub-kernel-exec, sub-kernel-content, sub-kernel-hwcap]
created: 2026-10-08
---
## Fork

`pac_derive_keys` derives one key set (APIA, APIB, APDA, APDB) at boot from
`CNTPCT_EL0`, and `pac_apply_this_cpu` loads it on every CPU; nothing switches
it. The kernel and userspace both build with `-mbranch-protection=pac-ret+bti`.
So every EL0 process can `pacia`-sign values with the kernel's own
return-address key, and kernel pac-ret buys nothing against a local attacker
with a kernel-stack write; processes also share keys with each other
(`docs/WINE-STUDY.md` Appendix A, F1; task #5). ARCH 24.3 claimed the keys
were "not exposed to userspace", which was true of reading them and false of
using them.

## Research

- **Heritage.** Plan 9 predates PAC. Its model still applies: per-process
  state lives with the process, and the address space is the unit of
  isolation.
- **SOTA** (upstream sources read 2026-10-08):
  - Linux gives each task its own kernel APIA key (`thread.keys_kernel`,
    `copy_thread`). Its user keys are per task but copied at fork and clone and
    new at exec, so they are effectively per process. On every EL0 entry and exit
    it swaps only APIA ("the kernel does not use any keys besides IA"), and it
    loads IB/DA/DB/GA at a task switch.
  - FreeBSD does the same per thread (`ptrauth.c`). Its thread0 key is all
    zeros, a TODO.
  - OpenBSD keeps the user keys in the pmap, loads them at switch, and drops
    kernel pac-ret (BTI only).
  - XNU arm64e avoids entry swaps with Apple-only hardware diversifiers
    (`KERNKey`).
  - Fuchsia uses ShadowCallStack instead.
  - No kernel saves the swap by giving itself a separate key: one SCTLR enable
    bit covers EL0 and EL1.
  - Linux measured the cost of moving IB/DA/DB/GA off the exit path at 15.6 ns
    per round trip (M1, hypervisor); no figure exists for the bare APIA swap.
- **The tree.**
  - `struct AddrSpace` (LINEAGE L-1) is shared by threads, cloned by fork and
    replaced by exec.
  - The context switch already writes TTBR0 (`context.S`,
    `sched_activate_addrspace`).
  - `kern_random_bytes` fails closed until seeded, but exec's AT_RANDOM ignores
    the failure and hands out zeros (task #25).
  - hwfeat reads the boot CPU only, and each CPU enables PAC independently.

## The call

All three as recommended (operator, 2026-10-08, AskUserQuestion):

- **The kernel key is per thread** (Linux, FreeBSD). Each thread draws its own
  APIA key from the CSPRNG at creation, and it is loaded at every context
  switch. A signed kernel return address does not replay in another thread,
  even one whose kstack slot XT-3b's reaping recycled at the same address.
  Rejected:
  - one key per boot, which is cheaper at a switch but replayable across
    threads;
  - no kernel pac-ret, OpenBSD's choice, which would change ARCH 24.3's posture.
- **User keys are per address space.** All five are new at exec, copied at fork
  and shared by threads. APIA is swapped on every EL0 crossing; the other four
  load at an address-space switch.
- **No userspace without entropy.** The kernel does not launch init until the
  CSPRNG is seeded, and exec asserts it. Rejected:
  - failing each exec, which boots to a system that can run nothing;
  - falling back to the weak pool, which gives predictable keys.
- **A section-28 invariant, I-49:** kernel PAC keys never reach EL0, and an
  address space's keys are its own. It has an audit-trigger row and an EL0
  witness. The alternative was a hardening property with no audit trigger.

## Consequences

- **Chunks.**
  - PAC-0 is this scripture commit.
  - PAC-1 is the entropy gate and exec's fail-closed AT_RANDOM (task #25).
  - PAC-2 is the key model: user keys in `struct AddrSpace`, the kernel key in
    `struct Thread`, the swaps on entry and exit and at switch, and a secondary
    with different PAuth support refused.
  - PAC-3 is the EL0 witness, I-49 flipping to ENFORCED, and the audit round
    on the entry and exit paths (audit row 21 and the new PAC row).
- **Cost.** 4 MSR and 1 ISB per EL0 round trip, 2 MSR and an ISB per context
  switch, and 8 MSR at an address-space switch.
- **Left as it is.**
  - The boot-derived key of the threads created before seeding stays the
    tracked entropy deferral (handoff 005, item 34).
  - FPAC (exception class 0x1C) keeps its current mapping until ERRORS.md
    names one; a new `snare:*` name is an ABI change.
  - HWCAP_PACA/PACG for Linux guests is a later Vivarium decision.
