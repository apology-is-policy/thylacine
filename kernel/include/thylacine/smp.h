// SMP — secondary CPU bring-up via PSCI (P2-Ca).
//
// Per ARCHITECTURE.md §20: SMP up to 8 cores at v1.0; per-CPU data is
// the default; cross-core operations are explicit IPIs (P2-Cd).
//
// P2-Ca scope: bring secondaries up via PSCI CPU_ON, each runs a
// minimal asm trampoline that flips a per-CPU online flag + parks
// at WFI. NO per-CPU MMU, NO per-CPU PAC, NO per-CPU vector table —
// those land in P2-Cb (per-CPU run trees + per-CPU init), P2-Cc
// (per-CPU exception stacks). Secondaries at P2-Ca are observably
// alive but cannot execute kernel code beyond the trampoline.
//
// CPU index space: 0 = boot CPU; 1..N-1 = secondaries. The DTB
// cpu@N reg property is the MPIDR aff value used for PSCI_CPU_ON.

#ifndef THYLACINE_SMP_H
#define THYLACINE_SMP_H

#include <thylacine/dtb.h>
#include <thylacine/types.h>

// Per-CPU online flag. Set by secondary's trampoline at the very
// start of its execution (after PSCI bring-up); read by the primary
// in smp_init's wait loop. `volatile` so the compiler doesn't hoist
// the read out of the polling loop. Also published with dsb sy on the
// secondary side; cache coherence on QEMU virt is automatic in the
// inner-shareable domain.
//
// Indexed by cpu index (0..N-1). Boot CPU sets g_cpu_online[0] to
// true at smp_init entry; secondaries set their own slot in the asm
// trampoline.
extern volatile u8 g_cpu_online[DTB_MAX_CPUS];

// P2-Cb: per-CPU "fully initialized" flag. Set by per_cpu_main at the
// kernel's high VA AFTER PAC apply + MMU enable + VBAR install +
// TPIDR_EL1 set. Distinct from g_cpu_online (which is set in the
// low-PA trampoline before MMU is on) — together they let smp_init
// distinguish "trampoline reached" from "fully alive at high VA."
extern volatile u8 g_cpu_alive[DTB_MAX_CPUS];

// P2-Cb: per-CPU PAC keys (8 u64 halves: apia/apib/apda/apdb hi+lo).
// Derived ONCE on primary by `pac_derive_keys` (asm in start.S) from
// CNTPCT_EL0 + ROR chain. Loaded by `pac_apply_this_cpu` (asm in
// start.S) on every CPU — primary first, then each secondary as it
// comes up. Cross-CPU PAC consistency is REQUIRED for thread
// migration: a thread's signed return address on its kstack must
// auth-validate against APIA on whichever CPU resumes it.
//
// Layout (matched by pac_derive_keys / pac_apply_this_cpu in start.S):
//   [0]  apia_hi   [1]  apia_lo
//   [2]  apib_hi   [3]  apib_lo
//   [4]  apda_hi   [5]  apda_lo
//   [6]  apdb_hi   [7]  apdb_lo
extern u64 g_pac_keys[8];

// P2-Cb: per-secondary boot stacks. Used by the asm trampoline before
// per_cpu_main switches to a real per-CPU thread stack (P2-Cd or
// later when the scheduler picks an idle thread for this CPU).
//
// One stack per secondary (DTB_MAX_CPUS - 1 = 7). 16 KiB each. 16-byte
// aligned (AAPCS64 SP requirement).
#define SECONDARY_STACK_SIZE  16384u
extern char g_secondary_boot_stacks[DTB_MAX_CPUS - 1][SECONDARY_STACK_SIZE];

// P2-Cb: per-CPU main. Called from secondary_entry asm trampoline at
// the kernel's high VA after PAC + MMU + per-CPU stack are live.
// Sets VBAR_EL1 to the kernel vector table, TPIDR_EL1 to NULL (no
// per-CPU current thread at P2-Cb), flips g_cpu_alive[idx], and
// enters an idle WFI loop indefinitely. Noreturn.
//
// Visible publicly so the asm trampoline's adrp+add resolves it.
__attribute__((noreturn))
void per_cpu_main(int cpu_idx);

// Bring up all secondary CPUs (indices 1..dtb_cpu_count()-1) via
// PSCI_CPU_ON. Waits for each to set its online flag with a per-CPU
// timeout. Logs each bring-up via uart.
//
// Preconditions:
//   - dtb_init() succeeded.
//   - psci_init() returned true.
//
// Returns the number of secondaries that came online (0..N-1). On
// PSCI failure or timeout for a specific secondary, that secondary
// is skipped and counted as offline; the function continues with the
// next secondary.
//
// Idempotent only in the trivial sense (don't call twice). PSCI
// returns ALREADY_ON for a re-call which we treat as success.
unsigned smp_init(void);

// Number of CPUs total (including boot). Mirrors dtb_cpu_count() but
// caches the result. Returns 0 before smp_init runs.
unsigned smp_cpu_count(void);

// Number of CPUs currently online (= 1 + # secondaries that came up).
unsigned smp_cpu_online_count(void);

#endif // THYLACINE_SMP_H
