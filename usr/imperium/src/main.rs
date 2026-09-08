// /bin/imperium (IM-4) -- the lex curiata trigger: a thin, UNTRUSTED tool that
// requests self-elevation, tells you to confer with the SAK, and -- on
// conferral -- becomes the propagating legate root of an elevated sub-shell.
//
// IMPERIUM-DESIGN.md 3.1 + 11.6. The security property is that this tool renders
// NO authorization surface: the provincia (the exact cap-set) and the key prompt
// are corvus's, drawn on the console the kernel FROZE for everyone else after
// the SAK (I-27). A hostile program in your namespace could draw a fake tool,
// but it cannot press the SAK (a physical key the kernel catches) and cannot
// paint the trusted path. So the tool is deliberately dumb: post the intent,
// block on the deferred reply, redeem, spawn the shell.
//
// Flow (the default, elevate):
//   1. reject a non-interactive invocation (no controlling tty) -- UX, not the
//      gate: a script's request does nothing until a human presses BREAK, but
//      failing fast beats parking forever on a reply no one can confer.
//   2. refuse if already in a legate scope (IMPERIUM-DESIGN.md 11.4 consequence
//      11: propagating scopes never nest -- the kernel would refuse the redeem;
//      we say so first).
//   3. connect /srv/corvus/ctl, send IMPERIUM_REQUEST (verb 19).
//   4. print "confer with the SAK" and BLOCK on the reply -- corvus parks it
//      until the SAK episode concludes.
//   5. on OK: cap::use_grant -> a propagating legate ROOT; spawn `/bin/ut`
//      (fd 0/1/2 + identity inherited; the imperium caps FLOW to it via the
//      kernel's rfork carve, IM-2); wait; exit. The root's death sweeps the
//      whole scope (I-25) -- so `exit`/`abdicate` in the sub-shell ends it.
//
// `imperium --list` is the read-only sibling: what this shell currently holds,
// read from the kernel's unforgeable /proc/<pid>/imperium flag.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::vec::Vec;

use libthyla_rs::cap::{self, Caps};
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{
    env, fd_devclass, t_close, t_getpid, t_open, t_putstr, t_read, t_write, T_CAP_CHOWN,
    T_CAP_DAC_OVERRIDE, T_CAP_KILL, T_OREAD, T_ORDWR, T_WALK_OPEN_FROM_ROOT,
};

use fasces::{parse_imperium, Imperium};

const VERB_IMPERIUM_REQUEST: u8 = 19;
const VERB_CLEARANCE_LIST_SELF: u8 = 20;
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
        let _ = libthyla_rs::t_puts(b.as_ptr(), b.len());
    }
}

fn put_dec(n: u64) {
    let mut tmp = [0u8; 21];
    let mut i = 0usize;
    let mut v = n;
    if v == 0 {
        puts(b"0");
        return;
    }
    while v > 0 && i < tmp.len() {
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        i += 1;
    }
    let mut out = [0u8; 21];
    for j in 0..i {
        out[j] = tmp[i - 1 - j];
    }
    puts(&out[..i]);
}

fn put_hex64(v: u64) {
    let mut buf = [0u8; 18];
    buf[0] = b'0';
    buf[1] = b'x';
    for i in 0..16 {
        let nyb = ((v >> (60 - 4 * i)) & 0xf) as u8;
        buf[2 + i] = if nyb < 10 { b'0' + nyb } else { b'a' + (nyb - 10) };
    }
    puts(&buf);
}

fn usage() {
    t_putstr(concat!(
        "usage: imperium [chown] [dac] [kill]   request elevation; confer with the SAK\n",
        "       imperium --list                 show what this shell currently holds\n",
        "       imperium --help                 this help\n",
        "\n",
        "With no caps named, imperium requests the full level (CAP_DAC_OVERRIDE\n",
        "CAP_CHOWN CAP_KILL). Naming caps restricts the request to that subset.\n",
        "After `imperium`, an elevated sub-shell opens; `exit` or `abdicate` ends it.\n",
    ));
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

// The reply half. For IMPERIUM_REQUEST this read PARKS inside corvus -- the
// Rread is withheld until the SAK episode concludes (or the 60-s window
// expires -> Timeout, or the slot is taken -> Busy immediately).
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

/// Read + parse THIS Proc's `/proc/<pid>/imperium` (no /proc/self, so getpid +
/// format). `None` on any failure OR a non-legate `scope 0` -- shared parse in
/// `fasces::parse_imperium` so the tool, the prompt, and `abdicate` agree.
fn read_own_imperium() -> Option<Imperium> {
    let pid = unsafe { t_getpid() };
    if pid <= 0 {
        return None;
    }
    let mut path: Vec<u8> = Vec::new();
    path.extend_from_slice(b"/proc/");
    push_dec(&mut path, pid as u64);
    path.extend_from_slice(b"/imperium");
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; 256];
    let r = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    let _ = unsafe { t_close(fd) };
    if r <= 0 {
        return None;
    }
    let s = core::str::from_utf8(&buf[..r as usize]).ok()?;
    parse_imperium(s)
}

fn push_dec(out: &mut Vec<u8>, n: u64) {
    let mut tmp = [0u8; 21];
    let mut i = 0usize;
    let mut v = n;
    if v == 0 {
        out.push(b'0');
        return;
    }
    while v > 0 && i < tmp.len() {
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        i += 1;
    }
    for j in 0..i {
        out.push(tmp[i - 1 - j]);
    }
}

// Print the three imperium caps a bitmask holds, space-separated. The imperium
// level is exactly {DAC_OVERRIDE, CHOWN, KILL}; a further-redeem extra would
// show in the hex but not here (it names only the known imperium caps).
fn put_cap_names(caps: u64) {
    let mut first = true;
    for (bit, name) in [
        (T_CAP_DAC_OVERRIDE, "CAP_DAC_OVERRIDE"),
        (T_CAP_CHOWN, "CAP_CHOWN"),
        (T_CAP_KILL, "CAP_KILL"),
    ] {
        if caps & bit != 0 {
            if !first {
                t_putstr(" ");
            }
            t_putstr(name);
            first = false;
        }
    }
    if first {
        t_putstr("(none of the imperium caps)");
    }
}

// Connect corvus's ctl (a bounded retry: /srv/corvus may still be coming up at
// login). Returns the ctl fd, or -1.
fn connect_corvus() -> i64 {
    for _ in 0..64 {
        let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/corvus".as_ptr(), 11, T_OREAD) };
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

// `imperium --list`: current holdings (from the kernel /proc flag) PLUS the
// eligibility ladder ("what you could become", from corvus CLEARANCE_LIST_SELF).
fn cmd_list() -> i64 {
    // (a) Current holdings, from the kernel's unforgeable /proc flag.
    match read_own_imperium() {
        None => {
            t_putstr("imperium: not currently elevated (a plain shell)\n");
        }
        Some(im) => {
            t_putstr("imperium: elevated -- scope ");
            put_dec(im.scope);
            t_putstr(" (session ");
            put_dec(im.session);
            t_putstr(")\n  caps: ");
            put_cap_names(im.caps);
            t_putstr(" (");
            put_hex64(im.caps);
            t_putstr(")\n  rods ");
            put_dec(im.rods as u64);
            t_putstr(if im.axe {
                "  axe: PRESENT (power of life and death)\n"
            } else {
                "  axe: absent\n"
            });
            t_putstr(if im.propagating {
                "  propagating: yes (caps flow to children)\n"
            } else {
                "  propagating: no\n"
            });
        }
    }

    // (b) The eligibility ladder, from corvus (read-only; identity is this
    // Proc's kernel-stamped principal, so no token). Best-effort: an unreachable
    // corvus does not fail --list, which has already shown the holdings.
    let conn = connect_corvus();
    if conn < 0 {
        t_putstr("imperium: (corvus unreachable -- eligibility list unavailable)\n");
        return 0;
    }
    let sent = unsafe { send_request(conn, VERB_CLEARANCE_LIST_SELF, &[]) };
    if !sent {
        t_putstr("imperium: (transport error -- eligibility list unavailable)\n");
        let _ = unsafe { t_close(conn) };
        return 0;
    }
    let reply = unsafe { read_reply(conn) };
    let _ = unsafe { t_close(conn) };
    match reply {
        Some((STATUS_OK, body)) => print_eligible(&body),
        Some((STATUS_PERMISSION_DENIED, _)) => {
            t_putstr("imperium: no eligibility ladder (not a corvus user)\n");
        }
        Some((st, _)) => {
            t_putstr("imperium: eligibility list status=");
            put_dec(st as u64);
            t_putstr("\n");
        }
        None => {
            t_putstr("imperium: (transport error -- eligibility reply lost)\n");
        }
    }
    0
}

// Decode + print the CLEARANCE_LIST_SELF reply: count u8, then per level
// name_len u8 + name + auth_required u8 + time_bound u64 LE + caps_tlv_len u16 LE
// + caps_tlv. Fully bounds-checked -- a truncated field stops the walk rather
// than reading past the buffer.
fn print_eligible(body: &[u8]) {
    if body.is_empty() {
        t_putstr("imperium: (empty eligibility reply)\n");
        return;
    }
    let count = body[0];
    if count == 0 {
        t_putstr("imperium: eligible for no levels\n");
        return;
    }
    t_putstr("imperium: eligible levels (what you could become):\n");
    let mut off = 1usize;
    for _ in 0..count {
        if off >= body.len() {
            break;
        }
        let nl = body[off] as usize;
        off += 1;
        if off + nl > body.len() {
            break;
        }
        let name = &body[off..off + nl];
        off += nl;
        if off >= body.len() {
            break;
        }
        let auth = body[off];
        off += 1;
        if off + 8 > body.len() {
            break;
        }
        off += 8; // time_bound -- not shown here
        if off + 2 > body.len() {
            break;
        }
        let tl = (body[off] as usize) | ((body[off + 1] as usize) << 8);
        off += 2;
        if off + tl > body.len() {
            break;
        }
        let tlv = &body[off..off + tl];
        off += tl;
        let caps = caps_from_tlv(tlv);
        t_putstr("  ");
        puts(name);
        t_putstr(auth_label(auth));
        t_putstr(" caps ");
        put_hex64(caps);
        t_putstr("\n");
    }
}

// The caps bitmask from the versioned TLV (version u8 + tag u8 + len u16 LE +
// value). BITMASK tag = 1, value u64 LE. Unknown/short -> 0.
fn caps_from_tlv(tlv: &[u8]) -> u64 {
    if tlv.len() >= 12 && tlv[1] == 1 {
        u64::from_le_bytes([
            tlv[4], tlv[5], tlv[6], tlv[7], tlv[8], tlv[9], tlv[10], tlv[11],
        ])
    } else {
        0
    }
}

fn auth_label(auth: u8) -> &'static str {
    match auth {
        0 => " (session re-auth)",
        1 => " (distinct key, via the SAK)",
        _ => " (special auth)",
    }
}

// Map a non-OK status byte to a message + the process exit code.
fn report_denied(status: u8) -> i64 {
    match status {
        STATUS_BAD_AUTH => {
            t_putstr("imperium: denied (wrong key, or you declined)\n");
        }
        STATUS_PERMISSION_DENIED => {
            t_putstr("imperium: not eligible for the imperium level\n");
        }
        STATUS_NOT_FOUND => {
            t_putstr("imperium: no imperium level is configured\n");
        }
        STATUS_RATE_LIMITED => {
            t_putstr("imperium: too many failed attempts; wait and try again\n");
        }
        STATUS_TIMEOUT => {
            t_putstr("imperium: timed out (no SAK, or no key entered)\n");
        }
        STATUS_BUSY => {
            t_putstr("imperium: another imperium request is pending; try again\n");
        }
        _ => {
            t_putstr("imperium: unexpected status=");
            put_dec(status as u64);
            t_putstr("\n");
        }
    }
    1
}

// The elevate path: request -> confer -> redeem -> sub-shell.
fn cmd_elevate(self_restrict: u64) -> i64 {
    // (1) A non-interactive invocation cannot confer (no human to press the
    // SAK). Fail fast rather than posting a request that parks forever. fd 0
    // must be a terminal ('c' console or 't' pts).
    match fd_devclass(0) {
        Some(b'c') | Some(b't') => {}
        _ => {
            t_putstr(
                "imperium: needs an interactive terminal -- a request cannot be confirmed \
                 without a human at the console to press the SAK (Ctrl-A b)\n",
            );
            return 1;
        }
    }

    // (2) Already in a scope? The kernel refuses a nested propagating redeem
    // ("abdicate first"); say so before posting anything.
    if let Some(im) = read_own_imperium() {
        t_putstr("imperium: already elevated (scope ");
        put_dec(im.scope);
        t_putstr(") -- `abdicate` first to relinquish, then request again\n");
        return 1;
    }

    // (3) Connect corvus's ctl (a bounded retry: the /srv service may still be
    // coming up right at login).
    let conn = connect_corvus();
    if conn < 0 {
        t_putstr("imperium: cannot reach corvus (/srv/corvus)\n");
        return 1;
    }

    // (4) IMPERIUM_REQUEST: level_len u8 + level + self_restrict u64 LE +
    // valid_until_req u64 LE (0 = the level's own bound).
    let mut pl: Vec<u8> = Vec::new();
    pl.push(LEVEL_IMPERIUM.len() as u8);
    pl.extend_from_slice(LEVEL_IMPERIUM);
    pl.extend_from_slice(&self_restrict.to_le_bytes());
    pl.extend_from_slice(&0u64.to_le_bytes());

    // The request bytes go out BEFORE "confer with the SAK" is printed: a BREAK
    // that reached corvus ahead of the request would find nothing pending.
    if !unsafe { send_request(conn, VERB_IMPERIUM_REQUEST, &pl) } {
        t_putstr("imperium: transport error (request)\n");
        let _ = unsafe { t_close(conn) };
        return 1;
    }
    t_putstr("imperium: requesting ");
    if self_restrict == 0 {
        t_putstr("the full imperium level (CAP_DAC_OVERRIDE CAP_CHOWN CAP_KILL)");
    } else {
        put_cap_names(self_restrict);
    }
    t_putstr(" -- confer with the SAK (Ctrl-A b)\n");

    // (5) Block on the deferred reply.
    let (st, resp) = match unsafe { read_reply(conn) } {
        Some(x) => x,
        None => {
            t_putstr("imperium: transport error (the parked reply was lost)\n");
            let _ = unsafe { t_close(conn) };
            return 1;
        }
    };
    if st != STATUS_OK {
        let _ = unsafe { t_close(conn) };
        return report_denied(st);
    }
    if resp.len() != 12 {
        t_putstr("imperium: malformed OK reply\n");
        let _ = unsafe { t_close(conn) };
        return 1;
    }
    let granted = u64::from_le_bytes([
        resp[4], resp[5], resp[6], resp[7], resp[8], resp[9], resp[10], resp[11],
    ]);
    let _ = unsafe { t_close(conn) };
    if granted == 0 {
        t_putstr("imperium: OK reply granted no caps\n");
        return 1;
    }
    // Defense in depth: never redeem wider than requested (corvus already
    // bounds this, but the tool re-checks its own request).
    if self_restrict != 0 && granted & !self_restrict != 0 {
        t_putstr("imperium: refusing a grant wider than requested\n");
        return 1;
    }

    // (6) Redeem: become a PROPAGATING legate root.
    if cap::use_grant(Caps::from_bits(granted)).is_err() {
        t_putstr("imperium: could not redeem the grant\n");
        return 1;
    }
    t_putstr("imperium: conferred -- ");
    put_cap_names(granted);
    if granted & T_CAP_KILL != 0 {
        t_putstr("  (the axe is drawn)");
    }
    t_putstr("\n");

    // (7) Spawn the elevated sub-shell. fd 0/1/2 + identity inherited (Command
    // defaults); the default cap_mask (!0) lets the imperium caps FLOW through
    // the kernel's rfork carve (IM-2). Its exit -- via `exit` or `abdicate` --
    // returns here; this Proc's death then sweeps the whole scope (I-25).
    let mut cmd = Command::new("/bin/ut");
    cmd.stdin(Stdio::Inherit)
        .stdout(Stdio::Inherit)
        .stderr(Stdio::Inherit);
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(_) => {
            t_putstr("imperium: could not start the elevated shell (/bin/ut)\n");
            return 1;
        }
    };
    let code = match child.wait() {
        Ok(status) => status.code().unwrap_or(0) as i64,
        Err(_) => 0,
    };
    // The sub-shell has exited; leaving imperium (the root) tears the scope down.
    t_putstr("imperium: relinquished\n");
    code
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut self_restrict: u64 = 0;
    for op in env::args().operands() {
        match op {
            b"--help" | b"-h" => {
                usage();
                return 0;
            }
            b"--list" | b"-l" | b"edict" => {
                return cmd_list();
            }
            b"chown" => self_restrict |= T_CAP_CHOWN,
            b"dac" => self_restrict |= T_CAP_DAC_OVERRIDE,
            b"kill" => self_restrict |= T_CAP_KILL,
            _ => {
                t_putstr("imperium: unknown argument\n");
                usage();
                return 2;
            }
        }
    }
    cmd_elevate(self_restrict)
}
