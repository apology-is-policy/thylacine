// /init — first userspace process (P3-F).
//
// Per ARCHITECTURE.md §7.4 + §16. /init is the first userspace process
// the kernel exec's during bring-up. At v1.0 P3-F it's a tiny self-
// contained AArch64 program embedded in the kernel image: prints
// "hello\n" via SYS_PUTS, exits via SYS_EXITS(0).
//
// At Phase 4 / 5 /init becomes a real binary loaded from Stratum
// (Phase 4 9P client) or an embedded ramfs (Phase 5 fs surface);
// the v1.0 embedded blob is the bridge until ramfs lands.
//
// joey_run() is called from boot_main() AFTER all kernel bring-up
// (DTB, MMU, GIC, timer, proc/thread/sched, smp, in-kernel tests,
// fault_test) and BEFORE the "Thylacine boot OK" banner. Boot order
// rationale:
//
//   - Tests must pass first; /init runs in a verified kernel.
//   - "Thylacine boot OK" is the kernel-bring-up-success signal per
//     TOOLING.md §10. /init's success is part of bring-up's success
//     at v1.0, so it precedes the banner.
//   - If /init fails (rfork OOM / exec_setup error / non-zero exit
//     status), joey_run extincts — surfaces as the EXTINCTION:
//     prefix, which tools/test.sh recognizes as failure.
//
// At Phase 5+ when /init becomes long-running (real userspace
// shell + drivers + servers), this changes: /init becomes the
// post-boot transition target and the kernel hands control to it
// rather than blocking on its exit.

#ifndef THYLACINE_INIT_H
#define THYLACINE_INIT_H

#include <thylacine/types.h>   // bool (boot_mark_complete return)

// Build the embedded /init ELF, rfork a child Proc, exec_setup the blob,
// userland_enter into EL0, wait_pid for completion, verify exit_status==0.
// Extincts on any failure (so tools/test.sh observes EXTINCTION:).
void joey_run(void);

// A-5a: print the "Thylacine boot OK" banner (TOOLING.md section 10 ABI)
// exactly once. Called from the SYS_BOOT_COMPLETE handler when init (joey)
// signals its boot-test asserts passed -- joey now persists as the session
// supervisor, so the banner no longer rides joey's reap. ONE-SHOT (a 2nd call
// is a no-op). Returns true iff this call printed the banner. Implemented in
// kernel/main.c (where the banner string lives).
bool boot_mark_complete(void);

#endif // THYLACINE_INIT_H
