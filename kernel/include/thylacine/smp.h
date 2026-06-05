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

// P2-Cb: per-secondary boot stacks. Used by the asm trampoline and,
// thereafter, by the per-CPU idle thread (idle threads do not own a
// per-thread kstack — they run on this boot stack indefinitely).
//
// P5-secondary-stack-guard: each per-CPU slot is a 4 KiB guard page
// (mapped no-access) followed by the 16 KiB usable stack. The slot is
// page-aligned; the stack grows DOWN from the top of `usable`, so an
// overflow past the usable bottom crosses into `guard` and faults —
// build_page_tables (mmu.c) zeroes the guard's L3 PTE, and
// addr_is_stack_guard (fault.c) recognizes the FAR and extincts with
// "kernel stack overflow". This mirrors the boot CPU's
// `_boot_stack_guard` (kernel.ld + mmu.c). Before P5-secondary-stack-
// guard the slots were a bare 16 KiB each with no guard — an overflow
// silently corrupted the adjacent slot or BSS (el1h audit F1).
//
// One slot per secondary (DTB_MAX_CPUS - 1 = 7). SECONDARY_STACK_SLOT_
// SIZE is ALSO hardcoded as a literal in arch/arm64/start.S's
// secondary_entry trampoline (asm cannot include this header) — keep
// the two in sync; the _Static_assert below pins the C side.
#define SECONDARY_STACK_GUARD_SIZE   4096u
#define SECONDARY_STACK_USABLE_SIZE  16384u
#define SECONDARY_STACK_SLOT_SIZE \
    (SECONDARY_STACK_GUARD_SIZE + SECONDARY_STACK_USABLE_SIZE)

struct secondary_stack {
    char guard[SECONDARY_STACK_GUARD_SIZE];     // no-access; overflow lands here
    char usable[SECONDARY_STACK_USABLE_SIZE];   // SP grows down from usable end
};

_Static_assert(sizeof(struct secondary_stack) == SECONDARY_STACK_SLOT_SIZE,
               "secondary_stack must be exactly guard + usable, no padding");
_Static_assert(__builtin_offsetof(struct secondary_stack, guard) == 0,
               "guard must lead the slot so the slot base IS the guard page");
_Static_assert(SECONDARY_STACK_GUARD_SIZE == 4096u,
               "guard is exactly one 4 KiB page — build_page_tables (mmu.c) "
               "zeroes a single L3 PTE per secondary guard");
_Static_assert(SECONDARY_STACK_SLOT_SIZE % SECONDARY_STACK_GUARD_SIZE == 0,
               "slot must be a page-multiple so every slot base — and thus "
               "every guard page — is page-aligned when the array is");
_Static_assert(SECONDARY_STACK_SLOT_SIZE == 20480u,
               "SECONDARY_STACK_SLOT_SIZE is mirrored as a literal in "
               "arch/arm64/start.S secondary_entry — keep the two in sync");
_Static_assert(DTB_MAX_CPUS == 8u,
               "arch/arm64/start.S secondary_entry hardcodes the idx upper "
               "bound as a literal #8 — keep it in sync with DTB_MAX_CPUS");

extern struct secondary_stack g_secondary_boot_stacks[DTB_MAX_CPUS - 1];

// Per-CPU stack buffer — RESERVED (P5-el1h-kernel).
//
// The kernel runs uniformly at EL1h (ARCHITECTURE.md §12.1, invariant
// I-21): SPSel=1 always, sp = SP_EL1 = the running thread's own kernel
// stack, and an exception builds its register frame on that same
// per-thread stack. There is NO separate live per-CPU exception stack.
//
// This buffer is retained — but currently unused at runtime — as the
// pre-allocated per-CPU landing pad for a FUTURE dedicated
// stack-overflow / SError handler stack. Under the uniform-EL1h model
// a kernel-stack overflow into the guard page faults recursively
// (KERNEL_ENTRY builds the frame on the same overflowing stack); a
// vector-side check (the 0x200 slot inspecting SP against the guard)
// could switch to this buffer to recover cleanly. That hardening item
// is deferred; the buffer is kept so it lands without a layout change.
//
// 4 KiB per CPU × DTB_MAX_CPUS (8) = 32 KiB BSS. Indexed by cpu_idx
// (0 = boot, 1..N-1 = secondaries). The layout is asserted by test_smp.
#define EXCEPTION_STACK_SIZE  4096u
extern char g_exception_stacks[DTB_MAX_CPUS][EXCEPTION_STACK_SIZE];

// SMP redesign (ARCH §8.4.2): cpu0's dedicated idle stack -- a `struct
// secondary_stack` (leading guard page + usable), symmetric with each secondary's
// g_secondary_boot_stacks slot. The boot CPU's idle owns no per-thread kstack
// (kstack_base==NULL). The guard page is mapped no-access by build_page_tables
// (mmu.c) + recognized by fault.c, exactly like the secondary guards (#867).
extern struct secondary_stack g_bootcpu_idle_stack;

// High edge (SP grows down, 16-aligned) of g_bootcpu_idle_stack's usable region.
// boot_main passes it to thread_create_bootcpu_idle.
void *smp_bootcpu_idle_stack_top(void);

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

// #868: attach + enable IPI_RESCHED on the BOOT CPU (cpu0), making it a full
// SMP scheduling peer -- a peer's sched_notify_idle_peer can wake cpu0's idle
// immediately, not only via cpu0's timer. cpu0's GIC redistributor + interface
// are already up (gic_init); this only sets the handler + enables SGI 0 on
// cpu0's redistributor. Does NOT touch DAIF (boot_main unmasks after). Must run
// on cpu0, after gic_init.
void smp_boot_cpu_ipi_init(void);

// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1,
// invariant I-24). Broadcast IPI_RESCHED to all CPUs except self, kicking any
// peer running in userspace on another CPU into the kernel so it hits its
// IRQ-from-EL0 die-check (Linux kick_process). Called by proc_group_terminate;
// the periodic preemption timer is the floor if an IPI is missed.
void smp_resched_others(void);

// Enable preemptive scheduling on secondary CPUs (each arms its own per-CPU
// generic timer). Called once from boot_main at the production transition,
// after the UP-like in-kernel test suite. Before this, secondaries have no
// timer and stay quiescent (test-phase determinism); after, every CPU gets the
// preemptive tick so a CPU-bound thread on a secondary cannot monopolize it
// (#810 / invariants I-8, I-17). This is what makes smp_resched_others's
// "periodic preemption timer is the floor" true on secondaries.
void smp_enable_secondary_preemption(void);

#endif // THYLACINE_SMP_H
