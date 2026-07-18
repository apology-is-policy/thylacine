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
// without depending on TTIN (the documented follow-up; see
// docs/reference/136-ptyfs.md "Known caveats"). The sleep is LONG (30 s, ended
// by `^C`, never by its deadline): a short sleep's wall deadline keeps running
// WHILE the job is stopped, so the stop window and the post-resume window both
// race the expiry -- the PTY-4e F2 right edge, and the repro artifact that
// originally masked the re-stop leg (task #19).
//
// The matching discipline: the ut LINE EDITOR redraws the prompt on every
// keystroke, so the master stream is full of escape noise WHILE ut is at its
// prompt -- but a foreground EXTERNAL's OUTPUT window is clean (ut is blocked
// in its stop-aware wait). So the run/health legs match an UPPERCASE token
// emitted only by a `| tr a-z A-Z` pipeline (never in the lowercase editor
// echo -- the LS-CI idiom); the stop legs match the `Stopped` line (produced
// only by ut's job-control report -- signal chars are consumed, never echoed);
// the fg legs match the job command line `bi_fg` echoes before the handoff.
//
// Every silent-hang mode (a mis-routed signal -> a marker that never comes ->
// `t_read` parks forever) is converted into a named FAIL by the watchdog
// thread (PTY-4e F2): normal completion is a couple of seconds; the watchdog
// exit_groups(3) with a message after WATCHDOG_SECS.
//
// Ladder:
//   run    `echo runok | tr a-z A-Z` -> RUNOK  (a clean fg external + pipeline
//          under jc: own pgrp + terminal handoff + the stop-aware wait, clean)
//   stop   foreground `sleep 30`; `^Z` (0x1a) -> tty:susp -> the kernel stops
//          sleep's group -> ut's WAIT_UNTRACED report -> `[1]+ Stopped` on pts
//   jobs   the listing shows the job Stopped
//   restop `fg` resumes (terminal + SYS_TTY_CONT; the `sleep 30` echo); a
//          second `^Z` re-stops it -- the re-stop IS the resume proof (only a
//          RUNNING job can stop again) and the task-#19 decisive leg
//   bg     `bg` resumes the re-stopped job in the background (the `&` line;
//          no terminal handoff -- the job sleeps on, reading nothing)
//   int    `fg` foregrounds the RUNNING job (harmless cont); `^C` (0x03) ->
//          interrupt -> sleep default-terminates (LS-5) -> a live prompt.
//          Post-resume signal delivery, the F4 leg -- no deadline reliance.
//   health `echo fgok | tr a-z A-Z` -> FGOK (the shell owns the terminal and
//          still runs pipelines after the whole stop/resume cycle)
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
    t_burrow_attach, t_close, t_exit_group, t_fstat, t_open, t_putstr, t_read, t_wait_pid_for,
    t_write, thread, T_ORDWR, T_WALK_OPEN_FROM_ROOT,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// The ptyfs endpoint-qid contract: PTS_FLAG | N<<8 | filekind (1 = master).
const PTS_FLAG: u64 = 1 << 40;

/// The ut prompt tail: RIGHT TACK (U+22A2) -- see Repl::prompt.
const PROMPT: &[u8] = b"\xe2\x8a\xa2";

/// Per-leg read bound (each read blocks for >= 1 byte; the watchdog is the
/// silent-hang backstop, this caps a chatty-but-wrong stream).
const MAX_READS_PER_LEG: usize = 512;

/// Settle after a command line that foregrounds a job, before signalling it.
/// ut must read the accepted line, spawn the job, place it in a pgrp, and hand
/// it the terminal before the signal lands -- else a `^Z`/`^C` reaches ut
/// (self-managing: note-only) instead of the job, the job runs on, the marker
/// never arrives, and the watchdog names the leg. The settle is CNTVCT
/// (wall-anchored) while guest progress is CPU-bound, so the margin is NOT
/// dilation-invariant -- generous, and the watchdog converts the residual.
const SETTLE: Duration = Duration::from_millis(400);

/// The silent-hang watchdog (PTY-4e F2): a mis-routed signal leaves the ladder
/// blocked in `t_read` with no further bytes ever arriving -- ptyfs is
/// deliberately non-QTPOLL, so no read timeout exists. Normal completion is a
/// couple of seconds; 25 s covers heavy-host dilation while still converting a
/// hang into a clean named FAIL well inside the boot timeout.
const WATCHDOG_SECS: u64 = 25;
const WATCHDOG_STACK: u64 = 32 * 1024;

extern "C" fn watchdog_main(_arg: u64) {
    let _ = libthyla_rs::time::sleep(Duration::from_secs(WATCHDOG_SECS));
    t_putstr("jc-probe: FAIL (watchdog -- a leg hung silently)\n");
    // SAFETY: `!`-returning SVC.
    unsafe { t_exit_group(3) }
}

fn settle() {
    let _ = libthyla_rs::time::sleep(SETTLE);
}

/// Per-leg breadcrumb: with silent-hang legs bounded only by the watchdog,
/// the UART trail is what names the hung leg.
fn leg(name: &str) {
    t_putstr("jc-probe: leg ");
    t_putstr(name);
    t_putstr("\n");
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
    /// accumulated SINCE `mark`. Bounded by MAX_READS_PER_LEG (chatty streams)
    /// + the watchdog (silent ones).
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
    // The watchdog first: every later hang mode becomes a named FAIL. A
    // spawn failure degrades to the boot-timeout backstop (reported).
    let wd_stack = unsafe { t_burrow_attach(WATCHDOG_STACK) };
    if wd_stack < 0
        || unsafe {
            thread::spawn_raw(
                watchdog_main as *const () as u64,
                wd_stack as u64 + WATCHDOG_STACK,
                0,
                0,
            )
        }
        .is_err()
    {
        t_putstr("jc-probe: watchdog spawn failed (boot timeout is the backstop)\n");
    }

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
    leg("first-prompt");
    if !d.read_until(0, PROMPT, 1, "first-prompt") {
        return 3;
    }

    // (b) A clean foreground run under jc via a pipeline. The uppercase RUNOK
    //     appears only in `tr`'s output, never in the (lowercase) editor echo.
    leg("run");
    let m = d.mark();
    if !d.send(b"echo runok | tr a-z A-Z\r", "run") || !d.read_until(m, b"RUNOK", 1, "run") {
        return 4;
    }
    if !d.read_until(m, PROMPT, 1, "run-prompt") {
        return 4;
    }

    // (c) Foreground `sleep 30` + `^Z`. Settle so sleep is spawned + owns the
    //     terminal before the susp lands. 30 s: the deadline can never expire
    //     inside the ladder, so the stop window has no right edge and the
    //     later resumes prove RESUMPTION, never expiry (`^C` ends the job).
    leg("stop");
    if !d.send(b"sleep 30\r", "sleep") {
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
    leg("jobs");
    let m = d.mark();
    if !d.send(b"jobs\r", "jobs") || !d.read_until(m, b"Stopped", 1, "jobs") {
        return 6;
    }
    if !d.read_until(m, PROMPT, 1, "jobs-prompt") {
        return 6;
    }

    // (e) THE RE-STOP LEG (task #19's decisive experiment): `fg` resumes the
    //     stopped sleep (terminal handoff + SYS_TTY_CONT; bi_fg echoes the
    //     job's command line first -- the resume-in-progress marker), then a
    //     second `^Z` must stop it AGAIN. Only a RUNNING job can stop, so the
    //     second `Stopped` report is simultaneously the resume proof and the
    //     re-stop proof. (The original sleep-1 ladder could never see this:
    //     the deadline expired while stopped, so the resumed job exited
    //     before the second ^Z -- the artifact behind the #19 report.)
    leg("restop");
    let m = d.mark();
    if !d.send(b"fg\r", "restop-fg") || !d.read_until(m, b"sleep 30", 1, "restop-fg") {
        return 7;
    }
    settle();
    let restop_mark = d.mark();
    if !d.send(&[0x1a], "restop") || !d.read_until(restop_mark, b"Stopped", 1, "restop") {
        return 7;
    }
    if !d.read_until(restop_mark, PROMPT, 1, "restop-prompt") {
        return 7;
    }

    // (f) `bg` resumes the re-stopped job in the BACKGROUND (no terminal
    //     handoff; the `&` job line is bi_bg's). The job sleeps on, holding no
    //     terminal read -- the prompt stays ut's.
    leg("bg");
    let m = d.mark();
    if !d.send(b"bg\r", "bg") || !d.read_until(m, b" &", 1, "bg") {
        return 8;
    }
    if !d.read_until(m, PROMPT, 1, "bg-prompt") {
        return 8;
    }

    // (g) `fg` foregrounds the now-RUNNING job (a harmless cont -- POSIX
    //     SIGCONT on a running group), then `^C` -> interrupt -> sleep
    //     default-terminates (LS-5) -> fg's stop-aware wait returns Done ->
    //     a fresh prompt. Post-RESUME signal delivery through the same
    //     pts_tty_signal fan as the susp (the F4 coverage). The prompt
    //     assert is non-vacuous here: the `^C` byte is consumed by the
    //     ldisc (SignalXorByte -- never echoed), so no keystroke redraw can
    //     satisfy it -- only ut's return to the prompt loop.
    leg("int");
    let m = d.mark();
    if !d.send(b"fg\r", "int-fg") || !d.read_until(m, b"sleep 30", 1, "int-fg") {
        return 9;
    }
    settle();
    let int_mark = d.mark();
    if !d.send(&[0x03], "int") || !d.read_until(int_mark, PROMPT, 1, "int-prompt") {
        return 9;
    }

    // (h) The shell owns the terminal and still runs pipelines after the whole
    //     stop / re-stop / bg / fg / int cycle (FGOK, output-only -- the
    //     non-vacuous health token).
    leg("health");
    let m = d.mark();
    if !d.send(b"echo fgok | tr a-z A-Z\r", "health") || !d.read_until(m, b"FGOK", 1, "health") {
        return 10;
    }

    // (i) `exit` unwinds the session; the master EOFs; the reap is clean.
    leg("exit");
    if !d.send(b"exit\r", "exit") || !d.read_to_eof("exit") {
        return 11;
    }
    let mut status: i32 = -1;
    // SAFETY: SVC wrapper; &mut status is a valid writable i32.
    let reaped = unsafe { t_wait_pid_for(pid, 0, &mut status as *mut i32) };
    let _ = unsafe { t_close(mfd) };
    if reaped != pid as i64 || status != 0 {
        t_putstr("jc-probe: FAIL (hosted ut exit status)\n");
        return 11;
    }

    t_putstr("jc-probe: PASS (run/stop/jobs/fg-restop/bg/fg-int/exit over a hosted ut)\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run()
}
