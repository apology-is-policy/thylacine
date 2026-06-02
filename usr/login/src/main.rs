// /sbin/login (A-5a) -- the native session-login core.
//
// joey's getty-loop spawns /sbin/login with a console handle (SYS_CONSOLE_OPEN)
// as fd 0/1/2; the boot-path CI E2E spawns it with a seeded pipe as fd 0 and a
// capture pipe as fd 1/2 (the Unix login-reads-the-tty model -- login reads fd 0
// and writes fd 1 regardless of what they are wired to). login:
//
//   1. reads {user, passphrase} from fd 0,
//   2. authenticates against corvus (/srv/corvus AUTH, verb 1) -> a session token,
//   3. resolves the user's identity (RESOLVE_NAME verb 12 -> principal_id +
//      primary_gid; RESOLVE_ID verb 11 -> supp_gids),
//   4. spawns `ut` STAMPED with that identity (login holds CAP_SET_IDENTITY) and
//      WITHOUT it (the shell is a user, not an identity-stamper) -- so the shell
//      is born AS the user, not PRINCIPAL_SYSTEM,
//   5. waits the shell; on its exit (logout) closes the corvus session
//      (SESSION_CLOSE verb 3, which zeroes the keypair) and exits.
//
// login itself runs as PRINCIPAL_SYSTEM (inherited from joey) and is NEVER
// console-attached (joey relinquished its attach at the bringup->session
// boundary, and spawn does not confer the bit) -- so I-27 holds: during a session
// corvus is the sole console-attached Proc. IDENTITY-DESIGN.md section 9.9.
//
// On any failure login exits non-zero; the getty respawns it with a fresh prompt.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::vec::Vec;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{
    t_close, t_putstr, t_read, t_srv_connect, t_write, T_CAP_CSPRNG_READ, T_CAP_LOCK_PAGES,
};

const VERB_AUTH: u8 = 1;
const VERB_SESSION_CLOSE: u8 = 3;
const VERB_RESOLVE_ID: u8 = 11;
const VERB_RESOLVE_NAME: u8 = 12;
const CORVUS_PROTOCOL_VERSION: u8 = 1;
const STATUS_OK: u8 = 0;
const TOKEN_LEN: usize = 33;

const FD_IN: i64 = 0; // the tty (or seeded creds pipe)
const FD_OUT: i64 = 1; // the tty (or capture pipe)
const MAX_LINE: usize = 256;
const SHELL: &str = "ut";

// The user shell runs as the authenticated user with NO elevation/identity/grant
// caps -- a shell is not an identity-stamper. It keeps the two benign
// fork-grantable user caps (mlock + getrandom). login must HOLD these to pass
// them down, so joey grants login CAP_SET_IDENTITY | LOCK_PAGES | CSPRNG_READ.
const SHELL_CAPS: u64 = T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ;

fn write_out(buf: &[u8]) {
    let mut off = 0usize;
    while off < buf.len() {
        let w = unsafe { t_write(FD_OUT, buf.as_ptr().add(off), buf.len() - off) };
        if w <= 0 {
            return; // fd-1 write failures are non-fatal (no console)
        }
        off += w as usize;
    }
}

// Decimal-format a u32 into `out` (no_std, no alloc churn beyond the Vec push).
fn put_u32(out: &mut Vec<u8>, mut v: u32) {
    if v == 0 {
        out.push(b'0');
        return;
    }
    let mut tmp = [0u8; 10];
    let mut n = 0;
    while v > 0 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    while n > 0 {
        n -= 1;
        out.push(tmp[n]);
    }
}

// Read one line from `fd` (byte-at-a-time so a single underlying read cannot
// straddle two lines -- correct for both /dev/cons and a pipe). Strips CR; stops
// at LF or EOF. Returns false only on EOF/error with nothing read.
fn read_line(fd: i64, out: &mut Vec<u8>) -> bool {
    out.clear();
    loop {
        let mut b = [0u8; 1];
        let r = unsafe { t_read(fd, b.as_mut_ptr(), 1) };
        if r <= 0 {
            return !out.is_empty();
        }
        match b[0] {
            b'\n' => return true,
            b'\r' => {}
            c => {
                if out.len() < MAX_LINE {
                    out.push(c);
                }
            }
        }
    }
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

// One corvus verb exchange (the legate-prover / joey codec): write
// [verb, version, len_lo, len_hi, payload]; read [status, len_lo, len_hi,
// payload]. Returns (status, payload) or None on a transport error.
unsafe fn exchange(fd: i64, verb: u8, payload: &[u8]) -> Option<(u8, Vec<u8>)> {
    let hdr = [
        verb,
        CORVUS_PROTOCOL_VERSION,
        (payload.len() & 0xff) as u8,
        ((payload.len() >> 8) & 0xff) as u8,
    ];
    if !write_all(fd, &hdr) {
        return None;
    }
    if !payload.is_empty() && !write_all(fd, payload) {
        return None;
    }
    let mut rh = [0u8; 3];
    if !read_exact(fd, &mut rh) {
        return None;
    }
    let rlen = (rh[1] as usize) | ((rh[2] as usize) << 8);
    let mut resp = Vec::new();
    resp.resize(rlen, 0);
    if rlen > 0 && !read_exact(fd, &mut resp) {
        return None;
    }
    Some((rh[0], resp))
}

fn rd_u32_le(b: &[u8], off: usize) -> u32 {
    (b[off] as u32) | ((b[off + 1] as u32) << 8) | ((b[off + 2] as u32) << 16) | ((b[off + 3] as u32) << 24)
}

// AUTH (verb 1): user_len(1) + user + pass_len(2 LE) + pass -> 33-byte token.
unsafe fn auth(fd: i64, user: &[u8], pass: &[u8]) -> Option<[u8; TOKEN_LEN]> {
    let mut pl: Vec<u8> = Vec::new();
    pl.push(user.len() as u8);
    pl.extend_from_slice(user);
    pl.push((pass.len() & 0xff) as u8);
    pl.push(((pass.len() >> 8) & 0xff) as u8);
    pl.extend_from_slice(pass);
    let (st, resp) = exchange(fd, VERB_AUTH, &pl)?;
    if st != STATUS_OK || resp.len() != TOKEN_LEN {
        return None;
    }
    let mut token = [0u8; TOKEN_LEN];
    token.copy_from_slice(&resp);
    Some(token)
}

// RESOLVE_NAME (verb 12): name_len(1) + name -> {principal_id u32, primary_gid u32}.
unsafe fn resolve_name(fd: i64, user: &[u8]) -> Option<(u32, u32)> {
    let mut pl: Vec<u8> = Vec::new();
    pl.push(user.len() as u8);
    pl.extend_from_slice(user);
    let (st, resp) = exchange(fd, VERB_RESOLVE_NAME, &pl)?;
    if st != STATUS_OK || resp.len() != 8 {
        return None;
    }
    Some((rd_u32_le(&resp, 0), rd_u32_le(&resp, 4)))
}

// RESOLVE_ID (verb 11): principal_id u32 -> {primary_gid u32, supp_count u8,
// supp[count] u32, name_len u8, name}. We extract supp_gids; missing/short is a
// non-fatal empty set (the user simply has no supplementary groups).
unsafe fn resolve_supp_gids(fd: i64, principal_id: u32) -> Vec<u32> {
    let mut out: Vec<u32> = Vec::new();
    let pl = principal_id.to_le_bytes();
    let (st, resp) = match exchange(fd, VERB_RESOLVE_ID, &pl) {
        Some(x) => x,
        None => return out,
    };
    if st != STATUS_OK || resp.len() < 5 {
        return out;
    }
    let count = resp[4] as usize;
    let mut off = 5usize;
    for _ in 0..count {
        if off + 4 > resp.len() {
            break;
        }
        out.push(rd_u32_le(&resp, off));
        off += 4;
    }
    out
}

unsafe fn session_close(fd: i64, token: &[u8; TOKEN_LEN]) {
    let _ = exchange(fd, VERB_SESSION_CLOSE, token);
}

// Bounded retry on /srv/corvus (corvus is up before the getty-loop; this only
// covers the accept-queue race, like legate-prover).
fn connect_corvus() -> i64 {
    for _ in 0..64 {
        let c = unsafe { t_srv_connect(b"corvus".as_ptr(), 6, b"ctl".as_ptr(), 3) };
        if c >= 0 {
            return c;
        }
    }
    -1
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut user: Vec<u8> = Vec::new();
    let mut pass: Vec<u8> = Vec::new();

    write_out(b"\nThylacine login: ");
    if !read_line(FD_IN, &mut user) || user.is_empty() {
        write_out(b"login: no username\n");
        return 1;
    }
    write_out(b"password: ");
    if !read_line(FD_IN, &mut pass) {
        write_out(b"login: no passphrase\n");
        return 1;
    }

    let conn = connect_corvus();
    if conn < 0 {
        write_out(b"login: corvus unavailable\n");
        return 1;
    }

    let token = match unsafe { auth(conn, &user, &pass) } {
        Some(t) => t,
        None => {
            write_out(b"login incorrect\n");
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };

    let (pid, gid) = match unsafe { resolve_name(conn, &user) } {
        Some(x) => x,
        None => {
            write_out(b"login: identity resolve failed\n");
            unsafe { session_close(conn, &token) };
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };
    let supp = unsafe { resolve_supp_gids(conn, pid) };

    // Resolution marker -- emitted via t_putstr (the UART) so it lands in the
    // boot log for the CI E2E to grep AND on the live console (the UART IS the
    // console at v1.0 single-console). Deterministic: "login: <user> uid=N gid=M".
    // (Prompts/errors use write_out(fd 1) -- the tty in the live path; in the
    // seeded E2E fd 1 is the creds read-end so they no-op, which is fine: the
    // CI gate is login's exit code + this marker in the boot log.)
    {
        let mut line: Vec<u8> = Vec::new();
        line.extend_from_slice(b"login: ");
        line.extend_from_slice(&user);
        line.extend_from_slice(b" uid=");
        put_u32(&mut line, pid);
        line.extend_from_slice(b" gid=");
        put_u32(&mut line, gid);
        line.push(b'\n');
        if let Ok(s) = core::str::from_utf8(&line) {
            t_putstr(s);
        }
    }

    // Spawn the shell AS the user. fd 0/1/2 inherit login's (the tty), so the
    // shell reads + writes the same console. The shell gets the user's identity
    // (SPAWN_IDENTITY_SET) but NOT CAP_SET_IDENTITY.
    let mut child = match Command::new(SHELL)
        .identity(pid, gid, &supp)
        .caps(SHELL_CAPS)
        .stdin(Stdio::Inherit)
        .stdout(Stdio::Inherit)
        .stderr(Stdio::Inherit)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            write_out(b"login: shell spawn failed\n");
            unsafe { session_close(conn, &token) };
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };

    // Session leader: wait the shell. Its exit IS logout (regardless of status).
    let _ = child.wait();

    // Logout: close the corvus session (zeroes the keypair) + drop the conn.
    unsafe { session_close(conn, &token) };
    let _ = unsafe { t_close(conn) };
    0
}
