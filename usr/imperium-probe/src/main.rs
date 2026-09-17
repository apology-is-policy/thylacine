// /bin/imperium-probe (IM-3) -- the boot prover for the lex curiata
// (IMPERIUM-DESIGN.md 11.5 + 11.8): request -> the harness presses the SAK ->
// corvus confers on the trusted path -> the reply -> the redeem.
//
// Run from a LOGIN session (its kernel-stamped principal must be a corvus user:
// the boot chain's PRINCIPAL_SYSTEM is refused, which joey's own ladder proves):
//
//   1. connect /srv/corvus/ctl and send IMPERIUM_REQUEST (verb 19) for the
//      `imperium` level restricted to the caps named on argv (`chown` / `dac` /
//      `kill`; none = the whole level).
//   2. print `requested ... -- press the SAK` and block on the DEFERRED reply:
//      the read parks inside corvus until the episode concludes (or the 60-s
//      window expires -> TIMEOUT, or the slot is taken -> BUSY at once).
//   3. on OK redeem the grant (cap::use_grant -> a PROPAGATING legate root)
//      and print /proc/<pid>/imperium, the kernel's unforgeable account of the
//      scope: `propagating 1`, the rods, the axe.
//
// Thin and UNTRUSTED by construction (section 3.1): it renders no
// authorization surface -- the provincia and the key prompt are corvus's, on
// the console the kernel froze for everyone else. IM-4's usr/imperium is the
// real tool; this proves the corvus half and gives the harness its tokens.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::vec::Vec;
use libthyla_rs::cap::{self, Caps};
use libthyla_rs::{
    env, t_close, t_getpid, t_open, t_puts, t_putstr, t_read, t_write, T_CAP_CHOWN,
    T_CAP_DAC_OVERRIDE, T_CAP_KILL, T_OREAD, T_ORDWR, T_WALK_OPEN_FROM_ROOT,
};

const VERB_IMPERIUM_REQUEST: u8 = 19;
const CORVUS_PROTOCOL_VERSION: u8 = 1;
const LEVEL_IMPERIUM: &[u8] = b"imperium";

const STATUS_OK: u8 = 0;
const STATUS_BAD_AUTH: u8 = 1;
const STATUS_PERMISSION_DENIED: u8 = 2;
const STATUS_NOT_FOUND: u8 = 3;
const STATUS_RATE_LIMITED: u8 = 4;
const STATUS_TIMEOUT: u8 = 7;
const STATUS_BUSY: u8 = 8;

fn puts(b: &[u8]) {
    // SAFETY: a borrowed slice is readable for its length.
    unsafe {
        let _ = t_puts(b.as_ptr(), b.len());
    }
}

fn put_hex64(v: u64) {
    let mut buf = [0u8; 16];
    for (i, b) in buf.iter_mut().enumerate() {
        let n = ((v >> (60 - 4 * i)) & 0xf) as u8;
        *b = if n < 10 { b'0' + n } else { b'a' + (n - 10) };
    }
    puts(&buf);
}

fn put_dec(mut n: u64) {
    let mut tmp = [0u8; 21];
    let mut i = 0;
    if n == 0 {
        puts(b"0");
        return;
    }
    while n > 0 && i < tmp.len() {
        tmp[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }
    let mut out = [0u8; 21];
    for j in 0..i {
        out[j] = tmp[i - 1 - j];
    }
    puts(&out[..i]);
}

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { libthyla_rs::t_exits(1) }
}

unsafe fn write_all(fd: i64, buf: &[u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let w = t_write(fd, buf.as_ptr().add(off), buf.len() - off);
        if w <= 0 {
            return false;
        }
        off += w as usize;
    }
    true
}

unsafe fn read_exact(fd: i64, buf: &mut [u8]) -> bool {
    let mut off = 0usize;
    while off < buf.len() {
        let r = t_read(fd, buf.as_mut_ptr().add(off), buf.len() - off);
        if r <= 0 {
            return false;
        }
        off += r as usize;
    }
    true
}

// The request half of a corvus verb exchange.
unsafe fn send_request(fd: i64, verb: u8, payload: &[u8]) -> bool {
    let hdr = [
        verb,
        CORVUS_PROTOCOL_VERSION,
        (payload.len() & 0xff) as u8,
        ((payload.len() >> 8) & 0xff) as u8,
    ];
    if !write_all(fd, &hdr) {
        return false;
    }
    if !payload.is_empty() && !write_all(fd, payload) {
        return false;
    }
    true
}

// The reply half: this read is where a DEFERRED reply parks -- corvus
// withholds the Rread until the episode concludes.
unsafe fn read_reply(fd: i64) -> Option<(u8, Vec<u8>)> {
    let mut rh = [0u8; 3];
    if !read_exact(fd, &mut rh) {
        return None;
    }
    let rlen = (rh[1] as usize) | ((rh[2] as usize) << 8);
    let mut resp = alloc::vec![0u8; rlen];
    if rlen > 0 && !read_exact(fd, &mut resp) {
        return None;
    }
    Some((rh[0], resp))
}

// The kernel's account of the scope this Proc is in (0400, owner-readable).
fn print_proc_imperium() {
    let pid = unsafe { t_getpid() };
    if pid <= 0 {
        t_putstr("imperium-probe: getpid FAILED\n");
        return;
    }
    let mut path: Vec<u8> = Vec::new();
    path.extend_from_slice(b"/proc/");
    let mut tmp = [0u8; 21];
    let mut n = pid as u64;
    let mut i = 0;
    while n > 0 && i < tmp.len() {
        tmp[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }
    for j in 0..i {
        path.push(tmp[i - 1 - j]);
    }
    path.extend_from_slice(b"/imperium");
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        t_putstr("imperium-probe: open ");
        puts(&path);
        t_putstr(" FAILED\n");
        return;
    }
    let mut buf = [0u8; 256];
    let r = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    let _ = unsafe { t_close(fd) };
    t_putstr("imperium-probe: ");
    puts(&path);
    t_putstr(": ");
    if r > 0 {
        let mut end = r as usize;
        while end > 0 && (buf[end - 1] == b'\n' || buf[end - 1] == b'\r') {
            end -= 1;
        }
        puts(&buf[..end]);
    } else {
        t_putstr("(read failed)");
    }
    t_putstr("\n");
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // argv: cap names restrict the level (the STS-style self_restrict subset);
    // `level=<name>` picks a level other than imperium (a deny-path lever).
    let mut self_restrict: u64 = 0;
    let mut level: &[u8] = LEVEL_IMPERIUM;
    for op in env::args().operands() {
        match op {
            b"chown" => self_restrict |= T_CAP_CHOWN,
            b"dac" => self_restrict |= T_CAP_DAC_OVERRIDE,
            b"kill" => self_restrict |= T_CAP_KILL,
            _ if op.starts_with(b"level=") => level = &op[6..],
            _ => {
                t_putstr("imperium-probe: usage: imperium-probe [chown] [dac] [kill] [level=NAME]\n");
                return 2;
            }
        }
    }

    let mut conn: i64 = -1;
    for _ in 0..64 {
        let root =
            unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/corvus".as_ptr(), 11, T_OREAD) };
        if root >= 0 {
            let ctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_ORDWR) };
            let _ = unsafe { t_close(root) };
            if ctl >= 0 {
                conn = ctl;
                break;
            }
        }
    }
    if conn < 0 {
        fail("imperium-probe: FAIL connect /srv/corvus\n");
    }

    // IMPERIUM_REQUEST: level_len u8 + level + self_restrict u64 LE +
    // valid_until_req u64 LE (0 = the level's own bound).
    let mut pl: Vec<u8> = Vec::new();
    pl.push(level.len() as u8);
    pl.extend_from_slice(level);
    pl.extend_from_slice(&self_restrict.to_le_bytes());
    pl.extend_from_slice(&0u64.to_le_bytes());

    // The request bytes go out BEFORE the operator is told to press the SAK:
    // the harness keys on this line, and a BREAK that reached corvus ahead of
    // the request would find nothing pending.
    if !unsafe { send_request(conn, VERB_IMPERIUM_REQUEST, &pl) } {
        fail("imperium-probe: FAIL transport (request write)\n");
    }
    t_putstr("imperium-probe: requested ");
    puts(level);
    t_putstr(" caps 0x");
    put_hex64(self_restrict);
    t_putstr(" -- press the SAK (Ctrl-A b) to confer\n");

    let (st, resp) = match unsafe { read_reply(conn) } {
        Some(x) => x,
        None => fail("imperium-probe: FAIL transport (the parked read died)\n"),
    };
    match st {
        STATUS_OK => {}
        STATUS_BAD_AUTH => fail("imperium-probe: DENIED\n"),
        STATUS_PERMISSION_DENIED => fail("imperium-probe: PERMISSION-DENIED\n"),
        STATUS_NOT_FOUND => fail("imperium-probe: NOT-FOUND (no such level)\n"),
        STATUS_RATE_LIMITED => fail("imperium-probe: RATE-LIMITED\n"),
        STATUS_TIMEOUT => fail("imperium-probe: TIMEOUT\n"),
        STATUS_BUSY => fail("imperium-probe: BUSY\n"),
        other => {
            t_putstr("imperium-probe: status=");
            put_dec(other as u64);
            t_putstr("\n");
            unsafe { libthyla_rs::t_exits(1) }
        }
    }
    if resp.len() != 12 {
        fail("imperium-probe: FAIL OK reply is not 12 bytes\n");
    }
    let session = u32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]]);
    let granted = u64::from_le_bytes([
        resp[4], resp[5], resp[6], resp[7], resp[8], resp[9], resp[10], resp[11],
    ]);
    t_putstr("imperium-probe: OK session=");
    put_dec(session as u64);
    t_putstr(" caps=0x");
    put_hex64(granted);
    t_putstr("\n");
    if session == 0 || granted == 0 {
        fail("imperium-probe: FAIL zero session or caps in the OK reply\n");
    }
    if self_restrict != 0 && granted & !self_restrict != 0 {
        fail("imperium-probe: FAIL granted caps wider than the self-restriction\n");
    }

    // Redeem for our own stripes: a PROPAGATING legate root (the kernel refuses
    // it into an existing scope -- IM-2's "propagating never nests").
    if cap::use_grant(Caps::from_bits(granted)).is_err() {
        fail("imperium-probe: FAIL cap use_grant (redeem)\n");
    }
    t_putstr("imperium-probe: redeemed -- a propagating legate root\n");
    print_proc_imperium();
    t_putstr("imperium-probe: PASS\n");
    let _ = unsafe { t_close(conn) };
    0
}
