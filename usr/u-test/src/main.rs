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
use libutopia::line_editor::{
    EditorAction, LineEditor, StaticCompletionSource,
};
use libutopia::parser::{
    parse, parse_expr_tokens, tokenize, BinOp, CommandKind, DqPart, ExprContext, ExprKind, MatchOp,
    ParseErrorKind, RedirectKind, StatementKind, TokenKind, UnOp, Word,
};
// U-5d types for flow_parser_full.
use libutopia::eval::{eval_expr, Env, EvalErrorKind, Value};

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
    if let Err(rc) = flow_parser_lexer() {
        return rc;
    }
    if let Err(rc) = flow_parser_core() {
        return rc;
    }
    if let Err(rc) = flow_parser_expr() {
        return rc;
    }
    if let Err(rc) = flow_parser_full() {
        return rc;
    }
    if let Err(rc) = flow_eval_expr() {
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
    // Probe 14 -- U-4d: Tab completion via the shipped
    // StaticCompletionSource. Three candidates with common prefix
    // "app": single-Tab extends to common prefix; second-Tab with
    // buffer="app" emits ShowCompletions for the main loop.
    let mut le = LineEditor::new();
    le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
        alloc::vec![
            String::from("apple"),
            String::from("application"),
            String::from("apparatus"),
        ],
    )));
    let _ = le.feed_bytes(b"a");
    let r = le.feed_byte(0x09); // Tab
    match r {
        EditorAction::Redraw => {}
        _ => {
            t_putstr("u-test: flow_line_editor: Tab common-prefix Redraw FAILED\n");
            return Err(1);
        }
    }
    if le.buffer() != "app" {
        t_putstr("u-test: flow_line_editor: Tab common-prefix extension FAILED\n");
        return Err(1);
    }
    // Second Tab from "app" -- all three candidates equally match;
    // no further common-prefix extension -> ShowCompletions.
    let r = le.feed_byte(0x09);
    match r {
        EditorAction::ShowCompletions(cands) => {
            if cands.len() != 3 {
                t_putstr("u-test: flow_line_editor: Tab ShowCompletions count FAILED\n");
                return Err(1);
            }
        }
        _ => {
            t_putstr("u-test: flow_line_editor: Tab ShowCompletions FAILED\n");
            return Err(1);
        }
    }
    if le.buffer() != "app" {
        t_putstr("u-test: flow_line_editor: Tab ShowCompletions buffer unchanged FAILED\n");
        return Err(1);
    }
    // Probe 15 -- U-4d: Tab with single matching candidate completes it.
    let mut le = LineEditor::new();
    le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
        alloc::vec![String::from("uniqueword")],
    )));
    let _ = le.feed_bytes(b"uniq");
    let r = le.feed_byte(0x09);
    match r {
        EditorAction::Redraw => {}
        _ => {
            t_putstr("u-test: flow_line_editor: Tab single-candidate Redraw FAILED\n");
            return Err(1);
        }
    }
    if le.buffer() != "uniqueword" || le.cursor() != 10 {
        t_putstr("u-test: flow_line_editor: Tab single-candidate replace FAILED\n");
        return Err(1);
    }
    // Probe 16 -- U-4d: Tab with no completion source is NoChange.
    let mut le = LineEditor::new();
    let _ = le.feed_bytes(b"xy");
    let r = le.feed_byte(0x09);
    match r {
        EditorAction::NoChange => {}
        _ => {
            t_putstr("u-test: flow_line_editor: Tab without source NoChange FAILED\n");
            return Err(1);
        }
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

// =============================================================================
// Flow 8 -- libutopia::parser::lexer (U-5a tokenizer for the rc-shape parser)
// =============================================================================
//
// 7 probes covering the load-bearing token shapes from UTOPIA-SHELL-
// DESIGN.md sections 5-9. Each probe validates a distinct lex
// surface so a regression in one place doesn't mask another.
//
// The lexer's correctness IS proven by the ~50 cfg(test) host unit
// tests in libutopia/src/parser/lexer.rs::tests; the probes below
// validate that the same code paths produce the same answers on
// the actual Thylacine target (the ThylaAlloc heap path differs
// from the host's allocator, and a UTF-8 char-walk bug would
// surface here if it did anywhere).
fn flow_parser_lexer() -> Result<(), i64> {
    // Probe 1 -- empty input emits only the synthetic EOF token.
    {
        let toks = match tokenize("") {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 1 tokenize(\"\") FAILED\n");
                return Err(1);
            }
        };
        if toks.len() != 1 || !matches!(toks[0].kind, TokenKind::Eof) {
            t_putstr("u-test: flow_parser_lexer: probe 1 empty -> Eof FAILED\n");
            return Err(1);
        }
    }

    // Probe 2 -- multi-word command + Pipe operator + Eof.
    {
        let toks = match tokenize("foo | bar") {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 2 tokenize FAILED\n");
                return Err(1);
            }
        };
        if toks.len() != 4 {
            t_putstr("u-test: flow_parser_lexer: probe 2 token count FAILED\n");
            return Err(1);
        }
        match &toks[0].kind {
            TokenKind::Word(s) if s == "foo" => {}
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 2 Word(foo) FAILED\n");
                return Err(1);
            }
        }
        if !matches!(&toks[1].kind, TokenKind::Pipe) {
            t_putstr("u-test: flow_parser_lexer: probe 2 Pipe FAILED\n");
            return Err(1);
        }
        match &toks[2].kind {
            TokenKind::Word(s) if s == "bar" => {}
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 2 Word(bar) FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 3 -- reserved word recognition (`let` is a keyword).
    {
        let toks = match tokenize("let x = 5") {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 3 tokenize FAILED\n");
                return Err(1);
            }
        };
        if !matches!(&toks[0].kind, TokenKind::Let) {
            t_putstr("u-test: flow_parser_lexer: probe 3 Let reserved FAILED\n");
            return Err(1);
        }
        if !matches!(&toks[2].kind, TokenKind::Equal) {
            t_putstr("u-test: flow_parser_lexer: probe 3 Equal FAILED\n");
            return Err(1);
        }
    }

    // Probe 4 -- DqString with literal+var+literal+subst parts
    // (the canonical interp example from scripture section 6.5).
    {
        let src = "let greeting = \"Hello, $user from $(pwd)\"";
        let toks = match tokenize(src) {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 4 tokenize FAILED\n");
                return Err(1);
            }
        };
        // [Let, Word(greeting), Equal, DoubleQuoted, Eof] = 5 tokens.
        if toks.len() != 5 {
            t_putstr("u-test: flow_parser_lexer: probe 4 token count FAILED\n");
            return Err(1);
        }
        match &toks[3].kind {
            TokenKind::DoubleQuoted(parts) => {
                if parts.len() != 4 {
                    t_putstr("u-test: flow_parser_lexer: probe 4 dqparts count FAILED\n");
                    return Err(1);
                }
                match &parts[0] {
                    DqPart::Literal(s) if s == "Hello, " => {}
                    _ => {
                        t_putstr("u-test: flow_parser_lexer: probe 4 literal 0 FAILED\n");
                        return Err(1);
                    }
                }
                match &parts[1] {
                    DqPart::Var(s) if s == "user" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_lexer: probe 4 Var FAILED\n");
                        return Err(1);
                    }
                }
                match &parts[2] {
                    DqPart::Literal(s) if s == " from " => {}
                    _ => {
                        t_putstr("u-test: flow_parser_lexer: probe 4 literal 2 FAILED\n");
                        return Err(1);
                    }
                }
                match &parts[3] {
                    DqPart::Subst(s) if s == "pwd" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_lexer: probe 4 Subst FAILED\n");
                        return Err(1);
                    }
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 4 DoubleQuoted FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 5 -- heredoc body collection: <<EOF body EOF.
    {
        let toks = match tokenize("cat <<EOF\nhello\nEOF\n") {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 5 tokenize FAILED\n");
                return Err(1);
            }
        };
        // [Word(cat), HeredocStart, Newline, HeredocBody, Eof] = 5.
        if toks.len() != 5 {
            t_putstr("u-test: flow_parser_lexer: probe 5 token count FAILED\n");
            return Err(1);
        }
        match &toks[1].kind {
            TokenKind::HeredocStart {
                tag,
                interp: true,
                strip_tabs: false,
            } if tag == "EOF" => {}
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 5 HeredocStart FAILED\n");
                return Err(1);
            }
        }
        match &toks[3].kind {
            TokenKind::HeredocBody(parts) => {
                if parts.len() != 1 {
                    t_putstr("u-test: flow_parser_lexer: probe 5 body parts FAILED\n");
                    return Err(1);
                }
                match &parts[0] {
                    DqPart::Literal(s) if s == "hello\n" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_lexer: probe 5 body literal FAILED\n");
                        return Err(1);
                    }
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 5 HeredocBody FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 6 -- regex literal after =~ (the one-shot lex mode).
    {
        let toks = match tokenize("$x =~ /^foo/") {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 6 tokenize FAILED\n");
                return Err(1);
            }
        };
        if toks.len() != 4 {
            t_putstr("u-test: flow_parser_lexer: probe 6 token count FAILED\n");
            return Err(1);
        }
        match &toks[0].kind {
            TokenKind::Var(s) if s == "x" => {}
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 6 Var(x) FAILED\n");
                return Err(1);
            }
        }
        if !matches!(&toks[1].kind, TokenKind::EqualTilde) {
            t_putstr("u-test: flow_parser_lexer: probe 6 EqualTilde FAILED\n");
            return Err(1);
        }
        match &toks[2].kind {
            TokenKind::Regex(s) if s == "^foo" => {}
            _ => {
                t_putstr("u-test: flow_parser_lexer: probe 6 Regex FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 7 -- error case + span tracking: unterminated single
    // quote returns ParseErrorKind::UnterminatedSingleQuote;
    // separately, span on a basic word covers the right bytes.
    {
        let err = match tokenize("'no close") {
            Ok(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 7 expected error FAILED\n");
                return Err(1);
            }
            Err(e) => e,
        };
        if !matches!(err.kind, ParseErrorKind::UnterminatedSingleQuote) {
            t_putstr("u-test: flow_parser_lexer: probe 7 ParseErrorKind FAILED\n");
            return Err(1);
        }
        let toks = match tokenize("hello") {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_lexer: probe 7 span tokenize FAILED\n");
                return Err(1);
            }
        };
        if toks[0].span.start != 0 || toks[0].span.end != 5 {
            t_putstr("u-test: flow_parser_lexer: probe 7 Word span FAILED\n");
            return Err(1);
        }
        if toks[1].span.start != 5 || toks[1].span.end != 5 {
            t_putstr("u-test: flow_parser_lexer: probe 7 Eof point-span FAILED\n");
            return Err(1);
        }
    }

    t_putstr("u-test: parser lexer OK\n");
    Ok(())
}

// =============================================================================
// Flow 9 -- libutopia::parser::parse (U-5b parser core + AST)
// =============================================================================
//
// 8 probes covering the load-bearing AST shapes from UTOPIA-SHELL-
// DESIGN.md sections 5-9. The parser builds on U-5a's token stream
// to produce a Script AST. The probes validate that the heap
// allocation paths (Box, Vec, BTreeMap-via-VecDeque) work under
// ThylaAlloc and that the recursive-descent walks produce the same
// AST shape on the actual Thylacine target as in the host cfg(test)
// tests.
fn flow_parser_core() -> Result<(), i64> {
    // Probe 1 -- empty script -> zero statements.
    {
        let s = match parse("") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 1 parse(\"\") FAILED\n");
                return Err(1);
            }
        };
        if !s.statements.is_empty() {
            t_putstr("u-test: flow_parser_core: probe 1 empty -> 0 stmts FAILED\n");
            return Err(1);
        }
    }

    // Probe 2 -- simple multi-statement script via semicolons.
    {
        let s = match parse("ls; cat /etc/hosts; wc -l") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 2 parse FAILED\n");
                return Err(1);
            }
        };
        if s.statements.len() != 3 {
            t_putstr("u-test: flow_parser_core: probe 2 statement count FAILED\n");
            return Err(1);
        }
    }

    // Probe 3 -- pipeline with tolerate flag on left element.
    {
        let s = match parse("cmd1 ?| cmd2 | cmd3") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 3 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                if p.elements.len() != 3 {
                    t_putstr("u-test: flow_parser_core: probe 3 pipeline len FAILED\n");
                    return Err(1);
                }
                if !p.elements[0].tolerate_failure {
                    t_putstr("u-test: flow_parser_core: probe 3 cmd1 tolerate FAILED\n");
                    return Err(1);
                }
                if p.elements[1].tolerate_failure || p.elements[2].tolerate_failure {
                    t_putstr("u-test: flow_parser_core: probe 3 cmd2/cmd3 tolerate FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_core: probe 3 not Pipeline FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 4 -- if/else with branches.
    {
        let s = match parse("if (x == 0) { a } else if (x < 10) { b } else { c }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 4 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::If(i) => {
                if i.then_branch.len() != 1 {
                    t_putstr("u-test: flow_parser_core: probe 4 then_branch FAILED\n");
                    return Err(1);
                }
                if i.elif_branches.len() != 1 {
                    t_putstr("u-test: flow_parser_core: probe 4 elif_branches FAILED\n");
                    return Err(1);
                }
                if i.else_branch.is_none() {
                    t_putstr("u-test: flow_parser_core: probe 4 else_branch FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_core: probe 4 not If FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 5 -- for loop with body.
    {
        let s = match parse("for (f in $files) { cat $f }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 5 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::For(f) => {
                if f.var_name != "f" {
                    t_putstr("u-test: flow_parser_core: probe 5 var_name FAILED\n");
                    return Err(1);
                }
                if f.body.len() != 1 {
                    t_putstr("u-test: flow_parser_core: probe 5 body FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_core: probe 5 not For FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 6 -- fn declaration with positional args + nested body.
    {
        let s = match parse("fn greet name { echo hello $name }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 6 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => {
                if f.name != "greet" {
                    t_putstr("u-test: flow_parser_core: probe 6 fn name FAILED\n");
                    return Err(1);
                }
                if f.args.len() != 1 || f.args[0] != "name" {
                    t_putstr("u-test: flow_parser_core: probe 6 fn args FAILED\n");
                    return Err(1);
                }
                if f.body.len() != 1 {
                    t_putstr("u-test: flow_parser_core: probe 6 fn body FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_core: probe 6 not FnDecl FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 7 -- heredoc body inlined into Redirect::Heredoc.
    {
        let s = match parse("cat <<EOF\nhello\nEOF\n") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: probe 7 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                let redir = &p.elements[0].command.redirects;
                if redir.len() != 1 {
                    t_putstr("u-test: flow_parser_core: probe 7 redirect count FAILED\n");
                    return Err(1);
                }
                match &redir[0].kind {
                    RedirectKind::Heredoc {
                        interp,
                        strip_tabs,
                        body,
                    } => {
                        if !*interp || *strip_tabs {
                            t_putstr("u-test: flow_parser_core: probe 7 heredoc flags FAILED\n");
                            return Err(1);
                        }
                        if body.len() != 1 {
                            t_putstr("u-test: flow_parser_core: probe 7 body parts FAILED\n");
                            return Err(1);
                        }
                        match &body[0] {
                            DqPart::Literal(s) if s == "hello\n" => {}
                            _ => {
                                t_putstr("u-test: flow_parser_core: probe 7 body content FAILED\n");
                                return Err(1);
                            }
                        }
                    }
                    _ => {
                        t_putstr("u-test: flow_parser_core: probe 7 not Heredoc FAILED\n");
                        return Err(1);
                    }
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_core: probe 7 not Pipeline FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 8 -- error case: postfix `?` followed by a non-terminator.
    {
        match parse("cmd ? arg") {
            Ok(_) => {
                t_putstr("u-test: flow_parser_core: probe 8 expected error FAILED\n");
                return Err(1);
            }
            Err(e) => {
                if !matches!(e.kind, ParseErrorKind::UnexpectedTokenAfterFailPropagate) {
                    t_putstr("u-test: flow_parser_core: probe 8 wrong ParseErrorKind FAILED\n");
                    return Err(1);
                }
            }
        }
    }

    // Bonus -- exercise the Word::Concat path so the ThylaAlloc heap
    // touches the Concat allocation.
    {
        let s = match parse("echo $a^$b") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_core: bonus Concat parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Simple(sc) => {
                    if sc.words.len() != 2 {
                        t_putstr("u-test: flow_parser_core: bonus words.len FAILED\n");
                        return Err(1);
                    }
                    match &sc.words[1] {
                        Word::Concat(parts) => {
                            if parts.len() != 2 {
                                t_putstr("u-test: flow_parser_core: bonus parts.len FAILED\n");
                                return Err(1);
                            }
                        }
                        _ => {
                            t_putstr("u-test: flow_parser_core: bonus not Concat FAILED\n");
                            return Err(1);
                        }
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_core: bonus not Simple FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_core: bonus not Pipeline FAILED\n");
                return Err(1);
            }
        }
    }

    t_putstr("u-test: parser core OK\n");
    Ok(())
}

// =============================================================================
// Flow 10 -- libutopia parser expression layer (U-5c)
// =============================================================================
//
// Exercises the U-5c expression parser end-to-end via parse() (which
// internally invokes parse_expr_tokens at each placeholder site) plus
// direct calls to parse_expr_tokens(). Validates that the AST now has
// ONE canonical shape: every expression slot carries an `Expr` (no
// more `Vec<Token>` placeholders).
//
// Heap-touching paths: every probe allocates Vec<Token> (lex output),
// `Box<Expr>` (BinOp/UnOp/Match/VarIndex/Subst children), `Vec<Expr>`
// (List, Concat). A regression in ThylaAlloc on any of these would
// surface here.
fn flow_parser_expr() -> Result<(), i64> {
    // Probe 1 -- arith literal lifts to Integer + Add.
    {
        let s = match parse("let n = (( 1 + 2 ))") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 1 parse FAILED\n");
                return Err(1);
            }
        };
        let v = match &s.statements[0].kind {
            StatementKind::Let(l) => &l.value.kind,
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 1 not Let FAILED\n");
                return Err(1);
            }
        };
        let arith_kind = match v {
            ExprKind::BinOp(BinOp::Add, l, r) => match (&l.kind, &r.kind) {
                (ExprKind::Integer(1), ExprKind::Integer(2)) => true,
                _ => false,
            },
            _ => false,
        };
        if !arith_kind {
            t_putstr("u-test: flow_parser_expr: probe 1 wrong arith shape FAILED\n");
            return Err(1);
        }
    }

    // Probe 2 -- explicit list literal in value position.
    {
        let s = match parse("let files = (a b c)") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 2 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Let(l) => match &l.value.kind {
                ExprKind::List(elems) => {
                    if elems.len() != 3 {
                        t_putstr("u-test: flow_parser_expr: probe 2 list len FAILED\n");
                        return Err(1);
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 2 not List FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 2 not Let FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 3 -- if condition lifts to BinOp(Eq).
    {
        let s = match parse("if ($x == 0) { echo zero }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 3 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::If(i) => match &i.cond.kind {
                ExprKind::BinOp(BinOp::Eq, _, _) => {}
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 3 wrong cond shape FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 3 not If FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 4 -- for list expression: `for (f in $files)` -> Var.
    {
        let s = match parse("for (f in $files) { cat $f }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 4 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::For(f) => match &f.list_expr.kind {
                ExprKind::Var(v) => {
                    if v != "files" {
                        t_putstr("u-test: flow_parser_expr: probe 4 var name FAILED\n");
                        return Err(1);
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 4 not Var FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 4 not For FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 5 -- VarIndex `$files(1)` lifts to VarIndex(name, Integer).
    {
        let s = match parse("let x = $files(1)") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 5 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Let(l) => match &l.value.kind {
                ExprKind::VarIndex(name, idx) => {
                    if name != "files" {
                        t_putstr("u-test: flow_parser_expr: probe 5 name FAILED\n");
                        return Err(1);
                    }
                    if !matches!(idx.kind, ExprKind::Integer(1)) {
                        t_putstr("u-test: flow_parser_expr: probe 5 idx FAILED\n");
                        return Err(1);
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 5 not VarIndex FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 5 not Let FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 6 -- concat `$a^$b` lifts to ExprKind::Concat.
    {
        let s = match parse("let x = $a^$b") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 6 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Let(l) => match &l.value.kind {
                ExprKind::Concat(parts) => {
                    if parts.len() != 2 {
                        t_putstr("u-test: flow_parser_expr: probe 6 concat len FAILED\n");
                        return Err(1);
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 6 not Concat FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 6 not Let FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 7 -- match operator `matches` in cond context.
    {
        let s = match parse("if ($file matches *.c) { echo C }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 7 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::If(i) => match &i.cond.kind {
                ExprKind::Match(MatchOp::Glob, _, _) => {}
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 7 not Match(Glob) FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 7 not If FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 8 -- substitution body lifts to a sub-Script.
    {
        let s = match parse("let x = $(echo hi)") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 8 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Let(l) => match &l.value.kind {
                ExprKind::Subst(sub_script) => {
                    if sub_script.statements.is_empty() {
                        t_putstr(
                            "u-test: flow_parser_expr: probe 8 empty sub-script FAILED\n",
                        );
                        return Err(1);
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_expr: probe 8 not Subst FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 8 not Let FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 9 -- direct parse_expr_tokens API: arith with paren grouping.
    // `(1 + 2) * 3` -> Mul(Add(1, 2), 3).
    {
        let src = "(1 + 2) * 3";
        let toks = match tokenize(src) {
            Ok(t) => t,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 9 tokenize FAILED\n");
                return Err(1);
            }
        };
        // Strip the trailing Eof so the expression parser sees only
        // body tokens (mirrors what parse.rs does at each placeholder
        // site -- the collected tokens don't include EOF).
        let body: Vec<_> = toks
            .into_iter()
            .filter(|t| !matches!(t.kind, TokenKind::Eof))
            .collect();
        let e = match parse_expr_tokens(body, src.len(), ExprContext::Arith) {
            Ok(e) => e,
            Err(_) => {
                t_putstr("u-test: flow_parser_expr: probe 9 parse FAILED\n");
                return Err(1);
            }
        };
        match &e.kind {
            ExprKind::BinOp(BinOp::Mul, l, r) => match (&l.kind, &r.kind) {
                (ExprKind::BinOp(BinOp::Add, _, _), ExprKind::Integer(3)) => {}
                _ => {
                    t_putstr(
                        "u-test: flow_parser_expr: probe 9 wrong grouping shape FAILED\n",
                    );
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: probe 9 not BinOp(Mul) FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 10 -- error case: arith literal that isn't an integer.
    {
        let src = "hello";
        let toks = tokenize(src).expect("lex ok");
        let body: Vec<_> = toks
            .into_iter()
            .filter(|t| !matches!(t.kind, TokenKind::Eof))
            .collect();
        match parse_expr_tokens(body, src.len(), ExprContext::Arith) {
            Ok(_) => {
                t_putstr("u-test: flow_parser_expr: probe 10 expected error FAILED\n");
                return Err(1);
            }
            Err(e) => {
                if !matches!(e.kind, ParseErrorKind::InvalidArithLiteral) {
                    t_putstr("u-test: flow_parser_expr: probe 10 wrong error FAILED\n");
                    return Err(1);
                }
            }
        }
    }

    // Bonus -- unary not in cond context.
    {
        let s = parse("if (! $x) { echo nope }").expect("parse ok");
        match &s.statements[0].kind {
            StatementKind::If(i) => match &i.cond.kind {
                ExprKind::UnOp(UnOp::Not, _) => {}
                _ => {
                    t_putstr("u-test: flow_parser_expr: bonus not UnOp FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_expr: bonus not If FAILED\n");
                return Err(1);
            }
        }
    }

    t_putstr("u-test: parser expr OK\n");
    Ok(())
}

// =====================================================================
// flow_parser_full -- U-5d: pattern matching + try/catch + trace +
// on/mask + case-as-expression. The U-5 parser arc closes here; this
// probe validates the final statement-level + expression-level
// surface end-to-end on the Thylacine target under ThylaAlloc.
// =====================================================================

fn flow_parser_full() -> Result<(), i64> {
    // Probe 1: case-statement basic shape.
    {
        let s = match parse("case $x { *.c => echo C ; *.rs => echo Rust ; * => echo other }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 1 parse FAILED\n");
                return Err(1);
            }
        };
        if s.statements.len() != 1 {
            t_putstr("u-test: flow_parser_full: probe 1 stmt count FAILED\n");
            return Err(1);
        }
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                if c.arms.len() != 3 {
                    t_putstr("u-test: flow_parser_full: probe 1 arm count FAILED\n");
                    return Err(1);
                }
                match &c.scrutinee.kind {
                    ExprKind::Var(v) if v == "x" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_full: probe 1 scrutinee FAILED\n");
                        return Err(1);
                    }
                }
                // Arm 1: single pattern `*.c`.
                if c.arms[0].patterns.len() != 1 {
                    t_putstr("u-test: flow_parser_full: probe 1 arm[0] pat count FAILED\n");
                    return Err(1);
                }
                match &c.arms[0].patterns[0].kind {
                    ExprKind::Word(w) if w == "*.c" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_full: probe 1 arm[0] pat shape FAILED\n");
                        return Err(1);
                    }
                }
                // Arm 2: catchall via `*` (still just Word("*")).
                match &c.arms[2].patterns[0].kind {
                    ExprKind::Word(w) if w == "*" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_full: probe 1 catchall FAILED\n");
                        return Err(1);
                    }
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_full: probe 1 not Case FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 2: multi-pattern arm `*.c *.h => body`.
    {
        let s = match parse("case $x { *.c *.h => echo C ; * => echo other }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 2 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                if c.arms[0].patterns.len() != 2 {
                    t_putstr("u-test: flow_parser_full: probe 2 multi-pat FAILED\n");
                    return Err(1);
                }
                match &c.arms[0].patterns[1].kind {
                    ExprKind::Word(w) if w == "*.h" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_full: probe 2 pat[1] shape FAILED\n");
                        return Err(1);
                    }
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_full: probe 2 not Case FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 3: case with brace-block arm body.
    {
        let s = match parse("case $x { *.c => { build $x ; install $x } }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 3 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Case(c) => match &c.arms[0].body.kind {
                StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                    CommandKind::BraceBlock(stmts) if stmts.len() == 2 => {}
                    _ => {
                        t_putstr("u-test: flow_parser_full: probe 3 brace body FAILED\n");
                        return Err(1);
                    }
                },
                _ => {
                    t_putstr("u-test: flow_parser_full: probe 3 body not Pipeline FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_full: probe 3 not Case FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 4: try / catch.
    {
        let s = match parse("try { risky ; cleanup } catch { recover }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 4 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Try(t) => {
                if t.body.len() != 2 || t.catch.len() != 1 {
                    t_putstr("u-test: flow_parser_full: probe 4 body/catch len FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_full: probe 4 not Try FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 5: try without catch -> error.
    {
        match parse("try { a }") {
            Ok(_) => {
                t_putstr("u-test: flow_parser_full: probe 5 should error FAILED\n");
                return Err(1);
            }
            Err(_) => {} // expected
        }
    }

    // Probe 6: trace block.
    {
        let s = match parse("trace { cmd1 ; cmd2 ; cmd3 }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 6 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Trace(t) => {
                if t.body.len() != 3 {
                    t_putstr("u-test: flow_parser_full: probe 6 body len FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_full: probe 6 not Trace FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 7: on note 'snare:int' { body }.
    {
        let s = match parse("on note 'snare:int' { cleanup ; echo done }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 7 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::OnNote(o) => {
                match &o.note_name.kind {
                    ExprKind::SingleQuoted(s) if s == "snare:int" => {}
                    _ => {
                        t_putstr("u-test: flow_parser_full: probe 7 name FAILED\n");
                        return Err(1);
                    }
                }
                if o.body.len() != 2 {
                    t_putstr("u-test: flow_parser_full: probe 7 body len FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_full: probe 7 not OnNote FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 8: mask note 'snare:stop' { body }.
    {
        let s = match parse("mask note 'snare:stop' { critical }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 8 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::MaskNote(m) => match &m.note_name.kind {
                ExprKind::SingleQuoted(s) if s == "snare:stop" => {}
                _ => {
                    t_putstr("u-test: flow_parser_full: probe 8 name FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_full: probe 8 not MaskNote FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 9: on without note keyword -> error.
    {
        match parse("on banana { a }") {
            Ok(_) => {
                t_putstr("u-test: flow_parser_full: probe 9 should error FAILED\n");
                return Err(1);
            }
            Err(e) => match e.kind {
                ParseErrorKind::UnexpectedToken { .. } => {}
                _ => {
                    t_putstr("u-test: flow_parser_full: probe 9 wrong error FAILED\n");
                    return Err(1);
                }
            },
        }
    }

    // Probe 10: case-as-expression via let.
    {
        let s = match parse("let kind = case $f { *.c => 'C' ; * => 'unknown' }") {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 10 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Let(l) => match &l.value.kind {
                ExprKind::Case(c) => {
                    if c.arms.len() != 2 {
                        t_putstr("u-test: flow_parser_full: probe 10 arm count FAILED\n");
                        return Err(1);
                    }
                    match &c.arms[0].value.kind {
                        ExprKind::SingleQuoted(s) if s == "C" => {}
                        _ => {
                            t_putstr(
                                "u-test: flow_parser_full: probe 10 arm[0] value FAILED\n",
                            );
                            return Err(1);
                        }
                    }
                }
                _ => {
                    t_putstr("u-test: flow_parser_full: probe 10 value not Case FAILED\n");
                    return Err(1);
                }
            },
            _ => {
                t_putstr("u-test: flow_parser_full: probe 10 not Let FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 11: newline-separated case arms (the more idiomatic
    // multi-line form).
    {
        let s = match parse(
            "case $f {\n  *.c => echo C\n  *.rs => echo Rust\n  * => echo other\n}",
        ) {
            Ok(s) => s,
            Err(_) => {
                t_putstr("u-test: flow_parser_full: probe 11 parse FAILED\n");
                return Err(1);
            }
        };
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                if c.arms.len() != 3 {
                    t_putstr("u-test: flow_parser_full: probe 11 arm count FAILED\n");
                    return Err(1);
                }
            }
            _ => {
                t_putstr("u-test: flow_parser_full: probe 11 not Case FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 12: empty case-pattern -> error.
    {
        match parse("case $x { => body }") {
            Ok(_) => {
                t_putstr("u-test: flow_parser_full: probe 12 should error FAILED\n");
                return Err(1);
            }
            Err(_) => {} // any error variant is acceptable
        }
    }

    t_putstr("u-test: parser full OK\n");
    Ok(())
}

// =============================================================================
// Flow 12 -- eval (U-6a): the recursive expression evaluator
// =============================================================================
//
// Validates libutopia::eval against the parser's Expr AST. This is
// the first runtime exercise of the eval module; the cfg(test)
// tests in eval/expr.rs cover the same shapes but can't run on
// host (no_std + no global allocator). The boot-time exercise
// runs them under ThylaAlloc, on real arm64.
//
// Coverage (15 probes):
//   1.  Integer atom in Arith context
//   2.  Arithmetic add + mul + precedence
//   3.  Div-by-zero error
//   4.  Var lookup (scalar)
//   5.  Undefined var -> empty Value
//   6.  VarLen on a list
//   7.  VarIndex 1-indexed
//   8.  VarIndex out-of-range -> empty
//   9.  VarSlice inclusive
//   10. VarNoSplit join-with-space
//   11. List literal flattening
//   12. Concat with var interpolation
//   13. DoubleQuoted interpolation ($var + $#var)
//   14. case-as-expression first-match-wins
//   15. Subst / Regex deferred -> NotImplemented
fn flow_eval_expr() -> Result<(), i64> {
    // Lex + parse + eval helper. The library exposes parse_expr_tokens
    // directly so the test can pin the context (Arith vs Value vs
    // Cond) -- which matters for atoms like "42" (Integer in Arith,
    // Word in Value). The trailing Eof token must be stripped before
    // calling parse_expr_tokens (parse.rs does the same when it
    // forwards a placeholder body) -- otherwise the parser reports
    // TrailingTokensInExpr.
    fn ev(env: &Env, s: &str, ctx: ExprContext) -> Result<Value, libutopia::eval::EvalError> {
        let toks = tokenize(s).map_err(|_| {
            libutopia::eval::EvalError::new(
                EvalErrorKind::Internal("test lex failed"),
                libutopia::parser::Span::new(0, 0),
            )
        })?;
        let body: Vec<_> = toks
            .into_iter()
            .filter(|t| !matches!(t.kind, TokenKind::Eof))
            .collect();
        let e = parse_expr_tokens(body, s.len(), ctx).map_err(|_| {
            libutopia::eval::EvalError::new(
                EvalErrorKind::Internal("test parse failed"),
                libutopia::parser::Span::new(0, 0),
            )
        })?;
        eval_expr(env, &e)
    }

    // Probe 1: integer atom in Arith.
    {
        let env = Env::new();
        let v = match ev(&env, "42", ExprContext::Arith) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 1 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "42" {
            t_putstr("u-test: flow_eval_expr: probe 1 value FAILED\n");
            return Err(1);
        }
    }

    // Probe 2: arithmetic add + mul.
    {
        let env = Env::new();
        let v = match ev(&env, "1 + 2 * 3", ExprContext::Arith) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 2 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "7" {
            t_putstr("u-test: flow_eval_expr: probe 2 precedence FAILED\n");
            return Err(1);
        }
    }

    // Probe 3: div-by-zero.
    {
        let env = Env::new();
        match ev(&env, "1 / 0", ExprContext::Arith) {
            Ok(_) => {
                t_putstr("u-test: flow_eval_expr: probe 3 should error FAILED\n");
                return Err(1);
            }
            Err(e) if e.kind == EvalErrorKind::DivByZero => {}
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 3 wrong error FAILED\n");
                return Err(1);
            }
        }
    }

    // Probe 4: var lookup (scalar).
    {
        let mut env = Env::new();
        env.let_set("x", Value::scalar("hello"));
        let v = match ev(&env, "$x", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 4 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "hello" {
            t_putstr("u-test: flow_eval_expr: probe 4 value FAILED\n");
            return Err(1);
        }
    }

    // Probe 5: undefined var is empty.
    {
        let env = Env::new();
        let v = match ev(&env, "$nope", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 5 eval FAILED\n");
                return Err(1);
            }
        };
        if !v.is_empty() {
            t_putstr("u-test: flow_eval_expr: probe 5 should be empty FAILED\n");
            return Err(1);
        }
    }

    // Probe 6: VarLen on a 3-element list.
    {
        let mut env = Env::new();
        let mut xs = Vec::new();
        xs.push(alloc::string::String::from("a"));
        xs.push(alloc::string::String::from("b"));
        xs.push(alloc::string::String::from("c"));
        env.let_set("xs", Value::list(xs));
        let v = match ev(&env, "$#xs", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 6 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "3" {
            t_putstr("u-test: flow_eval_expr: probe 6 length FAILED\n");
            return Err(1);
        }
    }

    // Probe 7: VarIndex (1-indexed) returns second element.
    {
        let mut env = Env::new();
        let mut xs = Vec::new();
        xs.push(alloc::string::String::from("a"));
        xs.push(alloc::string::String::from("b"));
        xs.push(alloc::string::String::from("c"));
        env.let_set("xs", Value::list(xs));
        let v = match ev(&env, "$xs(2)", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 7 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "b" {
            t_putstr("u-test: flow_eval_expr: probe 7 index FAILED\n");
            return Err(1);
        }
    }

    // Probe 8: VarIndex out-of-range -> empty.
    {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec![alloc::string::String::from("a")]));
        let v = match ev(&env, "$xs(99)", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 8 eval FAILED\n");
                return Err(1);
            }
        };
        if !v.is_empty() {
            t_putstr("u-test: flow_eval_expr: probe 8 should be empty FAILED\n");
            return Err(1);
        }
    }

    // Probe 9: VarSlice inclusive.
    {
        let mut env = Env::new();
        let mut xs = Vec::new();
        xs.push(alloc::string::String::from("a"));
        xs.push(alloc::string::String::from("b"));
        xs.push(alloc::string::String::from("c"));
        xs.push(alloc::string::String::from("d"));
        env.let_set("xs", Value::list(xs));
        let v = match ev(&env, "$xs(2 3)", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 9 eval FAILED\n");
                return Err(1);
            }
        };
        if v.0.len() != 2 || v.0[0] != "b" || v.0[1] != "c" {
            t_putstr("u-test: flow_eval_expr: probe 9 slice FAILED\n");
            return Err(1);
        }
    }

    // Probe 10: VarNoSplit collapses list -> "a b c d".
    {
        let mut env = Env::new();
        let mut xs = Vec::new();
        xs.push(alloc::string::String::from("a"));
        xs.push(alloc::string::String::from("b c"));
        xs.push(alloc::string::String::from("d"));
        env.let_set("args", Value::list(xs));
        let v = match ev(&env, "$\"args", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 10 eval FAILED\n");
                return Err(1);
            }
        };
        if v.0.len() != 1 || v.0[0] != "a b c d" {
            t_putstr("u-test: flow_eval_expr: probe 10 join FAILED\n");
            return Err(1);
        }
    }

    // Probe 11: List literal flattening.
    {
        let env = Env::new();
        let v = match ev(&env, "(a b c)", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 11 eval FAILED\n");
                return Err(1);
            }
        };
        if v.0.len() != 3 || v.0[0] != "a" || v.0[2] != "c" {
            t_putstr("u-test: flow_eval_expr: probe 11 list FAILED\n");
            return Err(1);
        }
    }

    // Probe 12: Concat with var interp.
    {
        let mut env = Env::new();
        env.let_set("x", Value::scalar("42"));
        let v = match ev(&env, "pre-^$x^-post", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 12 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "pre-42-post" {
            t_putstr("u-test: flow_eval_expr: probe 12 concat FAILED\n");
            return Err(1);
        }
    }

    // Probe 13: DoubleQuoted interpolation -- $var and $#var both.
    {
        let mut env = Env::new();
        env.let_set("user", Value::scalar("alice"));
        let mut xs = Vec::new();
        xs.push(alloc::string::String::from("a"));
        xs.push(alloc::string::String::from("b"));
        env.let_set("xs", Value::list(xs));
        let v = match ev(&env, "\"hello $user; count=$#xs\"", ExprContext::Value) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 13 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "hello alice; count=2" {
            t_putstr("u-test: flow_eval_expr: probe 13 interp FAILED\n");
            return Err(1);
        }
    }

    // Probe 14: case-as-expression matches the *.c arm.
    {
        let mut env = Env::new();
        env.let_set("f", Value::scalar("foo.c"));
        let v = match ev(
            &env,
            "case $f { *.c => 'C source' ; *.rs => 'Rust source' ; * => 'unknown' }",
            ExprContext::Value,
        ) {
            Ok(v) => v,
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 14 eval FAILED\n");
                return Err(1);
            }
        };
        if v.as_scalar() != "C source" {
            t_putstr("u-test: flow_eval_expr: probe 14 arm FAILED\n");
            return Err(1);
        }
    }

    // Probe 15: $(cmd) substitution + =~ regex -> NotImplemented.
    {
        let env = Env::new();
        match ev(&env, "$(echo hi)", ExprContext::Value) {
            Ok(_) => {
                t_putstr("u-test: flow_eval_expr: probe 15 subst should error FAILED\n");
                return Err(1);
            }
            Err(e) if matches!(e.kind, EvalErrorKind::NotImplemented(_)) => {}
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 15 wrong subst error FAILED\n");
                return Err(1);
            }
        }
        let mut env = Env::new();
        env.let_set("v", Value::scalar("foo"));
        match ev(&env, "($v =~ /^foo/)", ExprContext::Cond) {
            Ok(_) => {
                t_putstr("u-test: flow_eval_expr: probe 15 regex should error FAILED\n");
                return Err(1);
            }
            Err(e) if matches!(e.kind, EvalErrorKind::NotImplemented(_)) => {}
            Err(_) => {
                t_putstr("u-test: flow_eval_expr: probe 15 wrong regex error FAILED\n");
                return Err(1);
            }
        }
    }

    t_putstr("u-test: eval expr OK\n");
    Ok(())
}
