// /sbin/corvus — Thylacine key-agent daemon.
//
// Sub-chunks (per CORVUS-DESIGN.md §10):
//   P5-corvus-bringup-a (`7487054`):   startup hardening + readiness banner.
//   P5-corvus-bringup-b (this chunk):  /srv/corvus/ Spoor server skeleton +
//                                      binary frame wire codec + AUTH +
//                                      SESSION_CLOSE verbs.
//   P5-corvus-bringup-c (next):        Argon2id backend integration + state
//                                      file format (magic CRVS).
//   P5-corvus-bringup-d onward:        UNWRAP + admin verbs + audit log.
//
// At this sub-chunk corvus is a single-peer userspace daemon that:
//
//   1. Runs the hardening sequence at startup (unchanged from -a).
//
//   2. Enters a server loop reading binary frames from fd 0 (rx) and
//      writing response frames to fd 1 (tx). The pipe pair is installed
//      by joey via SYS_SPAWN_FULL — corvus inherits fd 0 = c2s_rd, fd 1
//      = s2c_wr. The peer Proc (joey itself at this skeleton; per-user
//      stratumd processes in production) writes verb frames + reads
//      response frames over the same pipe pair.
//
//   3. Dispatches verb_id ∈ { AUTH, SESSION_CLOSE } per the binary frame
//      format in CORVUS-DESIGN.md §6.4. Every other verb_id is refused
//      with status=BadFormat (5).
//
//   4. AUTH skeleton: parses (user_len, user, pass_len, passphrase); at
//      this sub-chunk the passphrase is NOT verified (Argon2id backend
//      lands at -c); a non-empty passphrase always succeeds. Generates
//      a 33-byte session token ("s" + 16 bytes of CSPRNG entropy
//      hex-encoded to 32 chars). Stores the session record. Returns
//      status=OK + token.
//
//   5. SESSION_CLOSE: parses 33-byte token. If it matches the active
//      session, clears the slot + returns status=OK. Otherwise
//      status=NotFound.
//
//   6. Exits 0 on rx EOF (peer closed its write side).
//
// Single-slot session table at this skeleton (one peer). Multi-slot
// expansion (multiple per-user stratumd peers + Proc identity binding)
// lands when the kernel exposes peer Proc identity on Spoors (per
// CORVUS-DESIGN §6.2). Until then, the binding is "the Proc on the
// other end of fd 0", and at v1.0 spawn-time discipline pins the peer
// identity (only joey-or-its-delegates can talk to corvus).
//
// Spec correspondence (specs/corvus.tla; P5-corvus-spec at c00de63):
//
//   AuthSuccess(p, u)     — handle_auth() insert path.
//   SessionClose(p)       — handle_session_close() clear path.
//   SessionUserImmutable  — Session struct's bound_user is set once
//                           at install() + never written again until
//                           clear() (which zeroes the entire record);
//                           there is no setter for bound_user.
//   AdminElevate(p)       — NOT YET (deferred to a later sub-chunk).
//   Unwrap(p, d)          — NOT YET (UNWRAP verb deferred).
//   AdminVerb(p)          — NOT YET.

#![no_std]
#![no_main]

use libthyla_rs::{
    t_close, t_explicit_bzero, t_getrandom, t_mlockall, t_putstr, t_read, t_set_dumpable,
    t_set_traceable, t_write,
};

// =============================================================================
// Wire constants — CORVUS-DESIGN.md §6.4.
// =============================================================================

const VERB_AUTH: u8 = 1;
const VERB_SESSION_CLOSE: u8 = 3;

const STATUS_OK: u8 = 0;
#[allow(dead_code)]
const STATUS_BAD_AUTH: u8 = 1;
const STATUS_PERMISSION_DENIED: u8 = 2;
const STATUS_NOT_FOUND: u8 = 3;
#[allow(dead_code)]
const STATUS_RATE_LIMITED: u8 = 4;
const STATUS_BAD_FORMAT: u8 = 5;
const STATUS_INTERNAL_ERROR: u8 = 6;

// 33 bytes = 's' + 32 hex chars (16 bytes of CSPRNG entropy hex-encoded).
const TOKEN_LEN: usize = 33;
const TOKEN_ENTROPY_BYTES: usize = 16;

const MAX_USER_LEN: usize = 32;
const MAX_PASS_LEN: usize = 256;

// Worst case: AUTH = 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN = 291.
// SESSION_CLOSE = TOKEN_LEN = 33. Pick the larger.
const MAX_PAYLOAD_LEN: usize = 1 + MAX_USER_LEN + 2 + MAX_PASS_LEN;
// Response: largest payload is the session token (33). Frame size = 3 + 33.
const MAX_RESPONSE_FRAME: usize = 3 + TOKEN_LEN;

const RX_FD: i64 = 0;
const TX_FD: i64 = 1;

// =============================================================================
// Session table — single-slot at this skeleton.
// =============================================================================
//
// Spec (specs/corvus.tla) models Sessions as SUBSET SessionRecord with at
// most one record per owner_proc. This skeleton has one peer, so a
// single static slot suffices. Multi-slot expansion lands when corvus
// serves multiple peer Procs (per-user stratumd processes) over distinct
// Spoor pairs.
//
// Spec's identity model is (creation_proc, bound_user). At this skeleton
// we don't yet have peer Proc identity exposed on the pipe (the
// kernel-stamped /srv/corvus/peer/ surface lands at -c or later), so the
// stored creation_proc is logically "the only peer" — implicit. We
// store bound_user (the username string from AUTH) and the session
// token as the identifying state.

#[repr(C)]
struct Session {
    active: bool,
    user_len: u8,
    user: [u8; MAX_USER_LEN],
    token: [u8; TOKEN_LEN],
}

static mut SESSION: Session = Session {
    active: false,
    user_len: 0,
    user: [0; MAX_USER_LEN],
    token: [0; TOKEN_LEN],
};

// Accessor wrappers go through raw pointers + element-by-element writes
// so we don't take a &mut reference to the static (Rust 1.77+'s
// static_mut_refs lint fires on `&mut SESSION` patterns). This keeps the
// code lint-clean and Miri-honest in case the kernel ever multiplexes
// corvus's server loop in the future.

unsafe fn session_active() -> bool {
    core::ptr::read(core::ptr::addr_of!(SESSION.active))
}

unsafe fn session_install(user: &[u8], token: &[u8; TOKEN_LEN]) {
    let s = core::ptr::addr_of_mut!(SESSION);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    // Wipe + install user
    for i in 0..MAX_USER_LEN {
        core::ptr::write(user_ptr.add(i), 0);
    }
    for i in 0..user.len() {
        core::ptr::write(user_ptr.add(i), user[i]);
    }
    // Install token
    for i in 0..TOKEN_LEN {
        core::ptr::write(token_ptr.add(i), token[i]);
    }
    // user_len + active LAST so partial reads can never see a half-set state
    core::ptr::write(core::ptr::addr_of_mut!((*s).user_len), user.len() as u8);
    core::ptr::write(core::ptr::addr_of_mut!((*s).active), true);
}

unsafe fn session_token_matches(candidate: &[u8]) -> bool {
    if candidate.len() != TOKEN_LEN {
        return false;
    }
    if !session_active() {
        return false;
    }
    let token_ptr = core::ptr::addr_of!(SESSION.token) as *const u8;
    // Constant-time compare: never short-circuit on mismatch. (At this
    // skeleton the token is fresh CSPRNG entropy; not strictly required,
    // but the discipline carries forward to future secret-equality
    // checks in -c onward.)
    let mut diff: u8 = 0;
    for i in 0..TOKEN_LEN {
        let tok_byte = core::ptr::read(token_ptr.add(i));
        diff |= tok_byte ^ candidate[i];
    }
    diff == 0
}

unsafe fn session_clear() {
    let s = core::ptr::addr_of_mut!(SESSION);
    // Clear active FIRST so a concurrent reader (none at v1.0; future-
    // proof) can't observe a stale token bound to a cleared session.
    core::ptr::write(core::ptr::addr_of_mut!((*s).active), false);
    let user_ptr = core::ptr::addr_of_mut!((*s).user) as *mut u8;
    let token_ptr = core::ptr::addr_of_mut!((*s).token) as *mut u8;
    for i in 0..MAX_USER_LEN {
        core::ptr::write(user_ptr.add(i), 0);
    }
    for i in 0..TOKEN_LEN {
        core::ptr::write(token_ptr.add(i), 0);
    }
    core::ptr::write(core::ptr::addr_of_mut!((*s).user_len), 0);
}

// =============================================================================
// Hex encoding.
// =============================================================================

fn nibble_to_hex(n: u8) -> u8 {
    let n = n & 0x0f;
    if n < 10 {
        b'0' + n
    } else {
        b'a' + (n - 10)
    }
}

// =============================================================================
// Frame I/O.
// =============================================================================

// read_exact — loop t_read until `buf.len()` bytes received OR EOF/error.
// Returns the count read on success (== buf.len()), 0 on clean EOF at
// frame boundary, negative on error or short read across EOF.
unsafe fn read_exact(fd: i64, buf: &mut [u8]) -> i64 {
    let mut got: usize = 0;
    while got < buf.len() {
        let n = t_read(fd, buf.as_mut_ptr().add(got), buf.len() - got);
        if n == 0 {
            // EOF. If we've read nothing, signal clean EOF; else short
            // read at frame boundary — protocol violation.
            return if got == 0 { 0 } else { -1 };
        }
        if n < 0 {
            return -1;
        }
        got += n as usize;
    }
    got as i64
}

// write_all — loop t_write until all bytes drained.
unsafe fn write_all(fd: i64, buf: &[u8]) -> i64 {
    let mut sent: usize = 0;
    while sent < buf.len() {
        let n = t_write(fd, buf.as_ptr().add(sent), buf.len() - sent);
        if n <= 0 {
            return -1;
        }
        sent += n as usize;
    }
    sent as i64
}

// send_response — encode + write a response frame. Returns 0 on
// success, -1 on transport error.
unsafe fn send_response(fd: i64, status: u8, payload: &[u8]) -> i64 {
    if payload.len() > 0xFFFF {
        return -1;
    }
    let mut frame = [0u8; MAX_RESPONSE_FRAME];
    if 3 + payload.len() > frame.len() {
        return -1;
    }
    frame[0] = status;
    let len_lo = (payload.len() & 0xFF) as u8;
    let len_hi = ((payload.len() >> 8) & 0xFF) as u8;
    frame[1] = len_lo;
    frame[2] = len_hi;
    for i in 0..payload.len() {
        frame[3 + i] = payload[i];
    }
    if write_all(fd, &frame[..3 + payload.len()]) < 0 {
        return -1;
    }
    0
}

// =============================================================================
// Verb handlers.
// =============================================================================

// handle_auth — parse AUTH payload + mint session token.
//
// Payload format:
//   [0]            user_len u8 (1..=MAX_USER_LEN)
//   [1..1+ul]      user
//   [1+ul..3+ul]   pass_len u16 LE (1..=MAX_PASS_LEN)
//   [3+ul..]       passphrase
//
// At this skeleton: passphrase is NOT verified (no Argon2id backend
// yet); presence + valid framing = success. The user string is the
// session's bound_user (per spec) — distinct users get distinct
// sessions. One-session-per-peer enforced via the active flag.
unsafe fn handle_auth(payload: &[u8]) -> i64 {
    if payload.len() < 3 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let user_len = payload[0] as usize;
    if user_len == 0 || user_len > MAX_USER_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() < 1 + user_len + 2 {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    let user = &payload[1..1 + user_len];
    let pass_len = (payload[1 + user_len] as usize) | ((payload[2 + user_len] as usize) << 8);
    if pass_len == 0 || pass_len > MAX_PASS_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if payload.len() != 1 + user_len + 2 + pass_len {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }

    // Spec's one-session-per-Proc precondition (AuthSuccess's
    // `~(\E s : s.owner_proc = p)`). At this skeleton "the peer" is
    // implicit; an active session blocks AUTH.
    if session_active() {
        return send_response(TX_FD, STATUS_PERMISSION_DENIED, &[]);
    }

    // Generate 16 bytes of entropy via CSPRNG → 32-char hex → prepend 's'.
    let mut entropy = [0u8; TOKEN_ENTROPY_BYTES];
    let rc = t_getrandom(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES, 0);
    if rc != TOKEN_ENTROPY_BYTES as i64 {
        return send_response(TX_FD, STATUS_INTERNAL_ERROR, &[]);
    }
    let mut token = [0u8; TOKEN_LEN];
    token[0] = b's';
    for i in 0..TOKEN_ENTROPY_BYTES {
        token[1 + 2 * i] = nibble_to_hex(entropy[i] >> 4);
        token[1 + 2 * i + 1] = nibble_to_hex(entropy[i]);
    }
    // Wipe the raw entropy buffer — the hex form lives in `token` and
    // the session table; the raw bytes have no further use.
    let _ = t_explicit_bzero(entropy.as_mut_ptr(), TOKEN_ENTROPY_BYTES);

    session_install(user, &token);

    // Per CORVUS-DESIGN.md §6.4: the AUTH success payload is the
    // session token (33 bytes).
    send_response(TX_FD, STATUS_OK, &token)
}

// handle_session_close — verify token + clear session.
unsafe fn handle_session_close(payload: &[u8]) -> i64 {
    if payload.len() != TOKEN_LEN {
        return send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
    }
    if !session_token_matches(payload) {
        return send_response(TX_FD, STATUS_NOT_FOUND, &[]);
    }
    session_clear();
    send_response(TX_FD, STATUS_OK, &[])
}

// =============================================================================
// Server loop.
// =============================================================================

unsafe fn server_loop() -> i64 {
    let mut header = [0u8; 3];
    let mut payload = [0u8; MAX_PAYLOAD_LEN];
    loop {
        let n = read_exact(RX_FD, &mut header);
        if n == 0 {
            // Clean EOF — peer closed write side.
            return 0;
        }
        if n < 0 {
            return -1;
        }
        let verb_id = header[0];
        let payload_len = (header[1] as usize) | ((header[2] as usize) << 8);
        if payload_len > MAX_PAYLOAD_LEN {
            // Frame too large; emit BAD_FORMAT response, terminate (the
            // protocol's framed; we can't safely drain a too-large
            // payload without knowing it's bytes count, which we already
            // refused).
            let _ = send_response(TX_FD, STATUS_BAD_FORMAT, &[]);
            return -1;
        }
        if payload_len > 0 {
            let n = read_exact(RX_FD, &mut payload[..payload_len]);
            if n != payload_len as i64 {
                return -1;
            }
        }
        let rc = match verb_id {
            VERB_AUTH => handle_auth(&payload[..payload_len]),
            VERB_SESSION_CLOSE => handle_session_close(&payload[..payload_len]),
            _ => send_response(TX_FD, STATUS_BAD_FORMAT, &[]),
        };
        if rc < 0 {
            return -1;
        }
        // Wipe payload before next iteration (defence-in-depth: future
        // verbs may carry secrets like passphrases or wrapped DEKs).
        let _ = t_explicit_bzero(payload.as_mut_ptr(), payload_len);
    }
}

// =============================================================================
// Startup hardening (unchanged from -a).
// =============================================================================

const PROBE_LEN: usize = 32;

#[cold]
#[inline(never)]
fn step_fail(step: u8, rc: i64) -> ! {
    t_putstr("corvus: STEP=");
    let digit = b'0' + step;
    let buf = [digit, 0];
    let _ = t_putstr(unsafe { core::str::from_utf8_unchecked(&buf[..1]) });
    t_putstr(" FAIL rc=");
    let nibble = (rc as u8) & 0x0f;
    let hex_char = if nibble < 10 {
        b'0' + nibble
    } else {
        b'a' + (nibble - 10)
    };
    let hex_buf = [hex_char, 0];
    let _ = t_putstr(unsafe { core::str::from_utf8_unchecked(&hex_buf[..1]) });
    t_putstr("\n");
    unsafe { libthyla_rs::t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("corvus: skeleton starting (P5-corvus-bringup-b)\n");

    let rc = unsafe { t_mlockall(0) };
    if rc != 0 {
        step_fail(1, rc);
    }
    let rc = unsafe { t_set_dumpable(0) };
    if rc != 0 {
        step_fail(2, rc);
    }
    let rc = unsafe { t_set_traceable(0) };
    if rc != 0 {
        step_fail(3, rc);
    }
    let mut probe: [u8; PROBE_LEN] = [0; PROBE_LEN];
    let rc = unsafe { t_getrandom(probe.as_mut_ptr(), PROBE_LEN, 0) };
    if rc != PROBE_LEN as i64 {
        step_fail(4, rc);
    }
    let rc = unsafe { t_explicit_bzero(probe.as_mut_ptr(), PROBE_LEN) };
    if rc != 0 {
        step_fail(5, rc);
    }

    t_putstr("corvus: ready (hardening applied; serving /srv/corvus/ over fd 0/1)\n");

    // Enter server loop. Exits 0 on clean EOF; non-zero on any wire
    // error. The boot test framework reaps corvus's exit via joey's
    // wait_pid and surfaces non-zero.
    let rc = unsafe { server_loop() };
    if rc < 0 {
        t_putstr("corvus: server_loop FAILED\n");
        return 1;
    }

    // Wipe any residual session state before exiting. The
    // explicit_bzero discipline carries forward to every shutdown path.
    unsafe { session_clear() };

    // Close our pipe fds explicitly. The kernel will release them on
    // exit anyway, but the explicit close exercises the cleanup path.
    let _ = unsafe { t_close(RX_FD) };
    let _ = unsafe { t_close(TX_FD) };

    t_putstr("corvus: server_loop returned EOF; shutting down clean\n");
    0
}
