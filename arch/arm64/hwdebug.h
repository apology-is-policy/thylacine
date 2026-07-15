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
