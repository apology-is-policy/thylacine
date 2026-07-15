// arch/arm64/hwdebug.c — arm64 hardware-debug bring-up + the 8a-2a verify.
//
// See hwdebug.h for the contract. 8a-2a is the empirical HVF/TCG confirmation
// that a guest-programmed EL0 hardware breakpoint delivers EC 0x30 to guest
// EL1. The register recipe (DEBUG-FS-DESIGN §5.2) is the same one 8a-2b uses;
// here it drives a single self-scoped breakpoint that the kernel swallows.

#include "hwdebug.h"
#include "hwfeat.h"

#include <thylacine/types.h>
#include <thylacine/spinlock.h>

// DBGBCR<n>_EL1 for an enabled, EL0-only, full-instruction breakpoint:
//   E=1 (bit 0), PMC=0b10 (bits 2:1 → EL0), BAS=0xF (bits 8:5), HMC=0, SSC=0,
//   unlinked. DEBUG-FS-DESIGN §5.2.
#define DBGBCR_EL0_BP   0x1E5ull
#define MDSCR_MDE_BIT   (1ull << 15)   // MDSCR_EL1.MDE — enable HW bp/wp exceptions

void hwdebug_init_cpu(void) {
    // The OS Lock (OSLSR_EL1.OSLK) is LOCKED at reset and suppresses debug
    // exceptions. Clear it + the OS Double-Lock, and idle MDSCR/DBGBCR0
    // (MDE=0, no armed breakpoint). Banked per-PE → runs on every CPU.
    __asm__ __volatile__(
        "msr oslar_el1, xzr\n"    // OS Lock  → unlocked (OSLAR.OSLK write of 0)
        "msr osdlr_el1, xzr\n"    // OS Double-Lock → clear
        "msr mdscr_el1, xzr\n"    // MDE=0, SS=0 — known state
        "msr dbgbcr0_el1, xzr\n"  // breakpoint 0 disabled
        "isb\n"
        ::: "memory");
}

void hwdebug_enumerate(void) {
    u64 dfr0;
    __asm__ __volatile__("mrs %0, id_aa64dfr0_el1" : "=r"(dfr0));
    // BRPs (bits 15:12) + 1 = number of breakpoints; WRPs (23:20) + 1 =
    // watchpoints (both fields hold count-minus-one; architectural min 2/2).
    g_hw_features.num_brps = (u8)(((dfr0 >> 12) & 0xF) + 1);
    g_hw_features.num_wrps = (u8)(((dfr0 >> 20) & 0xF) + 1);
}

// The single, one-at-a-time verify slot. `armed` is the live-breakpoint flag;
// `valid` stays set after a verify concludes so the ctl read can report the
// result to the arming Proc. All under g_hwbp.lock (a leaf — no nested locks,
// no sleep; touched only when a breakpoint is armed/disarmed or actually fires,
// never on the common exception path).
static struct {
    spin_lock_t lock;
    bool armed;    // a breakpoint is currently programmed
    bool valid;    // a verify has been run — `fired`/`elr`/`pid` are meaningful
    bool fired;    // the armed breakpoint delivered EC 0x30
    int  pid;      // the Proc that armed it (only it reads the result)
    u64  va;       // the armed VA (4-byte aligned)
    u64  elr;      // the EL0 PC that trapped
} g_hwbp = { .lock = SPIN_LOCK_INIT };

// Program DBGBVR0/DBGBCR0 + MDSCR.MDE on THIS CPU. Caller holds g_hwbp.lock.
static void hwbp_program_locked(u64 va) {
    u64 mdscr;
    __asm__ __volatile__("mrs %0, mdscr_el1" : "=r"(mdscr));
    mdscr |= MDSCR_MDE_BIT;
    __asm__ __volatile__(
        "msr dbgbvr0_el1, %0\n"
        "msr dbgbcr0_el1, %1\n"
        "msr mdscr_el1, %2\n"
        "isb\n"
        :: "r"(va), "r"(DBGBCR_EL0_BP), "r"(mdscr) : "memory");
}

// Disable DBGBCR0 + clear MDSCR.MDE on THIS CPU. Caller holds g_hwbp.lock.
static void hwbp_unprogram_locked(void) {
    u64 mdscr;
    __asm__ __volatile__("mrs %0, mdscr_el1" : "=r"(mdscr));
    mdscr &= ~MDSCR_MDE_BIT;
    __asm__ __volatile__(
        "msr dbgbcr0_el1, xzr\n"
        "msr dbgbvr0_el1, xzr\n"
        "msr mdscr_el1, %0\n"
        "isb\n"
        :: "r"(mdscr) : "memory");
}

bool hwdebug_verify_arm(int pid, u64 va) {
    va &= ~3ull;   // A64 instructions are 4-byte aligned; DBGBVR ignores [1:0]
    irq_state_t s = spin_lock_irqsave(&g_hwbp.lock);
    bool ok = !g_hwbp.armed;   // one at a time
    if (ok) {
        g_hwbp.armed = true;
        g_hwbp.valid = true;
        g_hwbp.fired = false;
        g_hwbp.pid   = pid;
        g_hwbp.va    = va;
        g_hwbp.elr   = 0;
        hwbp_program_locked(va);
    }
    spin_unlock_irqrestore(&g_hwbp.lock, s);
    return ok;
}

bool hwdebug_verify_on_ec(u64 elr) {
    irq_state_t s = spin_lock_irqsave(&g_hwbp.lock);
    bool hit = g_hwbp.armed;
    if (hit) {
        // Only the verify arms a breakpoint today (8a-2b adds the real
        // per-thread install), so any EC 0x30 while armed IS the verify's
        // breakpoint. Record + disarm so the resumed EL0 instruction does not
        // re-trap. Runs on the CPU whose DBGBCR0 fired = the CPU that armed it.
        g_hwbp.fired = true;
        g_hwbp.elr   = elr;
        g_hwbp.armed = false;   // consumed; `valid` stays for the result read
        hwbp_unprogram_locked();
    }
    spin_unlock_irqrestore(&g_hwbp.lock, s);
    return hit;
}

bool hwdebug_verify_result(int pid, bool *fired, u64 *elr) {
    irq_state_t s = spin_lock_irqsave(&g_hwbp.lock);
    bool have = g_hwbp.valid && g_hwbp.pid == pid;
    if (have) {
        if (fired) *fired = g_hwbp.fired;
        if (elr)   *elr   = g_hwbp.elr;
    }
    spin_unlock_irqrestore(&g_hwbp.lock, s);
    return have;
}

void hwdebug_verify_disarm(void) {
    irq_state_t s = spin_lock_irqsave(&g_hwbp.lock);
    if (g_hwbp.armed) {
        hwbp_unprogram_locked();
        g_hwbp.armed = false;
    }
    spin_unlock_irqrestore(&g_hwbp.lock, s);
}
