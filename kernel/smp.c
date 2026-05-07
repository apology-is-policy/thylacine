// SMP secondary bring-up (P2-Ca).
//
// Iterates DTB cpus, brings each non-boot secondary up via PSCI_CPU_ON
// pointing at the asm trampoline secondary_entry (in start.S). The
// trampoline runs at low PA (no MMU), flips g_cpu_online[idx], and
// parks at WFI. Primary waits for the flag with a per-CPU timeout.
//
// At P2-Ca secondaries are alive but cannot execute kernel code beyond
// the trampoline (no MMU, no PAC, no per-CPU exception stack). P2-Cb
// adds full per-CPU init bringing them into the scheduler.

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/gic.h"
#include "../arch/arm64/kaslr.h"
#include "../arch/arm64/psci.h"
#include "../arch/arm64/timer.h"
#include "../arch/arm64/uart.h"

// Linker-defined kernel image bounds — used to compute secondary_entry's PA.
extern char _kernel_start[];

// Asm trampoline label (defined in arch/arm64/start.S).
extern void secondary_entry(void);

// Per-CPU online flag. Set by secondary_entry's trampoline (asm strb)
// before pac_apply / mmu_program / branch-to-high-VA. Earliest signal
// that "trampoline reached + idx valid + per-CPU stack assigned."
volatile u8 g_cpu_online[DTB_MAX_CPUS];

// P2-Cb: per-CPU "fully initialized at high VA" flag. Set by
// per_cpu_main after VBAR_EL1 + TPIDR_EL1 are configured. The
// stricter "alive" signal — secondary's PAC + MMU work and the C
// runtime is reachable.
volatile u8 g_cpu_alive[DTB_MAX_CPUS];

// P2-Cb: PAC keys (8 u64 halves). See smp.h for layout. Populated by
// pac_derive_keys (asm in start.S, called once by primary); consumed
// by pac_apply_this_cpu (asm in start.S, called by every CPU).
u64 g_pac_keys[8];

// P2-Cb: per-secondary boot stacks. 7 secondaries × 16 KiB = 112 KiB BSS.
// 16-byte aligned (AAPCS64 SP requirement). Used by secondary_entry
// asm trampoline; later (P2-Cd+) replaced by per-CPU idle thread
// stacks once the scheduler picks them.
__attribute__((aligned(16)))
char g_secondary_boot_stacks[DTB_MAX_CPUS - 1][SECONDARY_STACK_SIZE];

// P2-Cc: per-CPU exception stacks. See smp.h for design rationale.
// 8 CPUs × 4 KiB = 32 KiB BSS. 16-byte aligned for AAPCS64 SP.
//
// Each CPU's start-of-init asm sets SP_EL1 = &this[cpu_idx + 1] (i.e.,
// the top of slot cpu_idx) and msr SPSel, #0 to switch to SP_EL0 for
// normal kernel work. From that point on, hardware exception entry
// switches SP to SP_EL1 = this per-CPU stack automatically; KERNEL_EXIT
// + ERET restore SPSel from SPSR_EL1.M[0] = 0 (EL1t), back to SP_EL0.
__attribute__((aligned(16)))
char g_exception_stacks[DTB_MAX_CPUS][EXCEPTION_STACK_SIZE];

// P2-Cc: per-CPU SP-at-exception observability slot. timer_irq_handler
// writes &local on its first invocation per CPU; smp.exception_stack_smoke
// reads to verify the address falls inside g_exception_stacks[cpu_idx].
volatile uintptr_t g_exception_stack_observed[DTB_MAX_CPUS];

// P2-Cdc: IPI_RESCHED receive counter per CPU. Incremented by the
// IPI handler on every receive; tests read to verify cross-CPU
// IPI delivery.
volatile u64 g_ipi_resched_count[DTB_MAX_CPUS];

unsigned smp_cpu_idx_self(void) {
    u64 mpidr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (unsigned)(mpidr & 0xffu);
}

// P2-Cdc: handler for IPI_RESCHED.
//
// Called from gic_dispatch via vectors.S IRQ slot when SGI 0 is
// received on any CPU. Increments this CPU's receive counter (for
// observability) and otherwise does nothing — the IPI's job is to
// wake the receiving CPU from WFI. preempt_check_irq runs after
// this handler returns (vectors.S IRQ slot calls preempt_check_irq
// after exception_irq_curr_el → gic_dispatch → ipi_resched_handler);
// if the receiving CPU's need_resched was set by a cross-CPU placer,
// preempt_check_irq picks it up and calls sched(). At v1.0 P2-Cdc no
// cross-CPU placer exists yet (P2-Ce work-stealing introduces it);
// IPI_RESCHED here is purely a "wake from WFI" signal proving the
// SGI delivery path works.
static void ipi_resched_handler(u32 intid, void *arg) {
    (void)intid;
    (void)arg;
    unsigned cpu = smp_cpu_idx_self();
    if (cpu < DTB_MAX_CPUS) {
        g_ipi_resched_count[cpu]++;
    }
}

void smp_cpu_ipi_init(unsigned cpu_idx) {
    if (cpu_idx == 0) extinction("smp_cpu_ipi_init: cpu_idx 0 reserved for boot path");
    if (!gic_init_secondary(cpu_idx))
        extinction("smp_cpu_ipi_init: gic_init_secondary failed");
    if (!gic_attach(IPI_RESCHED, ipi_resched_handler, NULL))
        extinction("smp_cpu_ipi_init: gic_attach(IPI_RESCHED) failed");
    if (!gic_enable_irq(IPI_RESCHED))
        extinction("smp_cpu_ipi_init: gic_enable_irq(IPI_RESCHED) failed");

    // Unmask IRQs at PSTATE. From this point the secondary admits
    // GIC SGIs (and any per-CPU PPIs we enable later — timer, etc.).
    __asm__ __volatile__("msr daifclr, #2" ::: "memory");
}

static unsigned g_cpu_count;
static unsigned g_cpu_online_count;

// Per-secondary timeout. PSCI bring-up + trampoline execution + flag
// store + dsb sy is microseconds at most on QEMU virt; bare metal might
// be milliseconds. 100 ms is generous.
#define SMP_BRINGUP_TIMEOUT_TICKS  100

// Compute the PA of secondary_entry. The trampoline is part of the
// kernel image (.text); its VA was assigned by the linker to
// KASLR_LINK_VA + offset, then patched by .rela.dyn relocations. The
// PA is kaslr_kernel_pa_start() + (VA - link_VA_of_kernel_start).
//
// Equivalently: PA = kaslr_kernel_pa_start() + (VA - link_kernel_start_VA).
// Since (link_kernel_start_VA) is the base of the kernel image's load
// address in the original linker script, and at runtime
// kaslr_kernel_pa_start() returns the actual load PA, the offset
// (secondary_entry - _kernel_start) is the same VA-relative offset
// regardless of the slide.
static u64 compute_secondary_entry_pa(void) {
    u64 entry_va  = (u64)(uintptr_t)secondary_entry;
    u64 kernel_va = (u64)(uintptr_t)_kernel_start;
    u64 offset    = entry_va - kernel_va;
    return kaslr_kernel_pa_start() + offset;
}

// Translate a PSCI return code to a uart-printable summary.
static const char *psci_status_str(int status) {
    switch (status) {
    case 0:    return "SUCCESS";
    case -1:   return "NOT_SUPPORTED";
    case -2:   return "INVALID_PARAMETERS";
    case -3:   return "DENIED";
    case -4:   return "ALREADY_ON";
    case -5:   return "ON_PENDING";
    case -6:   return "INTERNAL_FAILURE";
    case -7:   return "NOT_PRESENT";
    case -8:   return "DISABLED";
    case -9:   return "INVALID_ADDRESS";
    default:   return "UNKNOWN";
    }
}

// Wait for `flag[idx]` to become true with timeout. Returns true if
// the flag was observed; false on timeout. The polling loop reads
// through `volatile` so the compiler re-fetches each iteration; the
// explicit `dmb ish` ensures the read happens after any pending
// inner-shareable store from the secondary.
static bool wait_for_flag(volatile u8 *flag, u64 timeout_ticks) {
    u64 start = timer_get_ticks();
    while (!*flag) {
        if ((timer_get_ticks() - start) > timeout_ticks) return false;
        __asm__ __volatile__("dmb ish\n" ::: "memory");
        // No WFI here — IRQs may not be live; we want to spin so we
        // notice the flag promptly.
    }
    return true;
}

unsigned smp_init(void) {
    g_cpu_count = dtb_cpu_count();
    if (g_cpu_count == 0) {
        // Malformed DTB — shouldn't happen. Treat as UP.
        g_cpu_count = 1;
        g_cpu_online[0] = 1;
        g_cpu_online_count = 1;
        return 0;
    }

    // Mark boot CPU online + alive unconditionally — boot ran the
    // full init via boot_main; both flags are conceptually true.
    g_cpu_online[0] = 1;
    g_cpu_alive[0]  = 1;
    g_cpu_online_count = 1;

    // PSCI required for secondary bring-up.
    if (!psci_is_ready()) {
        uart_puts("  smp: PSCI not available — secondaries held\n");
        return 0;
    }

    if (g_cpu_count > DTB_MAX_CPUS) {
        uart_puts("  smp: cpu count exceeds DTB_MAX_CPUS — capping\n");
        g_cpu_count = DTB_MAX_CPUS;
    }

    u64 entry_pa = compute_secondary_entry_pa();

    unsigned brought_up = 0;
    for (unsigned i = 1; i < g_cpu_count; i++) {
        u64 mpidr = 0;
        if (!dtb_cpu_mpidr(i, &mpidr)) continue;

        int rc = psci_cpu_on(mpidr, entry_pa, /*context_id=*/(u64)i);

        if (rc == 0 || rc == -4 /*ALREADY_ON*/) {
            // Wait for the secondary to reach per_cpu_main (alive)
            // — the stricter "fully initialized at high VA" signal.
            // The earlier g_cpu_online flag (set in trampoline) means
            // "trampoline ran"; we want both, but watching alive
            // catches PAC/MMU/VBAR failures that would leave the
            // secondary stuck mid-init.
            if (wait_for_flag(&g_cpu_alive[i], SMP_BRINGUP_TIMEOUT_TICKS)) {
                brought_up++;
                g_cpu_online_count++;
            } else {
                uart_puts("  smp: cpu ");
                uart_putdec((u64)i);
                if (g_cpu_online[i]) {
                    uart_puts(" trampoline reached but per_cpu_main timed out (PAC/MMU/VBAR fail?)\n");
                } else {
                    uart_puts(" PSCI ok but trampoline never ran\n");
                }
            }
        } else {
            uart_puts("  smp: cpu ");
            uart_putdec((u64)i);
            uart_puts(" PSCI_CPU_ON failed (");
            uart_puts(psci_status_str(rc));
            uart_puts(")\n");
        }
    }

    return brought_up;
}

unsigned smp_cpu_count(void) {
    return g_cpu_count;
}

unsigned smp_cpu_online_count(void) {
    return g_cpu_online_count;
}

// ---------------------------------------------------------------------------
// per_cpu_main (P2-Cb).
//
// Reached at the kernel's high VA from the secondary_entry asm
// trampoline (start.S) AFTER:
//   - per-CPU boot stack assigned (via SP).
//   - PAC keys loaded (pac_apply_this_cpu).
//   - MMU enabled with primary's tables (mmu_program_this_cpu).
//   - Long-branch via kaslr_high_va_addr to this function.
//
// Sets up the remaining per-CPU state:
//   1. VBAR_EL1 — exception vector table (shared with primary at v1.0;
//      P2-Cd may give each CPU its own routing table for IRQ affinity).
//   2. TPIDR_EL1 — per-CPU current_thread pointer. NULL at P2-Cb (no
//      per-CPU run tree yet); P2-Cd or later assigns each CPU's idle
//      thread.
//   3. Flips g_cpu_alive[idx] — the "fully initialized" signal that
//      smp_init's wait loop watches for.
//
// Then enters an idle WFI loop indefinitely. With IRQs masked (PSCI
// default), nothing wakes the CPU — at P2-Cd when GIC SGIs land, the
// boot CPU can send IPI_RESCHED to wake a secondary into useful work.
// ---------------------------------------------------------------------------

extern char _exception_vectors[];

__attribute__((noreturn))
void per_cpu_main(int cpu_idx) {
    if (cpu_idx <= 0 || cpu_idx >= (int)DTB_MAX_CPUS) {
        extinction("per_cpu_main: invalid cpu_idx");
    }

    // VBAR_EL1 — install the kernel exception vector table. ISB so
    // any subsequent exception sees the new VBAR.
    u64 vbar = (u64)(uintptr_t)_exception_vectors;
    __asm__ __volatile__(
        "msr vbar_el1, %0\n"
        "isb\n"
        :: "r"(vbar) : "memory"
    );

    // P2-Cd: allocate this CPU's idle thread + park in TPIDR_EL1.
    // The idle thread doesn't own a kstack — it runs on the per-CPU
    // boot stack already current via SP_EL0 (set in secondary_entry).
    // We must set TPIDR_EL1 BEFORE sched_init since sched_init reads
    // current_thread() to record the per-CPU idle pointer.
    struct Thread *idle = thread_init_per_cpu_idle((unsigned)cpu_idx);
    if (!idle) extinction("per_cpu_main: thread_init_per_cpu_idle returned NULL");
    set_current_thread(idle);
    __asm__ __volatile__("msr tpidr_el0, xzr" ::: "memory");

    // Initialize THIS CPU's per-CPU sched state.
    sched_init((unsigned)cpu_idx);

    // P2-Cdc: per-CPU GIC bring-up + IPI handler attachment + IRQ
    // unmask. Done BEFORE g_cpu_alive flip + the idle loop so:
    //   1. The boot CPU's smp_init wait still observes alive flip as
    //      the "fully ready" signal — by the time alive is true, this
    //      CPU is also IPI-ready.
    //   2. The first WFI in the idle loop has IRQs unmasked, so an
    //      incoming IPI_RESCHED actually wakes it.
    smp_cpu_ipi_init((unsigned)cpu_idx);

    // Publish the "fully alive" flag. smp_init's wait loop watches
    // this. With MMU on, the store goes through cacheable memory;
    // dsb sy ensures global visibility. Order: flag flip after
    // sched_init + IPI bring-up so a downstream observer can rely on
    // the per-CPU sched being functional + IPI-receivable whenever
    // g_cpu_alive[idx] is true.
    g_cpu_alive[cpu_idx] = 1;
    __asm__ __volatile__("dsb sy" ::: "memory");

    // Per-CPU idle loop. With IRQs now unmasked + the per-CPU GIC
    // brought up, an IPI_RESCHED from any other CPU will wake this
    // CPU's WFI. sched() yields to anything placed on this CPU's run
    // tree (P2-Ce work-stealing introduces cross-CPU placement); if
    // empty, sched() returns and we WFI again until the next event.
    //
    // P3-G: idle_in_wfi flag flips TRUE before WFI, FALSE after. Peer
    // CPUs read it to identify wake targets in sched_notify_idle_peer.
    // Without this, ready/wakeup placing work elsewhere wouldn't wake
    // this CPU — it'd starve in WFI indefinitely (no per-CPU timer
    // at v1.0). Closes R5-H F78. Maps to scheduler.tla `EnterWFI`.
    for (;;) {
        sched();
        sched_set_idle_in_wfi(true);
        __asm__ __volatile__("wfi" ::: "memory");
        sched_set_idle_in_wfi(false);
    }
}
