// /bin/kaua-term-probe -- the KT-1.5a transport boot-prove.
//
// The kaua-term process + the seam wire codec are host-tested, but the
// PROCESS-level transport (the pts host, the two blocking threads, the codec
// over a real pipe) is not host-testable. This probe proves it at boot: spawn a
// kaua-term on a pipe pair, drain its UP channel with a blocking `t_read` --
// Loom cannot read a pipe (a Loom read needs a dev9p handle, kernel/loom.c:1198;
// halcyond's own drain is a `poll(2)` over the pipes, HALCYON 14.11.7) -- decode
// the record stream, and assert the hosted program's output + clean exit
// arrived. joey spawns + reaps + asserts exit 0 + the "kaua-term-probe: PASS"
// marker (THYLA_BOOT_PROBES). This is the transport half of KT-1.5; the halcyond
// ingest (grid + scrollback) + multi-tile multiplex is KT-1.5b/1.5c (HALCYON 14.11).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use kaua_term::wire::{parse_record, FrameDecoder};
use kaua_term::{Control, Record};
use libthyla_rs::process::{pipe, Command, Stdio};
use libthyla_rs::{t_putstr, t_read, t_wait_pid_for};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

const MARKER: &str = "probe-kt15a";

fn fail(msg: &str) -> i64 {
    t_putstr(msg);
    1
}

fn run() -> i64 {
    // The two seam channels as pipes (halcyond owns them at KT-1.5c). up = the
    // kaua-term's records to us; down = our input to it (unused here -- echo
    // needs none; the kaua-term exits on its child's exit regardless).
    let (up_rd, up_wr) = match pipe() {
        Ok(p) => p,
        Err(_) => return fail("kaua-term-probe: FAIL -- pipe(up)\n"),
    };
    let (down_rd, _down_wr) = match pipe() {
        Ok(p) => p,
        Err(_) => return fail("kaua-term-probe: FAIL -- pipe(down)\n"),
    };

    // Spawn the kaua-term hosting `echo <marker>` on its internal pts. fd 0 =
    // down (read), fd 1 = up (write); the kaua-term's app never sees these
    // (spawn installs only the pts slaves). The parent copies of down_rd + up_wr
    // are consumed by the spawn (closed here on return).
    let mut cmd = Command::new("/bin/kaua-term");
    cmd.arg("40");
    cmd.arg("10");
    cmd.arg("/bin/echo");
    cmd.arg(MARKER);
    cmd.stdin(Stdio::File(down_rd));
    cmd.stdout(Stdio::File(up_wr));
    let child = match cmd.spawn() {
        Ok(c) => c,
        Err(_) => return fail("kaua-term-probe: FAIL -- spawn kaua-term\n"),
    };
    let pid = child.pid();

    // Drain the UP channel with a blocking pipe read until EOF. A blocking
    // `t_read` on a pipe returns >0 with data, or 0 when every write end is
    // closed -- and the kaua-term's fd 1 (the up pipe's write end) closes when
    // the Proc terminates (the #926/#68 close-handles-at-exit path delivers the
    // EOF at termination, not at reap). The kaua-term is multi-thread (output +
    // input); when its hosted echo exits, the output thread returns -> SYS_EXITS
    // -> exits_code sees the input thread as a live peer -> proc_group_terminate
    // cascade (#811) death-interrupts it, so BOTH threads exit and fd 1 closes.
    // We keep _down_wr open so that input thread stays blocked on fd 0 (closing
    // it would EOF fd 0 and tear the kaua-term down before echo ran).
    let rawfd: i64 = up_rd.as_raw_fd() as i64;
    let mut dec = FrameDecoder::new();
    let mut seen = String::new(); // accumulated CellDiff glyphs
    let mut exit_code: Option<i32> = None;
    let mut buf = [0u8; 4096];
    loop {
        // SAFETY: SVC wrapper over this function's own stack buffer.
        let n = unsafe { t_read(rawfd, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            break; // EOF (kaua-term terminated, fd 1 closed) or error
        }
        dec.push(&buf[..n as usize]);
        loop {
            match dec.next_frame() {
                Some(Ok((tag, payload))) => {
                    if let Ok(rec) = parse_record(tag, &payload) {
                        match rec {
                            Record::CellDiff { changed, .. } => {
                                for (_, _, cell) in changed {
                                    seen.push(cell.ch);
                                }
                            }
                            Record::Control(Control::Exit(c)) => exit_code = Some(c),
                            _ => {}
                        }
                    }
                }
                Some(Err(_)) => return fail("kaua-term-probe: FAIL -- wire decode error\n"),
                None => break,
            }
        }
    }

    // Reap the kaua-term.
    let mut st: i32 = 0;
    let reaped = unsafe { t_wait_pid_for(pid, 0, &mut st as *mut i32) };
    if reaped != pid as i64 {
        return fail("kaua-term-probe: FAIL -- reap wrong pid\n");
    }

    // Verify: the hosted echo's output rode the CellDiffs, and the exit was clean.
    if !seen.contains(MARKER) {
        t_putstr("kaua-term-probe: FAIL -- marker not in the cell stream\n");
        return 1;
    }
    match exit_code {
        Some(0) => {
            t_putstr("kaua-term-probe: PASS -- transport + bin + codec over the pipe\n");
            0
        }
        _ => fail("kaua-term-probe: FAIL -- no clean Control::Exit(0)\n"),
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
