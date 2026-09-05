// /bin/kaua-term -- the Halcyon per-tile terminal process (KT-1).
//
// halcyond spawns one of these per tile and wires two channels to it via the
// standard stdio slots (no extra-fd inheritance needed):
//   fd 0  = the DOWN channel: framed Input records (Key / Resize) from halcyond.
//   fd 1  = the UP channel:   framed cell records (CellDiff/ScrollOff/Control/
//                             Mode) to halcyond, which it reaps on its per-tile
//                             Loom ring (H-3c-2 pattern; the ring is halcyond's
//                             side, KT-1.5). fd 2 stays stderr for diagnostics.
// The pts is INTERNAL: the kaua-term mints it (ptyhold), spawns the app (default
// /bin/ut) on the slave as fd 0/1/2, and holds the master. The app never sees
// the halcyond channels -- spawn installs only the three slave slots (the same
// non-inheritance ptyhost relies on for drain-then-EOF).
// The pts ADVERTISES its host's render tier (KAUA-TERM.md R1; H-4d): `--beacon
// <none|cells|rich>` is written to this process's /env/BEACON before the spawn,
// so the app inherits it and (with its stdout answering 't' to SYS_FD_DEVCLASS)
// emits the markup its host renders. Absent = none, fail-closed: a host that
// declared nothing renders no frames, so the app must not emit them.
//
// Two blocking threads, like ptyhost, because the pts master is non-QTPOLL:
//   - OUTPUT (this thread): master -> the vt parser -> the record producer ->
//     encode -> fd 1. Ends deterministically at the app's exit (drain-then-EOF).
//   - INPUT  (spawned): fd 0 -> decode Input -> encode Key to the master;
//     Resize -> pts winsize (kernel SIGWINCH to the fg pgrp) + flag the output
//     thread to resize its vt.
// The two directions share exactly two lock-free flags (DECCKM state + a pending
// resize), benign-racy by terminal-mux convention: a mode/size flip observed one
// key or frame late is cosmetic and self-corrects.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use kaua_term::wire::{encode_record, parse_input, FrameDecoder, Input};
use kaua_term::{encode_key, Control, Producer, Record};
use ptyhold::{set_winsize, Master};
use vt::{Vt, DAYLIGHT};

use libthyla_rs::{
    env, t_burrow_attach, t_close, t_exit_group, t_putstr, t_read, t_wait_pid_for, t_write, thread,
    torpor,
};

// A lazy 32 MiB span: one capped ScrollOff is held as cells, serialized and
// framed before the up-pipe write, and the 4 MiB default could not hold the
// three copies of a wide bulk scroll (pages commit only as touched).
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAllocN<{ 32 * 1024 * 1024 }> =
    libthyla_rs::alloc::ThylaAllocN;

const DOWN_FD: i64 = 0;
const UP_FD: i64 = 1;
const PUMP_STACK: u64 = 64 * 1024;

// A blocking (parking) mutex, the 3-state futex pattern: 0 = free, 1 = held with
// no waiters, 2 = held with possible waiters. It serializes MASTER writes across
// the two threads (see write_master). A spinlock would be wrong here -- a master
// write can block server-side, so the holder may park mid-write; a waiter must
// park too, not burn a core. Only the master needs it (fd 1 has a single writer).
struct WriteLock(AtomicU32);

impl WriteLock {
    const fn new() -> WriteLock {
        WriteLock(AtomicU32::new(0))
    }
    fn lock(&self) {
        if self
            .0
            .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            return;
        }
        // Contended: claim as "held, maybe-waiters" and park until free.
        while self.0.swap(2, Ordering::Acquire) != 0 {
            let _ = torpor::wait(&self.0, 2, None);
        }
    }
    fn unlock(&self) {
        if self.0.swap(0, Ordering::Release) == 2 {
            let _ = torpor::wake_one(&self.0);
        }
    }
}

// The state the two threads share. mfd + n are immutable after mint; the two
// atomics + the write lock are the only mutable cross-thread state (file header).
struct Shared {
    mfd: i64,
    n: u64,
    app_cursor: AtomicBool,
    // (cols << 16) | rows, or 0 for "no pending resize".
    pending_resize: AtomicU32,
    // Serializes the master's TWO writers: the output thread's terminal replies
    // (CPR/DSR/DA) and the input thread's re-encoded keys. Without it a reply
    // written during a keystroke could interleave mid-sequence and corrupt the
    // app's stdin (ut/kaua emits [6n for its size handshake, so this is live).
    master_write: WriteLock,
}

/// A zero-count write is back-pressure, not an error: a raw-mode pts master
/// accepts what fits and replies 0 when its ring is full. Retry briefly so a
/// key's bytes stay whole; give up (and drop the rest) only when the ring
/// stays full past the bound or the fd errors.
const WRITE_ZERO_RETRIES: u32 = 200;

fn write_all(fd: i64, buf: &[u8]) {
    let mut off = 0usize;
    let mut zeros = 0u32;
    while off < buf.len() {
        // SAFETY: SVC wrapper over this thread's own buffer.
        let w = unsafe { t_write(fd, buf.as_ptr().add(off), buf.len() - off) };
        if w < 0 {
            break;
        }
        if w == 0 {
            zeros += 1;
            if zeros > WRITE_ZERO_RETRIES {
                break;
            }
            let _ = libthyla_rs::time::sleep(libthyla_rs::time::Duration::from_millis(1));
            continue;
        }
        zeros = 0;
        off += w as usize;
    }
}

// A master write held under the lock, so a reply and a key never interleave.
fn write_master(sh: &Shared, buf: &[u8]) {
    if buf.is_empty() {
        return;
    }
    sh.master_write.lock();
    write_all(sh.mfd, buf);
    sh.master_write.unlock();
}

fn emit(recs: &[Record], out: &mut Vec<u8>) {
    out.clear();
    for r in recs {
        encode_record(r, out);
    }
    if !out.is_empty() {
        write_all(UP_FD, out);
    }
}

/// Apply a posted resize to the vt + producer and emit the full redraw.
fn apply_resize(
    sh: &Shared,
    vt: &mut Vt,
    prod: &mut Producer,
    recs: &mut Vec<Record>,
    out: &mut Vec<u8>,
) {
    let packed = sh.pending_resize.swap(0, Ordering::Relaxed);
    if packed != 0 {
        vt.resize((packed >> 16) as usize, (packed & 0xffff) as usize);
        recs.clear();
        // A shrink pushes the rows that left the top into the vt's pending
        // boundaries: drain them through the producer FIRST (rows only -- a
        // diff here would run against the old-geometry shadow), so the
        // history precedes the resized screen on the wire in this same emit
        // (a quiet app would otherwise never surface them).
        prod.drain_pending(vt, recs);
        prod.resized(vt, recs);
        emit(recs, out);
    }
}

// The INPUT thread: fd 0 -> Input -> the master / winsize.
extern "C" fn pump_in(arg: u64) {
    // SAFETY: `arg` is the &'static Shared pointer the main thread passed.
    let sh = unsafe { &*(arg as *const Shared) };
    let mut dec = FrameDecoder::new();
    let mut rbuf = [0u8; 1024];
    let mut kbuf: Vec<u8> = Vec::new();
    'down: loop {
        // SAFETY: SVC wrapper over this thread's own stack buffer.
        let n = unsafe { t_read(DOWN_FD, rbuf.as_mut_ptr(), rbuf.len()) };
        if n <= 0 {
            break; // halcyond closed the down channel
        }
        dec.push(&rbuf[..n as usize]);
        loop {
            match dec.next_frame() {
                Some(Ok((tag, payload))) => match parse_input(tag, &payload) {
                    Ok(Input::Key(ev)) => {
                        kbuf.clear();
                        encode_key(&ev, sh.app_cursor.load(Ordering::Relaxed), &mut kbuf);
                        write_master(sh, &kbuf);
                    }
                    Ok(Input::Resize { cols, rows }) => {
                        // Flag the output thread BEFORE setting the winsize, so
                        // the app's SIGWINCH redraw is already processed at the
                        // new size when the output thread wakes on it.
                        sh.pending_resize
                            .store(((cols as u32) << 16) | rows as u32, Ordering::Relaxed);
                        set_winsize(sh.n, cols, rows);
                    }
                    // halcyond is trusted; a bad frame is a bug, not an attack --
                    // drop the record rather than tearing the tile down.
                    Err(_) => {}
                },
                // An oversize frame is unrecoverable stream desync: the channel
                // is gone for good (reading on would only grow the buffer with
                // every key dropped), so end the kaua-term as on EOF.
                Some(Err(_)) => break 'down,
                None => break,
            }
        }
    }
    // Down channel gone: end the whole kaua-term (the group cascade unwinds the
    // output thread's parked master read; process exit closes the master).
    // SAFETY: `!`-returning SVC.
    unsafe { t_exit_group(0) }
}

fn parse_dim(a: Option<&[u8]>) -> Option<u16> {
    let s = core::str::from_utf8(a?).ok()?;
    s.parse::<u16>().ok().filter(|&d| d >= 1)
}

fn run() -> i64 {
    // argv: kaua-term [--beacon TIER] <cols> <rows> [prog [args...]]
fn write_env_beacon(tier: &str) -> bool {
    use libthyla_rs::io::Write as _;
    match libthyla_rs::fs::File::create("/env/BEACON") {
        Ok(mut f) => f.write_all(tier.as_bytes()).is_ok(),
        Err(_) => false,
    }
}

    let mut args = env::args();
    let _argv0 = args.next();
    let mut next = args.next();
    let mut tier = "none";
    if next == Some(b"--beacon".as_slice()) {
        tier = match args.next() {
            Some(b"rich") => "rich",
            Some(b"cells") => "cells",
            Some(b"none") => "none",
            _ => {
                t_putstr("kaua-term: --beacon takes none|cells|rich\n");
                return 2;
            }
        };
        next = args.next();
    }
    let cols = parse_dim(next).unwrap_or(80);
    let rows = parse_dim(args.next()).unwrap_or(24);
    let mut argv: Vec<String> = Vec::new();
    for a in args {
        match core::str::from_utf8(a) {
            Ok(s) => argv.push(String::from(s)),
            Err(_) => {
                t_putstr("kaua-term: non-utf8 argument\n");
                return 2;
            }
        }
    }
    if argv.is_empty() {
        argv.push(String::from("/bin/ut"));
    }

    // Mint the pts, seed its size to the tile, host the app on the slave.
    let master = match Master::mint() {
    // The advertisement precedes the spawn: the app's env is a deep copy of
    // ours at that instant.
    if !write_env_beacon(tier) {
        t_putstr("kaua-term: /env/BEACON write failed (the app inherits the caller's tier)\n");
    }

        Ok(m) => m,
        Err(_) => {
            t_putstr("kaua-term: pts mint failed\n");
            return 2;
        }
    };
    master.seed_winsize(cols, rows);
    let child = match master.spawn_on_slave(&argv) {
        Ok(c) => c,
        Err(_) => {
            t_putstr("kaua-term: spawn failed\n");
            let _ = unsafe { t_close(master.mfd) };
            return 2;
        }
    };
    let pid = child.pid();
    let mfd = master.mfd;

    let sh: &'static Shared = Box::leak(Box::new(Shared {
        mfd,
        n: master.n,
        app_cursor: AtomicBool::new(false),
        pending_resize: AtomicU32::new(0),
        master_write: WriteLock::new(),
    }));

    // The input thread. A spawn failure degrades to output-only (the tile still
    // renders; it just cannot be typed at) -- reported, not fatal.
    let stack = unsafe { t_burrow_attach(PUMP_STACK) };
    if stack < 0
        || unsafe {
            thread::spawn_raw(
                pump_in as *const () as u64,
                stack as u64 + PUMP_STACK,
                sh as *const Shared as u64,
                0,
            )
        }
        .is_err()
    {
        t_putstr("kaua-term: input thread spawn failed (output-only)\n");
    }

    // The output thread (this one): master -> producer -> records -> fd 1.
    // Cells are born in the compositor's Daylight palette (HALCYON.md 14.12):
    // the seam ships resolved RGB, so halcyond cannot re-theme downstream -- the
    // tile grid must composite coherently with halcyond's Daylight transcript.
    let mut vt = Vt::with_palette(cols as usize, rows as usize, DAYLIGHT);
    vt.set_capture_events(true);
    let mut prod = Producer::new(&vt);
    let mut recs: Vec<Record> = Vec::new();
    let mut out: Vec<u8> = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        // Apply a pending resize before processing new output, so the CellDiff
        // is computed against the correct geometry.
        apply_resize(sh, &mut vt, &mut prod, &mut recs, &mut out);
        // SAFETY: SVC wrapper over this thread's own stack buffer.
        let n = unsafe { t_read(mfd, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            break; // app exited: drain-then-EOF on the master
        }
        // The resize is posted while this read is parked, and the bytes it
        // returns are usually the app's SIGWINCH repaint at the NEW size: apply
        // it again here, or that repaint is parsed at the old geometry.
        apply_resize(sh, &mut vt, &mut prod, &mut recs, &mut out);
        recs.clear();
        // Each capped ScrollOff is shipped as it lands: the rows one read can
        // yield are the VT's to decide (a five-byte `ESC [ 36 S` is 36 rows),
        // so records must leave the heap as they form, not after the chunk.
        prod.feed_into(
            &mut vt,
            &buf[..n as usize],
            &mut recs,
            &mut |r: &mut Vec<Record>| {
                emit(r, &mut out);
                r.clear();
            },
        );
        sh.app_cursor.store(vt.app_cursor(), Ordering::Relaxed);
        // The terminal's own replies (CPR etc.) go back to the app on the master,
        // under the lock so they never interleave with an input-thread key write.
        if !vt.reply.is_empty() {
            write_master(sh, &vt.reply);
            vt.reply.clear();
        }
        // The aurora-config OSC channel is not a tile concern; drop it.
        vt.settings_req.clear();
        emit(&recs, &mut out);
    }

    // Reap the app, forward its exit as a Control record, then tear down (process
    // exit closes the master -> tty:hup inward + unwinds the input thread).
    let mut status: i32 = 0;
    // SAFETY: SVC wrapper; &mut status is a valid writable i32.
    let reaped = unsafe { t_wait_pid_for(pid, 0, &mut status as *mut i32) };
    let code = if reaped == pid as i64 { status } else { 2 };
    out.clear();
    encode_record(&Record::Control(Control::Exit(code)), &mut out);
    write_all(UP_FD, &out);
    let _ = unsafe { t_close(mfd) };
    code as i64
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
