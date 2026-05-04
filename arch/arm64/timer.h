// ARM Generic Timer — physical timer at EL1 non-secure (P1-G).
//
// Per ARCHITECTURE.md §12.3 (interrupt handling) + §22.2 (DTB-driven
// hardware discovery).
//
// The ARMv8 generic timer is a system-register peripheral (no MMIO);
// its interrupts arrive via the GIC as PPIs (per-CPU). We use the
// EL1 non-secure physical timer:
//
//   counter:   CNTPCT_EL0           always-running 64-bit counter
//   freq:      CNTFRQ_EL0           counter frequency (Hz)
//   timer val: CNTP_TVAL_EL0        decrementing 32-bit; fires when 0
//   timer cmp: CNTP_CVAL_EL0        absolute compare value (alternative)
//   ctl:       CNTP_CTL_EL0         {ENABLE, IMASK, ISTATUS}
//   intid:     PPI 14 = INTID 30    (architecturally fixed, ARMv8 ARM D11.2.4)
//
// At P1-G the timer fires at 1000 Hz; the IRQ handler increments a
// global tick counter and reloads CNTP_TVAL_EL0. The counter is
// observed in the boot banner ("tick: N") to confirm the kernel is
// receiving IRQs.
//
// Phase 2 will use this as the scheduler tick. A separate timer_oneshot
// API for higher-precision deadline scheduling lands when the EEVDF
// scheduler arrives.

#ifndef THYLACINE_ARCH_ARM64_TIMER_H
#define THYLACINE_ARCH_ARM64_TIMER_H

#include "gic.h"

#include <thylacine/types.h>

// Architectural INTID for the EL1 non-secure physical timer (PPI 14).
// CNTHCTL_EL2.{EL1PCEN,EL1PCTEN}=1 is set in start.S's EL2 drop so EL1
// can read the counter and program the timer without trapping; on
// QEMU virt direct EL1 entry the bits are already set by the firmware.
#define TIMER_INTID_EL1_PHYS_NS  GIC_PPI_TO_INTID(14)   /* = 30 */

// One-time bring-up. Reads CNTFRQ_EL0, computes the reload count for
// `hz` Hz, programs CNTP_TVAL_EL0 + CNTP_CTL_EL0 (ENABLE, !IMASK).
// Caller follows with gic_attach(TIMER_INTID_EL1_PHYS_NS, ...) and
// gic_enable_irq(TIMER_INTID_EL1_PHYS_NS) to route the IRQ through.
//
// `hz` must be > 0 and < CNTFRQ_EL0; typical 1000 Hz gives 1 ms ticks.
// Returns false if hz is out of range.
bool timer_init(u32 hz);

// IRQ handler. Signature matches gic_irq_handler_t. Increments the tick
// counter and reloads CNTP_TVAL_EL0. Caller wires this via gic_attach.
void timer_irq_handler(u32 intid, void *arg);

// Read the current tick count (incremented once per timer fire).
// Returns 0 before timer_init runs.
u64 timer_get_ticks(void);

// Read CNTPCT_EL0 (the architectural counter; not affected by ticks).
// Useful for fine-grained delta measurements; coarse ticks come from
// timer_get_ticks.
u64 timer_get_counter(void);

// Read CNTFRQ_EL0 (counter frequency in Hz). Cached after timer_init.
u32 timer_get_freq(void);

// Spin-wait until at least N ticks have elapsed. Used at boot to
// observe the timer firing before printing the tick count. WFI inside
// the loop so the CPU sleeps until the next IRQ rather than spin-hot.
void timer_busy_wait_ticks(u64 n);

#endif // THYLACINE_ARCH_ARM64_TIMER_H
