// /bin/caps-probe -- does this process hold CAP_SET_IDENTITY?
//
// There is no leaf that prints a Proc's caps, so the probe asks the one gate
// that reads the bit: SYS_SPAWN with an identity request is refused (-1)
// before anything else when the caller lacks CAP_SET_IDENTITY. Two arms, one
// variable apart: a PLAIN spawn of the same binary must succeed (the spawn
// path works; the binary exists), and the SAME spawn with an identity set
// must be refused. A refusal with the control green is the cap gate, not a
// broken spawn. Run from a session tile or the console shell: both must
// print REFUSED; login (which holds the cap) would print ACCEPTED.
//
// Output rides SYS_PUTS (the kernel console), so it reaches the serial log
// from a tile whose fd 1 is a pts.

#![no_std]
#![no_main]

extern crate alloc;

use libthyla_rs::process::Command;
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

const TARGET: &str = "/bin/true";
/// Any real, non-SYSTEM principal id: the gate is on the caller's cap, not
/// on the target identity, so the value only has to pass the id validity
/// check.
const OTHER_PRINCIPAL: u32 = 1;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut rc = 0;
    match Command::new(TARGET).spawn() {
        Ok(mut c) => {
            let _ = c.wait();
            t_putstr("caps-probe: plain spawn OK\n");
        }
        Err(_) => {
            t_putstr("caps-probe: plain spawn FAILED -- control broken\n");
            rc = 2;
        }
    }
    match Command::new(TARGET)
        .identity(OTHER_PRINCIPAL, OTHER_PRINCIPAL, &[])
        .spawn()
    {
        Ok(mut c) => {
            let _ = c.wait();
            t_putstr("caps-probe: identity spawn ACCEPTED -- CAP_SET_IDENTITY leaked\n");
            rc = 1;
        }
        Err(_) => {
            t_putstr("caps-probe: identity spawn REFUSED\n");
        }
    }
    rc
}
