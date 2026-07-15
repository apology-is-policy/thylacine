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
#include <thylacine/proc.h>     // 8a-2b: struct Proc + proc_debug_stop_deliver (bp-fire -> stop)
#include <thylacine/thread.h>   // 8a-2b: current_thread() in the EC handler
#include <thylacine/smp.h>      // 8a-2b: smp_cpu_idx_self() -- the per-CPU install key
#include "../../mm/slub.h"      // 8a-2b: kfree (hwdebug_free)

// DBGBCR<n>_EL1 for an enabled, EL0-only, full-instruction breakpoint:
//   E=1 (bit 0), PMC=0b10 (bits 2:1 → EL0), BAS=0xF (bits 8:5), HMC=0, SSC=0,
//   unlinked. DEBUG-FS-DESIGN §5.2.
#define DBGBCR_EL0_BP   0x1E5ull
#define MDSCR_MDE_BIT   (1ull << 15)   // MDSCR_EL1.MDE — enable HW bp/wp exceptions

// The v1.0 usable breakpoint count = min(implemented, DEBUG_HWBP_SLOTS). Computed
// at enumerate (num_brps is boot-CPU-derived; homogeneous v1.0 target -- the
// AT_HWCAP row's F5 secondary-AND-in seam applies identically here). load/clear
// touch only slots [0, g_debug_max_bp); init clears all IMPLEMENTED slots.
static u32 g_debug_max_bp;

// Per-CPU "our debug regs are loaded" flag. Set by hwdebug_switch_in when it
// loads a debugged Proc's breakpoints onto this CPU; cleared when it disables
// them. Lets the common (non-debugged) switch skip every MSR -- a single bool
// read + a not-taken branch. Written only on the local CPU under IRQ-mask (the
// switch path masks; the EC benign path masks) so no cross-CPU race.
static bool g_cpu_debug_loaded[DTB_MAX_CPUS];

// Write DBGBVR<n>_EL1 = vr, DBGBCR<n>_EL1 = cr (no ISB -- callers batch one ISB
// after the last slot + the MDSCR write, per ARM ARM B2.4). n bounds the caller
// to implemented slots (< g_hw_features.num_brps). A single-instruction register
// index needs a compile-time register name, so a bounded slot generator (the
// Linux AARCH64_DBG_REG idiom) -- confined + immediately #undef'd, not a macro
// abstraction.
static void hwbp_write_slot(unsigned n, u64 vr, u64 cr) {
    switch (n) {
#define BP_SLOT(N) case N: __asm__ __volatile__( \
        "msr dbgbvr" #N "_el1, %0\n msr dbgbcr" #N "_el1, %1" \
        :: "r"(vr), "r"(cr) : "memory"); break
    BP_SLOT(0);  BP_SLOT(1);  BP_SLOT(2);  BP_SLOT(3);
    BP_SLOT(4);  BP_SLOT(5);  BP_SLOT(6);  BP_SLOT(7);
    BP_SLOT(8);  BP_SLOT(9);  BP_SLOT(10); BP_SLOT(11);
    BP_SLOT(12); BP_SLOT(13); BP_SLOT(14); BP_SLOT(15);
#undef BP_SLOT
    default: break;
    }
}

// Write DBGWVR<n>_EL1 = vr, DBGWCR<n>_EL1 = cr. Used at v1.0 only to CLEAR wp
// slots (vr=cr=0) so a reset-garbage DBGWCR cannot fire once MDE is set for a
// breakpoint; 8a-2b-3 arms real watchpoints through it.
static void hwwp_write_slot(unsigned n, u64 vr, u64 cr) {
    switch (n) {
#define WP_SLOT(N) case N: __asm__ __volatile__( \
        "msr dbgwvr" #N "_el1, %0\n msr dbgwcr" #N "_el1, %1" \
        :: "r"(vr), "r"(cr) : "memory"); break
    WP_SLOT(0);  WP_SLOT(1);  WP_SLOT(2);  WP_SLOT(3);
    WP_SLOT(4);  WP_SLOT(5);  WP_SLOT(6);  WP_SLOT(7);
    WP_SLOT(8);  WP_SLOT(9);  WP_SLOT(10); WP_SLOT(11);
    WP_SLOT(12); WP_SLOT(13); WP_SLOT(14); WP_SLOT(15);
#undef WP_SLOT
    default: break;
    }
}

#define MDSCR_SS_BIT   (1ull << 0)   // MDSCR_EL1.SS — enable the single-step machine (8a-2b-2)

static void hwdebug_set_mdscr(bool mde, bool ss) {
    u64 mdscr;
    __asm__ __volatile__("mrs %0, mdscr_el1" : "=r"(mdscr));
    if (mde) mdscr |= MDSCR_MDE_BIT; else mdscr &= ~MDSCR_MDE_BIT;
    if (ss)  mdscr |= MDSCR_SS_BIT;  else mdscr &= ~MDSCR_SS_BIT;
    __asm__ __volatile__("msr mdscr_el1, %0\n isb" :: "r"(mdscr) : "memory");
}

// Load `next`'s debug state onto THIS CPU: arm the breakpoints (slots [0, bp_count)
// EXCEPT a step-over VA -- loaded disabled so a step FROM a breakpointed PC does
// not re-trap; the rest of [0, g_debug_max_bp) disabled), then set MDSCR (MDE if
// any bp is armed, SS if a step is in flight) + one ISB. Caller: hwdebug_switch_in
// (IRQ-masked, CPU stable). `bp_va` is quiescent-mutated (stopped-only), stable
// behind the atomic bp_count gate; NULL is safe when bp_count==0 (loop never reads
// it). MDSCR.SS is loaded here (not held per-CPU) so it FOLLOWS the thread across
// an IRQ-preempt migration mid-step (the Linux per-task model).
static void hwdebug_load_debug(u32 bp_count, const u64 *bp_va, u64 skip_va, bool ss) {
    u64 skip = skip_va & ~3ull;
    for (u32 n = 0; n < g_debug_max_bp; n++) {
        if (n < bp_count && !(skip_va && (bp_va[n] & ~3ull) == skip))
            hwbp_write_slot(n, bp_va[n] & ~3ull, DBGBCR_EL0_BP);
        else
            hwbp_write_slot(n, 0, 0);   // unused slot, or the step-over bp (disabled for the step)
    }
    hwdebug_set_mdscr(bp_count > 0, ss);
}

// Disable all our breakpoint slots + MDE + SS on THIS CPU.
static void hwdebug_clear_debug(void) {
    for (u32 n = 0; n < g_debug_max_bp; n++) hwbp_write_slot(n, 0, 0);
    hwdebug_set_mdscr(false, false);
}

// Clear ONLY MDSCR.SS on THIS CPU (leave MDE / bps as-is) -- the EC 0x32 disarm.
// IRQ-masked to pin the CPU across the register write.
static void hwdebug_disable_ss_this_cpu(void) {
    u64 s;
    __asm__ __volatile__("mrs %0, daif\n msr daifset, #2" : "=r"(s) :: "memory");
    u64 mdscr;
    __asm__ __volatile__("mrs %0, mdscr_el1" : "=r"(mdscr));
    mdscr &= ~MDSCR_SS_BIT;
    __asm__ __volatile__("msr mdscr_el1, %0\n isb" :: "r"(mdscr) : "memory");
    __asm__ __volatile__("msr daif, %0" :: "r"(s) : "memory");
}

// Local IRQ mask (mirror of spinlock.h's mechanism) -- pins the CPU across a
// this-CPU debug-register write from the EC benign path (the switch path already
// runs IRQ-masked).
static inline u64 hwdebug_irq_save(void) {
    u64 s;
    __asm__ __volatile__("mrs %0, daif\n msr daifset, #2" : "=r"(s) :: "memory");
    return s;
}
static inline void hwdebug_irq_restore(u64 s) {
    __asm__ __volatile__("msr daif, %0" :: "r"(s) : "memory");
}

void hwdebug_init_cpu(void) {
    // The OS Lock (OSLSR_EL1.OSLK) is LOCKED at reset and suppresses debug
    // exceptions. Clear it + the OS Double-Lock, and idle MDSCR (MDE=0, SS=0).
    // Banked per-PE → runs on every CPU.
    __asm__ __volatile__(
        "msr oslar_el1, xzr\n"    // OS Lock  → unlocked (OSLAR.OSLK write of 0)
        "msr osdlr_el1, xzr\n"    // OS Double-Lock → clear
        "msr mdscr_el1, xzr\n"    // MDE=0, SS=0 — known state
        "isb\n"
        ::: "memory");
    // Clear EVERY implemented bp/wp control register (reset value is UNKNOWN). A
    // stale E-bit in ANY slot would fire the first time we later set MDE=1 for a
    // breakpoint (MDE gates BOTH bp and wp exceptions), so this must cover all
    // implemented slots -- not just the ones the v1.0 table uses. num_brps/wrps
    // are enumerated before this runs (hw_features_detect precedes init on the
    // boot CPU; secondaries see the boot value). Homogeneous v1.0 assumption.
    unsigned nb = g_hw_features.num_brps; if (nb > 16) nb = 16;
    unsigned nw = g_hw_features.num_wrps; if (nw > 16) nw = 16;
    for (unsigned n = 0; n < nb; n++) hwbp_write_slot(n, 0, 0);
    for (unsigned n = 0; n < nw; n++) hwwp_write_slot(n, 0, 0);
    __asm__ __volatile__("isb" ::: "memory");
}

void hwdebug_enumerate(void) {
    u64 dfr0;
    __asm__ __volatile__("mrs %0, id_aa64dfr0_el1" : "=r"(dfr0));
    // BRPs (bits 15:12) + 1 = number of breakpoints; WRPs (23:20) + 1 =
    // watchpoints (both fields hold count-minus-one; architectural min 2/2).
    u32 nb = ((u32)(dfr0 >> 12) & 0xF) + 1u;
    u32 nw = ((u32)(dfr0 >> 20) & 0xF) + 1u;
    g_hw_features.num_brps = (u8)nb;
    g_hw_features.num_wrps = (u8)nw;
    g_debug_max_bp = nb < DEBUG_HWBP_SLOTS ? nb : DEBUG_HWBP_SLOTS;
}

// ---- 8a-2b: the per-Proc breakpoint table + install + EC route ----------

bool hwdebug_bp_add(struct debug_hw *hw, u64 va) {
    if (!hw) return false;
    va &= ~3ull;   // A64 instructions are 4-byte aligned
    for (u32 i = 0; i < hw->bp_count; i++)
        if (hw->bp_va[i] == va) return false;                 // already present
    if (hw->bp_count >= g_debug_max_bp) return false;         // table full (or 0 slots)
    hw->bp_va[hw->bp_count] = va;
    __atomic_store_n(&hw->bp_count, hw->bp_count + 1, __ATOMIC_RELEASE);
    return true;
}

bool hwdebug_bp_remove(struct debug_hw *hw, u64 va) {
    if (!hw) return false;
    va &= ~3ull;
    for (u32 i = 0; i < hw->bp_count; i++) {
        if (hw->bp_va[i] != va) continue;
        // Compact: move the last entry into the hole, then drop the count. The
        // count drop is RELEASE-ordered last so a concurrent switch-IN / match
        // reads a consistent (count, va[]) prefix.
        hw->bp_va[i] = hw->bp_va[hw->bp_count - 1];
        __atomic_store_n(&hw->bp_count, hw->bp_count - 1, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

void hwdebug_bp_clear_all(struct debug_hw *hw) {
    if (hw) __atomic_store_n(&hw->bp_count, 0u, __ATOMIC_RELEASE);
}

void hwdebug_free(struct debug_hw *hw) {
    if (hw) kfree(hw);
}

void hwdebug_switch_in(struct Thread *next) {
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) return;
    struct Proc *p = next ? next->proc : NULL;
    struct debug_hw *hw = p ? __atomic_load_n(&p->debug_hw, __ATOMIC_ACQUIRE) : NULL;
    u32 count = hw ? __atomic_load_n(&hw->bp_count, __ATOMIC_ACQUIRE) : 0;
    bool ss = next ? __atomic_load_n(&next->debug_ss_armed, __ATOMIC_ACQUIRE) : false;
    if (count > 0 || ss) {
        u64 skip = ss ? next->debug_stepover_va : 0;   // step-over: skip this bp during the step
        hwdebug_load_debug(count, count ? hw->bp_va : (const u64 *)0, skip, ss);
        g_cpu_debug_loaded[cpu] = true;
    } else if (g_cpu_debug_loaded[cpu]) {
        hwdebug_clear_debug();
        g_cpu_debug_loaded[cpu] = false;
    }
    // else: the common path -- next is not debugged/stepping and this CPU has
    // nothing loaded -> zero MSRs.
}

// Disable this CPU's breakpoints + MDE + SS (the EC benign path). IRQ-masked so
// the CPU is pinned across the local register writes.
static void hwdebug_disable_this_cpu(void) {
    u64 s = hwdebug_irq_save();
    unsigned cpu = smp_cpu_idx_self();
    hwdebug_clear_debug();
    if (cpu < DTB_MAX_CPUS) g_cpu_debug_loaded[cpu] = false;
    hwdebug_irq_restore(s);
}

bool hwdebug_breakpoint_from_el0(u64 elr) {
    struct Thread *t = current_thread();
    if (!t || t->magic != THREAD_MAGIC) return false;
    struct Proc *p = t->proc;
    if (!p || p->magic != PROC_MAGIC) return false;
    struct debug_hw *hw = __atomic_load_n(&p->debug_hw, __ATOMIC_ACQUIRE);
    if (!hw) return false;   // never debugged -> a truly stray EC 0x30 -> fatal at the caller

    u64 pc = elr & ~3ull;
    u32 count = __atomic_load_n(&hw->bp_count, __ATOMIC_ACQUIRE);
    bool match = false;
    for (u32 i = 0; i < count && i < DEBUG_HWBP_SLOTS; i++)
        if (hw->bp_va[i] == pc) { match = true; break; }

    if (match) {
        // A real breakpoint hit: request the whole Proc stop (Delve all-stop).
        // proc_debug_stop_deliver sets p->debug_stop_req (RELEASE) + kicks the
        // peers; the current thread returns from here to the EL0-return tail,
        // observes the flag, and parks (el0_return_stop_check). Death still wins
        // (the tail's die-check precedes the stop-check). The debugger learns via
        // /proc/<pid>/wait and reads regs.pc == this bp VA (ELR = the bp'd
        // instruction, not yet executed). No lock needed: the flag store is
        // atomic and this thread observes it at its own tail.
        proc_debug_stop_deliver(p);
        return true;
    }

    // No match, but this Proc HAS a debug_hw (is/was debugged): a STALE bp that
    // was loaded on this CPU before a table change (a detach cleared the table;
    // its reload has not reached this CPU). Benign -- ONLY the kernel arms a bp
    // (EL0 cannot touch MDSCR/DBGB*), so an unmatched fire is never an attack.
    // Disable this CPU's debug regs and resume the instruction (the caller
    // returns -> the tail sees no stop -> eret re-executes it, now un-trapped).
    hwdebug_disable_this_cpu();
    return true;
}

bool hwdebug_singlestep_from_el0(u64 elr) {
    (void)elr;   // the debugger reads the advanced PC via /proc/<pid>/regs, not here
    struct Thread *t = current_thread();
    if (!t || t->magic != THREAD_MAGIC) return false;
    if (!__atomic_load_n(&t->debug_ss_armed, __ATOMIC_ACQUIRE)) {
        // A software-step EC with no armed step: spurious (only the kernel sets
        // MDSCR.SS). Benign -- clear SS on this CPU + resume the instruction.
        // Never fatal (EL0 cannot arm it); defense-in-depth.
        hwdebug_disable_ss_this_cpu();
        return true;
    }
    struct Proc *p = t->proc;
    if (!p || p->magic != PROC_MAGIC) return false;
    // Our step completed: exactly one EL0 instruction executed (ELR = the NEXT
    // PC). Disarm the step (the per-Thread flags + this CPU's MDSCR.SS) and
    // re-stop the whole Proc so the thread re-parks at the tail (death wins there)
    // and the debugger's step/wait returns with the advanced regs.pc. Clearing
    // debug_ss_armed BEFORE the re-stop keeps a racing switch-IN from re-arming SS
    // (specs/debug_step.tla StepExec -> Tail: one instruction, then re-park).
    __atomic_store_n(&t->debug_ss_armed, false, __ATOMIC_RELEASE);
    t->debug_stepover_va = 0;
    hwdebug_disable_ss_this_cpu();
    proc_debug_stop_deliver(p);
    return true;
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
    // 8a-2b: the real per-Proc install now also arms breakpoints, so match ELR
    // against the verify's OWN VA -- swallow only the verify's bp, letting a real
    // per-Proc bp fall through to hwdebug_breakpoint_from_el0. (They never coexist
    // in practice: the verify is boot-only + self-scoped; real bps are session-
    // only. The ELR match keeps that a fact, not an assumption.)
    bool hit = g_hwbp.armed && ((g_hwbp.va & ~3ull) == (elr & ~3ull));
    if (hit) {
        // Record + disarm so the resumed EL0 instruction does not re-trap. Runs
        // on the CPU whose DBGBCR0 fired = the CPU that armed it.
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
