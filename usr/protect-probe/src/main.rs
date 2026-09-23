// /protect-probe -- B-1a: the permission ceiling, driven from EL0 through the
// native syscalls (ARCH 6.5 "The permission ceiling"; SYS_BURROW_RESERVE /
// SYS_BURROW_PROTECT). Spawned by joey at boot; prints one "protect-probe:
// <leg> OK" line per leg and exits 0, or "protect-probe: FAIL <leg>" and exits 1.
//
// What the kernel unit suite cannot see is the EL0 side of the contract: that a
// raise really installs on the next touch, that bytes survive a lowering as
// seen through the user mapping, that the errnos come back as -errno in x0. The
// one thing THIS binary cannot show is a refused access -- that is a snare
// death -- which is /protect-guard-child's job.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::ptr::{read_volatile, write_volatile};
use libthyla_rs::{
    t_burrow_detach, t_burrow_protect, t_burrow_reserve, t_putstr, T_BURROW_PROTECT_SEAL,
    T_BURROW_PROT_NONE, T_BURROW_PROT_READ, T_BURROW_PROT_WRITE,
};

const PAGE: u64 = 4096;
const NONE: u64 = T_BURROW_PROT_NONE;
const R: u64 = T_BURROW_PROT_READ;
const RW: u64 = T_BURROW_PROT_READ | T_BURROW_PROT_WRITE;
// BURROW_PROT_EXEC. Deliberately absent from libthyla-rs: it is never a target,
// and this probe spells it only to watch it be refused.
const X: u64 = 4;
const E_NOMEM: i64 = -12;
const E_ACCES: i64 = -13;
const E_INVAL: i64 = -22;
const PATTERN: u64 = 0x1111_2222_3333_4444;

fn fail(leg: &str) -> i64 {
    t_putstr("protect-probe: FAIL ");
    t_putstr(leg);
    t_putstr("\n");
    1
}

fn ok(leg: &str) {
    t_putstr("protect-probe: ");
    t_putstr(leg);
    t_putstr(" OK\n");
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    unsafe {
        // 1. The reserve-then-commit idiom: none, raise, write, read back.
        let va = t_burrow_reserve(4 * PAGE, NONE, 0);
        if va <= 0 {
            return fail("reserve-none");
        }
        let va = va as u64;
        if t_burrow_protect(va, 4 * PAGE, RW, 0) != 0 {
            return fail("raise-rw");
        }
        write_volatile(va as *mut u64, PATTERN);
        if read_volatile(va as *const u64) != PATTERN {
            return fail("write-read");
        }
        ok("reserve-raise-write");

        // 2. RELRO: lower to read-only; the bytes are still there, readable.
        if t_burrow_protect(va, PAGE, R, 0) != 0 {
            return fail("lower-r");
        }
        if read_volatile(va as *const u64) != PATTERN {
            return fail("read-after-lower");
        }
        ok("relro-read-only-keeps-bytes");

        // 3. Grow / shrink / grow: a range at none keeps its contents.
        if t_burrow_protect(va, 4 * PAGE, NONE, 0) != 0 {
            return fail("shrink-none");
        }
        if t_burrow_protect(va, 4 * PAGE, RW, 0) != 0 {
            return fail("regrow-rw");
        }
        if read_volatile(va as *const u64) != PATTERN {
            return fail("content-across-none");
        }
        ok("grow-shrink-grow-keeps-contents");

        // 4. Seal page 2 at R: RW is gone for good, descent stays open.
        if t_burrow_protect(va + 2 * PAGE, PAGE, R, T_BURROW_PROTECT_SEAL) != 0 {
            return fail("seal");
        }
        if t_burrow_protect(va + 2 * PAGE, PAGE, RW, 0) != E_ACCES {
            return fail("seal-holds");
        }
        if t_burrow_protect(va + 2 * PAGE, PAGE, NONE, 0) != 0 {
            return fail("seal-descends");
        }
        if t_burrow_protect(va + 2 * PAGE, PAGE, R, 0) != 0 {
            return fail("seal-returns-to-r");
        }
        ok("seal");

        // 5. X is never a target, and the refusal precedes the lookup: the
        //    same unmapped range answers EACCES for X and ENOMEM for R.
        let unmapped: u64 = 0x3800_0000;
        if t_burrow_protect(unmapped, PAGE, R | X, 0) != E_ACCES {
            return fail("x-refused");
        }
        if t_burrow_protect(unmapped, PAGE, R, 0) != E_NOMEM {
            return fail("hole-enomem");
        }
        if t_burrow_reserve(PAGE, R | X, 0) != E_ACCES {
            return fail("reserve-x");
        }
        if t_burrow_protect(va, PAGE, R | X, 0) != E_ACCES {
            return fail("x-on-a-live-mapping");
        }
        ok("x-never-a-target");

        // 6. Malformed words, each its own EINVAL, none of them changing anything.
        if t_burrow_protect(va + 1, PAGE, R, 0) != E_INVAL {
            return fail("unaligned");
        }
        if t_burrow_protect(va, PAGE, T_BURROW_PROT_WRITE, 0) != E_INVAL {
            return fail("w-only");
        }
        if t_burrow_protect(va, PAGE, R, 2) != E_INVAL {
            return fail("flags");
        }
        if t_burrow_protect(va, 0, R, 0) != E_INVAL {
            return fail("len-0");
        }
        if read_volatile(va as *const u64) != PATTERN {
            return fail("refusals-changed-something");
        }
        ok("refusals");

        // 7. An aligned reservation: 2 MiB.
        let a = t_burrow_reserve(PAGE, RW, 21);
        if a <= 0 || (a as u64) & ((1u64 << 21) - 1) != 0 {
            return fail("align");
        }
        write_volatile(a as *mut u64, 7);
        if t_burrow_detach(a as u64, PAGE) != 0 {
            return fail("detach-aligned");
        }
        ok("aligned-reserve");

        // 8. The pieces the protects left -- [0,2) RW, [2,3) R sealed, [3,4) RW
        //    -- detach one by one (the native detach matches a mapping exactly;
        //    the range form is B-1a'), which is also the piece-detach from EL0.
        if t_burrow_detach(va, 2 * PAGE) != 0 {
            return fail("detach-piece-0");
        }
        if t_burrow_detach(va + 2 * PAGE, PAGE) != 0 {
            return fail("detach-piece-1");
        }
        if t_burrow_detach(va + 3 * PAGE, PAGE) != 0 {
            return fail("detach-piece-2");
        }
        // And the address space is really free again: a fresh page-aligned
        // reserve lands at the base the first one had.
        let again = t_burrow_reserve(PAGE, RW, 0);
        if again != va as i64 {
            return fail("range-reusable-after-piece-detach");
        }
        let _ = t_burrow_detach(again as u64, PAGE);
        ok("piece-detach");
    }
    t_putstr("protect-probe: ALL OK\n");
    0
}
