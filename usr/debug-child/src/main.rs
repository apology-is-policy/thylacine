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
//   * x21 holds the address of a 4-word stack region; region[0] = SENTINEL_MEM
//     (the debugger READ-checks it via /proc/<pid>/mem), region[1] = a resume
//     flag the debugger WRITES to 1 (loop1 -> loop2), region[2] = an exit flag
//     the debugger WRITES to 1 (loop2 -> the watch phase), region[3] = the WATCH
//     TARGET (8a-2b-3): the debugger arms a WRITE watchpoint on &region[3] (VA =
//     x21 + 24), then the watched store below traps (EC 0x34 -> the whole-Proc
//     stop). After the debugger removes the wp and resumes, the store completes.
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
// 8a-2b-3: the value the watched store writes into region[3]. Its exact value is
// immaterial to the E2E (the debugger verifies the STOP, not the byte); it just has
// to be a store the WRITE watchpoint traps.
const WATCH_VALUE: u64 = 0xDEB0_0002_CAFE_0002;

// The 8a-2b breakpoint landmark: debug-probe arms a HW breakpoint at this
// function's entry (its VA is pinned in x22 during the park loop, so the debugger
// reads it from /proc/<pid>/regs), then loop2 calls it and the bp fires (EC 0x30
// -> the whole-Proc stop; the debugger's regs.pc == this VA). #[inline(never)] +
// the volatile nops keep it a real, called-into function with a stable entry.
// The RUN of nops makes it single-STEPPABLE (8a-2b-2): from the bp'd entry the
// debugger steps a few times and each step advances pc by exactly one A64
// instruction (4 bytes) through the linear prologue+nops -- so it exercises both
// the SS machine AND the step-over-breakpoint dance (the first step is FROM the
// breakpointed entry, which the step-over must skip).
#[no_mangle]
#[inline(never)]
extern "C" fn bp_landmark() {
    unsafe {
        core::arch::asm!(
            "nop", "nop", "nop", "nop", "nop", "nop",
            options(nostack, preserves_flags),
        );
    }
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
    // A 4-word stack region the debugger reaches via /proc/<pid>/mem:
    //   [0] = SENTINEL_MEM  (read-checked)
    //   [1] = resume flag   (written 1 to release loop1 into loop2)
    //   [2] = exit flag     (written 1 to release loop2 into the watch phase)
    //   [3] = watch target  (8a-2b-3: the WRITE watchpoint is armed on &region[3];
    //                        the watched store below traps). Pre-initialized here
    //                        (before any wp is armed) so its page is resident.
    // write_volatile pins the initial stores before the asm block reads region[1]
    // and before the debugger reads region[0].
    let mut region: [u64; 4] = [0; 4];
    unsafe {
        core::ptr::write_volatile(&mut region[0], SENTINEL_MEM);
        core::ptr::write_volatile(&mut region[1], 0u64);
        core::ptr::write_volatile(&mut region[2], 0u64);
        core::ptr::write_volatile(&mut region[3], 0u64);
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
    // runs clean and we fall through to the watch phase.
    loop {
        bp_landmark();
        if unsafe { core::ptr::read_volatile(&region[2]) } != 0 {
            break;
        }
        yield_now();
    }

    // 8a-2b-3 watch phase: a single store to region[3]. If the debugger armed a
    // WRITE watchpoint on &region[3] (which it did, before releasing loop2 above),
    // this STR traps (EC 0x34 -> the whole-Proc stop; the debugger's regs.pc == this
    // store instruction). After the debugger removes the wp (hwrmwatch, stopped-only)
    // and resumes, the STR re-executes untrapped and we exit(0). The wp is the ONLY
    // reason this store stops -- no bp is armed here (the debugger disarmed it).
    unsafe { core::ptr::write_volatile(&mut region[3], WATCH_VALUE); }

    // Keep `region` live to the asm (its address escaped into the block).
    let _ = unsafe { core::ptr::read_volatile(&region[1]) };
    unsafe { t_exits(0) }
}
