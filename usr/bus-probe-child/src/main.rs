// /bus-probe-child -- B-1a' audit F17: an abort the pager cannot resolve is a
// snare:bus death, never a livelock.
//
// Reserves a page RW, writes it (the leaf is in), prints the marker, and then
// does a 4-byte load-exclusive from offset 14 of it: misaligned, AND crossing
// a 16-byte boundary, so it is an Alignment fault under every rule -- the
// ARMv8.0 one (exclusives and ordered accesses take their natural alignment,
// regardless of SCTLR.A) and FEAT_LSE2's (Apple Silicon under HVF, QEMU's
// `max`), which relaxes ORDERED accesses to "must not cross 16 bytes" while
// SCTLR_EL1.nAA = 0 and leaves exclusives strict. (The first draft of this
// probe did `ldar` from va + 1 and SURVIVED on the Apple core: inside one
// 16-byte quantity, LSE2 permits it.) The CPU raises a Data Abort with FSC
// 0x21 on a page that is MAPPED and ADMITS the access. No page install
// resolves that; the kernel MUST terminate this Proc via snare:bus. Answered
// HANDLED instead (the pre-F17 pager), the ERET re-executes the same load into
// the same abort forever: joey never reaps this child and the boot hangs. joey
// reaps it with pouch_smoke_one_expect_fault: the marker must be present AND
// the exit status non-zero. Surviving the load prints SURVIVED and exits 0,
// which joey reads as the failure it is.
//
// The marker goes out on fd 1 (t_write), not through t_putstr: t_putstr is
// SYS_PUTS, the console, and joey's census reads the child's stdout pipe.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::ptr::write_volatile;
use libthyla_rs::{t_burrow_reserve, t_write, T_BURROW_PROT_READ, T_BURROW_PROT_WRITE};

const PAGE: u64 = 4096;

fn say(s: &str) {
    unsafe {
        let _ = t_write(1, s.as_ptr(), s.len());
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    unsafe {
        let va = t_burrow_reserve(PAGE, T_BURROW_PROT_READ | T_BURROW_PROT_WRITE, 0);
        if va <= 0 {
            say("bus-probe-child: reserve failed\n");
            return 2;
        }
        let va = va as u64;
        write_volatile(va as *mut u64, 1);   // the leaf is in: the page admits the access
        say("bus-probe-child: misaligned load-exclusive across a 16-byte boundary\n");
        let v: u32;
        core::arch::asm!("ldxr {v:w}, [{a}]", "clrex", v = out(reg) v, a = in(reg) va + 14,
                         options(nostack, preserves_flags));   // FSC 0x21: dies here
        if v == 0xFFFF_FFFF {
            say("bus-probe-child: impossible\n");
        }
        say("bus-probe-child: SURVIVED -- the abort was answered as handled\n");
    }
    0
}
