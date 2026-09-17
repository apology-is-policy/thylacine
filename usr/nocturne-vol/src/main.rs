// /bin/nocturne-vol -- the shell tool for the system sink volume (NOCTURNE.md
// 6.4/6.8, N-3a-3). The sink-authority surface is NOT the shared /dev/nocturne
// mount (a mount is one connection and carries the mounter's SYSTEM identity --
// the F1 bypass); it is the per-connection /srv/nocturne-ctl post, where the
// server sees THIS program as the peer. So volume WRITES ride a direct
// connection here, and the 6.8 gate judges the caller: PRINCIPAL_SYSTEM, or
// CAP_HOSTOWNER, or the audio-graph clearance, or the console-owner session
// (the person at the keyboard). Reading the level is ungated (it is public
// info, also `cat /dev/nocturne/volume`).
//
//   nocturne-vol                 -- print the current audio/mix levels
//   nocturne-vol audio 50        -- set both channels of `audio` to 50
//   nocturne-vol audio 70 90     -- set audio L=70 R=90
//   nocturne-vol mix 40          -- set the master `mix`
//
// The operands are passed as one Plan 9 volume(3) line; the server validates
// (0..100, known control) and applies it atomically.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::io::Write;
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};

const NOC_CTL: &[u8] = b"/srv/nocturne-ctl";

/// Write to fd 1 (the shell captures this) and the console, so the tool is
/// legible whether run from a pts tile or the bare UART.
fn say(s: &str) {
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
    let _ = t_putstr(s);
}

/// Connect to the sink-authority post: a fresh per-client server connection, so
/// nocturned resolves US as the peer (not a mount's SYSTEM owner).
fn connect() -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, NOC_CTL.as_ptr(), NOC_CTL.len(), T_OREAD) }
}

/// Read + print the current volume from `root`.
fn show(root: i64) -> i64 {
    let vol = unsafe { t_open(root, b"volume".as_ptr(), 6, T_OREAD) };
    if vol < 0 {
        say("nocturne-vol: cannot open volume\n");
        return 1;
    }
    let mut buf = [0u8; 128];
    let n = unsafe { t_read(vol, buf.as_mut_ptr(), buf.len()) };
    let _ = unsafe { t_close(vol) };
    if n <= 0 {
        say("nocturne-vol: empty volume read\n");
        return 1;
    }
    if let Ok(s) = core::str::from_utf8(&buf[..n as usize]) {
        say(s);
    }
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let root = connect();
    if root < 0 {
        say("nocturne-vol: /srv/nocturne-ctl unreachable (no audio device?)\n");
        return 1;
    }

    // No operands: just report the current level.
    let mut ops = env::args().operands();
    let first = match ops.next() {
        Some(a) => a,
        None => {
            let rc = show(root);
            let _ = unsafe { t_close(root) };
            return rc;
        }
    };

    // Build the volume(3) line from the operands: "<control> <v>...".
    let mut line: Vec<u8> = Vec::new();
    line.extend_from_slice(first);
    for a in ops {
        line.push(b' ');
        line.extend_from_slice(a);
    }
    line.push(b'\n');

    let vol = unsafe { t_open(root, b"volume".as_ptr(), 6, T_OWRITE) };
    if vol < 0 {
        // The mode admits the open; a rejection here is the walk, not the gate.
        say("nocturne-vol: cannot open volume for write\n");
        let _ = unsafe { t_close(root) };
        return 1;
    }
    let n = unsafe { t_write(vol, line.as_ptr(), line.len()) };
    let _ = unsafe { t_close(vol) };

    if n < 0 {
        // The 6.8 gate refused (EPERM) or the grammar was invalid (EINVAL); the
        // errno is not surfaced through the byte fd, so name both possibilities.
        let mut msg = String::from(
            "nocturne-vol: refused -- need the audio-graph clearance or the console session, ",
        );
        msg.push_str("or the request was not `audio|mix <0..100> [<r>]`\n");
        say(&msg);
        let _ = unsafe { t_close(root) };
        return 1;
    }

    // Confirm by reading the applied level back.
    let rc = show(root);
    let _ = unsafe { t_close(root) };
    rc
}
