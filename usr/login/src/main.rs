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

use alloc::string::String;
use alloc::vec::Vec;
use libthyla_rs::env;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::{
    t_attach_9p_srv, t_close, t_explicit_bzero, t_mount, t_open, t_poll, t_putstr, t_read,
    t_readdir, t_set_dumpable, t_set_traceable, t_torpor_wait, t_unmount, t_walk_create,
    t_walk_open, t_write, TPollFd, T_CAP_CSPRNG_READ, T_CAP_LOCK_PAGES, T_MREPL, T_OPATH, T_OREAD,
    T_ORDWR, T_OWRITE, T_POLLIN, T_SPAWN_PERM_CONSOLE_OWNER, T_SPAWN_PERM_MAY_POST_SERVICE,
    T_WALK_CREATE_DMDIR, T_WALK_OPEN_FROM_ROOT,
};

const VERB_AUTH: u8 = 1;
const VERB_SESSION_CLOSE: u8 = 3;
const VERB_RECOVER: u8 = 8;
const VERB_RESOLVE_ID: u8 = 11;
const VERB_RESOLVE_NAME: u8 = 12;
const CORVUS_PROTOCOL_VERSION: u8 = 1;
const STATUS_OK: u8 = 0;
const TOKEN_LEN: usize = 33;

// A-5c-c: the `!recover` login sentinel enters the passphrase-recovery UX. `!`
// is not a valid corvus username char ([A-Za-z0-9._-]), so it cannot collide
// with a real user typed at the prompt.
const RECOVERY_SENTINEL: &[u8] = b"!recover";

const FD_IN: i64 = 0; // the tty (or seeded creds pipe)
const FD_OUT: i64 = 1; // the tty (or capture pipe)
const MAX_LINE: usize = 256;
const SHELL: &str = "/bin/ut";  // #58: resolved via joey's post-pivot /bin bind

// The user shell runs as the authenticated user with NO elevation/identity/grant
// caps -- a shell is not an identity-stamper. It keeps the two benign
// fork-grantable user caps (mlock + getrandom). login must HOLD these to pass
// them down, so joey grants login CAP_SET_IDENTITY | LOCK_PAGES | CSPRNG_READ.
const SHELL_CAPS: u64 = T_CAP_LOCK_PAGES | T_CAP_CSPRNG_READ;

// ── LS-6 / #94-B: the console line discipline via the inherited consctl fd ──
//
// joey's getty (and the seeded do_login_e2e) pass "--consctl-fd 3" + an inherited
// /dev/consctl handle at fd 3 when the console is real. login -- which is NOT
// console-attached -- drives the line discipline through that handle: the kernel
// devdev_open mint-gate kept joey the only minter, but consctl I/O is reachable
// by the delegated holder of the inherited fd (#94-B). The five-flag stty-style
// grammar is the LS-8b consctl ABI (cons_set_mode_cmd applies one write
// atomically). The mode strings are ABSOLUTE (every flag named), so the result is
// independent of the prior state.
//
//   COOKED_ECHO   = the username read: line editing + visible typing + Enter (CR)
//                   terminates the canonical line (ICRNL) + clean output (ONLCR).
//   COOKED_NOECHO = the passphrase read: same, but ECHO clear -> the kernel's
//                   HARD no-output guarantee masks the passphrase (LS-8b).
//   DEFAULT       = ISIG only (== CONS_TERMIOS_DEFAULT): raw byte-at-a-time, no
//                   echo, Ctrl-C -> interrupt. Restored before the shell, whose
//                   line editor wants raw per-keystroke input.
const MODE_COOKED_ECHO: &[u8] = b"+icanon +echo +isig +icrnl +onlcr";
const MODE_COOKED_NOECHO: &[u8] = b"+icanon -echo +isig +icrnl +onlcr";
const MODE_DEFAULT: &[u8] = b"-icanon -echo +isig -icrnl -onlcr";

// #106-F3 drift guard: login's restore-before-shell discipline MUST equal ut's
// prompt mode (libutopia::eval::console::PROMPT_MODE) so the login->ut boundary
// does not flip the console. Both are const-asserted to the SAME literal, here on
// the no_std device build, so a drift on either side fails its own build.
const _: () = {
    const fn bytes_eq(a: &[u8], b: &[u8]) -> bool {
        if a.len() != b.len() {
            return false;
        }
        let mut i = 0;
        while i < a.len() {
            if a[i] != b[i] {
                return false;
            }
            i += 1;
        }
        true
    }
    assert!(bytes_eq(MODE_DEFAULT, b"-icanon -echo +isig -icrnl -onlcr"));
};

// Parse "--consctl-fd N" from argv -> the consctl fd, or -1 if absent/malformed.
// N is the child fd the inherited handle landed at (always 3 in the trusted
// joey->login chain, but parsed generically).
fn parse_consctl_fd() -> i64 {
    let mut it = env::args().operands();
    while let Some(a) = it.next() {
        if a == b"--consctl-fd" {
            // Lower bound 3: a consctl fd is ALWAYS an inherited slot past the 3
            // stdio fds (joey hands it at fd 3), so 0/1/2 is never valid.
            // Rejecting them keeps a stray `--consctl-fd 1` from writing the mode
            // string to a stdio fd as raw bytes + a FALSE `login: consctl ok`
            // witness (audit F3); a value below the stdio range is malformed.
            return match it.next().and_then(parse_u64) {
                Some(v) if (3..=i64::MAX as u64).contains(&v) => v as i64,
                _ => -1,
            };
        }
    }
    -1
}

// Apply a console mode via the inherited consctl fd. Best-effort: returns true
// iff the whole command was accepted (cons_set_mode_cmd returns n on success,
// -1 on a malformed token OR -- on a pre-#94-B kernel -- the I/O gate). A false
// result means login simply runs without the line discipline (no regression --
// the raw byte-at-a-time prompt the pre-LS-6 login used). A negative fd (no
// console / not passed) is a no-op.
fn set_console_mode(consctl_fd: i64, cmd: &[u8]) -> bool {
    if consctl_fd < 0 {
        return false;
    }
    let w = unsafe { t_write(consctl_fd, cmd.as_ptr(), cmd.len()) };
    w == cmd.len() as i64
}

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
            // A raw console (no line discipline at v1.0) delivers CR (0x0d) on
            // Enter, not LF -- accept either as the line terminator. ut's line
            // editor already accepts both; login's hand-rolled reader must too,
            // or a real keypress at the getty prompt never completes a line.
            b'\n' | b'\r' => return true,
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
    let r = exchange(fd, VERB_AUTH, &pl);
    // pl held a copy of the cleartext passphrase -- scrub before it drops
    // (mallocng recycles freed heap; match corvus's explicit_bzero discipline).
    let _ = t_explicit_bzero(pl.as_mut_ptr(), pl.len());
    let (st, mut resp) = r?;
    if st != STATUS_OK || resp.len() != TOKEN_LEN {
        let _ = t_explicit_bzero(resp.as_mut_ptr(), resp.len());
        return None;
    }
    let mut token = [0u8; TOKEN_LEN];
    token.copy_from_slice(&resp);
    let _ = t_explicit_bzero(resp.as_mut_ptr(), resp.len());
    Some(token)
}

// RECOVER(user) (verb 8): subject_kind(1)=1 + user_len(1) + user +
// phrase_len(2 LE) + phrase + new_pass_len(2 LE) + new_pass. NO session token,
// NO capability -- the user lost the passphrase, so phrase-knowledge + corvus's
// rate limit are the entire gate. OK reply: phrase_len(2 LE) + fresh_phrase.
unsafe fn recover_user(
    fd: i64,
    user: &[u8],
    phrase: &[u8],
    new_pass: &[u8],
) -> Option<(u8, Vec<u8>)> {
    // A-5c-c (audit F3): user_len is a single wire byte, so bound it so the cast
    // is provably lossless -- never silently truncate an over-long name to a
    // shorter (valid-looking) one. phrase_len/new_pass_len are u16-encoded (no
    // truncation) and read_line caps inputs at MAX_LINE; corvus re-validates all
    // three. None -> a clean "recovery failed" (no truncated request on the wire).
    if user.is_empty() || user.len() > 255 {
        return None;
    }
    let mut pl: Vec<u8> = Vec::new();
    pl.push(1u8); // subject_kind = 1 (user)
    pl.push(user.len() as u8);
    pl.extend_from_slice(user);
    pl.push((phrase.len() & 0xff) as u8);
    pl.push(((phrase.len() >> 8) & 0xff) as u8);
    pl.extend_from_slice(phrase);
    pl.push((new_pass.len() & 0xff) as u8);
    pl.push(((new_pass.len() >> 8) & 0xff) as u8);
    pl.extend_from_slice(new_pass);
    let r = exchange(fd, VERB_RECOVER, &pl);
    // pl held the cleartext recovery phrase + the new passphrase; scrub before
    // the buffer drops (mallocng recycles freed heap; #828 A-F1 discipline).
    let _ = t_explicit_bzero(pl.as_mut_ptr(), pl.len());
    r
}

// The login `!recover` recovery UX (A-5c-c). A user who lost their passphrase
// types `!recover` at the prompt; login then reads the user, the printed
// recovery phrase, and a new passphrase, and drives corvus RECOVER(user). On
// success corvus rolls a FRESH phrase, which login surfaces (the user records it
// and logs in normally with the new passphrase). login is NOT console-attached
// and creates NO session here -- it returns to the getty prompt (exit code is
// the CI gate). Secrets (phrase, new passphrase, the returned fresh phrase) are
// scrubbed on every path.
unsafe fn do_recover_flow() -> i64 {
    write_out(b"recover user: ");
    let mut ruser: Vec<u8> = Vec::new();
    if !read_line(FD_IN, &mut ruser) || ruser.is_empty() {
        write_out(b"recovery aborted (no user)\n");
        return 1;
    }
    write_out(b"recovery phrase: ");
    let mut phrase: Vec<u8> = Vec::new();
    if !read_line(FD_IN, &mut phrase) || phrase.is_empty() {
        write_out(b"recovery aborted (no phrase)\n");
        return 1;
    }
    write_out(b"new passphrase: ");
    let mut newpass: Vec<u8> = Vec::new();
    if !read_line(FD_IN, &mut newpass) || newpass.is_empty() {
        let _ = t_explicit_bzero(phrase.as_mut_ptr(), phrase.len());
        write_out(b"recovery aborted (no passphrase)\n");
        return 1;
    }

    let conn = connect_corvus();
    if conn < 0 {
        let _ = t_explicit_bzero(phrase.as_mut_ptr(), phrase.len());
        let _ = t_explicit_bzero(newpass.as_mut_ptr(), newpass.len());
        write_out(b"login: corvus unavailable\n");
        return 1;
    }
    let res = recover_user(conn, &ruser, &phrase, &newpass);
    // The phrase + new passphrase are consumed; scrub login's copies.
    let _ = t_explicit_bzero(phrase.as_mut_ptr(), phrase.len());
    let _ = t_explicit_bzero(newpass.as_mut_ptr(), newpass.len());
    let _ = t_close(conn);

    match res {
        Some((STATUS_OK, mut resp)) => {
            // resp = phrase_len(2 LE) + fresh_phrase.
            let plen = if resp.len() >= 2 {
                (resp[0] as usize) | ((resp[1] as usize) << 8)
            } else {
                0
            };
            if plen > 0 && resp.len() == 2 + plen {
                write_out(b"\nPassphrase reset. NEW recovery phrase (write it down; the old one no longer works):\n");
                write_out(&resp[2..2 + plen]);
                write_out(b"\nLog in with your new passphrase.\n");
                // Boot-log marker for the CI E2E (in the seeded run fd 1 is the
                // creds pipe's read end, so write_out no-ops; t_putstr -> UART).
                t_putstr("login: recovery ok (RECOVER(user) reset the passphrase; fresh phrase issued)\n");
                let _ = t_explicit_bzero(resp.as_mut_ptr(), resp.len());
                return 0;
            }
            let _ = t_explicit_bzero(resp.as_mut_ptr(), resp.len());
            write_out(b"recovery failed (malformed response)\n");
            t_putstr("login: recovery FAILED (malformed response)\n");
            1
        }
        Some((_st, _resp)) => {
            write_out(b"recovery failed (bad phrase, unknown user, or rate-limited)\n");
            t_putstr("login: recovery FAILED (corvus rejected)\n");
            1
        }
        None => {
            // transport error OR an over-long input rejected by recover_user.
            write_out(b"recovery failed\n");
            t_putstr("login: recovery FAILED (transport or malformed input)\n");
            1
        }
    }
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
        let root = unsafe { t_attach_9p_srv(conn, core::ptr::null(), 0, 0, 0) };
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
        // pl already carries the session token; scrub on this early-return too
        // (RW-6 R4-F1 -- the bzero below at the write path was the only covered leg).
        let _ = t_explicit_bzero(pl.as_mut_ptr(), pl.len());
        return false;
    }
    let ok = write_one(fd, &pl);
    if !ok {
        t_putstr("login: provision-dek write rejected (coordinator)\n");
    }
    // pl carried the corvus session token; scrub before it drops (#828 A-F1).
    let _ = t_explicit_bzero(pl.as_mut_ptr(), pl.len());
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

// ── A-5b per-user encrypted-home bind (IDENTITY-DESIGN section 9.9) ──
//
// Once the DEK is installed (the home dataset is unlockable by the coordinator),
// login binds the user's encrypted home at /home/<user>. The home is a separate
// Stratum dataset (users/<user>) that login does NOT serve directly: it spawns a
// per-user proxy stratumd (--role client) AS the user, so the proxy's connection
// to the boot coordinator carries the user's SO_PEERCRED and the coordinator
// stamps the user as owner. The proxy posts /srv/home-<user> in the session
// namespace; login attaches that 9P service and mounts it at /home/<user>. The
// shell, spawned next, inherits the bind (the kernel territory_clone deep-copies
// the mount table).
//
// The proxy is --single-session: it serves login's one attach and exits when
// login closes it at logout; login then reaps it. login (PRINCIPAL_SYSTEM)
// cannot KILL the user-owned proxy (the kill axes are owner / CAP_HOSTOWNER /
// CAP_KILL, none of which login holds against a user-owned Proc), so the
// cooperative serve-one teardown is the mechanism. login confers
// MAY_POST_SERVICE on the proxy via the one-hop delegation (login holds the bit
// from joey); it does NOT confer CONSOLE_TRUSTED (I-27).

const COORD_FS_PATH: &str = "/srv/stratum-fs";

// The session backing a bound /home/<user>: the proxy Child (reaped at logout),
// login's dev9p attach root fd, and the mount path (for t_unmount).
struct HomeSession {
    proxy: Child,
    attach_root: i64,
    mount_path: Vec<u8>,
}

// mkdir-if-absent then O_PATH-open: a non-opened walkable handle usable as a
// mountpoint or as the parent for the next level (the joey mkdir_or_open idiom).
// `parent_fd` is T_WALK_OPEN_FROM_ROOT or a prior O_PATH handle. Returns the dir
// fd (>= 0) or -1.
unsafe fn mkdir_or_open(parent_fd: i64, name: &[u8]) -> i64 {
    let cf = t_walk_create(parent_fd, name.as_ptr(), name.len(), T_OREAD,
                           T_WALK_CREATE_DMDIR | 0o755);
    if cf >= 0 {
        let _ = t_close(cf);
    }
    t_walk_open(parent_fd, name.as_ptr(), name.len(), T_OPATH)
}

// Drain whatever is currently readable on the proxy's captured stderr to the
// boot log (UART), strictly non-blocking (poll timeout 0). Mirrors joey's
// stratumd stderr drain: surfaces the proxy's startup diagnostics AND keeps the
// pipe buffer from filling (a full buffer would stall the proxy's next stderr
// write). No-op when `fd` < 0.
unsafe fn drain_proxy_stderr(fd: i64) {
    if fd < 0 {
        return;
    }
    loop {
        let mut pfd = TPollFd { fd: fd as i32, events: T_POLLIN, revents: 0 };
        let pr = t_poll(&mut pfd as *mut TPollFd, 1, 0);
        if pr <= 0 || (pfd.revents & T_POLLIN) == 0 {
            break;
        }
        let mut chunk = [0u8; 256];
        let r = t_read(fd, chunk.as_mut_ptr(), chunk.len());
        if r <= 0 {
            break;
        }
        if let Ok(s) = core::str::from_utf8(&chunk[..r as usize]) {
            t_putstr(s);
        }
    }
}

// bind_home: spawn the per-user proxy, wait for its /srv/home-<user> post,
// attach it, and mount it at /home/<user>. Returns the session handle (torn down
// at logout) or None on any failure -- a home that cannot be bound is a failed
// login (the boot E2E gates the whole path).
unsafe fn bind_home(user: &[u8], pid: u32, gid: u32, supp: &[u32]) -> Option<HomeSession> {
    let user_str = core::str::from_utf8(user).ok()?;

    // "/srv/home-<user>" (the proxy's session-namespace service) and the
    // attach aname "ds:<user>" (the Stratum `ds:<name>` child-dataset selector;
    // the dataset NAME = the username, as minted by provision-dek). The same
    // string is the proxy's --datasets-allowed pattern (the I-1 gate).
    let mut listen = String::from("/srv/home-");
    listen.push_str(user_str);
    let mut allowed = String::from("ds:");
    allowed.push_str(user_str);

    // Spawn the proxy AS the user. .perm(MAY_POST_SERVICE) is the one-hop
    // delegation (login holds the bit from joey); .identity stamps the user so
    // the proxy->coordinator SO_PEERCRED is the user; --datasets-allowed scopes
    // the Tattach to users/<user> (the I-1 boundary). stderr is captured for
    // boot-log diagnostics; stdin/stdout inherit login's.
    let mut cmd = Command::new("/bin/stratumd");  // #58: post-pivot /bin bind
    cmd.identity(pid, gid, supp)
        .caps(T_CAP_CSPRNG_READ)
        .perm(T_SPAWN_PERM_MAY_POST_SERVICE)
        .arg("--role").arg("client")
        .arg("--listen").arg(listen.clone())
        .arg("--coordinator-socket").arg(COORD_FS_PATH)
        .arg("--datasets-allowed").arg(allowed.clone())
        .arg("--single-session")
        .stdin(Stdio::Inherit)
        .stdout(Stdio::Inherit)
        .stderr(Stdio::Piped);
    let mut proxy = match cmd.spawn() {
        Ok(c) => c,
        Err(_) => {
            t_putstr("login: home proxy spawn failed\n");
            return None;
        }
    };
    let err_fd = proxy.stderr.as_ref().map(|f| f.as_raw_fd() as i64).unwrap_or(-1);

    // Wait for the proxy to libsodium-init + dial the coordinator + post
    // /srv/home-<user>, then attach. Between retries, yield the vCPU via a 1 ms
    // torpor timeout (the joey-stratumd pacing: busy-spin starves the proxy's
    // threads under TCG). `pacer` is a never-woken sleep-only sentinel.
    // 3000 * 1 ms = ~3 s outer bound.
    let listen_b = listen.as_bytes();
    let allowed_b = allowed.as_bytes();
    let mut attach_root: i64 = -1;
    let pacer: u32 = 0;
    for _ in 0..3000 {
        let conn = t_open(T_WALK_OPEN_FROM_ROOT, listen_b.as_ptr(), listen_b.len(), T_ORDWR);
        if conn >= 0 {
            // attach the proxy's session via the `ds:<user>` child-dataset aname.
            let r = t_attach_9p_srv(conn, allowed_b.as_ptr(), allowed_b.len(), 0, 0);
            let _ = t_close(conn);
            if r >= 0 {
                attach_root = r;
                break;
            }
        }
        drain_proxy_stderr(err_fd);
        let _ = t_torpor_wait(&pacer as *const u32, 0, 1000);
    }
    drain_proxy_stderr(err_fd);
    if attach_root < 0 {
        t_putstr("login: home attach failed (proxy never posted /srv/home)\n");
        // The single-session proxy never got its client; it is still in accept.
        // login cannot kill it; drop the Child (the kernel reaps it when login
        // exits). This is a failed login.
        return None;
    }

    // Mount the attached home at /home/<user>. The mountpoint must exist: mkdir
    // /home then /home/<user> (login runs as SYSTEM and owns the boot-pool root,
    // so these creates are permitted), then graft.
    let home_dir = mkdir_or_open(T_WALK_OPEN_FROM_ROOT, b"home");
    if home_dir < 0 {
        t_putstr("login: /home mkdir failed\n");
        let _ = t_close(attach_root);
        // Closing the attach EOFs the single-session proxy's upstream; reap it so
        // it does not orphan to kproc as a permanent zombie (#828 C-F1). The proxy
        // got its session here (attach_root >= 0), so wait() cannot hang.
        let _ = proxy.wait();
        return None;
    }
    let ud = mkdir_or_open(home_dir, user);
    let _ = t_close(home_dir);
    if ud < 0 {
        t_putstr("login: /home/<user> mkdir failed\n");
        let _ = t_close(attach_root);
        let _ = proxy.wait(); // reap the attached single-session proxy (#828 C-F1)
        return None;
    }
    let _ = t_close(ud);

    let mut mount_path: Vec<u8> = Vec::new();
    mount_path.extend_from_slice(b"/home/");
    mount_path.extend_from_slice(user);
    if t_mount(mount_path.as_ptr(), mount_path.len(), attach_root, T_MREPL) != 0 {
        t_putstr("login: /home/<user> mount failed\n");
        let _ = t_close(attach_root);
        let _ = proxy.wait(); // reap the attached single-session proxy (#828 C-F1)
        return None;
    }

    // Drop login's read end of the proxy stderr pipe now that startup is done:
    // steady-state proxy stderr writes then fail fast (EOF) instead of stalling
    // on a full pipe buffer that nobody drains during the session.
    drain_proxy_stderr(err_fd);
    proxy.stderr = None;

    {
        let mut line: Vec<u8> = Vec::new();
        line.extend_from_slice(b"login: home ");
        line.extend_from_slice(user);
        line.extend_from_slice(b" bound at /home/");
        line.extend_from_slice(user);
        line.push(b'\n');
        if let Ok(s) = core::str::from_utf8(&line) {
            t_putstr(s);
        }
    }

    Some(HomeSession { proxy, attach_root, mount_path })
}

// unbind_home: logout teardown. Unmount /home/<user> + close login's attach ->
// the proxy's single upstream EOFs -> the proxy returns from serve and exits ->
// reap it (ut was already reaped, so this wait collects the proxy).
unsafe fn unbind_home(mut sess: HomeSession) {
    let _ = t_unmount(sess.mount_path.as_ptr(), sess.mount_path.len());
    let _ = t_close(sess.attach_root);
    let _ = sess.proxy.wait();
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Secret hygiene (#828 A-F1): login handles the cleartext passphrase + the
    // corvus session token. Forbid core dumps + debug-Spoor attach for this Proc
    // (one-way; matches corvus's startup discipline) so neither secret can leak
    // via a future dump/trace surface. mlockall is intentionally omitted -- it
    // needs CAP_LOCK_PAGES (login holds it only to pass down) and v1.0 has no swap.
    unsafe {
        let _ = t_set_dumpable(0);
        let _ = t_set_traceable(0);
    }
    let mut user: Vec<u8> = Vec::new();
    let mut pass: Vec<u8> = Vec::new();

    // LS-6 (#94-B): cooked + echo for the username read (line editing + visible
    // typing). The first mode-set doubles as the boot-log witness that a
    // non-attached login can drive the INHERITED consctl fd (the #94-B gate drop);
    // the hard regression for the gate is the kernel devdev.cons_gate test.
    let consctl_fd = parse_consctl_fd();
    if consctl_fd >= 0 {
        if set_console_mode(consctl_fd, MODE_COOKED_ECHO) {
            t_putstr("login: consctl ok (line discipline via inherited fd)\n");
        } else {
            t_putstr("login: consctl unavailable (mode-set rejected)\n");
        }
    }

    write_out(b"\nThylacine login: ");
    if !read_line(FD_IN, &mut user) || user.is_empty() {
        set_console_mode(consctl_fd, MODE_DEFAULT); // restore before exit
        write_out(b"login: no username\n");
        return 1;
    }
    // A-5c-c: the `!recover` sentinel enters the passphrase-recovery UX instead
    // of a login -- it resets a user's passphrase from the printed recovery
    // phrase and returns to the getty prompt (no session is created). `!` cannot
    // begin a real corvus username, so this cannot shadow a user.
    if user.as_slice() == RECOVERY_SENTINEL {
        // Recovery reads (phrase, new passphrase) run at the default raw prompt
        // (unchanged from pre-LS-6); restore before entering. A cooked + masked
        // recovery flow is a small follow-up, not a regression.
        set_console_mode(consctl_fd, MODE_DEFAULT);
        return unsafe { do_recover_flow() };
    }
    write_out(b"password: ");
    set_console_mode(consctl_fd, MODE_COOKED_NOECHO); // mask the passphrase (HARD no-echo)
    let pw_ok = read_line(FD_IN, &mut pass);
    set_console_mode(consctl_fd, MODE_DEFAULT); // restore for auth + the shell's raw editor
    if !pw_ok {
        write_out(b"login: no passphrase\n");
        // read_line may have partially filled pass before failing; scrub (RW-6 R4-F1).
        unsafe {
            let _ = t_explicit_bzero(pass.as_mut_ptr(), pass.len());
        }
        return 1;
    }

    let conn = connect_corvus();
    if conn < 0 {
        write_out(b"login: corvus unavailable\n");
        // This leg never reaches the post-auth scrub below; scrub the cleartext
        // passphrase here too (RW-6 R4-F1 -- the #828 A-F1 "every path" hole).
        unsafe {
            let _ = t_explicit_bzero(pass.as_mut_ptr(), pass.len());
        }
        return 1;
    }

    let auth_res = unsafe { auth(conn, &user, &pass) };
    // The passphrase is consumed by auth; scrub login's copy on every path (A-F1).
    unsafe {
        let _ = t_explicit_bzero(pass.as_mut_ptr(), pass.len());
    }
    let mut token = match auth_res {
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

    // A-5b: bind the user's encrypted home at /home/<user> BEFORE spawning the
    // shell, so the shell inherits the mount (the kernel deep-copies the mount
    // table on spawn). FATAL: a home that cannot be bound is a failed login.
    let home = match unsafe { bind_home(&user, pid, gid, &supp) } {
        Some(h) => h,
        None => {
            t_putstr("login: home bind failed\n");
            unsafe { dump_ctl_events(ctl) };
            let _ = unsafe { evict_dek(ctl, dsid) };
            let _ = unsafe { t_close(ctl) };
            unsafe { session_close(conn, &token) };
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };

    // Spawn the shell AS the user. fd 0/1/2 inherit login's (the tty), so the
    // shell reads + writes the same console. The shell gets the user's identity
    // (SPAWN_IDENTITY_SET) but NOT CAP_SET_IDENTITY. It inherits the /home/<user>
    // bind established above. LS-5 (P1): confer CONSOLE_OWNER so the kernel makes
    // `ut` the g_console_owner -- the Proc that receives the `interrupt` note on
    // Ctrl-C. login holds MAY_POST_SERVICE (from joey), which is the gate for
    // CONSOLE_OWNER; the owner bit never confers console-attach (I-27 untouched).
    let mut shell_cmd = Command::new(SHELL);
    shell_cmd
        .identity(pid, gid, &supp)
        .caps(SHELL_CAPS)
        .perm(T_SPAWN_PERM_CONSOLE_OWNER)
        .stdin(Stdio::Inherit)
        .stdout(Stdio::Inherit)
        .stderr(Stdio::Inherit);
    // #113: tell the shell its home (there is no kernel envp for $HOME). The
    // shell sets $home -- which `cd` with no argument and the prompt's ~ resolve
    // -- and chdirs into /home/<user> at startup, the same path bound above.
    let mut home_path: Vec<u8> = Vec::with_capacity(6 + user.len());
    home_path.extend_from_slice(b"/home/");
    home_path.extend_from_slice(&user);
    if let Ok(h) = core::str::from_utf8(&home_path) {
        shell_cmd.arg("--home").arg(h);
    }
    // #94-B-b: forward the inherited /dev/consctl fd to the session shell so the
    // shell -- not login -- owns the console line discipline (the controlling-
    // terminal termios) for the session. It lands at the child's fd 3 (the first
    // inherited slot after the 3 stdio slots); ut is told its number via
    // --consctl-fd. Neither login nor ut is console-attached -- consctl I/O is
    // ungated (#94-B-a), the attach/SAK gate (I-27) untouched; ut holds it
    // privately (it never re-forwards it to a user child).
    if consctl_fd >= 0 {
        shell_cmd
            .inherit_fd(consctl_fd as i32)
            .arg("--consctl-fd")
            .arg("3");
    }
    let mut child = match shell_cmd.spawn() {
        Ok(c) => c,
        Err(_) => {
            write_out(b"login: shell spawn failed\n");
            unsafe { unbind_home(home) };
            let _ = unsafe { evict_dek(ctl, dsid) };
            let _ = unsafe { t_close(ctl) };
            unsafe { session_close(conn, &token) };
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };

    // Session leader: wait the shell. Its exit IS logout (regardless of status).
    let _ = child.wait();

    // Logout: tear down the home (unmount /home/<user> + close the attach -> the
    // single-session proxy's upstream EOFs -> the proxy exits -> reap it), then
    // evict the session DEK (the conn-bound lease; conn-destroy would also
    // auto-evict, but explicit eviction zeroes promptly), drop the /ctl attach,
    // close the corvus session (zeroes the keypair) + drop the conn.
    unsafe { unbind_home(home) };
    let _ = unsafe { evict_dek(ctl, dsid) };
    let _ = unsafe { t_close(ctl) };
    unsafe { session_close(conn, &token) };
    let _ = unsafe { t_close(conn) };
    // Scrub the session token (#828 A-F1); it is dead once the session is closed.
    unsafe {
        let _ = t_explicit_bzero(token.as_mut_ptr(), token.len());
    }
    0
}
