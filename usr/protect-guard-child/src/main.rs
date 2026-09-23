// /protect-guard-child -- B-1a: a range sealed at none is a guard (ARCH 6.5).
//
// Reserves two pages RW, writes page 1 to prove the reservation is live
// memory, seals page 0 at none, prints the marker, and then writes through
// page 0. The kernel MUST refuse that fault -- arch/arm64/fault.c step 2, prot
// none, before any Burrow is resolved -- and terminate this Proc via snare:segv.
// joey reaps it with pouch_smoke_one_expect_fault: the marker must be present
// AND the exit status non-zero. Surviving the write prints SURVIVED and exits
// 0, which joey reads as the failure it is: the guard did not guard.
//
// The marker goes out on fd 1 (t_write), not through t_putstr: t_putstr is
// SYS_PUTS, the console, and joey's census reads the child's stdout pipe.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::ptr::write_volatile;
use libthyla_rs::{
    t_burrow_protect, t_burrow_reserve, t_write, T_BURROW_PROTECT_SEAL, T_BURROW_PROT_NONE,
    T_BURROW_PROT_READ, T_BURROW_PROT_WRITE,
};

const PAGE: u64 = 4096;

fn say(s: &str) {
    unsafe {
        let _ = t_write(1, s.as_ptr(), s.len());
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    unsafe {
        let va = t_burrow_reserve(2 * PAGE, T_BURROW_PROT_READ | T_BURROW_PROT_WRITE, 0);
        if va <= 0 {
            say("protect-guard-child: reserve failed\n");
            return 2;
        }
        let va = va as u64;
        write_volatile((va + PAGE) as *mut u64, 1);   // page 1: live memory
        if t_burrow_protect(va, PAGE, T_BURROW_PROT_NONE, T_BURROW_PROTECT_SEAL) != 0 {
            say("protect-guard-child: seal failed\n");
            return 2;
        }
        say("protect-guard-child: touching the guard\n");
        write_volatile(va as *mut u64, 0xBAD);        // the guard: dies here
        say("protect-guard-child: SURVIVED -- the guard did not guard\n");
    }
    0
}
