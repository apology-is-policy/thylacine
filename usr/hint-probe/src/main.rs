// /hint-probe -- the EL0 wait hints retire as hints (XT-3a). start.S writes
// SCTLR_EL1 whole on every entry path: nTWE set, so WFE runs at EL0, and nTWI
// clear, so a WFI that would wait traps and exception.c's EC_WFX arm retires
// it. EL0 can witness only that every wait comes back. A WFI that killed the
// Proc (an EL2 boot or HVF before the composition, or the composition without
// the arm) fails joey's reap; an arm that returned without advancing ELR traps
// on the same WFI forever, so the boot never reaches its banner.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::t_write;

fn say(s: &str) {
    unsafe {
        let _ = t_write(1, s.as_ptr(), s.len());
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    unsafe {
        for _ in 0..64 {
            core::arch::asm!("wfi", options(nomem, nostack, preserves_flags));
        }
        say("hint-probe: 64 WFI retired\n");
        // SEVL sets this PE's event register, so the WFE after it normally
        // consumes the event and completes at once. An interrupt between the
        // two can consume the event first; the WFE then waits for the next
        // interrupt, which the scheduler tick bounds while this thread runs.
        for _ in 0..64 {
            core::arch::asm!("sevl", "wfe", options(nomem, nostack, preserves_flags));
        }
        say("hint-probe: 64 WFE retired\n");
    }
    say("hint-probe: exit 0\n");
    0
}
