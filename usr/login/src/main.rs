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
//   4. drives the boot coordinator's /ctl (A-5b) over a persistent attach held
//      for the session: provision-dek (idempotent ensure-home) -> name->id
//      bridge -> install-dek (UNWRAP the per-user home DEK into the live map;
//      the lease is conn-bound). login forwards only the opaque corvus token --
//      it never holds the raw DEK; the coordinator UNWRAPs/WRAPs via corvus,
//   5. spawns `ut` STAMPED with that identity (login holds CAP_SET_IDENTITY) and
//      WITHOUT it (the shell is a user, not an identity-stamper) -- so the shell
//      is born AS the user, not PRINCIPAL_SYSTEM,
//   6. waits the shell; on its exit (logout) evicts the session DEK (evict-dek),
//      drops the /ctl attach, closes the corvus session (SESSION_CLOSE verb 3,
//      which zeroes the keypair) and exits.
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
    t_attach_9p_srv, t_close, t_open, t_putstr, t_read, t_readdir, t_write, T_CAP_CSPRNG_READ,
    T_CAP_LOCK_PAGES, T_OREAD, T_ORDWR, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
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
        // stalk-3b-β two-step (D5): open /srv/corvus (the connect; 9p-mode -> a
        // dev9p root), then open "ctl" relative to it. The walked ctl fid holds
        // the attach session, so the root fd is closed at once.
        let root =
            unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/corvus".as_ptr(), 11, T_OREAD) };
        if root >= 0 {
            let ctl = unsafe { t_open(root, b"ctl".as_ptr(), 3, T_ORDWR) };
            let _ = unsafe { t_close(root) };
            if ctl >= 0 {
                return ctl;
            }
        }
    }
    -1
}

// ── A-5b per-user encrypted-home DEK lifecycle (IDENTITY-DESIGN section 9.9) ──
//
// login drives the boot coordinator's /ctl (a byte-mode service the coordinator
// posts at /srv/stratum-ctl) over a persistent 9P attach held for the whole
// session: provision-dek (idempotent ensure-home) -> name->id bridge ->
// install-dek (UNWRAP into the live DEK map; the lease is CONN-BOUND, so the
// attach must stay open until logout) -> [session] -> evict-dek. login NEVER
// holds the raw DEK -- it forwards only the opaque corvus session token; the
// coordinator UNWRAPs/WRAPs via corvus. Reaching the coordinator's /ctl is the
// first end-to-end exercise of the corvus reach wired in #827a-1.
//
// The coordinator reports the SYSTEM-gated DEK nodes (provision/install/evict-
// dek, mode 0200) as owned by PRINCIPAL_SYSTEM, so the kernel's owner-first rwx
// on the /ctl attach (A-3 dev9p enforcement) permits login -- which runs as
// PRINCIPAL_SYSTEM -- to write them (Stratum-side getattr reconciliation).

const CTL_SRV_PATH: &[u8] = b"/srv/stratum-ctl";

// One Twrite of the whole buffer. The /ctl verbs require EXACT-length single
// delivery (a partial Twrite is a partial payload -> EINVAL), so a short write
// is a hard failure, not a resumable one.
unsafe fn write_one(fd: i64, buf: &[u8]) -> bool {
    let w = t_write(fd, buf.as_ptr(), buf.len());
    w >= 0 && (w as usize) == buf.len()
}

// Decimal-format a u64 into `out`.
fn put_u64(out: &mut Vec<u8>, mut v: u64) {
    if v == 0 {
        out.push(b'0');
        return;
    }
    let mut tmp = [0u8; 20];
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

fn parse_u64(s: &[u8]) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(v)
}

// Attach the coordinator's /ctl: open=connect the byte-mode /srv/stratum-ctl,
// then wrap it in the kernel 9P client. The byte-conn fd is closed after a
// successful attach (the attach holds its own ref; the rings are
// kernel_attached, so closing the now-redundant conn handle does NOT EOF
// them -- the joey 16c pattern). Bounded retry covers the accept race, like
// connect_corvus. Returns the dev9p root fd (>= 0) or -1.
fn attach_ctl() -> i64 {
    for _ in 0..64 {
        let conn = unsafe {
            t_open(T_WALK_OPEN_FROM_ROOT, CTL_SRV_PATH.as_ptr(), CTL_SRV_PATH.len(), T_ORDWR)
        };
        if conn < 0 {
            continue;
        }
        let root = unsafe { t_attach_9p_srv(conn, core::ptr::null(), 0, 0) };
        let _ = unsafe { t_close(conn) };
        if root >= 0 {
            return root;
        }
    }
    -1
}

// provision-dek: idempotent ensure-home. Payload (single Twrite, exact length):
//   owner_uid(u32 LE) owner_gid(u32 LE) name_len(u8) name path_len(u8) path token[33]
// name = the dataset name (= the username; single component, no '/'); path =
// the corvus key path "users/<user>"; owner = the user's principal/gid (the
// home root is born user-owned 0700). The coordinator folds EEXIST -> OK, so a
// returning user is a no-op.
unsafe fn provision_dek(ctl_root: i64, owner_uid: u32, owner_gid: u32,
                        user: &[u8], token: &[u8; TOKEN_LEN]) -> bool {
    if user.is_empty() || user.len() > 255 {
        return false;
    }
    let mut path: Vec<u8> = Vec::new();
    path.extend_from_slice(b"users/");
    path.extend_from_slice(user);
    if path.len() > 255 {
        return false;
    }
    let mut pl: Vec<u8> = Vec::new();
    pl.extend_from_slice(&owner_uid.to_le_bytes());
    pl.extend_from_slice(&owner_gid.to_le_bytes());
    pl.push(user.len() as u8);
    pl.extend_from_slice(user);
    pl.push(path.len() as u8);
    pl.extend_from_slice(&path);
    pl.extend_from_slice(token);

    let node = b"datasets/provision-dek";
    let fd = t_open(ctl_root, node.as_ptr(), node.len(), T_OWRITE);
    if fd < 0 {
        t_putstr("login: provision-dek open denied (kernel rwx / walk)\n");
        return false;
    }
    let ok = write_one(fd, &pl);
    if !ok {
        t_putstr("login: provision-dek write rejected (coordinator)\n");
    }
    let _ = t_close(fd);
    ok
}

// Read a /ctl file relative to the attach root, up to `cap` bytes. The /ctl
// files are synthetic (size reported 0); read until EOF.
unsafe fn read_ctl_file(ctl_root: i64, path: &[u8], cap: usize) -> Option<Vec<u8>> {
    let fd = t_open(ctl_root, path.as_ptr(), path.len(), T_OREAD);
    if fd < 0 {
        return None;
    }
    let mut out: Vec<u8> = Vec::new();
    let mut chunk = [0u8; 512];
    loop {
        let r = t_read(fd, chunk.as_mut_ptr(), chunk.len());
        if r < 0 {
            let _ = t_close(fd);
            return None;
        }
        if r == 0 {
            break;
        }
        out.extend_from_slice(&chunk[..r as usize]);
        if out.len() >= cap {
            break;
        }
    }
    let _ = t_close(fd);
    Some(out)
}

// Debug aid: dump the coordinator's /ctl/events tail to the boot log so a DEK
// failure surfaces the server-side error code (the coordinator logs
// "provision-dek uid=.. result=err:.." there). World-readable (0444).
unsafe fn dump_ctl_events(ctl_root: i64) {
    if let Some(body) = read_ctl_file(ctl_root, b"events", 4096) {
        t_putstr("login: /ctl/events tail:\n");
        // Emit the last ~600 bytes (whole-line-ish) -- enough for the last
        // few event lines without flooding.
        let start = body.len().saturating_sub(600);
        if let Ok(s) = core::str::from_utf8(&body[start..]) {
            t_putstr(s);
        }
        t_putstr("\nlogin: /ctl/events end\n");
    }
}

// True if `body` (a /ctl properties file) has a "name: <user>" line. The
// dataset name is stamped by provision-dek; the first "name: "-prefixed line
// carries it.
fn properties_name_is(body: &[u8], user: &[u8]) -> bool {
    for line in body.split(|&c| c == b'\n') {
        if let Some(val) = line.strip_prefix(b"name: ") {
            return val == user;
        }
    }
    false
}

// name->id bridge: provision-dek is write-only and returns no id, so resolve
// the home dataset id by enumerating /ctl/datasets and matching each
// <id>/properties "name:" line against the username. Returns the id or None.
unsafe fn resolve_dataset_id(ctl_root: i64, user: &[u8]) -> Option<u64> {
    let node = b"datasets";
    let dir = t_open(ctl_root, node.as_ptr(), node.len(), T_OREAD);
    if dir < 0 {
        return None;
    }
    let mut found: Option<u64> = None;
    let mut buf = [0u8; 2048];
    'outer: loop {
        let n = t_readdir(dir, buf.as_mut_ptr(), buf.len());
        if n <= 0 {
            break;
        }
        let n = n as usize;
        let mut off = 0usize;
        // Each entry: qid(13) + offset(8) + type(1) + name_len(2 LE) + name.
        while off + 24 <= n {
            let name_len = (buf[off + 22] as usize) | ((buf[off + 23] as usize) << 8);
            let name_start = off + 24;
            if name_start + name_len > n {
                break;
            }
            // Copy the name out before borrowing buf again for the read.
            let mut name: Vec<u8> = Vec::new();
            name.extend_from_slice(&buf[name_start..name_start + name_len]);
            off = name_start + name_len;
            // Only the <id> dirs are numeric; skip "provision-dek", ".", "..".
            let id = match parse_u64(&name) {
                Some(v) => v,
                None => continue,
            };
            let mut ppath: Vec<u8> = Vec::new();
            ppath.extend_from_slice(b"datasets/");
            ppath.extend_from_slice(&name);
            ppath.extend_from_slice(b"/properties");
            if let Some(body) = read_ctl_file(ctl_root, &ppath, 8192) {
                if properties_name_is(&body, user) {
                    found = Some(id);
                    break 'outer;
                }
            }
        }
    }
    let _ = t_close(dir);
    found
}

// install-dek: UNWRAP the home's DEK into the live map (the conn-bound lease).
// Payload = the 33-byte token, single Twrite.
unsafe fn install_dek(ctl_root: i64, dsid: u64, token: &[u8; TOKEN_LEN]) -> bool {
    let mut path: Vec<u8> = Vec::new();
    path.extend_from_slice(b"datasets/");
    put_u64(&mut path, dsid);
    path.extend_from_slice(b"/install-dek");
    let fd = t_open(ctl_root, path.as_ptr(), path.len(), T_OWRITE);
    if fd < 0 {
        return false;
    }
    let ok = write_one(fd, token);
    let _ = t_close(fd);
    ok
}

// evict-dek: zero + remove the session DEK. Payload = a single trigger byte
// (content ignored). Only the owning conn may evict; conn-destroy auto-evicts
// too, but explicit eviction zeroes promptly.
unsafe fn evict_dek(ctl_root: i64, dsid: u64) -> bool {
    let mut path: Vec<u8> = Vec::new();
    path.extend_from_slice(b"datasets/");
    put_u64(&mut path, dsid);
    path.extend_from_slice(b"/evict-dek");
    let fd = t_open(ctl_root, path.as_ptr(), path.len(), T_OWRITE);
    if fd < 0 {
        return false;
    }
    let ok = write_one(fd, b"x");
    let _ = t_close(fd);
    ok
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

    // A-5b DEK lifecycle. FATAL: an encrypted home that cannot be provisioned
    // or unlocked is a failed login (pam_mount parity). Because the joey
    // login-E2E gate is login's exit code, making this fatal lets the boot E2E
    // cover the whole DEK path. The /ctl attach is held until logout (the
    // install-dek lease is conn-bound).
    // DEK errors go to the UART (t_putstr -> boot log), not fd 1: in the
    // seeded CI path fd 1 is the creds pipe (no console), and these are
    // fatal boot-path diagnostics that must be greppable.
    let ctl = attach_ctl();
    if ctl < 0 {
        t_putstr("login: coordinator /ctl unavailable\n");
        unsafe { session_close(conn, &token) };
        let _ = unsafe { t_close(conn) };
        return 1;
    }
    if !unsafe { provision_dek(ctl, pid, gid, &user, &token) } {
        t_putstr("login: home provisioning failed\n");
        unsafe { dump_ctl_events(ctl) };
        let _ = unsafe { t_close(ctl) };
        unsafe { session_close(conn, &token) };
        let _ = unsafe { t_close(conn) };
        return 1;
    }
    let dsid = match unsafe { resolve_dataset_id(ctl, &user) } {
        Some(id) => id,
        None => {
            t_putstr("login: home dataset unresolved\n");
            let _ = unsafe { t_close(ctl) };
            unsafe { session_close(conn, &token) };
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };
    if !unsafe { install_dek(ctl, dsid, &token) } {
        t_putstr("login: home key install failed\n");
        unsafe { dump_ctl_events(ctl) };
        let _ = unsafe { t_close(ctl) };
        unsafe { session_close(conn, &token) };
        let _ = unsafe { t_close(conn) };
        return 1;
    }
    {
        let mut line: Vec<u8> = Vec::new();
        line.extend_from_slice(b"login: dek ");
        line.extend_from_slice(&user);
        line.extend_from_slice(b" ds=");
        put_u64(&mut line, dsid);
        line.extend_from_slice(b" home provisioned + unlocked\n");
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
            let _ = unsafe { evict_dek(ctl, dsid) };
            let _ = unsafe { t_close(ctl) };
            unsafe { session_close(conn, &token) };
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };

    // Session leader: wait the shell. Its exit IS logout (regardless of status).
    let _ = child.wait();

    // Logout: evict the session DEK (the conn-bound lease; conn-destroy would
    // also auto-evict, but explicit eviction zeroes promptly), drop the /ctl
    // attach, then close the corvus session (zeroes the keypair) + drop the conn.
    let _ = unsafe { evict_dek(ctl, dsid) };
    let _ = unsafe { t_close(ctl) };
    unsafe { session_close(conn, &token) };
    let _ = unsafe { t_close(conn) };
    0
}
