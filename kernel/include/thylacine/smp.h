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

#include <stdint.h>
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

// P2-Cc: per-CPU exception stacks. SP_EL1 on each CPU points at the
// top of its own slot; the kernel runs in SPSel=0 mode (SP=SP_EL0) for
// normal work, and ARM exception entry hardware-switches to SP_EL1 =
// this per-CPU stack. Closes the P1-F shared-stack limitation: a
// stack-overflow fault on the kernel SP_EL0 stack now lands on a
// distinct SP_EL1 buffer, so KERNEL_ENTRY's `sub sp, sp, #...` runs
// on a known-good stack and exception_sync_curr_el's stack-overflow
// diagnostic is reachable instead of recursively faulting.
//
// 4 KiB per CPU × DTB_MAX_CPUS (8) = 32 KiB BSS. 4 KiB exceeds
// EXCEPTION_CTX_SIZE (288 B) by ~14× — generous headroom for handler
// frames + nested exceptions if they ever land. Sized to one page so
// the linker's stack alignment costs nothing extra.
//
// Indexed by cpu_idx (0 = boot, 1..N-1 = secondaries). Set up by
// start.S _real_start (CPU 0) and start.S secondary_entry (CPU 1+).
#define EXCEPTION_STACK_SIZE  4096u
extern char g_exception_stacks[DTB_MAX_CPUS][EXCEPTION_STACK_SIZE];

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

// P2-Cc: observability hook for the per-CPU exception-stack discipline.
// timer_irq_handler captures `&local` (the SP at C handler entry) into
// this slot on its first call from each CPU. The test
// smp.exception_stack_smoke checks that the captured address falls
// inside the corresponding g_exception_stacks[idx] slot.
//
// One slot per CPU. Indexed by smp_cpu_idx_self() at write time;
// readers also index by cpu_idx. Initial value 0 = "not yet observed."
extern volatile uintptr_t g_exception_stack_observed[DTB_MAX_CPUS];

// Returns this CPU's index from MPIDR_EL1.Aff0 (low 8 bits). Boot CPU
// reports 0; secondaries report 1..N-1. Valid at any context where
// MPIDR is readable (always, at EL1+). Used by per-CPU observability
// hooks (timer_irq_handler) and may be used as a per-CPU dispatch key
// in future sub-chunks.
unsigned smp_cpu_idx_self(void);

// P2-Cdc: IPI (Inter-Processor Interrupt) infrastructure.
//
// SGI INTID assignments. v1.0 P2-Cdc lands only IPI_RESCHED — used to
// wake a secondary's WFI when its run tree gets new work or when
// preempt_check_irq needs to fire on that CPU. P2-Ce / P2-Cf may add
// more (IPI_TLB_FLUSH for cross-CPU TLB invalidation; IPI_HALT for
// shutdown). ARCH §20.4 reserves these names.
#define IPI_RESCHED       0u    // SGI 0
// #define IPI_TLB_FLUSH  1u    // SGI 1 — P2-Ce or later
// #define IPI_HALT       2u    // SGI 2 — P2-Cd2+ shutdown
// #define IPI_GENERIC    3u    // SGI 3 — generic callback delivery

// P2-Cdc: IPI_RESCHED hit count per CPU. Set by the IPI handler each
// time a CPU receives an IPI_RESCHED. Tests use this to verify cross-
// CPU IPI delivery (boot sends to secondary; secondary's slot
// increments). Read by anyone; only the receiving CPU writes its slot.
extern volatile u64 g_ipi_resched_count[DTB_MAX_CPUS];

// P2-Cdc: secondary IPI bring-up. Called from per_cpu_main on each
// secondary AFTER sched_init + g_cpu_alive flag is set. Performs the
// per-CPU GIC redistributor + CPU interface init via gic_init_secondary,
// attaches the IPI_RESCHED handler, enables SGI 0 at the source, and
// unmasks DAIF.I so IRQs can be delivered. From this point on, the
// secondary can receive IPIs.
//
// Must run BEFORE the secondary's idle loop enters its first WFI; the
// loop body's `for(;;){sched();wfi;}` pattern relies on IRQs being
// enabled to wake from WFI when an IPI arrives.
void smp_cpu_ipi_init(unsigned cpu_idx);

#endif // THYLACINE_SMP_H
