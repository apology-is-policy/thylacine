// /bin/kaua-term-probe -- the KT-1.5a transport boot-prove.
//
// The kaua-term process + the seam wire codec are host-tested, but the
// PROCESS-level transport (the pts host, the two blocking threads, the codec
// over a real pipe) is not host-testable. This probe proves it at boot, the way
// halcyond will drive it (KT-1.5c): spawn a kaua-term on a pipe pair, read its
// UP channel over a Loom ring (the H-3c-2 pattern -- Loom read on a pipe fd),
// decode the record stream, and assert the hosted program's output + clean exit
// arrived. joey spawns + reaps + asserts exit 0 + the "kaua-term-probe: PASS"
// marker (THYLA_BOOT_PROBES). This is the transport half of KT-1.5; the halcyond
// spawn + multiplex + grid/scrollback ingest is KT-1.5b/1.5c (HALCYON 14.11).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use kaua_term::wire::{parse_record, FrameDecoder};
use kaua_term::{Control, Record};
use libthyla_rs::loom::{RegisteredBuffer, Ring, Sqe};
use libthyla_rs::process::{pipe, Command, Stdio};
use libthyla_rs::{t_putstr, t_wait_pid_for};

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

    // Read the UP channel over a Loom ring: register the pipe read end as a
    // handle, then read into a registered buffer until EOF (the kaua-term closes
    // fd 1 when it exits).
    let ring = match Ring::setup(8, 0) {
        Ok(r) => r,
        Err(_) => return fail("kaua-term-probe: FAIL -- Ring::setup\n"),
    };
    let mut buf = match RegisteredBuffer::new(65536) {
        Ok(b) => b,
        Err(_) => return fail("kaua-term-probe: FAIL -- RegisteredBuffer::new\n"),
    };
    if ring.register_buffers(&[buf.buf_reg()]).is_err() {
        return fail("kaua-term-probe: FAIL -- register_buffers\n");
    }
    if ring.register_handles(&[up_rd.as_raw_fd()]).is_err() {
        return fail("kaua-term-probe: FAIL -- register_handles\n");
    }

    let mut dec = FrameDecoder::new();
    let mut seen = String::new(); // accumulated CellDiff glyphs
    let mut exit_code: Option<i32> = None;
    let cap = buf.len() as u32;
    loop {
        let cqe = match ring.submit_one_wait(&Sqe::read(0, 0, cap, 0, 0, 1)) {
            Ok(c) => c,
            Err(_) => return fail("kaua-term-probe: FAIL -- Loom read submit\n"),
        };
        let n = cqe.result;
        if n <= 0 {
            break; // EOF (kaua-term closed fd 1 on exit) or error
        }
        dec.push(&buf.as_mut_slice()[..n as usize]);
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
            t_putstr("kaua-term-probe: PASS -- transport + bin + codec over the ring\n");
            0
        }
        _ => fail("kaua-term-probe: FAIL -- no clean Control::Exit(0)\n"),
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
