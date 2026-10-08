// /hint-probe -- the EL0 wait hints retire as hints (XT-3a). start.S writes
// SCTLR_EL1 whole on every entry path: nTWE set, so WFE runs at EL0, and nTWI
// clear, so WFI traps and exception.c's EC_WFX arm retires it. Every wait here
// must come back: a WFI that killed the Proc (the EL2-entry boots before the
// composition) fails joey's reap, and an arm that returned without advancing
// ELR traps on the same WFI forever, so the boot never reaches its banner.

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
        // SEVL sets this PE's event register, so the WFE after it consumes the
        // event and completes without waiting on an interrupt.
        for _ in 0..64 {
            core::arch::asm!("sevl", "wfe", options(nomem, nostack, preserves_flags));
        }
        say("hint-probe: 64 WFE retired\n");
    }
    say("hint-probe: exit 0\n");
    0
}
