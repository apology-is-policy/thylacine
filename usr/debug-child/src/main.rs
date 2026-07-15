// /debug-child -- the target Proc for the Stage-8a debug-fs in-guest E2E
// (debug-probe drives it). It exists to be a GENUINELY-parked EL0 thread that a
// cross-Proc debugger stops, inspects, and resumes -- the one thing a kernel
// unit test cannot produce (a kthread never EL0-returns, so it never reaches the
// stop checkpoint or carries an EL0 trapframe).
//
// The contract with debug-probe (all values below MUST match its constants):
//   * x20 holds SENTINEL_REG for the whole park loop -- the debugger reads
//     /proc/<pid>/regs and asserts x20 == SENTINEL_REG (the exact register proof
//     against a real trapframe).
//   * x21 holds the address of a 2-word stack region; region[0] = SENTINEL_MEM
//     (the debugger READ-checks it via /proc/<pid>/mem), region[1] = a resume
//     flag the debugger WRITES to 1 (via /proc/<pid>/mem) then `start`s to
//     release us.
//   * The loop yields each iteration (SYS_YIELD), so the debug stop parks us at
//     the SYNC EL0-return tail with x20/x21 captured verbatim in the trapframe
//     (the kernel saves+restores all GPRs across a syscall, so a stop mid-loop
//     freezes them at the pinned values).
//
// debug-probe guarantees termination (it writes the flag + `start`, or `killgrp`
// on any failure path), so the loop is deliberately unbounded -- the parent owns
// our lifetime.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::t_exits;

// MUST match debug-probe.
const SENTINEL_REG: u64 = 0xDEB0_DEB0;
const SENTINEL_MEM: u64 = 0xDEB0_0001_CAFE_0001;

// NB: debug-child prints NOTHING. It runs concurrently with debug-probe on
// another CPU; any console write would interleave (byte-race) with the probe's
// output and make the E2E report unreadable. Its liveness is proven by
// debug-probe successfully stopping + inspecting it.
#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // A 2-word stack region the debugger reaches via /proc/<pid>/mem:
    //   [0] = SENTINEL_MEM  (read-checked)   [1] = resume flag (written 1 to release)
    // write_volatile pins the initial stores before the asm block reads region[1]
    // and before the debugger reads region[0].
    let mut region: [u64; 2] = [0; 2];
    unsafe {
        core::ptr::write_volatile(&mut region[0], SENTINEL_MEM);
        core::ptr::write_volatile(&mut region[1], 0u64);
    }
    let ptr = region.as_mut_ptr();

    // Pin SENTINEL_REG in x20 and the region pointer in x21 across a yield loop.
    // `in("x20")`/`in("x21")` reserve both registers for the whole block, and the
    // yield's trapframe save/restore preserves them across every iteration, so a
    // debug stop at any tail freezes x20/x21 at exactly these values. We spin
    // until the debugger sets region[1] != 0, then fall through and exit(0).
    unsafe {
        core::arch::asm!(
            "2:",
            "mov x8, #87",        // SYS_YIELD
            "svc #0",
            "ldr x9, [x21, #8]",  // reload the resume flag (region[1])
            "cbz x9, 2b",
            in("x20") SENTINEL_REG,
            in("x21") ptr,
            out("x8") _,
            out("x9") _,
        );
    }

    // Keep `region` live to the asm (its address escaped into the block).
    let _ = unsafe { core::ptr::read_volatile(&region[1]) };
    unsafe { t_exits(0) }
}
