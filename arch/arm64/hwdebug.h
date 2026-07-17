// arch/arm64/hwdebug.h — arm64 hardware-debug (Go IDE Stage 8a-2).
//
// 8a-2a lands the two reusable 8a-2b prerequisites (the per-CPU OS-Lock unlock
// + the ID_AA64DFR0_EL1 breakpoint/watchpoint enumeration) plus a minimal,
// self-scoped EL0-breakpoint VERIFY: the empirical confirmation that a
// guest-programmed hardware breakpoint delivers its debug exception (EC 0x30)
// to guest EL1 on this substrate (TCG always; HVF on Apple Silicon is the young
// path the design requires proven before 8a-2b builds on it). The full
// per-thread bp/wp install + single-step + watchpoints land at 8a-2b; this
// header carries only what 8a-2a needs.

#ifndef THYLACINE_ARCH_ARM64_HWDEBUG_H
#define THYLACINE_ARCH_ARM64_HWDEBUG_H

#include <thylacine/types.h>

// Per-PE debug bring-up: clear the OS Lock (LOCKED at reset — it suppresses
// debug exceptions) + the OS Double-Lock, and put MDSCR/DBGBCR0 in a known
// idle state (MDE=0, no breakpoint armed). Called on EVERY CPU (boot in
// main.c; each secondary in per_cpu_main) because these are banked per-PE.
void hwdebug_init_cpu(void);

// Read ID_AA64DFR0_EL1 and record the breakpoint / watchpoint counts into
// g_hw_features (num_brps / num_wrps = the DFR0 field + 1; architectural min
// 2 / 2). Called once at boot from hw_features_detect.
void hwdebug_enumerate(void);

// ---- 8a-2b: real per-Proc hardware breakpoints --------------------------
//
// DEBUG-FS-DESIGN section 5. A debugger arms breakpoints at user VAs in a
// STOPPED target (the ctl `hwbreak <hexva>` / `hwrmbreak <hexva>` verbs); the
// kernel installs that Proc's breakpoint set into a CPU's DBGB* + MDSCR.MDE on
// every context-switch-IN to one of the target's threads and disables them on
// switch-OUT (the per-CPU-MDE isolation that fires a bp ONLY while the debugged
// Proc runs -- PMC=EL0 alone does NOT isolate two Procs sharing a user VA). A bp
// fire (EC 0x30) routes to a whole-Proc stop via the 8a-1 checkpoint machinery.

#define DEBUG_HWBP_SLOTS 4u   // v1.0 breakpoint table size (clamped to num_brps)
#define DEBUG_HWWP_SLOTS 4u   // v1.0 watchpoint table size (clamped to num_wrps; 8a-2b-3)

// Watchpoint access flags -- the LSC (load/store control) the debugger wants to
// trap. R traps reads/loads, W traps writes/stores; both = data-access. At least
// one must be set (the ctl parser rejects an empty rwx).
#define DEBUG_WP_R  0x1u
#define DEBUG_WP_W  0x2u

// The per-Proc debug-register table (lazily kmalloc'd on the first hwbreak/hwwatch,
// freed at proc_free). MUTATED only when the target is fully-stopped (all threads
// parked, no CPU running a target thread -> the ctx-switch reader is quiescent), so
// the switch-IN read needs no lock; bp_count / wp_count are __atomic so a
// detach-while-running (count -> 0) races cleanly with a switch-IN / a match.
struct debug_hw {
    u32 bp_count;                     // armed breakpoints occupy bp_va[0 .. bp_count)
    u64 bp_va[DEBUG_HWBP_SLOTS];      // 4-byte-aligned user VAs
    // 8a-2b-3: the watchpoint table (parallel to the bp table; same lifetime +
    // quiescence discipline). Each armed slot is a data-access watchpoint on the
    // region [wp_va, wp_va + wp_len) within ONE 8-byte doubleword.
    u32 wp_count;                     // armed watchpoints occupy slots [0 .. wp_count)
    u64 wp_va[DEBUG_HWWP_SLOTS];      // the watched VA (encode aligns to the doubleword)
    u32 wp_len[DEBUG_HWWP_SLOTS];     // region length 1..8 (single doubleword: (va&7)+len<=8)
    u8  wp_flags[DEBUG_HWWP_SLOTS];   // DEBUG_WP_R | DEBUG_WP_W
};

// Table mutation (caller holds g_proc_table_lock; the target is fully-stopped for
// add/remove -- hwbreak/hwrmbreak/hwwatch/hwrmwatch; detach's count=0 clear is
// stopped-or-running-benign). hwdebug_free is the proc_free teardown.
bool hwdebug_bp_add(struct debug_hw *hw, u64 va);     // false: table full or already present
bool hwdebug_bp_remove(struct debug_hw *hw, u64 va);  // false: not present
void hwdebug_bp_clear_all(struct debug_hw *hw);       // bp_count = 0 (detach / close release)
void hwdebug_free(struct debug_hw *hw);               // kfree (proc_free)

// Watchpoint table mutation (8a-2b-3; same discipline as the bp table). add rejects
// a cross-doubleword region ((va&7)+len>8), a bad len (0 or >8), empty flags, a
// duplicate VA, or a full table. remove keys on the watched VA.
bool hwdebug_wp_add(struct debug_hw *hw, u64 va, u32 len, u8 flags);  // false: bad/dup/full
bool hwdebug_wp_remove(struct debug_hw *hw, u64 va);                  // false: not present
void hwdebug_wp_clear_all(struct debug_hw *hw);                       // wp_count = 0

// Encode a watchpoint into DBGWVR/DBGWCR (EL0-only, the requested LSC, BAS covering
// [va, va+len) within the doubleword). Exposed for the EL1 unit test of the
// register math (the E2E proves DELIVERY; this proves the ENCODING).
void hwdebug_wp_encode(u64 va, u32 len, u8 flags, u64 *vr_out, u64 *cr_out);

// The context-switch install/clear hook. Loads next's Proc breakpoints onto THIS
// CPU (or clears them + MDE if next is not a debugged Proc and this CPU had them
// loaded). Called from the switch path with IRQs masked + the CPU stable
// (sched.c, after sched_install_asid_ttbr0, before cpu_switch_context).
struct Thread;
void hwdebug_switch_in(struct Thread *next);

// The EC 0x30 (breakpoint from EL0) handler. Returns true if handled: a matched
// bp -> the whole Proc stop is delivered (proc_debug_stop_deliver) and the
// current thread parks at the EL0-return tail; OR a benign STALE bp after a table
// change (detach cleared the table before this CPU reloaded) -> this CPU's debug
// regs are disabled and the instruction resumes. Returns false only when
// `current`'s Proc was never debugged (debug_hw == NULL) -- a truly stray EC the
// caller treats as fatal (only the kernel ever arms a bp; EL0 cannot).
bool hwdebug_breakpoint_from_el0(u64 elr);

// The EC 0x32 (software step from EL0) handler. Returns true if handled: an armed
// step completed (exactly one EL0 instruction executed) -> disarm SS + re-stop
// the whole Proc so the thread re-parks at the tail (the debugger reads the
// advanced regs.pc), OR a spurious step EC (no armed step) -> disable SS on this
// CPU + resume. Returns false only for a corrupt thread/Proc (defensive; only the
// kernel arms MDSCR.SS, so EL0 can never trigger a spurious step). The SS machine
// is armed by el0_return_stop_check (SPSR.SS in the resume frame) +
// hwdebug_switch_in (MDSCR.SS, per-thread so it survives a mid-step migration).
bool hwdebug_singlestep_from_el0(u64 elr);

// The EC 0x34 (data watchpoint from EL0) handler (8a-2b-3). Returns true if
// handled: an armed watchpoint fired (wp_count>0) -> the whole Proc stop is
// delivered and the current thread parks at the EL0-return tail (the debugger reads
// regs.pc); OR a benign STALE wp (wp_count==0 after a detach/hwrmwatch cleared the
// table before this CPU reloaded) -> this CPU's debug regs are disabled and the
// access resumes. Returns false only when `current`'s Proc was never debugged
// (debug_hw == NULL) -- a truly stray EC the caller treats as fatal. `far` is the
// faulting VA (imprecise-within-the-block on arm64; not gated on).
bool hwdebug_watchpoint_from_el0(u64 elr, u64 far);

// ---- The 8a-2a self-scoped EL0-breakpoint verify ------------------------
//
// A boot probe (usr/hwbp-verify) drives this via the debug-fs `hwverify` ctl
// verb (I-39 owner-gated + self-pid + user-half VA — kernel/devproc.c):
//
//   arm(pid, va)  — program DBGBVR0_EL1 = va, DBGBCR0_EL1 = 0x1E5 (E=1,
//                   PMC=0b10 EL0-only, BAS=0xF), MDSCR_EL1.MDE = 1. One at a
//                   time (a concurrent arm is refused). Records the arming pid
//                   so only that Proc reads the result back.
//   on_ec(elr)    — the EC 0x30 handler (exception.c) calls this: if a verify
//                   is armed, record fired + elr, DISARM (so the resumed
//                   instruction does not re-trap), return true (swallowed).
//                   Returns false if no verify is armed → the caller preserves
//                   the pre-8a-2a fatal-to-Proc default (no 8a-2b routing yet).
//   result(pid)   — the ctl read: if the last verify was armed by `pid`, fill
//                   `fired` (bool) + `elr` (the EL0 PC that trapped) and return
//                   true; else false (the read is empty, unchanged).
//   disarm()      — clear the armed breakpoint (the probe's give-up path).
//
// GLOBAL (one verify at a time) because it runs at boot bring-up, and the
// arm→trap window is a single EL0 instruction on one CPU. The lingering-armed-
// on-a-migrated-CPU corner is closed properly by 8a-2b's per-thread install;
// for the verify the probe retries and always disarms.

bool hwdebug_verify_arm(int pid, u64 va);
bool hwdebug_verify_on_ec(u64 elr);
bool hwdebug_verify_result(int pid, bool *fired, u64 *elr);
void hwdebug_verify_disarm(void);

#endif // THYLACINE_ARCH_ARM64_HWDEBUG_H
