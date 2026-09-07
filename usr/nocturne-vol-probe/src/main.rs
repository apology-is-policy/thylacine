// /bin/nocturne-vol-probe -- the N-3a-3 sink-volume authority witness
// (docs/NOCTURNE.md 6.4/6.8, I-46). N-3a-2 shipped this with an F1 bypass: the
// gate keyed on the CONNECTION's peer, but a write through joey's shared
// /dev/nocturne mount carries the mounter (SYSTEM), so any user could change
// system audio -- and the old witness only tested a direct connection, where
// the gate works, so it gave false assurance. N-3a-3 splits the tree: playback
// stays the mount, sink AUTHORITY moves to the per-connection /srv/nocturne-ctl
// post. This witness proves the split BOTH ways, and the arm that matters most
// is the one the old witness lacked -- a USER writing volume THROUGH the mount
// must be REFUSED.
//
//   PARENT (SYSTEM):
//     - a direct /srv/nocturne-ctl write is ACCEPTED and the Plan 9 volume(3)
//       grammar round-trips (audio/mix, one value or L R, mute, unknown ->
//       EINVAL). The SYSTEM axis + the file.
//     - /dev/nocturne/volume READS through the mount (cat works; read is public).
//
//   DENY child (a USER principal; argv "deny") -- BOTH must be refused:
//     - the F1 attack: writing volume THROUGH the /dev/nocturne mount is REFUSED
//       (the mounted node is read-only 0o444, so the kernel dev9p gate denies the
//       write-open for a non-owner; and h_write refuses a non-control conn even
//       if the open slips through). This is the regression the old witness never
//       had.
//     - a direct /srv/nocturne-ctl write is REFUSED (EPERM) -- not SYSTEM, no
//       CAP_AUDIO_GRAPH, not the console-owner session.
//
// The CAP_AUDIO_GRAPH-admit axis is covered by test_devcap.clearance_audio_graph
// (grant->redeem->cap) and the console-owner axis by the kernel
// proc_identity.peer_snapshot_console_owner test; a boot probe is neither a
// clearance holder nor the console-owner session, so it witnesses the SYSTEM
// allow + the two user denials here.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::io::Write;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};

// The sink-authority post (per-connection; the gate reads THIS program as peer).
const NOC_CTL: &[u8] = b"/srv/nocturne-ctl";
// The playback post -- a DIRECT connect here (control=false) must still refuse a
// volume write (the F2 arm: the `!control` guard, not a mount mode gate).
const NOC_PLAY: &[u8] = b"/srv/nocturne";
// The playback mount's volume node (read-only info; writes here are the F1 path).
const MOUNT_VOL: &[u8] = b"/dev/nocturne/volume";
const SELF_BIN: &str = "/bin/nocturne-vol-probe";
const DENY_PRINCIPAL: u32 = 1;

/// Write to BOTH the kernel console and fd 1 (joey's pouch_smoke checks fd 1).
fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("NOCTURNE-VOL-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
}

/// Connect DIRECTLY to /srv/nocturne-ctl -- a fresh per-client server conn, so
/// nocturned resolves the peer as US, not the /dev/nocturne mounter. <0 on fail.
fn connect_ctl() -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, NOC_CTL.as_ptr(), NOC_CTL.len(), T_OREAD) }
}

/// Write `msg` to the volume file on `root`; returns t_write's result (>=0 count
/// on success, <0 errno on refusal). A fresh volume fd each call.
fn write_volume(root: i64, msg: &[u8]) -> i64 {
    let vol = unsafe { t_open(root, b"volume".as_ptr(), 6, T_OWRITE) };
    if vol < 0 {
        return vol;
    }
    let n = unsafe { t_write(vol, msg.as_ptr(), msg.len()) };
    unsafe { t_close(vol) };
    n
}

/// True iff the volume file on `root` renders text containing `needle`.
fn volume_has(root: i64, needle: &[u8]) -> bool {
    let vol = unsafe { t_open(root, b"volume".as_ptr(), 6, T_OREAD) };
    if vol < 0 {
        return false;
    }
    let mut buf = [0u8; 128];
    let n = unsafe { t_read(vol, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(vol) };
    if n <= 0 {
        return false;
    }
    let got = &buf[..n as usize];
    !needle.is_empty() && got.windows(needle.len()).any(|w| w == needle)
}

/// The F1 attack: try to WRITE the sink volume through the shared /dev/nocturne
/// mount. Returns true iff the write was REFUSED at every step (open denied, or
/// open-then-write refused) -- i.e. no byte of a volume change rode the mount's
/// SYSTEM identity. False iff a write was ACCEPTED (F1 open).
fn mount_write_refused() -> bool {
    let m = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, MOUNT_VOL.as_ptr(), MOUNT_VOL.len(), T_OWRITE) };
    if m < 0 {
        // Denied at open: the mounted node is read-only (0o444), so the kernel
        // dev9p rwx gate refuses a non-owner write-open. F1 closed at the door.
        return true;
    }
    // Open slipped through (e.g. an owner/root bypass of the mode): the write
    // itself must still be refused by h_write's non-control-conn guard.
    let n = unsafe { t_write(m, b"audio 0\n".as_ptr(), 8) };
    unsafe { t_close(m) };
    n < 0
}

/// The DENY child (a user principal): BOTH the mount write path and a direct
/// control-post write MUST be refused. Exit 0 iff both were refused.
fn deny_child() -> i64 {
    if !mount_write_refused() {
        say("NOCTURNE-VOL-DENY FAIL: user wrote volume THROUGH the mount -- F1 open\n");
        return 1;
    }
    let root = connect_ctl();
    if root < 0 {
        say("NOCTURNE-VOL-DENY FAIL: child could not connect /srv/nocturne-ctl\n");
        return 3;
    }
    let n = write_volume(root, b"audio 50\n");
    unsafe { t_close(root) };
    if n >= 0 {
        say("NOCTURNE-VOL-DENY FAIL: user control-post write ACCEPTED -- gate bypassed\n");
        return 1;
    }
    say("NOCTURNE-VOL-DENY OK: user mount write + control-post write both refused\n");
    0
}

fn parent() -> i64 {
    let root = connect_ctl();
    if root < 0 {
        return fail("connect /srv/nocturne-ctl");
    }
    // Default is unity on both controls.
    if !volume_has(root, b"audio 100 100") || !volume_has(root, b"mix 100 100") {
        return fail("default volume was not 'audio 100 100' + 'mix 100 100'");
    }
    // Positive gate (SYSTEM axis) + the grammar parse/render round-trip.
    if write_volume(root, b"audio 50\n") < 0 {
        return fail("SYSTEM 'audio 50' write was refused");
    }
    if !volume_has(root, b"audio 50 50") {
        return fail("read-back after 'audio 50' was not 'audio 50 50'");
    }
    if write_volume(root, b"audio 70 90\n") < 0 || !volume_has(root, b"audio 70 90") {
        return fail("'audio 70 90' (per-channel) round-trip");
    }
    if write_volume(root, b"mix 40\n") < 0 || !volume_has(root, b"mix 40 40") {
        return fail("'mix 40' round-trip");
    }
    // An unknown control is refused (EINVAL), leaving the gain unchanged (F3:
    // no partial apply -- 'audio' would have set 50 if the two-pass leaked).
    if write_volume(root, b"bogus 1\n") >= 0 {
        return fail("invalid control 'bogus 1' was ACCEPTED");
    }
    // F3: a two-line write whose SECOND line is invalid must change NOTHING.
    if write_volume(root, b"audio 33\nmix bad\n") >= 0 {
        return fail("a write with a bad 2nd line was ACCEPTED");
    }
    if !volume_has(root, b"audio 70 90") {
        return fail("F3: a rejected multi-line write changed the gain (partial apply)");
    }
    // Mute, then restore unity.
    if write_volume(root, b"audio 0\n") < 0 || !volume_has(root, b"audio 0 0") {
        return fail("'audio 0' (mute) round-trip");
    }
    if write_volume(root, b"audio 100\n") < 0 || write_volume(root, b"mix 100\n") < 0 {
        return fail("reset to unity");
    }
    unsafe { t_close(root) };

    // The mount's volume READS (cat works; read is public, no authority).
    let mv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, MOUNT_VOL.as_ptr(), MOUNT_VOL.len(), T_OREAD) };
    if mv < 0 {
        return fail("/dev/nocturne/volume not readable through the mount");
    }
    let mut mbuf = [0u8; 64];
    let mn = unsafe { t_read(mv, mbuf.as_mut_ptr(), mbuf.len()) };
    unsafe { t_close(mv) };
    if mn <= 0 || !mbuf[..mn as usize].windows(5).any(|w| w == b"audio") {
        return fail("mount volume read did not render the level");
    }

    // F2 (round-6 audit): the `!control` guard is the SOLE F1 closer on a DIRECT
    // playback connection. The mount arm is closed by the 0o444 mode gate, but a
    // raw SrvConn to /srv/nocturne has no kernel mode gate, so only h_write's
    // non-control refusal stands there. Prove it: even THIS process (SYSTEM)
    // writing volume on a direct playback conn is REFUSED, because the playback
    // post never carries write authority regardless of the peer. A reorder that
    // checked authority before `!control` would reopen F1 here and fail this arm.
    let pb = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, NOC_PLAY.as_ptr(), NOC_PLAY.len(), T_OREAD) };
    if pb < 0 {
        return fail("connect /srv/nocturne (playback) direct");
    }
    let pn = write_volume(pb, b"audio 0\n");
    unsafe { t_close(pb) };
    if pn >= 0 {
        return fail("F2: a direct playback-post volume write was ACCEPTED (the !control guard)");
    }

    // Negative gate: a user-principal child -- BOTH the mount write path and the
    // direct control-post write MUST be refused. The parent holds
    // CAP_SET_IDENTITY (joey's spawn mask) to stamp the child's principal; the
    // child inherits this namespace (so it sees the mount + /srv) but not SYSTEM,
    // not the console-owner session, and no clearance. stderr -> a fresh pipe
    // (joey wires only fd 0+1; inheriting the missing fd 2 would fail the spawn).
    match Command::new(SELF_BIN)
        .arg("deny")
        .identity(DENY_PRINCIPAL, DENY_PRINCIPAL, &[])
        .caps(0)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(mut child) => match child.wait() {
            Ok(st) if st.success() => {}
            Ok(_) => return fail("deny child reported the gate was bypassed"),
            Err(_) => return fail("deny child wait() failed"),
        },
        Err(_) => return fail("could not spawn the deny child (missing CAP_SET_IDENTITY?)"),
    }

    say("NOCTURNE-VOL-PROBE PASS (control-post SYSTEM allow + grammar/F3 + mount read; user mount+control deny)\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let deny = env::args().operands().next().map_or(false, |a| a == b"deny");
    if deny {
        deny_child()
    } else {
        parent()
    }
}
