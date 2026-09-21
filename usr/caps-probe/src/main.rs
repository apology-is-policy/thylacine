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
    if let Some(stage) = libthyla_rs::env::args().nth(1) {
        if stage.starts_with(b"--seat-") {
            return core::str::from_utf8(stage).map(seat_probe).unwrap_or(2);
        }
    }
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

// Witness authority from an ordinary Halcyon shell descendant. This probe has
// no special role: a successful test never grants it trusted seat access.
fn seat_probe(stage: &str) -> i64 {
    use libthyla_rs::{fs, err::Error};
    use lictor::endpoint as seat;
    let elevated = match stage {
        "--seat-baseline" | "--seat-restored" | "--seat-denied" | "--seat-cancelled" => false,
        "--seat-elevated" => true,
        _ => return 2,
    };
    let group = unsafe { libthyla_rs::t_getpgid(0) };
    if group <= 0 || unsafe { libthyla_rs::t_tty_get_fg(1) } != group {
        t_putstr("seat-probe: FAIL caller lost foreground PTY session\n");
        return 1;
    }
    for op in [seat::STATUS, seat::INPUT, seat::ACK, seat::FRAME, seat::KEY,
               seat::RESTORED, seat::FAIL, seat::QUERY, seat::VISIBLE,
               seat::MASK, seat::GRANT, seat::CLIENT] {
        if seat::call(op, &mut seat::Message::default()).is_ok() {
            t_putstr("seat-probe: FAIL ordinary process reached trusted endpoint\n");
            return 1;
        }
    }
    let pci = unsafe { libthyla_rs::t_pci_claim(16) };
    if pci >= 0 {
        unsafe { libthyla_rs::t_close(pci); }
        t_putstr("seat-probe: FAIL ordinary process claimed GPU\n");
        return 1;
    }
    // Separate paths keep a stale successful create from masquerading as a
    // permission denial. /home is SYSTEM-owned and not writable by michael.
    let path = alloc::format!("/home/graphical-sak-{}", &stage[7..]);
    match fs::create_dir(&path) {
        Ok(()) if elevated => {
            if fs::remove_dir(&path).is_err() {
                t_putstr("seat-probe: FAIL cleanup\n"); return 1;
            }
        }
        Err(Error::PermissionDenied) if !elevated => {},
        _ => { t_putstr("seat-probe: FAIL DAC authority mismatch\n"); return 1; }
    }
    t_putstr("seat-probe: PASS "); t_putstr(&stage[7..]);
    t_putstr(" (endpoint denied; GPU denied; DAC verified)\n");
    0
}
