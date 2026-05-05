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
