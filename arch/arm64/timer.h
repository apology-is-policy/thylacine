// ARM Generic Timer — EL1 virtual timer (Lazarus W2; physical at P1-G).
//
// Per ARCHITECTURE.md §12.3 (interrupt handling) + §22.2 (DTB-driven
// hardware discovery).
//
// The ARMv8 generic timer is a system-register peripheral (no MMIO);
// its interrupts arrive via the GIC as PPIs (per-CPU). We use the
// VIRTUAL timer, not the EL1 physical timer:
//
//   counter:   CNTVCT_EL0           always-running 64-bit virtual counter
//   freq:      CNTFRQ_EL0           counter frequency (Hz; shared)
//   timer val: CNTV_TVAL_EL0        decrementing 32-bit; fires when 0
//   ctl:       CNTV_CTL_EL0         {ENABLE, IMASK, ISTATUS}
//   intid:     PPI 11 = INTID 27    (architecturally fixed, ARMv8 ARM D11.2.4)
//
// Why virtual, not physical: under a hypervisor (HVF on Apple Silicon is
// the Lazarus fast-dev target) the physical timer (CNTP_*) is reserved by
// the hypervisor, and an EL1 guest write to CNTP_TVAL_EL0 reflects as an
// EC=0 undefined-instruction exception. The virtual timer is the timer an
// EL1 guest is meant to drive and is accessible on every substrate -- QEMU
// TCG, HVF, and a direct-EL1 bare-metal boot (where CNTVOFF=0 makes the
// virtual counter equal the physical one). Using it uniformly keeps ONE
// timebase across all targets. (The physical *counter* read CNTPCT_EL0
// stays available at EL1 for entropy -- kaslr/canary; only the physical
// *timer* control is hypervisor-reserved.)
//
// The timer fires at 1000 Hz; the IRQ handler increments a global tick
// counter and reloads CNTV_TVAL_EL0.

#ifndef THYLACINE_ARCH_ARM64_TIMER_H
#define THYLACINE_ARCH_ARM64_TIMER_H

#include "gic.h"

#include <thylacine/types.h>

// Architectural INTID for the EL1 virtual timer (PPI 11). The virtual
// timer is accessible at EL1 on every substrate (the physical timer is
// hypervisor-reserved under HVF -- see the file header).
#define TIMER_INTID_EL1_VIRT  GIC_PPI_TO_INTID(11)   /* = 27 */

// CNTV_TVAL_EL0 is a 32-bit signed down-counter (ARMv8 ARM D11.2.4); the IRQ
// fires when it crosses 0. A reload (the periodic g_reload AND the tickless
// one-shot delta) must fit [TIMER_MIN_RELOAD, TIMER_MAX_RELOAD]: a reload of 1
// fires every counter tick (a handler can't keep up), so the floor of 100
// caps the IRQ rate under freq/100; a reload above INT32_MAX truncates and
// fires sooner than intended, so the ceiling is the register width. Public
// (not file-static) because they are the clamp contract a timer_arm_oneshot_cnt
// caller relies on, and the clamp helper's unit test pins against them.
#define TIMER_MIN_RELOAD   100u
#define TIMER_MAX_RELOAD   0x7FFFFFFFu     // INT32_MAX

// One-time bring-up (boot CPU). Reads CNTFRQ_EL0, caches the reload count
// for `hz` Hz, then arms the boot CPU's timer via timer_arm_this_cpu()
// (CNTV_TVAL_EL0 + CNTV_CTL_EL0 = ENABLE, !IMASK). Secondaries arm their
// own banked timer (timer_arm_this_cpu) at the production transition; #810.
// Caller follows with gic_attach(TIMER_INTID_EL1_VIRT, ...) and
// gic_enable_irq(TIMER_INTID_EL1_VIRT) to route the IRQ through.
//
// `hz` must be > 0 and < CNTFRQ_EL0; typical 1000 Hz gives 1 ms ticks.
// Returns false if hz is out of range.
bool timer_init(u32 hz);

// Arm THIS CPU's per-CPU-banked virtual timer using the reload computed
// by timer_init. Every CPU must call this (the boot CPU via timer_init;
// each secondary from per_cpu_main) to receive the preemptive scheduler
// tick -- CNTV_*_EL0 are banked per-CPU. The caller must also enable the
// timer PPI on this CPU's redistributor (gic_enable_irq, per-CPU). MUST
// run after timer_init has set the reload. See #810 / I-8 / I-17.
void timer_arm_this_cpu(void);

// Tickless idle (NO_HZ_IDLE; docs/TICKLESS-IDLE.md, #299). Arm THIS CPU's
// banked virtual timer as a ONE-SHOT firing at absolute counter value
// `target_cnt` (a CNTVCT timestamp -- e.g. a g_timerwait sleep_deadline, or
// timer_ns_to_counter(deadline_ns)). Reads CNTVCT, writes the clamped reload,
// leaves CNTV_CTL at ENABLE. When the shot fires, timer_irq_handler re-arms
// the periodic g_reload exactly as for any tick -- correct the moment the CPU
// has runnable work again; the idle loop (TI-2) re-arms the one-shot before
// each WFI so a genuinely-idle CPU takes no 1 kHz ticks. MUST run after
// timer_init (same precondition as timer_arm_this_cpu). At TI-1 NO production
// path calls this -- the periodic tick still drives idle; the idle-loop wiring
// lands at TI-2.
void timer_arm_oneshot_cnt(u64 target_cnt);

// Pure clamp helper behind timer_arm_oneshot_cnt: the CNTV_TVAL reload that
// fires at absolute counter `target_cnt` given the current counter `now_cnt`.
// CNTV_TVAL counts DOWN, so the reload is the delta (target - now), clamped to
// [TIMER_MIN_RELOAD, TIMER_MAX_RELOAD]. A target at-or-before now yields delta
// 0 -> clamps UP to MIN (fire ASAP, never a 0/negative reload that fires every
// counter tick); a target past the 32-bit horizon clamps DOWN to MAX (the idle
// loop re-arms; the tickless backstop keeps real arms well inside range).
// Split out so the clamp is unit-testable without touching the live timer.
u32 timer_oneshot_tval(u64 target_cnt, u64 now_cnt);

// IRQ handler. Signature matches gic_irq_handler_t. Increments the tick
// counter and reloads CNTV_TVAL_EL0. Caller wires this via gic_attach.
void timer_irq_handler(u32 intid, void *arg);

// Read the current tick count (incremented once per timer fire).
// Returns 0 before timer_init runs.
u64 timer_get_ticks(void);

// Read CNTVCT_EL0 (the virtual architectural counter; not affected by
// ticks). Useful for fine-grained delta measurements; coarse ticks come
// from timer_get_ticks. Same timebase as the virtual timer + tsleep.
u64 timer_get_counter(void);

// Read CNTFRQ_EL0 (counter frequency in Hz). Cached after timer_init.
u32 timer_get_freq(void);

// The full u64 CNTVCT frequency (== the divisor timer_now_ns uses). Distinct
// from timer_get_freq's u32 truncation; the vDSO clock page seeds this so a
// userspace cnt/freq is bit-identical to the syscall (vDSO audit F1).
u64 timer_freq_hz(void);

// P5-tsleep: current monotonic time in nanoseconds, derived from the
// virtual architectural counter (CNTVCT_EL0). The timebase for tsleep /
// poll / futex deadlines — a caller computes its deadline as
// `timer_now_ns() + timeout_ns`. Returns 0 before timer_init runs.
u64 timer_now_ns(void);

// P5-tsleep: convert an absolute timer_now_ns nanosecond timestamp to
// an architectural-counter value, comparable against
// timer_get_counter(). tsleep stores the converted deadline so the
// scheduler-tick scan compares raw counter values (no per-tick
// division). Returns 0 before timer_init runs.
//
// Exact while (ns / 1e9) * CNTFRQ stays within u64 — at a 62.5 MHz
// counter that is the entire u64 ns range; only a hypothetical counter
// above ~1 GHz could overflow for a near-UINT64_MAX ns and wrap. v1.0
// targets (QEMU virt, Pi 5) stay well within range.
u64 timer_ns_to_counter(u64 ns);

// Spin-wait until at least N ticks have elapsed. Used at boot to
// observe the timer firing before printing the tick count. WFI inside
// the loop so the CPU sleeps until the next IRQ rather than spin-hot.
void timer_busy_wait_ticks(u64 n);

// ---------------------------------------------------------------------------
// LS-K: wall clock (CLOCK_REALTIME) over the monotonic timebase.
//
// timer_now_ns() above IS CLOCK_MONOTONIC (ns since boot). The wall clock is a
// boot-time anchor: the kernel reads the RTC once (rtc_read_epoch_seconds) and
// ties that epoch to the monotonic counter, so CLOCK_REALTIME advances with the
// same fast counter without ever re-reading the slow RTC. See ARCH §22.6.
// ---------------------------------------------------------------------------

// Compute the wall-clock offset (epoch_ns - mono_now_ns) the realtime clock adds
// to the monotonic clock. `epoch_seconds` == 0 (no RTC) yields 0 -- the
// fail-soft case where CLOCK_REALTIME == CLOCK_MONOTONIC (1970 + uptime). Pure
// (no global state); split out so the offset math is unit-testable without
// mutating the live clock. For any plausible epoch, epoch_ns (~1.6e18) dominates
// mono_now_ns (boot-early), so the subtraction never underflows.
u64 timer_wallclock_offset_ns(u64 epoch_seconds, u64 mono_now_ns);

// Set the wall-clock anchor from the boot RTC read. WRITE-ONCE on the boot CPU,
// before smp_init (the g_freq discipline): the offset is a single aligned u64,
// so an unsynchronized read on any CPU is a coherent snapshot (no lock). Stores
// timer_wallclock_offset_ns(epoch_seconds, timer_now_ns()).
void timer_set_wallclock_anchor(u64 epoch_seconds);

// Re-anchor the wall clock at runtime (SYS_CLOCK_SETTIME, net-7a) from a
// full-nanosecond epoch. Same single atomic-u64 publish as the boot anchor, but
// callable after smp_init: a concurrent timer_realtime_ns on another CPU reads
// the old or the new offset, each coherent (the single-u64 design needs no
// seqlock). CLOCK_MONOTONIC is untouched. The caller (sys_clock_settime_handler)
// enforces the CAP_HOSTOWNER gate + clk_id == REALTIME + bounds epoch_ns so the
// seconds*1e9 + nsec composition cannot overflow.
void timer_reset_wallclock_anchor_ns(u64 epoch_ns);

// Current CLOCK_REALTIME in nanoseconds since the Unix epoch:
// timer_now_ns() + the wall-clock offset. Before timer_set_wallclock_anchor (or
// after anchoring a 0 epoch) this equals timer_now_ns() (== 1970 + uptime).
u64 timer_realtime_ns(void);

// The current wall-clock offset (the g_wallclock_offset_ns of CLOCK_REALTIME =
// CLOCK_MONOTONIC + offset). Used by vdso_init to seed the shared clock page
// from the boot anchor (which runs before burrow_init / vdso_init); subsequent
// updates ride wallclock_publish_ns -> vdso_publish_wall. An aligned-u64 atomic
// load (no seqlock; the single-field invariant).
u64 timer_wallclock_offset_ns_now(void);

// P4-Ic-latency: enable EL0 reads of the architectural counter via
// CNTKCTL_EL1. Sets EL0VCTEN (virtual counter; the timebase userspace
// should read, matching the virtual timer) and EL0PCTEN (physical
// counter; harmless where available). Without the relevant bit an EL0
// `mrs x, cntv?ct_el0` traps to EL1 with EC=0x18. Required for the
// IRQ-to-userspace latency benchmark + future vDSO clock_gettime.
// Per-CPU register; must be set on every CPU at bringup. Idempotent +
// <10 cycles; safe to call from boot_main + per_cpu_main.
void timer_enable_el0_counter_access(void);

#endif // THYLACINE_ARCH_ARM64_TIMER_H
