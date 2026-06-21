// ARM Generic Timer — EL1 virtual timer at fixed Hz (Lazarus W2; physical
// at P1-G -- see timer.h for why virtual: the physical timer is hypervisor-
// reserved under HVF on Apple Silicon).
//
// Driven by:
//   CNTFRQ_EL0   — counter frequency (Hz; shared by both timers)
//   CNTVCT_EL0   — virtual counter (always running)
//   CNTV_TVAL_EL0 — 32-bit signed decrementer; IRQ fires when it goes <= 0
//   CNTV_CTL_EL0  — bit 0 ENABLE, bit 1 IMASK, bit 2 ISTATUS (RO)
//
// The IRQ does not auto-clear — writing CNTV_TVAL_EL0 with a fresh
// reload value re-arms it; the GIC EOI handles the priority drop.
// IMASK stays clear; ENABLE stays set. ISTATUS reads as 1 while
// the timer is firing (we don't poll it because the GIC ack/EOI
// chain is the canonical path).
//
// The reload is computed at init time as `freq / hz` and cached. A
// freq of 62.5 MHz on QEMU virt + hz=1000 gives reload = 62500
// (well below the 32-bit range, no wraparound risk per fire).

#include "timer.h"

#include "gic.h"

#include <stdint.h>
#include <thylacine/extinction.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/types.h>

// Compile-time sanity: PPI 11 → INTID 27 (architectural per ARMv8 ARM
// D11.2.4). Catches a regression to GIC_PPI_TO_INTID's offset.
_Static_assert(TIMER_INTID_EL1_VIRT == 27,
               "TIMER_INTID_EL1_VIRT must be 27 (PPI 11 + offset 16)");

// ---------------------------------------------------------------------------
// CNT* system-register accessors.
// ---------------------------------------------------------------------------

static inline u64 read_cntfrq_el0(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline u64 read_cntvct_el0(void) {
    u64 v;
    // isb before the read so any prior store has retired before we
    // take the timestamp (per ARMv8 generic-timer access ordering).
    __asm__ __volatile__("isb\nmrs %0, cntvct_el0\n" : "=r"(v));
    return v;
}

static inline void write_cntv_tval_el0(u64 v) {
    __asm__ __volatile__("msr cntv_tval_el0, %0" :: "r"(v));
}

static inline void write_cntv_ctl_el0(u64 v) {
    __asm__ __volatile__("msr cntv_ctl_el0, %0\nisb\n" :: "r"(v));
}

#define CNTV_CTL_ENABLE     (1u << 0)
#define CNTV_CTL_IMASK      (1u << 1)
#define CNTV_CTL_ISTATUS    (1u << 2)

// ---------------------------------------------------------------------------
// State.
//
// `g_ticks` is `volatile` so a reader outside an IRQ-handler frame
// (e.g., a future scheduler tick deadline check that doesn't WFI)
// re-loads the value on each access instead of having it hoisted out
// of a loop. The IRQ handler is the sole writer; readers see a
// monotonic sequence (load of an aligned u64 is atomic on aarch64).
// `timer_busy_wait_ticks`'s WFI loop didn't need this strictly because
// the `"memory"` clobber on the wfi asm forces re-load, but the
// API-surface guarantee is what callers should be able to rely on.
// ---------------------------------------------------------------------------

static u64 g_freq;            // CNTFRQ_EL0 in Hz, cached at init (write-once before SMP)
// freq / hz reload count. Write-once on the boot CPU in timer_init, BEFORE any
// secondary arms its timer -- the smp_init bring-up barrier + the release/acquire
// on g_secondary_preempt_enabled (smp.c) order it -- so every CPU's
// timer_arm_this_cpu() + per-tick re-arm reads it unsynchronized yet always sees
// the final value. A future dynamic-reprogram writer would need to add ordering.
static u64 g_reload;
// Monotonic tick counter -- BOOT CPU ONLY (#810). Post-#810 every CPU takes timer
// IRQs, but only cpu0 increments g_ticks: a single-writer timebase for
// timer_busy_wait_ticks + diagnostics that preserves the pre-#810 1 kHz rate with
// no multi-writer race. volatile so non-IRQ readers re-load each access.
static volatile u64 g_ticks;

// ---------------------------------------------------------------------------
// timer_init.
// ---------------------------------------------------------------------------

// TIMER_MIN_RELOAD / TIMER_MAX_RELOAD bound every reload (the periodic
// g_reload below AND the tickless one-shot delta); they moved to timer.h as
// the public clamp contract for timer_arm_oneshot_cnt -- see the rationale
// there.

bool timer_init(u32 hz) {
    if (hz == 0) return false;

    g_freq = read_cntfrq_el0();
    if (g_freq == 0) {
        // No counter frequency advertised — firmware should have set
        // CNTFRQ_EL0 (QEMU virt does; bare metal sometimes needs the
        // bootloader to). Fail loudly rather than divide by zero.
        extinction("timer_init: CNTFRQ_EL0 = 0");
    }
    if ((u64)hz > g_freq) return false;

    u64 reload = g_freq / (u64)hz;
    if (reload < TIMER_MIN_RELOAD)  return false;
    if (reload > TIMER_MAX_RELOAD)  return false;
    g_reload = reload;
    g_ticks  = 0;

    // Arm the boot CPU's timer. Each secondary arms its own (per-CPU
    // banked) timer via timer_arm_this_cpu() from per_cpu_main.
    timer_arm_this_cpu();

    return true;
}

// Arm THIS CPU's banked physical timer using the reload computed by
// timer_init. CNTV_TVAL_EL0 / CNTV_CTL_EL0 are per-CPU banked, so the boot
// CPU's timer_init does NOT arm secondaries -- each CPU must call this to
// receive the preemptive scheduler tick. A secondary with no armed timer
// never preempts: a CPU-bound EL0 thread there monopolizes the CPU and
// starves co-runnable peers (I-8 / I-17 violation; #810). MUST run after
// timer_init has set g_reload (boot CPU); the EL1-phys-NS timer PPI must
// also be enabled on this CPU's redistributor (gic_enable_irq, per-CPU).
void timer_arm_this_cpu(void) {
    if (g_reload == 0)
        extinction("timer_arm_this_cpu before timer_init (g_reload unset)");
    write_cntv_tval_el0(g_reload);
    write_cntv_ctl_el0(CNTV_CTL_ENABLE);
}

// ---------------------------------------------------------------------------
// Tickless idle one-shot (NO_HZ_IDLE; docs/TICKLESS-IDLE.md, #299).
//
// An idle CPU does not need the 1 kHz periodic re-arm: it arms a SINGLE shot
// to the nearest pending deadline (min'd with a backstop at TI-2) and parks,
// so a genuinely-idle CPU takes zero timer IRQs until there is real work --
// closing the HVF per-tick vmexit storm (idle 332% -> ~0). The periodic tick
// stays byte-unchanged for any RUNNING thread (the slice model, I-17, the
// tick-coupled tests). TI-1 lands the primitive; the idle loop wires it at
// TI-2, so nothing here changes behavior yet.
// ---------------------------------------------------------------------------

u32 timer_oneshot_tval(u64 target_cnt, u64 now_cnt) {
    u64 delta = (target_cnt > now_cnt) ? (target_cnt - now_cnt) : 0;
    if (delta < TIMER_MIN_RELOAD) delta = TIMER_MIN_RELOAD;
    if (delta > TIMER_MAX_RELOAD) delta = TIMER_MAX_RELOAD;
    return (u32)delta;
}

void timer_arm_oneshot_cnt(u64 target_cnt) {
    if (g_reload == 0)
        extinction("timer_arm_oneshot_cnt before timer_init (g_reload unset)");
    u64 now = read_cntvct_el0();
    write_cntv_tval_el0(timer_oneshot_tval(target_cnt, now));
    write_cntv_ctl_el0(CNTV_CTL_ENABLE);
}

// ---------------------------------------------------------------------------
// IRQ handler.
// ---------------------------------------------------------------------------

void timer_irq_handler(u32 intid, void *arg) {
    (void)intid;
    (void)arg;
    // Re-arm THIS CPU's timer (CNTV_TVAL_EL0 is per-CPU banked; every CPU
    // with an armed timer re-arms its own). CNTV_CTL stays at ENABLE.
    write_cntv_tval_el0(g_reload);

    // g_ticks is the boot CPU's single-writer monotonic counter (the
    // timer_busy_wait_ticks timebase + diagnostics). Secondaries now also
    // tick (#810: per-CPU preemption), but they must NOT race g_ticks --
    // only the boot CPU increments it, preserving the pre-#810 timebase
    // and avoiding a multi-writer read-modify-write data race.
    if (smp_cpu_idx_self() == 0) g_ticks++;

    // P2-Bc: scheduler tick. Decrements current's slice; sets
    // need_resched if expired so preempt_check_irq triggers a
    // context switch on IRQ-return. No-op before sched_init. Runs on
    // EVERY CPU's tick (per-CPU state); secondaries get it now too.
    sched_tick();
}

// ---------------------------------------------------------------------------
// Diagnostics + busy-wait.
// ---------------------------------------------------------------------------

u64 timer_get_ticks(void)   { return g_ticks; }
u64 timer_get_counter(void) { return read_cntvct_el0(); }
u32 timer_get_freq(void)    { return (u32)g_freq; }

// P5-tsleep: counter <-> nanosecond conversion. Both use the split
// (quotient, remainder) form: a flat `value * 1e9` would overflow u64
// within ~5 minutes at a 62.5 MHz counter, but the whole-units term
// stays small for centuries and the remainder term is bounded well
// below 2^63 (the remainder is < g_freq or < 1e9, times the other
// factor). g_freq == 0 only before timer_init — fail soft to 0.
u64 timer_now_ns(void) {
    if (g_freq == 0) return 0;
    u64 cnt = read_cntvct_el0();
    return (cnt / g_freq) * 1000000000ull
         + (cnt % g_freq) * 1000000000ull / g_freq;
}

u64 timer_ns_to_counter(u64 ns) {
    if (g_freq == 0) return 0;
    return (ns / 1000000000ull) * g_freq
         + (ns % 1000000000ull) * g_freq / 1000000000ull;
}

// ---------------------------------------------------------------------------
// LS-K: wall clock (CLOCK_REALTIME).
//
// A single offset (epoch_ns - mono_ns_at_anchor) added to the monotonic clock
// yields the wall clock. One aligned u64, write-once on the boot CPU before
// smp_init: an aligned u64 load on aarch64 is atomic, and the smp bring-up
// barrier orders the single write before any secondary reads it, so
// clock_gettime on any CPU reads a coherent value with no lock. 0 (no RTC)
// keeps realtime == monotonic (1970 + uptime, the honest "no wall clock"
// signal). See ARCH §22.6.
// ---------------------------------------------------------------------------
static u64 g_wallclock_offset_ns;   // CLOCK_REALTIME = timer_now_ns() + this

u64 timer_wallclock_offset_ns(u64 epoch_seconds, u64 mono_now_ns) {
    u64 epoch_ns = epoch_seconds * 1000000000ull;
    // Fail-soft on a 0 epoch (no RTC), AND on the implausible case epoch_ns <
    // mono_now (which would underflow the u64 subtraction). Production never
    // hits the latter -- rtc_read_epoch_seconds returns 0 or >= the 2020 floor,
    // whose epoch_ns (>= 1.5e18) dominates a boot-early mono -- but this public
    // helper must be total against any caller/test input (LS-K audit F4).
    if (epoch_seconds == 0 || epoch_ns < mono_now_ns)
        return 0;
    return epoch_ns - mono_now_ns;
}

// Publish a fresh wall-clock offset from a full-nanosecond epoch. The boot
// anchor (write-once before smp_init) and the runtime re-anchor
// (SYS_CLOCK_SETTIME) both land here. The offset is a single aligned u64, so the
// store is atomic on aarch64: a concurrent timer_realtime_ns on another CPU
// reads either the old or the new offset, each internally consistent. A clock
// step is a deliberate discontinuity, so the sub-microsecond skew between a
// reader's timer_now_ns() and a racing publish is semantically irrelevant --
// there is no second field to tear against, so no seqlock is needed (the LS-K
// single-u64 design is what makes the runtime setter SMP-safe by construction).
// Fail-soft (offset 0 -> realtime == monotonic) on a 0 epoch or the implausible
// epoch_ns < mono (which would underflow), mirroring timer_wallclock_offset_ns.
static void wallclock_publish_ns(u64 epoch_ns) {
    u64 mono = timer_now_ns();
    u64 off  = (epoch_ns == 0 || epoch_ns < mono) ? 0 : (epoch_ns - mono);
    __atomic_store_n(&g_wallclock_offset_ns, off, __ATOMIC_RELAXED);
}

void timer_set_wallclock_anchor(u64 epoch_seconds) {
    // Byte-equivalent to the prior timer_wallclock_offset_ns(epoch_seconds, now)
    // path: the helper computed epoch_seconds*1e9 - now with the same fail-soft.
    // epoch_seconds comes from the RTC (0 or >= the 2020 floor), so the multiply
    // stays well inside u64.
    wallclock_publish_ns(epoch_seconds * 1000000000ull);
}

// Runtime re-anchor (SYS_CLOCK_SETTIME, net-7a) at full-nanosecond granularity.
// A distinct name from the boot anchor gives the audit-trigger surface a clean
// hook; the publish is the same single atomic store. CLOCK_MONOTONIC
// (timer_now_ns) is untouched. The caller (sys_clock_settime_handler) bounds
// epoch_ns so the seconds*1e9 + nsec composition cannot overflow.
void timer_reset_wallclock_anchor_ns(u64 epoch_ns) {
    wallclock_publish_ns(epoch_ns);
}

u64 timer_realtime_ns(void) {
    return timer_now_ns()
         + __atomic_load_n(&g_wallclock_offset_ns, __ATOMIC_RELAXED);
}

void timer_busy_wait_ticks(u64 n) {
    u64 target = g_ticks + n;
    while (g_ticks < target) {
        __asm__ __volatile__("wfi" ::: "memory");
    }
}

// CNTKCTL_EL1: EL0PCTEN (bit 0) gates EL0 reads of CNTPCT_EL0; EL0VCTEN
// (bit 1) gates CNTVCT_EL0. Reset value undefined; reset-to-0 on warm
// reset means an EL0 counter read traps to EL1 (EC=0x18) without the bit.
// We enable BOTH: the kernel timebase is now the virtual counter (the
// virtual timer is hypervisor-safe -- see the file header), so userspace
// should read CNTVCT_EL0 (EL0VCTEN); EL0PCTEN stays set too since the
// physical counter is readable at EL1/EL0 where available and costs
// nothing. Under HVF the virtual counter may carry a non-zero CNTVOFF, so
// EL0 + kernel must agree on the same (virtual) timebase -- they do.
#define CNTKCTL_EL0PCTEN  (1ull << 0)
#define CNTKCTL_EL0VCTEN  (1ull << 1)

void timer_enable_el0_counter_access(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntkctl_el1" : "=r"(v));
    v |= CNTKCTL_EL0PCTEN | CNTKCTL_EL0VCTEN;
    __asm__ __volatile__("msr cntkctl_el1, %0\nisb\n" :: "r"(v));
}
