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

#include <thylacine/context.h>           // fp_enable_this_cpu (P4-Ic5-FP)
#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>      // R7 F128: irq_state_t + irqsave/restore
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/gic.h"
#include "../arch/arm64/hwdebug.h"     // 8a-2: hwdebug_init_cpu (per-PE OS-Lock unlock)
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

// P2-Cb / P5-secondary-stack-guard: per-secondary boot stacks. 7
// secondaries × 20 KiB slots (4 KiB guard + 16 KiB usable) = 140 KiB
// BSS. Page-aligned so each slot's leading guard page is page-aligned
// for mmu.c's L3-PTE zeroing. Used by the secondary_entry asm
// trampoline and thereafter by each CPU's idle thread (idle threads
// run on this stack — they do not own a per-thread kstack).
//
// The guard page of each slot is mapped no-access by build_page_tables
// (mmu.c) so a secondary's stack overflow faults instead of silently
// corrupting the adjacent slot or BSS. See smp.h + arch/arm64/mmu.c +
// arch/arm64/fault.c.
__attribute__((aligned(4096)))
struct secondary_stack g_secondary_boot_stacks[DTB_MAX_CPUS - 1];

// Per-CPU stack buffer — RESERVED. See smp.h for rationale.
// 8 CPUs × 4 KiB = 32 KiB BSS. 16-byte aligned for AAPCS64 SP.
//
// P5-el1h-kernel: the kernel runs uniformly at EL1h and builds every
// exception frame on the interrupted thread's own kernel stack, so this
// buffer is NOT the live exception stack. It is retained as the
// pre-allocated per-CPU landing pad for the future dedicated
// stack-overflow / SError handler stack (a vector-side SP_EL0 check
// would switch to it). Until that hardening item lands the buffer is
// unused at runtime; the layout is still asserted by test_smp.
__attribute__((aligned(16)))
char g_exception_stacks[DTB_MAX_CPUS][EXCEPTION_STACK_SIZE];

// SMP redesign (deep-smp-review, ARCH 8.4.2): cpu0's idle thread runs on this
// dedicated BSS stack -- a `struct secondary_stack` EXACTLY like the secondaries'
// g_secondary_boot_stacks slots (leading guard page + 16 KiB usable), so cpu0 is
// fully symmetric with the secondaries (the idle owns no per-thread kstack;
// kstack_base==NULL -> CPU-pinned like the secondaries'). cpu0's _boot_stack
// belongs to kthread (suspended mid-wait_pid once joey_run blocks), so cpu0's
// idle needs its own stack. Page-aligned so the leading guard page is a whole
// page; build_page_tables (mmu.c) maps it no-access alongside the secondary
// guards, so an overflow OR a wild SP faults with "kernel stack overflow
// (bootcpu-idle guard)" (fault.c) instead of silently corrupting adjacent BSS --
// the same protection the retired thread_create'd g_bootcpu_idle had. #867.
// NON-static + extern in smp.h so mmu.c + fault.c can reach .guard[0]. Retires
// the old real-kstack g_bootcpu_idle (the #860 root cause).
__attribute__((aligned(4096)))
struct secondary_stack g_bootcpu_idle_stack;

void *smp_bootcpu_idle_stack_top(void) {
    return &g_bootcpu_idle_stack.usable[SECONDARY_STACK_USABLE_SIZE];
}

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

// #868: make the BOOT CPU a full IPI_RESCHED peer. The secondaries attach this
// SGI in smp_cpu_ipi_init (above); cpu0 never did, so a peer's
// sched_notify_idle_peer that picked cpu0's idle sent an IPI cpu0 dropped --
// cpu0's idle was instead woken only by its always-armed timer (<=1 ms). This
// attaches the same handler + enables SGI 0 on cpu0's own (banked)
// redistributor, so cpu0 is woken immediately like any secondary.
//
// cpu0's GIC redistributor + CPU interface are already up (gic_init runs
// redist_init_cpu(0) + cpu_iface_init -- the same pair gic_init_secondary runs;
// cpu0's timer PPI firing proves the interface admits IRQs), so no
// gic_init_secondary here. gic_attach is idempotent (one global handler slot per
// INTID). Does NOT touch DAIF: the caller (boot_main) unmasks right after, the
// same attach-then-unmask order as the timer + the secondaries. cpu0 receives no
// IPI during the UP-like in-kernel tests -- only kthread calls smp_resched_others
// there, and it targets others-not-self; secondaries stay parked.
void smp_boot_cpu_ipi_init(void) {
    if (smp_cpu_idx_self() != 0)
        extinction("smp_boot_cpu_ipi_init: must run on cpu0");
    if (!gic_attach(IPI_RESCHED, ipi_resched_handler, NULL))
        extinction("smp_boot_cpu_ipi_init: gic_attach(IPI_RESCHED) failed");
    if (!gic_enable_irq(IPI_RESCHED))
        extinction("smp_boot_cpu_ipi_init: gic_enable_irq(IPI_RESCHED) failed");
}

// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1, I-24).
// Broadcast IPI_RESCHED to every CPU except self. The reschedule IPI traps a
// peer running in userspace on another CPU into the kernel so it reaches its
// IRQ-from-EL0 die-check (el0_return_die_check) without waiting for a timer
// tick — Linux's kick_process. A CPU not running a peer of the terminating
// Proc simply no-ops its die-check. Rare path (process group-exit / kill); at
// most ncpus-1 SGI writes. gic_send_ipi bounds-checks the target, so an
// out-of-range / offline index is a quiet no-op (-smp 1 sends nothing).
void smp_resched_others(void) {
    unsigned self = smp_cpu_idx_self();
    unsigned n    = smp_cpu_count();
    for (unsigned c = 0; c < n; c++) {
        if (c == self) continue;
        (void)gic_send_ipi(c, IPI_RESCHED);
    }
}

// #810: secondary CPUs DEFER arming their per-CPU generic timer until the
// production transition (boot_main, after the UP-like in-kernel test suite).
// During the test phase, secondaries stay quiescent -- parked in WFI, woken
// only by an explicit notify IPI (sched_set_notify_enabled is OFF for tests).
// So the deterministic, single-CPU-scheduled in-kernel tests are not perturbed
// by a secondary self-waking on its own timer tick and stealing a test thread
// (which surfaced as `thread_free of RUNNING thread` in
// scheduler.preemption_smoke). This is the per-CPU-timer analog of P3-G's
// sched_set_notify_enabled gate; each secondary arms its own banked timer in
// per_cpu_main's idle loop once this flag is set.
static volatile bool g_secondary_preempt_enabled;

// Enable preemptive scheduling on secondary CPUs. Called once from boot_main
// at the production transition (alongside sched_set_notify_enabled(true),
// after test_run_all). Publishes the flag, then wakes every secondary out of
// WFI so it observes the flag and arms its own per-CPU timer promptly (rather
// than waiting for the first work-placement notify IPI). I-8 / I-17: every CPU
// then gets the preemptive tick, so a CPU-bound thread on a secondary can no
// longer monopolize it (the #810 exitgroup boot hang).
void smp_enable_secondary_preemption(void) {
    __atomic_store_n(&g_secondary_preempt_enabled, true, __ATOMIC_RELEASE);
    smp_resched_others();
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

    // RW-2 2A-F4: the PSCI context_id (`cpu_idx` -- used for this CPU's stack
    // slot, sched_init, and the per-CPU idle) and smp_cpu_idx_self()
    // (MPIDR&0xff -- used by EVERY runtime per-CPU access: this_cpu_sched,
    // sched_tick, preempt_check_irq, asid_resolve) MUST name the same slot. On
    // a sparse / cluster-MPIDR board (Aff1=cluster, Aff0 restarting at 0 per
    // cluster) they DIVERGE: this CPU initializes slot `cpu_idx` yet resolves
    // every runtime access to a DIFFERENT slot -- silently ALIASING another
    // CPU's live CpuSched, whose single-slot on_cpu handoff is then written by
    // two CPUs (the #860-class half-saved-ctx corruption). A bound check cannot
    // detect aliasing. This equality check makes the dense-Aff0 assumption
    // (I-15; the tracked DTB MPIDR->dense-logical-index map) FAIL LOUD on the
    // first non-dense board instead of corrupting silently. QEMU virt / RPi are
    // dense-Aff0 -> dormant on every v1.0 target.
    if ((unsigned)cpu_idx != smp_cpu_idx_self()) {
        extinction("per_cpu_main: PSCI context_id != MPIDR-derived cpu index "
                   "(sparse/cluster MPIDR topology unsupported)");
    }

    // P4-Ic5-FP: enable CPACR_EL1.FPEN = 0b11 on this secondary CPU
    // before any context switch. Boot CPU did this in boot_main; each
    // secondary does it independently here because CPACR_EL1 is
    // banked per-CPU on AArch64.
    fp_enable_this_cpu();

    // P4-Ic-latency: enable EL0 reads of CNTPCT_EL0 via CNTKCTL_EL1
    // on this secondary. Boot CPU does it in boot_main. Per-CPU
    // register (banked) → must be set independently on each CPU.
    timer_enable_el0_counter_access();

    // Go IDE Stage 8a-2: per-PE debug bring-up on this secondary (boot CPU does
    // it in boot_main). Clears the banked OS Lock so a guest-programmed EL0
    // hardware breakpoint can deliver if the debugged thread runs here.
    hwdebug_init_cpu();

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
    //
    // R7 F128 close: IRQ-masked window around (sched, flag-set, wfi).
    //
    // Race the masking closes:
    //   B (this CPU): sched() returns empty. Pre-fix, B would set the
    //     flag AFTER sched returned, leaving a window where B looks
    //     "running" to peer notifiers but is actually about to wfi.
    //   A (peer): ready(t) places work on A's tree, calls notify_idle_peer
    //     which reads B's flag — sees FALSE (window). Skips B.
    //   B: sets flag TRUE, wfi. No pending IPI; B sleeps with work
    //     available somewhere else; runnable thread starves until next
    //     unrelated wake event.
    //
    // The fix: mask IRQs, set flag TRUE, sched (may pick + run work or
    // return empty), wfi, on wake clear flag, unmask. While IRQs are
    // masked, an incoming IPI sets the GIC pend bit but does NOT
    // deliver until unmask. WFI architecturally still exits on a pending
    // IRQ even with PSTATE.I=1 (ARM ARM C5.3.2). So if A's notifier
    // sends an IPI any time after B sets flag=TRUE, B's wfi sees the
    // pending IRQ and exits; B clears flag, unmasks, IPI handler runs,
    // loop iterates and sched picks up the work.
    //
    // sched() internally takes its own spin_lock_irqsave/restore — so
    // calling sched with IRQs already masked is sound (sched saves
    // MASKED, restores MASKED).
    // #810: this secondary arms its OWN per-CPU timer the first time it
    // observes the production-preemption flag (set by smp_enable_secondary_-
    // preemption at boot_main's transition, after the UP-like in-kernel test
    // suite). Deferred to HERE -- not the bringup above -- so a secondary stays
    // quiescent (no self-wake, no work-stealing) during the deterministic test
    // phase, while production gets the preemptive tick on every CPU (I-8/I-17).
    // The timer + PPI registers are per-CPU banked, so each secondary must arm
    // its own (gic_enable_irq enables INTID 27 on THIS CPU's redistributor).
    bool timer_armed = false;
    for (;;) {
        if (!timer_armed &&
            __atomic_load_n(&g_secondary_preempt_enabled, __ATOMIC_ACQUIRE)) {
            if (!gic_enable_irq(TIMER_INTID_EL1_VIRT))
                extinction("per_cpu_main: gic_enable_irq(timer PPI) failed");
            timer_arm_this_cpu();
            timer_armed = true;
        }
        // Tickless idle (NO_HZ_IDLE; TI-2): once this secondary's timer PPI is
        // enabled (timer_armed), sched_idle_park arms a one-shot to the nearest
        // deadline-or-backstop instead of holding the 1 kHz periodic tick. Until
        // then it passes false -> the byte-identical pre-preempt behavior (just
        // WFI on IPI, quiescent during the deterministic test phase). The shared
        // body lives in sched.c::sched_idle_park.
        sched_idle_park(timer_armed);
    }
}
