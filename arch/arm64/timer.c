// ARM Generic Timer — EL1 non-secure physical timer at fixed Hz (P1-G).
//
// Driven by:
//   CNTFRQ_EL0   — counter frequency (Hz)
//   CNTPCT_EL0   — physical counter (always running)
//   CNTP_TVAL_EL0 — 32-bit signed decrementer; IRQ fires when it goes <= 0
//   CNTP_CTL_EL0  — bit 0 ENABLE, bit 1 IMASK, bit 2 ISTATUS (RO)
//
// The IRQ does not auto-clear — writing CNTP_TVAL_EL0 with a fresh
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

#include <thylacine/extinction.h>
#include <thylacine/types.h>

// Compile-time sanity: PPI 14 → INTID 30 (architectural per ARMv8 ARM
// D11.2.4). Catches a regression to GIC_PPI_TO_INTID's offset.
_Static_assert(TIMER_INTID_EL1_PHYS_NS == 30,
               "TIMER_INTID_EL1_PHYS_NS must be 30 (PPI 14 + offset 16)");

// ---------------------------------------------------------------------------
// CNT* system-register accessors.
// ---------------------------------------------------------------------------

static inline u64 read_cntfrq_el0(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline u64 read_cntpct_el0(void) {
    u64 v;
    // isb before the read so any prior store has retired before we
    // take the timestamp (per ARMv8 generic-timer access ordering).
    __asm__ __volatile__("isb\nmrs %0, cntpct_el0\n" : "=r"(v));
    return v;
}

static inline void write_cntp_tval_el0(u64 v) {
    __asm__ __volatile__("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline u64 read_cntp_ctl_el0(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, cntp_ctl_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_ctl_el0(u64 v) {
    __asm__ __volatile__("msr cntp_ctl_el0, %0\nisb\n" :: "r"(v));
}

#define CNTP_CTL_ENABLE     (1u << 0)
#define CNTP_CTL_IMASK      (1u << 1)
#define CNTP_CTL_ISTATUS    (1u << 2)

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

static u64 g_freq;            // CNTFRQ_EL0 in Hz, cached at init
static u64 g_reload;          // freq / hz, programmed into CNTP_TVAL_EL0
static volatile u64 g_ticks;  // incremented once per IRQ

// ---------------------------------------------------------------------------
// timer_init.
// ---------------------------------------------------------------------------

// CNTP_TVAL_EL0 is a 32-bit signed value per ARMv8 ARM D11.2.4. Reload
// values must fit in [1, INT32_MAX]; reload of 1 fires every counter
// tick (pathological — handler can't keep up), reload > INT32_MAX
// truncates and the timer fires sooner than intended. We bound on
// both sides: minimum reload of 100 keeps IRQ rate under freq/100
// (well under 1% CPU even at 5 GHz counter freq), maximum bounded by
// the architectural register width.
#define TIMER_MIN_RELOAD   100u
#define TIMER_MAX_RELOAD   0x7FFFFFFFu     // INT32_MAX

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

    // Program initial reload, then enable (IMASK=0 so IRQs flow).
    write_cntp_tval_el0(g_reload);
    write_cntp_ctl_el0(CNTP_CTL_ENABLE);

    return true;
}

// ---------------------------------------------------------------------------
// IRQ handler.
// ---------------------------------------------------------------------------

void timer_irq_handler(u32 intid, void *arg) {
    (void)intid;
    (void)arg;
    g_ticks++;
    write_cntp_tval_el0(g_reload);
    // CNTP_CTL stays at ENABLE; no need to rewrite.
}

// ---------------------------------------------------------------------------
// Diagnostics + busy-wait.
// ---------------------------------------------------------------------------

u64 timer_get_ticks(void)   { return g_ticks; }
u64 timer_get_counter(void) { return read_cntpct_el0(); }
u32 timer_get_freq(void)    { return (u32)g_freq; }

void timer_busy_wait_ticks(u64 n) {
    u64 target = g_ticks + n;
    while (g_ticks < target) {
        __asm__ __volatile__("wfi" ::: "memory");
    }
}
