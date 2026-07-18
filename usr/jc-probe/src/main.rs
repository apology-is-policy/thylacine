// /bin/jc-probe -- the PTY-4 job-control E2E (boot-fatal via joey).
//
// The DRIVER plays the human at a terminal: mint a pts, host a REAL `ut` on
// the slave (the ptyhost shape, minus the pumps -- the probe IS the outer
// terminal), and script the job-control ladder against the master.
//
// The stoppable foreground job is `sleep` (NOT `cat`): a v1.0 pts has no
// foreground-read arbitration (TTIN), so a stopped job holding an outstanding
// terminal READ would steal the shell's subsequent input off the shared pts
// slave. `sleep` blocks in the timer, holds no terminal read, and stops/
// resumes cleanly -- so it exercises the whole stop/report/resume machinery
// without depending on TTIN (the documented next PTY sub-chunk; see
// docs/reference/136-ptyfs.md "Known caveats"). Interactive `cat`-under-^Z
// lands with TTIN.
//
// The matching discipline: the ut LINE EDITOR redraws the prompt on every
// keystroke, so the master stream is full of escape noise WHILE ut is at its
// prompt -- but a foreground EXTERNAL's OUTPUT window is clean (ut is blocked
// in its stop-aware wait). So the run/int legs match an UPPERCASE token
// emitted only by a `| tr a-z A-Z` pipeline (never in the lowercase editor
// echo -- the LS-CI idiom), and the stop/jobs legs match the `Stopped` line
// (produced only by ut's job-control report, never by a typed line).
//
// Ladder:
//   run    `echo runok | tr a-z A-Z` -> RUNOK  (a clean fg external + pipeline
//          under jc: own pgrp + terminal handoff + the stop-aware wait, clean)
//   stop   foreground `sleep 30`; `^Z` (0x1a) -> tty:susp -> the kernel stops
//          sleep's group -> ut's WAIT_UNTRACED report -> `[1]+ Stopped` on pts
//   jobs   the listing shows the job Stopped
//   fg1    `fg` resumes (terminal + SYS_TTY_CONT); a second `^Z` re-stops it
//          -- a re-stop is the RESUME proof (a still-stopped job cannot stop
//          again; only a running one can)
//   int    `fg` resumes again; `^C` (0x03) -> interrupt to sleep's group ->
//          sleep terminates -> a live prompt; `echo intok | tr` -> INTOK
//          re-proves the shell owns the terminal
//   exit   `exit` -> drain-then-EOF + a clean reap.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;
use core::time::Duration;
use libthyla_rs::fs::OpenOptions;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{
    t_close, t_fstat, t_open, t_putstr, t_read, t_wait_pid_for, t_write, T_ORDWR,
    T_WALK_OPEN_FROM_ROOT,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// The ptyfs endpoint-qid contract: PTS_FLAG | N<<8 | filekind (1 = master).
const PTS_FLAG: u64 = 1 << 40;

/// The ut prompt tail: RIGHT TACK (U+22A2) -- see Repl::prompt.
const PROMPT: &[u8] = b"\xe2\x8a\xa2";

/// Per-leg read bound (each read blocks for >= 1 byte; the boot timeout is the
/// hang backstop, this caps a chatty-but-wrong stream).
const MAX_READS_PER_LEG: usize = 512;

/// Settle after a command line that foregrounds a job, before signalling it.
/// ut must read the accepted line, spawn the job, place it in a pgrp, and hand
/// it the terminal before the signal lands -- else a `^Z` reaches ut (which
/// ignores its own susp) instead of the job. A deliberate settle is the
/// standard PTY-harness synchronization (expect / LS-CI `sleep`); generous so
/// it holds under load.
const SETTLE: Duration = Duration::from_millis(400);

fn settle() {
    let _ = libthyla_rs::time::sleep(SETTLE);
}

fn open_rdwr(path: &str) -> i64 {
    // SAFETY: t_open is the SYS_OPEN SVC wrapper; path is a valid byte slice.
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_ORDWR) }
}

fn occurrences(hay: &[u8], needle: &[u8]) -> usize {
    if needle.is_empty() || hay.len() < needle.len() {
        return 0;
    }
    let mut c = 0;
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if &hay[i..i + needle.len()] == needle {
            c += 1;
            i += needle.len();
        } else {
            i += 1;
        }
    }
    c
}

struct Driver {
    mfd: i64,
    acc: Vec<u8>,
}

impl Driver {
    fn send(&self, b: &[u8], leg: &str) -> bool {
        // SAFETY: SVC wrapper; b is a valid slice, mfd the live master.
        let ok = unsafe { t_write(self.mfd, b.as_ptr(), b.len()) } == b.len() as i64;
        if !ok {
            t_putstr("jc-probe: FAIL send at leg ");
            t_putstr(leg);
            t_putstr("\n");
        }
        ok
    }

    /// Read the master until `needle` has appeared `count` times in the bytes
    /// accumulated SINCE `mark`. Bounded by MAX_READS_PER_LEG.
    fn read_until(&mut self, mark: usize, needle: &[u8], count: usize, leg: &str) -> bool {
        let mut reads = 0;
        loop {
            if occurrences(&self.acc[mark..], needle) >= count {
                return true;
            }
            if reads >= MAX_READS_PER_LEG {
                t_putstr("jc-probe: FAIL (marker never arrived) at leg ");
                t_putstr(leg);
                t_putstr("\n");
                return false;
            }
            let mut b = [0u8; 512];
            // SAFETY: SVC wrapper over this fn's stack buffer.
            let n = unsafe { t_read(self.mfd, b.as_mut_ptr(), b.len()) };
            if n <= 0 {
                t_putstr("jc-probe: FAIL (master EOF) at leg ");
                t_putstr(leg);
                t_putstr("\n");
                return false;
            }
            self.acc.extend_from_slice(&b[..n as usize]);
            reads += 1;
        }
    }

    /// Read until EOF (the exit leg). True iff EOF arrived within bounds.
    fn read_to_eof(&mut self, leg: &str) -> bool {
        let mut reads = 0;
        loop {
            if reads >= MAX_READS_PER_LEG {
                t_putstr("jc-probe: FAIL (no EOF) at leg ");
                t_putstr(leg);
                t_putstr("\n");
                return false;
            }
            let mut b = [0u8; 512];
            // SAFETY: SVC wrapper over this fn's stack buffer.
            let n = unsafe { t_read(self.mfd, b.as_mut_ptr(), b.len()) };
            if n <= 0 {
                return true;
            }
            self.acc.extend_from_slice(&b[..n as usize]);
            reads += 1;
        }
    }

    fn mark(&self) -> usize {
        self.acc.len()
    }
}

fn run() -> i64 {
    let mfd = open_rdwr("/dev/pts/ptmx");
    if mfd < 0 {
        t_putstr("jc-probe: FAIL open(/dev/pts/ptmx)\n");
        return 2;
    }
    let mut st = [0u8; 80];
    // SAFETY: SVC wrapper; st is a valid 80-byte t_stat buffer.
    if unsafe { t_fstat(mfd, st.as_mut_ptr()) } != 0 {
        t_putstr("jc-probe: FAIL fstat(master)\n");
        return 2;
    }
    let mut q = [0u8; 8];
    q.copy_from_slice(&st[8..16]);
    let qid = u64::from_le_bytes(q);
    if qid & PTS_FLAG == 0 || (qid & 0xff) != 1 {
        t_putstr("jc-probe: FAIL ptmx qid decode\n");
        return 2;
    }
    let n = (qid >> 8) & 0xff_ffff;

    // Host a real ut on the slave (three Files, one per stdio slot).
    let slave_path = format!("/dev/pts/{}", n);
    let mut slaves = Vec::new();
    for _ in 0..3 {
        match OpenOptions::new().read(true).write(true).open(&slave_path) {
            Ok(f) => slaves.push(f),
            Err(_) => {
                t_putstr("jc-probe: FAIL open(slave)\n");
                return 2;
            }
        }
    }
    let s2 = slaves.pop().unwrap();
    let s1 = slaves.pop().unwrap();
    let s0 = slaves.pop().unwrap();
    let mut cmd = Command::new("/bin/ut");
    cmd.stdin(Stdio::File(s0));
    cmd.stdout(Stdio::File(s1));
    cmd.stderr(Stdio::File(s2));
    let child = match cmd.spawn() {
        Ok(c) => c,
        Err(_) => {
            t_putstr("jc-probe: FAIL spawn(/bin/ut)\n");
            return 2;
        }
    };
    let pid = child.pid();

    let mut d = Driver {
        mfd,
        acc: Vec::new(),
    };

    // (a) The hosted shell's first prompt = the session dance landed and the
    //     pts is in PROMPT mode (raw, -icrnl: `\r` submits).
    if !d.read_until(0, PROMPT, 1, "first-prompt") {
        return 3;
    }

    // (b) A clean foreground run under jc via a pipeline. The uppercase RUNOK
    //     appears only in `tr`'s output, never in the (lowercase) editor echo.
    let m = d.mark();
    if !d.send(b"echo runok | tr a-z A-Z\r", "run") || !d.read_until(m, b"RUNOK", 1, "run") {
        return 4;
    }
    if !d.read_until(m, PROMPT, 1, "run-prompt") {
        return 4;
    }

    // (c) Foreground `sleep 1` + `^Z`. Settle so sleep is spawned + owns the
    //     terminal before the susp. The stop report + the reclaimed prompt. A
    //     short sleep so the deadline has elapsed by the time `fg` resumes it
    //     (leg e) -- the resume then runs the job to a clean, prompt exit.
    if !d.send(b"sleep 1\r", "sleep") {
        return 5;
    }
    settle();
    let stop_mark = d.mark();
    if !d.send(&[0x1a], "stop") || !d.read_until(stop_mark, b"Stopped", 1, "stop") {
        return 5;
    }
    if !d.read_until(stop_mark, PROMPT, 1, "stop-prompt") {
        return 5;
    }

    // (d) `jobs` lists it Stopped.
    let m = d.mark();
    if !d.send(b"jobs\r", "jobs") || !d.read_until(m, b"Stopped", 1, "jobs") {
        return 6;
    }
    if !d.read_until(m, PROMPT, 1, "jobs-prompt") {
        return 6;
    }

    // (e) `fg` resumes the stopped sleep (terminal + SYS_TTY_CONT) and waits.
    //     The resume is the PROOF: `fg` returning to a fresh prompt means cont
    //     woke sleep, sleep ran to completion, and the stop-aware wait reaped
    //     it (Done). A resume that did nothing would leave sleep stopped and
    //     `fg` would block on the wait forever -- so the prompt IS the proof.
    if !d.send(b"fg\r", "fg") {
        return 7;
    }
    if !d.read_until(d.mark(), PROMPT, 1, "fg-prompt") {
        return 7;
    }

    // (f) The shell reclaimed the terminal and still runs commands (INTOK,
    //     output-only again -- the shell-owns-the-terminal invariant post-jc).
    let m = d.mark();
    if !d.send(b"echo intok | tr a-z A-Z\r", "post-fg")
        || !d.read_until(m, b"INTOK", 1, "post-fg")
    {
        return 8;
    }

    // (g) `exit` unwinds the session; the master EOFs; the reap is clean.
    if !d.send(b"exit\r", "exit") || !d.read_to_eof("exit") {
        return 9;
    }
    let mut status: i32 = -1;
    // SAFETY: SVC wrapper; &mut status is a valid writable i32.
    let reaped = unsafe { t_wait_pid_for(pid, 0, &mut status as *mut i32) };
    let _ = unsafe { t_close(mfd) };
    if reaped != pid as i64 || status != 0 {
        t_putstr("jc-probe: FAIL (hosted ut exit status)\n");
        return 9;
    }

    t_putstr("jc-probe: PASS (run/stop/jobs/fg-resume/exit over a hosted ut)\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
