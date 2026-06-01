// /legate-prover (A-4a-3) — the end-to-end legate prover (IDENTITY-DESIGN §9.8,
// invariant I-25). Spawned by joey AFTER the corvus bringup has granted "michael"
// eligibility for the fs-admin clearance level. This binary proves the full
// userspace -> kernel legate chain that the kernel unit tests cannot reach:
//
//   1. connect /srv/corvus + AUTH as michael (fresh: joey closed its session) ->
//      a session token.
//   2. CLEARANCE_LIST -> asserts michael is eligible for fs-admin.
//   3. CLEARANCE_ACTIVATE(fs-admin) -> corvus reads OUR stripes (SYS_SRV_PEER),
//      registers the kernel clearance grant against them (SYS_CAP_GRANT_CLEARANCE,
//      A-4a-3-alpha), returns OK + a nonzero legate_session_id + the granted caps.
//   4. redeem via the cap device `use` (SYS_CAP_USE) -> proc_become_legate stamps
//      the cleared caps + the scope: WE ARE NOW A LEGATE ROOT.
//   5. spawn /legate-child -> it inherits our legate_scope_id (rfork-inherit)
//      WITHOUT the elevation-only caps (rfork-stripped): a live scope member.
//   6. exit clean (return 0) -> the kernel scope teardown (trigger 1, exits())
//      group-terminates the live scope member.
//
// corvus learns our stripes only from OUR connection's peer (SYS_SRV_PEER), which
// is why this E2E belongs here (joey could not register the grant for us). The
// member's actual death is the already-tested #809/#811 path + the test_proc
// teardown-walk unit test; this prover proves the corvus orchestration + the new
// grant syscall + the real redeem + that the integrated exits()-with-a-live-
// scope-member path runs cleanly (a healthy boot is the integration signal).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::vec::Vec;
use libthyla_rs::cap::{self, Caps};
use libthyla_rs::{
    t_putstr, t_read, t_srv_connect, t_write, T_CAP_CHOWN, T_CAP_DAC_OVERRIDE,
};

// Shared test passphrase for "michael" -- MUST match joey's pass_michael (the
// corvus bringup creates michael with it; we AUTH as michael fresh). A fixed
// test credential, like the SYSTEM_PASSPHRASE shared by joey + corvus.
const PASS_MICHAEL: &[u8] = b"correct-horse-battery-staple-v1";

const VERB_AUTH: u8 = 1;
const VERB_CLEARANCE_LIST: u8 = 14;
const VERB_CLEARANCE_ACTIVATE: u8 = 15;
const CORVUS_PROTOCOL_VERSION: u8 = 1;
const TOKEN_LEN: usize = 33;
const LEVEL_FS_ADMIN: &[u8] = b"fs-admin";

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { libthyla_rs::t_exits(1) }
}

// The corvus connection is a byte stream from here: the kernel drives the 9P.
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

// One corvus verb exchange: write [verb, version, len_lo, len_hi, payload]; read
// [status, len_lo, len_hi, payload]. Returns (status, payload) or None on a
// transport error.
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

// CLEARANCE_LIST reply: count u8, then per level {name_len u8, name,
// auth_required u8, time_bound u64 LE, caps_tlv_len u16 LE, caps_tlv}. Scan for a
// level named "fs-admin". Every length is bounds-checked against the buffer.
fn list_has_fs_admin(list: &[u8]) -> bool {
    if list.is_empty() {
        return false;
    }
    let count = list[0] as usize;
    let mut off = 1usize;
    for _ in 0..count {
        if off >= list.len() {
            return false;
        }
        let nl = list[off] as usize;
        off += 1;
        if off + nl > list.len() {
            return false;
        }
        let name = &list[off..off + nl];
        off += nl;
        // auth_required(1) + time_bound(8) + caps_tlv_len(2)
        if off + 1 + 8 + 2 > list.len() {
            return false;
        }
        off += 1 + 8;
        let tlv_len = (list[off] as usize) | ((list[off + 1] as usize) << 8);
        off += 2;
        if off + tlv_len > list.len() {
            return false;
        }
        off += tlv_len;
        if name == LEVEL_FS_ADMIN {
            return true;
        }
    }
    false
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("legate-prover: starting (A-4a-3 legate E2E)\n");

    // corvus is up (joey finished its bringup before spawning us). A short retry
    // covers the accept-queue race.
    let mut conn: i64 = -1;
    for _ in 0..64 {
        conn = unsafe { t_srv_connect(b"corvus".as_ptr(), 6, b"ctl".as_ptr(), 3) };
        if conn >= 0 {
            break;
        }
    }
    if conn < 0 {
        fail("legate-prover: FAIL connect /srv/corvus\n");
    }

    // AUTH as michael -> a session token.
    let mut auth_pl: Vec<u8> = Vec::new();
    auth_pl.push(7);
    auth_pl.extend_from_slice(b"michael");
    auth_pl.push((PASS_MICHAEL.len() & 0xff) as u8);
    auth_pl.push(((PASS_MICHAEL.len() >> 8) & 0xff) as u8);
    auth_pl.extend_from_slice(PASS_MICHAEL);
    let (st, resp) = match unsafe { exchange(conn, VERB_AUTH, &auth_pl) } {
        Some(x) => x,
        None => fail("legate-prover: FAIL AUTH transport\n"),
    };
    if st != 0 || resp.len() != TOKEN_LEN {
        fail("legate-prover: FAIL AUTH (bad status/token)\n");
    }
    let mut token = [0u8; TOKEN_LEN];
    token.copy_from_slice(&resp);

    // CLEARANCE_LIST -> assert fs-admin eligibility.
    let (st, list) = match unsafe { exchange(conn, VERB_CLEARANCE_LIST, &token) } {
        Some(x) => x,
        None => fail("legate-prover: FAIL CLEARANCE_LIST transport\n"),
    };
    if st != 0 {
        fail("legate-prover: FAIL CLEARANCE_LIST (status)\n");
    }
    if !list_has_fs_admin(&list) {
        fail("legate-prover: FAIL not eligible for fs-admin\n");
    }
    t_putstr("legate-prover: CLEARANCE_LIST shows fs-admin eligibility\n");

    // CLEARANCE_ACTIVATE fs-admin: self_restrict=0 (full level set),
    // valid_until_req=0 (level default = unbounded).
    let mut act_pl: Vec<u8> = Vec::new();
    act_pl.extend_from_slice(&token);
    act_pl.push(LEVEL_FS_ADMIN.len() as u8);
    act_pl.extend_from_slice(LEVEL_FS_ADMIN);
    act_pl.extend_from_slice(&0u64.to_le_bytes()); // self_restrict
    act_pl.extend_from_slice(&0u64.to_le_bytes()); // valid_until_req
    let (st, ar) = match unsafe { exchange(conn, VERB_CLEARANCE_ACTIVATE, &act_pl) } {
        Some(x) => x,
        None => fail("legate-prover: FAIL CLEARANCE_ACTIVATE transport\n"),
    };
    if st != 0 || ar.len() != 12 {
        fail("legate-prover: FAIL CLEARANCE_ACTIVATE (status/len)\n");
    }
    let session_id = u32::from_le_bytes([ar[0], ar[1], ar[2], ar[3]]);
    let granted = u64::from_le_bytes([ar[4], ar[5], ar[6], ar[7], ar[8], ar[9], ar[10], ar[11]]);
    if session_id == 0 {
        fail("legate-prover: FAIL legate_session_id == 0\n");
    }
    if granted != (T_CAP_DAC_OVERRIDE | T_CAP_CHOWN) {
        fail("legate-prover: FAIL granted caps != fs-admin set\n");
    }
    t_putstr("legate-prover: CLEARANCE_ACTIVATE ok (fs-admin granted)\n");

    // Redeem the grant for our own stripes -> we become a legate root.
    if cap::use_grant(Caps::from_bits(granted)).is_err() {
        fail("legate-prover: FAIL cap use_grant (redeem)\n");
    }
    t_putstr("legate-prover: redeemed clearance -- now a legate root\n");

    // We are now a legate root. At v1.0 a legate does the privileged work ITSELF
    // (102-legate.md: "the v1.0 use case is a privileged agent that HOLDS the
    // clearance," NOT an elevated shell forking commands), so this legate has NO
    // scope members. Exiting still runs the kernel exits() legate-root teardown
    // path (PROC_FLAG_LEGATE_ROOT -> the scope walk, which finds zero members and
    // the root exits cleanly).
    //
    // A scope-MEMBER teardown end-to-end is deferred to v1.x: it needs BOTH
    // fork-grantable clearance caps (today all clearance caps are elevation-only
    // -> rfork-stripped, so a child is unelevated) AND a general kproc
    // orphan-reaper (today a torn-down member, orphaned by the legate root's exit,
    // reparents to the strict-wait_pid kproc and extincts -- the documented v1.x
    // `wait_pid_for(pid)` lift; see usr/joey/joey.c + the A-4a-3 commit). The
    // member walk is unit-covered by test_proc::legate_scope_teardown; the
    // member death path is the #809/#811 universal death-interruptible sleep.
    t_putstr(
        "legate-prover: legate E2E OK (fs-admin activated+redeemed; now a legate root; exiting)\n",
    );
    0
}
