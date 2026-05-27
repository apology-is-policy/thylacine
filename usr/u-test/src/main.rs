// /u-test -- cumulative integration smoke for the libthyla-rs uplift
// (U-2-test, closes the U-2 arc).
//
// Spawned by joey at boot AFTER /alloc-smoke. Where alloc-smoke
// isolates each U-2X module's surface in its own block (one block per
// landed sub-chunk; FAIL paths exercised individually), u-test runs
// COMPOSED flows -- the cross-module patterns a Utopia builtin would
// reach for. The two binaries complement: alloc-smoke proves each
// module standalone; u-test proves they compose under realistic
// usage.
//
// Each flow:
//   1. heap-backed string + file read              (alloc + fs + io)
//   2. spawn-and-read-stdout pipeline              (process + fs + io)
//   3. notes round-trip with poll(timeout)         (notes + poll + time)
//   4. thread + futex coordination                 (thread + torpor + time)
//   5. 9P codec round-trip                         (ninep)
//   6. hardware cap-missing rejection              (hardware + cap)
//
// Exit semantics: each flow prints "u-test: <flow> OK" on success +
// bails on first FAIL with a tagged diagnostic + exits 1; final
// "u-test: all OK" precedes a clean exit 0.
//
// On success, joey prints "joey: /u-test reaped status=0; U-2 arc
// integration verified" and continues the boot.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{Dma, Irq, Mmio};
use libthyla_rs::io::Read;
use libthyla_rs::ninep;
use libthyla_rs::notes::{self, NoteTarget, Notes};
use libthyla_rs::poll::{PollEvent, PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::thread;
use libthyla_rs::time::{self, Duration};
use libthyla_rs::torpor::{self, WaitResult};
use libthyla_rs::{t_putstr, T_PROT_READ, T_PROT_WRITE};
use libutopia::line_editor::{EditorAction, LineEditor};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("u-test: starting (U-2-test; U-2 uplift arc integration smoke)\n");

    if let Err(rc) = flow_fs_io() {
        return rc;
    }
    if let Err(rc) = flow_process_pipe() {
        return rc;
    }
    if let Err(rc) = flow_notes_poll_timeout() {
        return rc;
    }
    if let Err(rc) = flow_thread_torpor() {
        return rc;
    }
    if let Err(rc) = flow_ninep_roundtrip() {
        return rc;
    }
    if let Err(rc) = flow_hardware_caps() {
        return rc;
    }
    if let Err(rc) = flow_line_editor() {
        return rc;
    }

    t_putstr("u-test: all OK\n");
    0
}

// =============================================================================
// Flow 1 -- alloc + fs + io: heap-backed read of /system.key
// =============================================================================
//
// Open /system.key, read the full contents into a heap-backed Vec via
// the t::io::Read trait, sanity-check the size against the build-time
// fixture (3656 bytes per the joey smoke). Validates: t::alloc (Vec
// growth), t::handle (Drop closes the fd at flow exit), t::fs::File
// (per-component SYS_WALK_OPEN), t::io::Read (read_to_end via the
// trait impl).
fn flow_fs_io() -> Result<(), i64> {
    let mut f = match File::open("/system.key") {
        Ok(f) => f,
        Err(_) => {
            t_putstr("u-test: flow_fs_io: File::open(/system.key) FAILED\n");
            return Err(1);
        }
    };
    let mut buf: Vec<u8> = Vec::new();
    if f.read_to_end(&mut buf).is_err() {
        t_putstr("u-test: flow_fs_io: read_to_end FAILED\n");
        return Err(1);
    }
    if buf.len() != 3656 {
        t_putstr("u-test: flow_fs_io: unexpected /system.key size FAILED\n");
        return Err(1);
    }
    // First byte should be in the keyfile envelope; non-zero proves
    // the read landed on real bytes (not a zero-padded buffer).
    if buf[0] == 0 && buf[16] == 0 && buf[100] == 0 {
        t_putstr("u-test: flow_fs_io: /system.key bytes appear all-zero FAILED\n");
        return Err(1);
    }
    t_putstr("u-test: alloc + fs + io OK\n");
    Ok(())
}

// =============================================================================
// Flow 2 -- process + pipe: pipe round-trip + spawn-and-wait
// =============================================================================
//
// Two composed primitives in one flow:
//
// (a) t::process::pipe() round-trip: parent owns both ends; writes via
//     the writer, reads via the reader, drops the writer + reads EOF.
//     Validates t::process::pipe + t::io::Read + t::io::Write.
//
// (b) Spawn hello-rs with all three stdio ends Piped; drop every pipe
//     end on the parent side immediately (mirrors the alloc-smoke
//     pattern); wait for child + verify exit clean. Validates
//     t::process::Command builder, Stdio::Piped path, t::process::
//     Child::wait + ExitStatus::success.
//
// hello-rs writes its banner via SYS_PUTS (t_puts) -- console-direct,
// NOT through fd 1 -- so we can't capture and validate its output
// through the pipe at v1.0. The stdout-content check belongs in a
// future Utopia-builtin test (e.g., `echo` writing to a captured
// pipe), once a fd-1-writing binary exists.
fn flow_process_pipe() -> Result<(), i64> {
    use libthyla_rs::io::Write;
    use libthyla_rs::process;

    // (a) pipe round-trip.
    let (mut rd, mut wr) = match process::pipe() {
        Ok(p) => p,
        Err(_) => {
            t_putstr("u-test: flow_process_pipe: pipe() FAILED\n");
            return Err(1);
        }
    };
    if wr.write(b"u-test-payload").is_err() {
        t_putstr("u-test: flow_process_pipe: pipe write FAILED\n");
        return Err(1);
    }
    let mut buf = [0u8; 16];
    let n = match rd.read(&mut buf) {
        Ok(n) => n,
        Err(_) => {
            t_putstr("u-test: flow_process_pipe: pipe read FAILED\n");
            return Err(1);
        }
    };
    if n != 14 || &buf[..n] != b"u-test-payload" {
        t_putstr("u-test: flow_process_pipe: pipe payload mismatch FAILED\n");
        return Err(1);
    }
    // Drop writer; subsequent read returns Ok(0) = EOF.
    drop(wr);
    let eof = match rd.read(&mut buf) {
        Ok(n) => n,
        Err(_) => {
            t_putstr("u-test: flow_process_pipe: pipe EOF read FAILED\n");
            return Err(1);
        }
    };
    if eof != 0 {
        t_putstr("u-test: flow_process_pipe: pipe EOF wrong-count FAILED\n");
        return Err(1);
    }
    drop(rd);

    // (b) spawn-and-wait. Drop every pipe end on the parent side --
    // hello-rs doesn't write to fd 1 so reading would block forever.
    let mut child = match Command::new("hello-rs")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("u-test: flow_process_pipe: spawn(hello-rs) FAILED\n");
            return Err(1);
        }
    };
    drop(child.stdin.take());
    drop(child.stdout.take());
    drop(child.stderr.take());

    let status = match child.wait() {
        Ok(s) => s,
        Err(_) => {
            t_putstr("u-test: flow_process_pipe: child.wait FAILED\n");
            return Err(1);
        }
    };
    if !status.success() {
        t_putstr("u-test: flow_process_pipe: child exited non-zero FAILED\n");
        return Err(1);
    }

    t_putstr("u-test: process + pipe OK\n");
    Ok(())
}

// =============================================================================
// Flow 3 -- notes + poll + time: timeout + delivery
// =============================================================================
//
// Open self-notes, register in a PollSet, poll with a 5ms timeout
// (must return empty -- no pending events), THEN self-send an
// "interrupt" and poll again with the same timeout (must return one
// READ-ready event), drain the note record, validate it parsed
// correctly. Validates: t::notes (Notes RAII + send + read),
// t::poll (PollSet + AsFd integration on Notes + timeout path),
// t::time::Duration (re-export from core::time).
fn flow_notes_poll_timeout() -> Result<(), i64> {
    let notes_fd = match Notes::open_self() {
        Ok(n) => n,
        Err(_) => {
            t_putstr("u-test: flow_notes_poll_timeout: Notes::open_self FAILED\n");
            return Err(1);
        }
    };
    // Drain any synthetic child_exit posted by the hello-rs reap in
    // flow_process_pipe; we want a clean baseline before the timeout
    // probe below.
    loop {
        match notes_fd.try_read() {
            Ok(Some(_)) => continue,
            Ok(None) => break,
            Err(_) => {
                t_putstr("u-test: flow_notes_poll_timeout: drain FAILED\n");
                return Err(1);
            }
        }
    }

    let mut ps = PollSet::new();
    ps.add(&notes_fd, PollEvents::READ);

    // First poll: 5ms timeout, no events queued. Expect Ok with empty
    // ready iterator. PollResults IS the iterator (no .iter()).
    let ready = match ps.poll(PollTimeout::Millis(5)) {
        Ok(r) => r,
        Err(_) => {
            t_putstr("u-test: flow_notes_poll_timeout: first poll FAILED\n");
            return Err(1);
        }
    };
    if ready.count() != 0 {
        t_putstr("u-test: flow_notes_poll_timeout: first poll returned events unexpectedly FAILED\n");
        return Err(1);
    }

    // Self-send an interrupt; expect the next poll to surface it.
    if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
        t_putstr("u-test: flow_notes_poll_timeout: notes::send FAILED\n");
        return Err(1);
    }
    let ready2 = match ps.poll(PollTimeout::Millis(50)) {
        Ok(r) => r,
        Err(_) => {
            t_putstr("u-test: flow_notes_poll_timeout: second poll FAILED\n");
            return Err(1);
        }
    };
    let evts: Vec<PollEvent> = ready2.collect();
    if evts.len() != 1 || !evts[0].is_readable() {
        t_putstr("u-test: flow_notes_poll_timeout: second poll wrong-shape FAILED\n");
        return Err(1);
    }

    // Drain the note.
    match notes_fd.read() {
        Ok(note) => {
            if note.name.as_str() != "interrupt" {
                t_putstr("u-test: flow_notes_poll_timeout: note name mismatch FAILED\n");
                return Err(1);
            }
        }
        Err(_) => {
            t_putstr("u-test: flow_notes_poll_timeout: notes_fd.read FAILED\n");
            return Err(1);
        }
    }

    t_putstr("u-test: notes + poll + time OK\n");
    Ok(())
}

// =============================================================================
// Flow 4 -- thread + torpor + time: producer/consumer coordination
// =============================================================================
//
// Parent allocates a shared AtomicU32 (the wake target) AND the
// clear-child-tid word (the join target). Spawns a child thread; the
// child calls torpor::wake_one on the shared atomic + exits. Parent
// waits on the atomic via torpor::wait (blocks until the child's wake
// crosses), then joins via thread::join_tid. Time::sleep gates a
// fallback timeout. Validates: t::thread (spawn_raw +
// set_tid_address + exit_self + join_tid), t::torpor (wait + wake_one
// + WaitResult), t::time::Duration as the wait timeout argument.
fn flow_thread_torpor() -> Result<(), i64> {
    // Targets live as statics so the child thread can reach them by
    // user-VA without lifetime juggling. SHARED is the wake target;
    // TID_WORD is the kernel clear-child-tid join handshake target.
    static SHARED: AtomicU32 = AtomicU32::new(0);
    static TID_WORD: AtomicU32 = AtomicU32::new(0xC0FFEE);
    SHARED.store(0, Ordering::SeqCst);
    TID_WORD.store(0xC0FFEE, Ordering::SeqCst);

    let stack: alloc::boxed::Box<[u8; 4096]> = alloc::boxed::Box::new([0u8; 4096]);
    let stack_top_va = unsafe {
        alloc::boxed::Box::leak(stack).as_mut_ptr().add(4096)
    } as u64;

    extern "C" fn child_entry(arg: u64) -> ! {
        // arg is (TID_WORD ptr stuffed into u64 -- but we ignore;
        // child reaches statics directly).
        let _ = arg;
        thread::set_tid_address(&TID_WORD);
        SHARED.store(0x1337, Ordering::Release);
        let _ = torpor::wake_one(&SHARED);
        thread::exit_self()
    }

    let _tid = match unsafe {
        thread::spawn_raw(
            child_entry as *const () as usize as u64,
            stack_top_va,
            0,
            0,
        )
    } {
        Ok(t) => t,
        Err(_) => {
            t_putstr("u-test: flow_thread_torpor: spawn_raw FAILED\n");
            return Err(1);
        }
    };

    // Wait on SHARED for the child's wake. Initial value is 0; the
    // child stores 0x1337 + wakes. The kernel evaluates the wait
    // condition (value == expected) atomically with the lock, so a
    // wait with expected=0 returns Ok(Woken) when child changes value.
    match torpor::wait(&SHARED, 0, Some(Duration::from_secs(2))) {
        Ok(WaitResult::Woken) | Ok(WaitResult::ValueMismatch) => {}
        Ok(WaitResult::TimedOut) => {
            t_putstr("u-test: flow_thread_torpor: torpor::wait timed out FAILED\n");
            return Err(1);
        }
        Err(_) => {
            t_putstr("u-test: flow_thread_torpor: torpor::wait FAILED\n");
            return Err(1);
        }
    }
    if SHARED.load(Ordering::Acquire) != 0x1337 {
        t_putstr("u-test: flow_thread_torpor: SHARED value wrong after wake FAILED\n");
        return Err(1);
    }

    // Join via clear-child-tid. The kernel clears TID_WORD + wakes
    // when the child reaches SYS_THREAD_EXIT.
    if thread::join_tid(&TID_WORD, 0xC0FFEE, Some(Duration::from_secs(2))).is_err() {
        t_putstr("u-test: flow_thread_torpor: join_tid FAILED\n");
        return Err(1);
    }
    if TID_WORD.load(Ordering::Acquire) != 0 {
        t_putstr("u-test: flow_thread_torpor: TID_WORD not cleared FAILED\n");
        return Err(1);
    }

    // Sleep a token amount via time::sleep -- just to verify the
    // wrapper still composes after a thread teardown.
    if time::sleep(Duration::from_millis(2)).is_err() {
        t_putstr("u-test: flow_thread_torpor: time::sleep FAILED\n");
        return Err(1);
    }

    t_putstr("u-test: thread + torpor + time OK\n");
    Ok(())
}

// =============================================================================
// Flow 5 -- ninep codec round-trip
// =============================================================================
//
// Hand-build a Tversion frame via the pack primitives + back-patch the
// header size, parse it back via parse_tversion, build an Rversion
// response, peek_header to verify the back-patched size. Validates the
// codec works under composed pack -> peek -> parse + build -> peek
// patterns (the shape any 9P server reaches for).
fn flow_ninep_roundtrip() -> Result<(), i64> {
    let mut tver = [0u8; 32];
    let p = ninep::pack_u32(&mut tver, 0, 0).unwrap();
    let p = ninep::pack_u8(&mut tver, p, ninep::P9_TVERSION).unwrap();
    let p = ninep::pack_u16(&mut tver, p, ninep::P9_NOTAG).unwrap();
    let p = ninep::pack_u32(&mut tver, p, 8192).unwrap();
    let p = ninep::pack_str(&mut tver, p, b"9P2000.L").unwrap();
    let _ = ninep::pack_u32(&mut tver, 0, p as u32).unwrap();

    let h = match ninep::peek_header(&tver[..p]) {
        Ok(h) => h,
        Err(_) => {
            t_putstr("u-test: flow_ninep_roundtrip: peek_header FAILED\n");
            return Err(1);
        }
    };
    if h.mtype != ninep::P9_TVERSION || h.tag != ninep::P9_NOTAG || h.size as usize != p {
        t_putstr("u-test: flow_ninep_roundtrip: header field mismatch FAILED\n");
        return Err(1);
    }

    let tv = match ninep::parse_tversion(&tver[..p]) {
        Ok(t) => t,
        Err(_) => {
            t_putstr("u-test: flow_ninep_roundtrip: parse_tversion FAILED\n");
            return Err(1);
        }
    };
    if tv.msize != 8192 || tv.version != b"9P2000.L" {
        t_putstr("u-test: flow_ninep_roundtrip: Tversion fields wrong FAILED\n");
        return Err(1);
    }

    // Build a matching Rversion + peek it back.
    let mut rver = [0u8; 32];
    let nrv = ninep::build_rversion(&mut rver, h.tag, tv.msize, tv.version).unwrap();
    let hr = ninep::peek_header(&rver[..nrv]).unwrap();
    if hr.mtype != ninep::P9_RVERSION || hr.tag != ninep::P9_NOTAG {
        t_putstr("u-test: flow_ninep_roundtrip: Rversion header wrong FAILED\n");
        return Err(1);
    }

    t_putstr("u-test: ninep codec OK\n");
    Ok(())
}

// =============================================================================
// Flow 6 -- hardware + cap: cap-missing rejection
// =============================================================================
//
// u-test runs without CAP_HW_CREATE (joey grants the cap only to the
// virtio-* family + corvus), so positive Mmio/Irq/Dma constructions
// are not reachable. Exercise the NEGATIVE rejection paths -- proves
// the typed wrappers map the kernel's -1 to Err(InvalidArgument).
// Composition value: this catches a regression in either the err
// module's mapping OR the hardware module's error helper.
fn flow_hardware_caps() -> Result<(), i64> {
    match unsafe {
        Mmio::new(0x0901_0000, 0x1000, Rights::READ | Rights::MAP, 0x0050_0000, T_PROT_READ)
    } {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("u-test: flow_hardware_caps: Mmio cap-missing wrong-error FAILED\n");
            return Err(1);
        }
    }
    match Irq::new(96, Rights::SIGNAL) {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("u-test: flow_hardware_caps: Irq cap-missing wrong-error FAILED\n");
            return Err(1);
        }
    }
    match unsafe {
        Dma::new(4096, Rights::READ | Rights::WRITE | Rights::MAP, 0x0060_0000, T_PROT_READ | T_PROT_WRITE)
    } {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("u-test: flow_hardware_caps: Dma cap-missing wrong-error FAILED\n");
            return Err(1);
        }
    }

    // Heap-allocate a String to prove t::alloc is still healthy after
    // the hw-handle rejections (the rejections allocate the Handle
    // struct via Box on the kernel side; the userspace Drop fires
    // through the Handle's Drop impl when Err is constructed -- a
    // historical bug pattern was leaking the handle slot on the
    // error path).
    let s = format!("u-test: composed flow tally complete @ {}", 6);
    if s.len() < 10 {
        t_putstr("u-test: flow_hardware_caps: format! sanity FAILED\n");
        return Err(1);
    }

    t_putstr("u-test: hardware + cap OK\n");
    Ok(())
}

// =============================================================================
// Flow 7 -- libutopia::line_editor (U-4a): pure-logic engine end-to-end
// =============================================================================
//
// The line editor is a pure-logic engine (no I/O; no PTY surface yet);
// every part of it is reachable from this in-process flow. Six probes:
//
//   1. Insert ASCII -> buffer + cursor advance, Accept clears state
//   2. ANSI arrow parsing across feed_byte boundaries (\x1b then [ then D)
//   3. Backspace + Ctrl-K + Ctrl-Y round-trip via the kill buffer
//   4. UTF-8 multi-byte char insertion + boundary-walking backspace
//   5. History up/down navigation through push_history-populated entries
//   6. render() produces ANSI escapes including the cursor positioning
//
// Each probe asserts state; any failure tags the probe + returns Err(1).
fn flow_line_editor() -> Result<(), i64> {
    use alloc::string::String;

    // Probe 1 -- insert ASCII + Accept.
    let mut le = LineEditor::new();
    let _ = le.feed_bytes(b"hello");
    if le.buffer() != "hello" || le.cursor() != 5 {
        t_putstr("u-test: flow_line_editor: insert ASCII FAILED\n");
        return Err(1);
    }
    match le.feed_byte(b'\r') {
        EditorAction::Accept(s) if s == "hello" => {}
        _ => {
            t_putstr("u-test: flow_line_editor: Enter -> Accept FAILED\n");
            return Err(1);
        }
    }
    if !le.buffer().is_empty() || le.cursor() != 0 {
        t_putstr("u-test: flow_line_editor: post-Accept reset FAILED\n");
        return Err(1);
    }

    // Probe 2 -- ANSI arrow parsing across byte boundaries.
    let _ = le.feed_bytes(b"abc");
    // ESC then [ then D (Left arrow) -- delivered one byte at a time
    // to prove the parser persists state across feed_byte calls.
    match le.feed_byte(0x1b) {
        EditorAction::NoChange => {}
        _ => {
            t_putstr("u-test: flow_line_editor: ESC alone -> NoChange FAILED\n");
            return Err(1);
        }
    }
    match le.feed_byte(b'[') {
        EditorAction::NoChange => {}
        _ => {
            t_putstr("u-test: flow_line_editor: ESC[ -> NoChange FAILED\n");
            return Err(1);
        }
    }
    match le.feed_byte(b'D') {
        EditorAction::Redraw => {}
        _ => {
            t_putstr("u-test: flow_line_editor: ESC[D -> Redraw FAILED\n");
            return Err(1);
        }
    }
    if le.cursor() != 2 {
        t_putstr("u-test: flow_line_editor: Left arrow cursor mismatch FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 3 -- Backspace + Ctrl-K kill-to-end + Ctrl-Y yank.
    let _ = le.feed_bytes(b"hello world");
    // Backspace -> "hello worl"
    le.feed_byte(0x7f);
    if le.buffer() != "hello worl" {
        t_putstr("u-test: flow_line_editor: Backspace FAILED\n");
        return Err(1);
    }
    // Ctrl-A then Ctrl-K -> kill_buffer="hello worl", buffer=""
    le.feed_byte(0x01);
    le.feed_byte(0x0b);
    if !le.buffer().is_empty() || le.kill_buffer() != "hello worl" {
        t_putstr("u-test: flow_line_editor: Ctrl-K kill FAILED\n");
        return Err(1);
    }
    // Ctrl-Y -> buffer restored.
    le.feed_byte(0x19);
    if le.buffer() != "hello worl" || le.cursor() != 10 {
        t_putstr("u-test: flow_line_editor: Ctrl-Y yank FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 4 -- UTF-8 multi-byte across feed_byte boundaries.
    // "café" = c \xc3 \xa9 -- the 2-byte é is delivered as two
    // separate feed_byte calls. The engine should accumulate in
    // Utf8 state then insert the complete char.
    le.feed_byte(b'c');
    le.feed_byte(b'a');
    le.feed_byte(b'f');
    le.feed_byte(0xc3);
    // After feeding the leading byte, buffer is still "caf" -- the
    // engine is awaiting the continuation byte.
    if le.buffer() != "caf" {
        t_putstr("u-test: flow_line_editor: UTF-8 partial buffer FAILED\n");
        return Err(1);
    }
    le.feed_byte(0xa9);
    if le.buffer() != "café" {
        t_putstr("u-test: flow_line_editor: UTF-8 complete buffer FAILED\n");
        return Err(1);
    }
    // Backspace should walk the multi-byte boundary correctly.
    le.feed_byte(0x7f);
    if le.buffer() != "caf" || le.cursor() != 3 {
        t_putstr("u-test: flow_line_editor: UTF-8 backspace FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 5 -- history nav.
    le.push_history(String::from("first command"));
    le.push_history(String::from("second command"));
    let _ = le.feed_bytes(b"draft");
    // Ctrl-P -> most recent.
    le.feed_byte(0x10);
    if le.buffer() != "second command" {
        t_putstr("u-test: flow_line_editor: history Ctrl-P 1 FAILED\n");
        return Err(1);
    }
    // Ctrl-P again -> "first command".
    le.feed_byte(0x10);
    if le.buffer() != "first command" {
        t_putstr("u-test: flow_line_editor: history Ctrl-P 2 FAILED\n");
        return Err(1);
    }
    // Ctrl-N -> "second command" again.
    le.feed_byte(0x0e);
    if le.buffer() != "second command" {
        t_putstr("u-test: flow_line_editor: history Ctrl-N 1 FAILED\n");
        return Err(1);
    }
    // Ctrl-N once more -> restore "draft" (saved current).
    le.feed_byte(0x0e);
    if le.buffer() != "draft" {
        t_putstr("u-test: flow_line_editor: history restore FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 6 -- render() produces the expected ANSI shape.
    let _ = le.feed_bytes(b"x");
    let r = le.render("> ");
    // Expected shape: \r\x1b[K> x\r\x1b[3C
    // (prompt width 2 + buffer width 1 = column 3)
    if !r.starts_with("\r\x1b[K") {
        t_putstr("u-test: flow_line_editor: render prefix FAILED\n");
        return Err(1);
    }
    if !r.contains("> x") {
        t_putstr("u-test: flow_line_editor: render content FAILED\n");
        return Err(1);
    }
    if !r.ends_with("\x1b[3C") {
        t_putstr("u-test: flow_line_editor: render cursor positioning FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 7 -- U-4b multi-line via unclosed brace, then close, then
    // multi-line Accept. Also covers Backspace joining a continuation
    // line back into the previous line.
    let _ = le.feed_bytes(b"{");
    let r = le.feed_byte(b'\r');
    match r {
        EditorAction::Redraw => {}
        _ => {
            t_putstr("u-test: flow_line_editor: unbalanced Enter -> Redraw FAILED\n");
            return Err(1);
        }
    }
    if le.buffer() != "{\n" {
        t_putstr("u-test: flow_line_editor: unbalanced buffer FAILED\n");
        return Err(1);
    }
    let _ = le.feed_bytes(b"  foo");
    le.feed_byte(b'\r');
    if le.buffer() != "{\n  foo\n" {
        t_putstr("u-test: flow_line_editor: multi-line buffer FAILED\n");
        return Err(1);
    }
    let _ = le.feed_bytes(b"}");
    let r = le.feed_byte(b'\r');
    match r {
        EditorAction::Accept(s) if s == "{\n  foo\n}" => {}
        _ => {
            t_putstr("u-test: flow_line_editor: multi-line Accept FAILED\n");
            return Err(1);
        }
    }

    // Probe 8 -- U-4b: single-quoted bracket doesn't trigger continuation.
    let _ = le.feed_bytes(b"'{'");
    let r = le.feed_byte(b'\r');
    match r {
        EditorAction::Accept(s) if s == "'{'" => {}
        _ => {
            t_putstr("u-test: flow_line_editor: single-quote isolates brackets FAILED\n");
            return Err(1);
        }
    }

    // Probe 9 -- U-4b: trailing-backslash continuation.
    let _ = le.feed_bytes(b"foo\\");
    let r = le.feed_byte(b'\r');
    match r {
        EditorAction::Redraw => {}
        _ => {
            t_putstr("u-test: flow_line_editor: trailing \\ continuation FAILED\n");
            return Err(1);
        }
    }
    if le.buffer() != "foo\\\n" {
        t_putstr("u-test: flow_line_editor: trailing-backslash buffer FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 11 -- U-4c: smart Up nav in multi-line buffer (cursor-up
    // when cursor not on first line; falls through to history when on
    // first line or single-line).
    le.push_history(String::from("older command"));
    let _ = le.feed_bytes(b"{");
    le.feed_byte(b'\r');
    let _ = le.feed_bytes(b"  body");
    // Cursor at end of line 1 (byte 8). Ctrl-P should cursor-up.
    let before_up = le.cursor();
    le.feed_byte(0x10);
    if le.cursor() >= before_up {
        t_putstr("u-test: flow_line_editor: smart Up cursor-up FAILED\n");
        return Err(1);
    }
    if le.buffer() != "{\n  body" {
        t_putstr("u-test: flow_line_editor: smart Up clobbered buffer FAILED\n");
        return Err(1);
    }
    // Ctrl-P again from line 0: history-prev to "older command".
    le.feed_byte(0x10);
    if le.buffer() != "older command" {
        t_putstr("u-test: flow_line_editor: smart Up fall-through to history FAILED\n");
        return Err(1);
    }
    le.reset();

    // Probe 12 -- U-4c: Ctrl-R incremental search, append + accept.
    // Fresh editor (reset() doesn't clear history; Probe 11 already
    // pushed "older command" which would shift indices below).
    // `String` is already in scope from the existing flow_line_editor
    // top-level `use alloc::string::String;`.
    let mut le = LineEditor::new();
    le.push_history(String::from("apple"));
    le.push_history(String::from("apricot"));
    le.push_history(String::from("banana"));
    le.feed_byte(0x12); // Ctrl-R
    if !le.is_searching() {
        t_putstr("u-test: flow_line_editor: Ctrl-R didn't enter search FAILED\n");
        return Err(1);
    }
    let _ = le.feed_bytes(b"ap");
    // Newest match for "ap" is index 1 ("apricot").
    if le.search_match_index() != Some(1) {
        t_putstr("u-test: flow_line_editor: search match_index FAILED\n");
        return Err(1);
    }
    // Ctrl-R again -> step to older match ("apple", index 0).
    le.feed_byte(0x12);
    if le.search_match_index() != Some(0) {
        t_putstr("u-test: flow_line_editor: search step-back FAILED\n");
        return Err(1);
    }
    // Enter -> Accept "apple".
    let r = le.feed_byte(b'\r');
    match r {
        EditorAction::Accept(s) if s == "apple" => {}
        _ => {
            t_putstr("u-test: flow_line_editor: search Accept FAILED\n");
            return Err(1);
        }
    }
    if le.is_searching() {
        t_putstr("u-test: flow_line_editor: search not exited after Accept FAILED\n");
        return Err(1);
    }

    // Probe 13 -- U-4c: search Cancel restores saved buffer.
    let mut le = LineEditor::new();
    le.push_history(String::from("hello"));
    let _ = le.feed_bytes(b"draft");
    le.feed_byte(0x12); // Ctrl-R; saves "draft"
    let _ = le.feed_bytes(b"hello");
    if le.search_match_index() != Some(0) {
        t_putstr("u-test: flow_line_editor: search-before-cancel match FAILED\n");
        return Err(1);
    }
    // Ctrl-C cancels + restores.
    le.feed_byte(0x03);
    if le.is_searching() {
        t_putstr("u-test: flow_line_editor: search not exited on Cancel FAILED\n");
        return Err(1);
    }
    if le.buffer() != "draft" || le.cursor() != 5 {
        t_putstr("u-test: flow_line_editor: search Cancel restore FAILED\n");
        return Err(1);
    }
    // Continue probe 10 with a fresh editor too (replaces the existing one).
    let mut le = LineEditor::new();

    // Probe 10 -- U-4b: multi-line render emits the ⋮ continuation glyph.
    let _ = le.feed_bytes(b"{");
    le.feed_byte(b'\r');
    let _ = le.feed_bytes(b"x");
    let r = le.render("> ");
    // The continuation glyph (U+22EE) must appear; the render must
    // contain a "\r\n" line break between the first and second lines.
    if !r.contains("\u{22ee}") {
        t_putstr("u-test: flow_line_editor: continuation glyph missing FAILED\n");
        return Err(1);
    }
    if !r.contains("\r\n\x1b[K") {
        t_putstr("u-test: flow_line_editor: multi-line break missing FAILED\n");
        return Err(1);
    }
    // Cursor on line 1 (the continuation line) at col cont_w + 1 (1 = `x`).
    // cont_w for prompt_width 2 is 2 (= `⋮ ` rendered).
    if !r.ends_with("\x1b[3C") {
        t_putstr("u-test: flow_line_editor: multi-line cursor positioning FAILED\n");
        return Err(1);
    }
    le.reset();

    t_putstr("u-test: line editor OK\n");
    Ok(())
}
