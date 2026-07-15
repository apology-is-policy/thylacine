// /debug-child -- the target Proc for the Stage-8a debug-fs in-guest E2E
// (debug-probe drives it). It exists to be a GENUINELY-parked EL0 thread that a
// cross-Proc debugger stops, inspects, resumes, and (8a-2b) HW-breakpoints -- the
// one thing a kernel unit test cannot produce (a kthread never EL0-returns, so it
// never reaches the stop checkpoint or carries an EL0 trapframe).
//
// The contract with debug-probe (all values below MUST match its constants):
//   * x20 holds SENTINEL_REG for the whole park loop -- the debugger reads
//     /proc/<pid>/regs and asserts x20 == SENTINEL_REG (the exact register proof
//     against a real trapframe).
//   * x21 holds the address of a 3-word stack region; region[0] = SENTINEL_MEM
//     (the debugger READ-checks it via /proc/<pid>/mem), region[1] = a resume
//     flag the debugger WRITES to 1 (loop1 -> loop2), region[2] = an exit flag
//     the debugger WRITES to 1 (loop2 -> exit).
//   * x22 holds the VA of bp_landmark() -- the debugger reads it from the stopped
//     frame and arms a HW breakpoint there (8a-2b). The FIRST loop2 call traps.
//   * loop1 yields each iteration (SYS_YIELD), so the debug stop parks us at the
//     SYNC EL0-return tail with x20/x21/x22 captured verbatim in the trapframe
//     (the kernel saves+restores all GPRs across a syscall).
//
// debug-probe guarantees termination (it writes the flags + `start`, or `killgrp`
// on any failure path), so both loops are deliberately unbounded -- the parent
// owns our lifetime.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::t_exits;

// MUST match debug-probe.
const SENTINEL_REG: u64 = 0xDEB0_DEB0;
const SENTINEL_MEM: u64 = 0xDEB0_0001_CAFE_0001;

// The 8a-2b breakpoint landmark: debug-probe arms a HW breakpoint at this
// function's entry (its VA is pinned in x22 during the park loop, so the debugger
// reads it from /proc/<pid>/regs), then loop2 calls it and the bp fires (EC 0x30
// -> the whole-Proc stop; the debugger's regs.pc == this VA). #[inline(never)] +
// the volatile nop keep it a real, called-into function with a stable entry.
#[no_mangle]
#[inline(never)]
extern "C" fn bp_landmark() {
    unsafe { core::arch::asm!("nop", options(nostack, preserves_flags)); }
}

// Cooperative yield (SYS_YIELD = 87) so the probe runs at -smp 1 between our
// loop iterations. x0 is the syscall return; x8 the number.
#[inline(always)]
fn yield_now() {
    unsafe {
        core::arch::asm!("mov x8, #87", "svc #0", out("x8") _, lateout("x0") _, options(nostack));
    }
}

// NB: debug-child prints NOTHING. It runs concurrently with debug-probe on
// another CPU; any console write would interleave (byte-race) with the probe's
// output and make the E2E report unreadable. Its liveness is proven by
// debug-probe successfully stopping + inspecting it.
#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // A 3-word stack region the debugger reaches via /proc/<pid>/mem:
    //   [0] = SENTINEL_MEM  (read-checked)
    //   [1] = resume flag   (written 1 to release loop1 into loop2)
    //   [2] = exit flag     (written 1 to release loop2 into exit)
    // write_volatile pins the initial stores before the asm block reads region[1]
    // and before the debugger reads region[0].
    let mut region: [u64; 3] = [0; 3];
    unsafe {
        core::ptr::write_volatile(&mut region[0], SENTINEL_MEM);
        core::ptr::write_volatile(&mut region[1], 0u64);
        core::ptr::write_volatile(&mut region[2], 0u64);
    }
    let ptr = region.as_mut_ptr();
    let bp = (bp_landmark as extern "C" fn()) as usize as u64;

    // loop1: pin SENTINEL_REG in x20, the region pointer in x21, and the bp
    // landmark VA in x22 across a yield loop. `in("x20"/"x21"/"x22")` reserve all
    // three for the whole block, and the yield's trapframe save/restore preserves
    // them, so a debug stop at any tail freezes them at exactly these values. We
    // spin until the debugger sets region[1] != 0, then fall through to loop2.
    unsafe {
        core::arch::asm!(
            "2:",
            "mov x8, #87",        // SYS_YIELD
            "svc #0",
            "ldr x9, [x21, #8]",  // reload the resume flag (region[1])
            "cbz x9, 2b",
            in("x20") SENTINEL_REG,
            in("x21") ptr,
            in("x22") bp,
            out("x8") _,
            out("x9") _,
        );
    }

    // loop2: call the breakpointed landmark each iteration until the debugger sets
    // region[2] != 0. The FIRST call traps (the armed bp -> the whole-Proc stop);
    // after the debugger disarms the bp, sets region[2], and resumes, the landmark
    // runs clean and we exit(0).
    loop {
        bp_landmark();
        if unsafe { core::ptr::read_volatile(&region[2]) } != 0 {
            break;
        }
        yield_now();
    }

    // Keep `region` live to the asm (its address escaped into the block).
    let _ = unsafe { core::ptr::read_volatile(&region[1]) };
    unsafe { t_exits(0) }
}
