// /coreutil-smoke -- U-6e-pre-b runtime verification driver for the adopted
// native coreutils. joey spawns this; it spawns each coreutil via
// libthyla_rs::process::Command with a piped stdin (fed a known input) +
// piped stdout (captured), asserts the output bytes + exit status, and
// reports per-check markers to the UART (t_putstr). Exit 0 iff every check
// passed; joey reaps the status and gates the boot.
//
// This is the FIRST runtime execution of the coreutils (the auxiliary track
// only compiled them) AND a second exercise of the native-argv path -- each
// tool reads its own argv through env::args(). A child's exit code reaches its
// parent as itself (#91), so each check pins the literal code.
//
// READ AS IT COMES, WITHIN A BOUND: every check runs its tool through
// `converse`, which feeds its stdin without waiting and reads its stdout and
// stderr as they fill, then reaps it -- all within LEAVE_BOUND, past which the
// tool is killed and its check fails. So a tool that says more than a pipe
// holds, never reads its input or never ends fails its check rather than
// holding the boot. A child's handles close when it exits (#68,
// proc_close_handles_at_exit), so its pipes reach their end then.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use core::time::Duration;

use libthyla_rs::err::Error;
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::{self, Child, Command, Stdio};
use libthyla_rs::{t_putstr, t_set_nonblock};
use libthyla_rs::time::{self, Instant};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// How long a tool may take: to finish a check, or to stop once its reader has
/// left.
const LEAVE_BOUND: Duration = Duration::from_secs(20);
/// How much of a tool's stdout a check keeps; the rest is read and dropped.
const OUT_MAX: usize = 64 * 1024;
/// How much of a tool's stderr a report quotes; the rest is read and dropped.
const SAID: usize = 300;

/// What a tool did: its exit code (None if it had to be killed at the bound),
/// what it wrote to stdout and the start of what it wrote to stderr.
struct Ran {
    code: Option<i32>,
    out: Vec<u8>,
    err: Vec<u8>,
}

// Run `name args...` with `input` on its stdin; None if it cannot be spawned.
fn run_tool(name: &str, args: &[&str], input: &[u8]) -> Option<Ran> {
    let mut cmd = Command::new(name);
    for a in args {
        cmd.arg(*a);
    }
    cmd.stdin(Stdio::Piped).stdout(Stdio::Piped).stderr(Stdio::Piped);
    let mut child = spawn(name, &mut cmd)?;
    Some(converse(&mut child, input, LEAVE_BOUND))
}

/// Spawn `cmd`, first saying why when it cannot be: the check that wanted it
/// then fails as "spawn failed".
fn spawn(name: &str, cmd: &mut Command) -> Option<Child> {
    cmd.spawn()
        .map_err(|e| t_putstr(&format!("coreutil-smoke: spawn {} failed: {}\n", name, e)))
        .ok()
}

/// Feed `input` to `child`'s stdin and read its stdout and stderr as they fill,
/// then reap it, all within `bound`: past it the child is killed. Nothing here
/// waits on the child alone, so one that says more than a pipe holds, never
/// reads its input or never ends cannot hold the boot. A pipe the caller has
/// already taken is not touched.
fn converse(child: &mut Child, input: &[u8], bound: Duration) -> Ran {
    let start = Instant::now();
    let mut si = child.stdin.take().filter(|_| !input.is_empty());
    // Non-blocking, so a child that is not reading never holds the loop: a
    // write of more than PIPE_BUF takes what room there is, and one of PIPE_BUF
    // or less goes whole or not at all (WouldBlock).
    if si.as_ref().is_some_and(|f| unsafe { t_set_nonblock(f.as_raw_fd() as i64, true) } != 0) {
        t_putstr("coreutil-smoke: a tool's stdin could not be made non-blocking\n");
        si = None;
    }
    let (mut so, mut se) = (child.stdout.take(), child.stderr.take());
    let (mut out, mut err) = (Vec::new(), Vec::new());
    let mut fed = 0;
    let mut buf = [0u8; 4096];
    while si.is_some() || so.is_some() || se.is_some() {
        let left = bound.saturating_sub(start.elapsed());
        if left.is_zero() {
            break;
        }
        let mut set = PollSet::new();
        if let Some(f) = &si {
            set.add(f, PollEvents::WRITE);
        }
        for f in [&so, &se].into_iter().flatten() {
            set.add(f, PollEvents::READ);
        }
        let ready: Vec<i32> = match set.poll(PollTimeout::Millis(u32::try_from(left.as_millis()).unwrap_or(u32::MAX))) {
            Ok(r) => r.map(|ev| ev.fd).collect(),
            Err(_) => break,
        };
        let is_ready = |f: &File| ready.contains(&f.as_raw_fd());
        if si.as_mut().is_some_and(|f| is_ready(f) && !feed(f, input, &mut fed)) {
            si = None;
        }
        if so.as_mut().is_some_and(|f| is_ready(f) && !take(f, &mut buf, &mut out, OUT_MAX)) {
            so = None;
        }
        if se.as_mut().is_some_and(|f| is_ready(f) && !take(f, &mut buf, &mut err, SAID)) {
            se = None;
        }
    }
    // Cut off at the bound: killed while its pipes are still open, so it is
    // reported as cut off, never as whatever losing its reader made it do.
    if si.is_some() || so.is_some() || se.is_some() {
        end(child);
        return Ran { code: None, out, err };
    }
    let code = settle(child, bound.saturating_sub(start.elapsed()));
    Ran { code, out, err }
}

/// The capture's own bound: a tool that never ends is killed at it, and what
/// it wrote until then is kept.
fn capture_ends_the_endless(c: &mut Checker) {
    const LABEL: &str = "the capture ends a tool that never does";
    let mut cmd = Command::new("yes");
    cmd.stdin(Stdio::Piped).stdout(Stdio::Piped).stderr(Stdio::Piped);
    let Some(mut child) = spawn("yes", &mut cmd) else {
        return c.fail(LABEL, "spawn failed");
    };
    // The second is counted from its first output, so a slow start is no failure.
    if !child.stdout.as_ref().is_some_and(|f| readable_within(f, LEAVE_BOUND)) {
        end(&mut child);
        return c.fail(LABEL, &format!("no output in {} s", LEAVE_BOUND.as_secs()));
    }
    let r = converse(&mut child, b"", Duration::from_secs(1));
    match r.code {
        None if r.out.starts_with(b"y\ny\n") => c.pass(LABEL),
        code => c.fail(LABEL, &format!("got code={:?} out_len={} (want it killed at 1 s, its y lines kept)", code, r.out.len())),
    }
}

// Write what the pipe takes now of what is left of `input`; false once nothing
// is left to write, because all of it went or the child closed its end.
fn feed(pipe: &mut File, input: &[u8], fed: &mut usize) -> bool {
    match pipe.write(&input[*fed..]) {
        Ok(n) => *fed += n,
        // The poll calls a pipe with a byte free writable, so a write that
        // needs more room pauses here rather than spinning on the poll.
        Err(Error::WouldBlock) => {
            let _ = time::sleep(Duration::from_millis(1));
        }
        Err(_) => *fed = input.len(),
    }
    *fed < input.len()
}

// Read what the pipe holds, keeping it while `keep` is under `max`; false once
// the pipe has ended.
fn take(pipe: &mut File, buf: &mut [u8], keep: &mut Vec<u8>, max: usize) -> bool {
    match pipe.read(buf) {
        Ok(0) | Err(_) => false,
        Ok(n) => {
            keep.extend_from_slice(&buf[..n.min(max.saturating_sub(keep.len()))]);
            true
        }
    }
}

// How a tool ended, for a report.
fn ended(code: Option<i32>) -> String {
    match code {
        Some(code) => format!("code={}", code),
        None => format!("still running {} s later", LEAVE_BOUND.as_secs()),
    }
}

fn said(err: &[u8]) -> String {
    String::from_utf8_lossy(err).into_owned()
}

struct Checker {
    fails: usize,
    checks: usize,
}

impl Checker {
    fn pass(&mut self, label: &str) {
        self.checks += 1;
        t_putstr(&format!("coreutil-smoke: {} ok\n", label));
    }

    fn fail(&mut self, label: &str, detail: &str) {
        self.checks += 1;
        self.fails += 1;
        t_putstr(&format!("coreutil-smoke: {} FAILED -- {}\n", label, detail));
    }

    // Assert exact stdout + exit code.
    fn expect(&mut self, label: &str, name: &str, args: &[&str], input: &[u8], want: &[u8], want_code: i32) {
        match run_tool(name, args, input) {
            Some(r) if r.out == want && r.code == Some(want_code) => self.pass(label),
            Some(r) => self.fail(
                label,
                &format!(
                    "got {} out_len={} (want code={} out_len={}); stderr: {:?}",
                    ended(r.code),
                    r.out.len(),
                    want_code,
                    want.len(),
                    said(&r.err)
                ),
            ),
            None => self.fail(label, "spawn failed"),
        }
    }

    // Assert stdout CONTAINS `needle` + exit code (for content we don't pin
    // byte-for-byte).
    fn expect_contains(&mut self, label: &str, name: &str, args: &[&str], input: &[u8], needle: &[u8], want_code: i32) {
        match run_tool(name, args, input) {
            Some(r) if r.code == Some(want_code) && window_contains(&r.out, needle) => self.pass(label),
            Some(r) => self.fail(
                label,
                &format!("got {} out_len={} (want code={}, contains {} bytes)", ended(r.code), r.out.len(), want_code, needle.len()),
            ),
            None => self.fail(label, "spawn failed"),
        }
    }
}

fn window_contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    needle.len() <= hay.len() && hay.windows(needle.len()).any(|w| w == needle)
}

// --- a reader that leaves ---

/// Reap `child` once it exits, or kill and reap it past `bound`: its exit
/// code, or None if it had to be killed.
fn settle(child: &mut Child, bound: Duration) -> Option<i32> {
    let start = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(st)) => return Some(st.raw()),
            Ok(None) => {}
            Err(_) => return None,
        }
        if start.elapsed() > bound {
            end(child);
            return None;
        }
        let _ = time::sleep(Duration::from_millis(20));
    }
}

/// Kill `child` and reap it within LEAVE_BOUND. One the kill did not end is
/// reported and left running, never waited on without a bound.
fn end(child: &mut Child) {
    let _ = child.kill();
    let start = Instant::now();
    while start.elapsed() < LEAVE_BOUND {
        if !matches!(child.try_wait(), Ok(None)) {
            return;
        }
        let _ = time::sleep(Duration::from_millis(20));
    }
    t_putstr(&format!("coreutil-smoke: pid {} still running {} s after its kill\n", child.pid(), LEAVE_BOUND.as_secs()));
}

/// Whether `f` has bytes to read, or its writer has closed, within `bound`: a
/// filter that neither writes nor exits would otherwise hold the read, and the
/// boot with it, forever.
fn readable_within(f: &File, bound: Duration) -> bool {
    let mut set = PollSet::new();
    set.add(f, PollEvents::READ);
    let ms = u32::try_from(bound.as_millis()).unwrap_or(u32::MAX);
    match set.poll(PollTimeout::Millis(ms)) {
        Ok(mut ready) => ready.next().is_some(),
        Err(_) => false,
    }
}

/// `producer | name args`, the filter's output read for two pipes' worth and
/// then dropped. The producer never ends, so a filter that kept reading would
/// never exit: it must exit 0 within the bound with nothing on stderr, and the
/// producer, its own reader gone, must stop too.
fn reader_leaves(c: &mut Checker, label: &str, producer: &[&str], name: &str, args: &[&str]) {
    let Some((mut feed, mut child)) = piped_into(c, label, producer, name, args) else {
        return;
    };
    // Two pipes' worth: a filter still running after that is keeping up with
    // its reader, where one that ended by itself would have written less.
    const ENOUGH: usize = 2 * 4096;
    let mut taken = 0usize;
    let mut buf = [0u8; 1024];
    let start = Instant::now();
    if let Some(so) = child.stdout.as_mut() {
        while taken < ENOUGH && readable_within(so, LEAVE_BOUND.saturating_sub(start.elapsed())) {
            match so.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => taken += n,
            }
        }
    }
    // Reaped here if it has already ended: its code is the report's.
    let by_itself = match child.try_wait() {
        Ok(Some(st)) => Some(st.raw()),
        _ => None,
    };
    drop(child.stdout.take());
    let left = converse(&mut child, b"", LEAVE_BOUND);
    let fed = settle(&mut feed, LEAVE_BOUND);
    // Ending with its reader still there is the failure whatever it wrote, and
    // an early end would otherwise read as output that stopped coming.
    if let Some(code) = by_itself {
        c.fail(label, &format!("ended by itself before its reader left, after {} bytes: code={} stderr {:?}", taken, code, said(&left.err)));
    } else if taken < ENOUGH {
        c.fail(
            label,
            &format!("{} bytes of output before it stopped writing, then {}; stderr: {:?}", taken, ended(left.code), said(&left.err)),
        );
    } else if left.code.is_none() {
        c.fail(label, &format!("still running {} s after its reader left", LEAVE_BOUND.as_secs()));
    } else if left.code != Some(0) || !left.err.is_empty() {
        c.fail(label, &format!("got {} stderr {:?} (want code=0, nothing)", ended(left.code), said(&left.err)));
    } else if fed != Some(0) {
        c.fail(label, &format!("the producer, its reader gone: code={:?} (want 0)", fed));
    } else {
        c.pass(label);
    }
}

/// `producer | name args`: both spawned, the filter's stdout and stderr piped
/// to this program; None, the check failed, if either could not be.
fn piped_into(c: &mut Checker, label: &str, producer: &[&str], name: &str, args: &[&str]) -> Option<(Child, Child)> {
    let mut feed = Command::new(producer[0]);
    for a in &producer[1..] {
        feed.arg(*a);
    }
    // This program has no descriptors of its own to lend (joey spawns it
    // with none), so every one of the producer's is a pipe.
    feed.stdin(Stdio::Piped).stdout(Stdio::Piped).stderr(Stdio::Piped);
    let mut feed = match feed.spawn() {
        Ok(ch) => ch,
        Err(e) => {
            c.fail(label, &format!("producer spawn failed: {}", e));
            return None;
        }
    };
    drop(feed.stdin.take());
    drop(feed.stderr.take());
    let Some(pipe) = feed.stdout.take() else {
        settle(&mut feed, Duration::ZERO);
        c.fail(label, "producer has no stdout");
        return None;
    };
    let mut cmd = Command::new(name);
    for a in args {
        cmd.arg(*a);
    }
    cmd.stdin(Stdio::File(pipe)).stdout(Stdio::Piped).stderr(Stdio::Piped);
    match cmd.spawn() {
        Ok(child) => Some((feed, child)),
        Err(e) => {
            settle(&mut feed, LEAVE_BOUND);
            c.fail(label, &format!("spawn failed: {}", e));
            None
        }
    }
}

/// `producer | name args`, where the producer never ends: the filter's first
/// output must be `want`, and must come within the bound -- it is shown as it
/// arrives, not held for what comes after.
fn shows_first(c: &mut Checker, label: &str, producer: &[&str], name: &str, args: &[&str], want: &[u8]) {
    let Some((mut feed, mut child)) = piped_into(c, label, producer, name, args) else {
        return;
    };
    let mut got = Vec::new();
    let mut buf = [0u8; 256];
    let start = Instant::now();
    if let Some(so) = child.stdout.as_mut() {
        while got.len() < want.len() && readable_within(so, LEAVE_BOUND.saturating_sub(start.elapsed())) {
            match so.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => got.extend_from_slice(&buf[..n]),
            }
        }
    }
    // The filter is still reading an input with no end: it is ended here, and
    // the producer, its reader gone, ends by itself.
    drop(child.stdout.take());
    let _ = converse(&mut child, b"", Duration::ZERO);
    let fed = settle(&mut feed, LEAVE_BOUND);
    if !got.starts_with(want) {
        c.fail(label, &format!("got {:?} within {} s (want {:?} first)", said(&got), LEAVE_BOUND.as_secs(), said(want)));
    } else if fed != Some(0) {
        c.fail(label, &format!("the producer, its reader gone: code={:?} (want 0)", fed));
    } else {
        c.pass(label);
    }
}

/// `name args`, given an input it has no reason to read, exits 0 within the
/// bound and writes nothing.
fn ends_quietly(c: &mut Checker, label: &str, name: &str, args: &[&str]) {
    let r = match run_tool(name, args, b"") {
        Some(r) => r,
        None => return c.fail(label, "spawn failed"),
    };
    match r.code {
        Some(0) if r.out.is_empty() && r.err.is_empty() => c.pass(label),
        code => c.fail(
            label,
            &format!("got {} out_len={} stderr {:?} (want code=0, nothing)", ended(code), r.out.len(), said(&r.err)),
        ),
    }
}

/// `name args` with its stdout's reader closed before it starts, so its first
/// write, even a banner, finds none: it must stop there and exit `want` within
/// the bound, saying nothing.
fn reader_gone_first(c: &mut Checker, label: &str, name: &str, args: &[&str], want: i32) {
    let lone = match process::pipe() {
        Ok((reader, writer)) => {
            drop(reader);
            writer
        }
        Err(e) => return c.fail(label, &format!("pipe failed: {}", e)),
    };
    let Some(r) = run_to(name, args, lone) else {
        return c.fail(label, "spawn failed");
    };
    match r.code {
        Some(code) if code == want && r.err.is_empty() => c.pass(label),
        code => c.fail(label, &format!("got {} stderr {:?} (want code={}, nothing)", ended(code), said(&r.err), want)),
    }
}

/// `name args` with its stdout sent to `to` and its stderr read, within the
/// bound; None if it cannot be spawned.
fn run_to(name: &str, args: &[&str], to: File) -> Option<Ran> {
    let mut cmd = Command::new(name);
    for a in args {
        cmd.arg(*a);
    }
    cmd.stdin(Stdio::Piped).stdout(Stdio::File(to)).stderr(Stdio::Piped);
    let mut child = spawn(name, &mut cmd)?;
    Some(converse(&mut child, b"", LEAVE_BOUND))
}

/// `name args` writing to /dev/full, where every write fails and not for want
/// of a reader: it must say so (`name: write error...`) and exit 1.
fn write_fails(c: &mut Checker, label: &str, name: &str, args: &[&str]) {
    let full = match OpenOptions::new().write(true).open("/dev/full") {
        Ok(f) => f,
        Err(e) => return c.fail(label, &format!("/dev/full: {}", e)),
    };
    let Some(r) = run_to(name, args, full) else {
        return c.fail(label, "spawn failed");
    };
    let want = format!("{}: write error", name);
    match r.code {
        Some(1) if r.err.starts_with(want.as_bytes()) => c.pass(label),
        code => c.fail(label, &format!("got {} stderr {:?} (want code=1, {:?}...)", ended(code), said(&r.err), want)),
    }
}

// --- H-1c-2 Beacon helpers ---

/// Assert `name args...` exits 0 with output free of ESC (0x1b) -- no SGR
/// and no OSC frames. The piped-stdout auto-gate proof.
fn beacon_clean(c: &mut Checker, label: &str, name: &str, args: &[&str]) {
    match run_tool(name, args, b"") {
        Some(Ran { code: Some(0), out, .. }) if !out.contains(&0x1b) => c.pass(label),
        Some(Ran { code: Some(0), .. }) => c.fail(label, "ESC bytes in piped output"),
        Some(r) => c.fail(label, &ended(r.code)),
        None => c.fail(label, "spawn failed"),
    }
}

/// Write our own /env/BEACON; children inherit it via env_clone_into.
fn write_env_beacon(value: &[u8]) -> bool {
    match libthyla_rs::fs::File::create("/env/BEACON") {
        Ok(mut f) => f.write_all(value).is_ok(),
        Err(_) => false,
    }
}

/// Run `name` twice -- the rich invocation and the plain one -- and assert
/// the P1 identity on real spawned output: the rich stream carries
/// `frame_needle`, and stripping every frame yields the plain run's bytes
/// EXACTLY. Both runs must agree on exit status too.
fn beacon_rich_vs_plain(
    c: &mut Checker,
    label: &str,
    name: &str,
    rich_args: &[&str],
    plain_args: &[&str],
    input: &[u8],
    frame_needle: &[u8],
) {
    let (rc, rout) = match run_tool(name, rich_args, input) {
        Some(r) => (r.code, r.out),
        None => return c.fail(label, "rich spawn failed"),
    };
    let (pc, pout) = match run_tool(name, plain_args, input) {
        Some(r) => (r.code, r.out),
        None => return c.fail(label, "plain spawn failed"),
    };
    if rc.is_none() || rc != pc {
        return c.fail(label, &format!("exit mismatch rich: {} plain: {}", ended(rc), ended(pc)));
    }
    if !window_contains(&rout, frame_needle) {
        return c.fail(label, "expected frame missing from rich output");
    }
    if beacon::wire::strip(&rout) != pout {
        return c.fail(label, "strip(rich) != plain");
    }
    c.pass(label);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut c = Checker { fails: 0, checks: 0 };

    // --- the capture itself: a tool that says more than a pipe holds (here
    // given more than one holds, too) is read as it writes, and one that never
    // ends is killed at the bound, so no check can hold the boot ---
    let mut big = Vec::new();
    for i in 0..4000 {
        big.extend_from_slice(format!("{}\n", i).as_bytes());
    }
    c.expect("the capture carries more than a pipe holds", "cat", &[], &big, &big, 0);
    capture_ends_the_endless(&mut c);

    // --- argv tools (no stdin) ---
    c.expect("echo", "echo", &["a", "b"], b"", b"a b\n", 0);
    c.expect("echo -n", "echo", &["-n", "x"], b"", b"x", 0);
    c.expect("seq N", "seq", &["3"], b"", b"1\n2\n3\n", 0);
    c.expect("seq F I L", "seq", &["2", "2", "6"], b"", b"2\n4\n6\n", 0);
    c.expect("true", "true", &[], b"", b"", 0);
    c.expect("false", "false", &[], b"", b"", 1);
    c.expect("basename", "basename", &["/a/b/c.txt"], b"", b"c.txt\n", 0);
    c.expect("basename suf", "basename", &["/a/b/c.txt", ".txt"], b"", b"c\n", 0);
    c.expect("dirname", "dirname", &["/a/b/c"], b"", b"/a/b\n", 0);
    // joey runs the pre-pivot probes in the initrd's bin/, where their names resolve.
    c.expect("pwd", "pwd", &[], b"", b"/bin\n", 0);

    // --- stdin filters ---
    c.expect("cat stdin", "cat", &[], b"hello\n", b"hello\n", 0);
    c.expect("wc -c", "wc", &["-c"], b"hello\n", b"      6\n", 0);
    c.expect("wc -l", "wc", &["-l"], b"a\nb\nc\n", b"      3\n", 0);
    c.expect("wc default", "wc", &[], b"a b c\n", b"      1      3      6\n", 0);
    c.expect("head -n", "head", &["-n", "2"], b"1\n2\n3\n4\n", b"1\n2\n", 0);
    // "-" is standard input, named so in a banner; a blank line comes before
    // every banner but the first.
    c.expect(
        "head - and banners",
        "head",
        &["-n", "1", "-", "/bin/version"],
        b"a\nb\n",
        b"==> standard input <==\na\n\n==> /bin/version <==\nThylacine v0.1-dev\n",
        0,
    );
    c.expect(
        "tail - and banners",
        "tail",
        &["-n", "1", "/bin/version", "-"],
        b"a\nb\n",
        b"==> /bin/version <==\nThylacine v0.1-dev\n\n==> standard input <==\nb\n",
        0,
    );
    c.expect("tail -n", "tail", &["-n", "2"], b"1\n2\n3\n4\n", b"3\n4\n", 0);
    c.expect("sort", "sort", &[], b"banana\napple\ncherry\n", b"apple\nbanana\ncherry\n", 0);
    c.expect("sort -n", "sort", &["-n"], b"10\n2\n1\n", b"1\n2\n10\n", 0);
    c.expect("uniq", "uniq", &[], b"a\na\nb\nb\nb\nc\n", b"a\nb\nc\n", 0);
    c.expect("tr upper", "tr", &["a-z", "A-Z"], b"hello\n", b"HELLO\n", 0);
    c.expect("tr -d", "tr", &["-d", "l"], b"hello\n", b"heo\n", 0);
    // RW-9 R4-F6: a POSIX class is rejected, not silently mangled. Pre-fix,
    // `tr -d '[:space:]'` stripped the literal bytes [ : s p a c e ] and left
    // " b\n" at exit 0 (silent corruption); now it is empty + exit 1.
    c.expect("tr class reject", "tr", &["-d", "[:space:]"], b"a b\n", b"", 1);
    c.expect("cut -f", "cut", &["-d:", "-f2"], b"a:b:c\n", b"b\n", 0);
    c.expect("cut -c", "cut", &["-c1-3"], b"abcdef\n", b"abc\n", 0);
    c.expect("grep match", "grep", &["ba"], b"foo\nbar\nbaz\n", b"bar\nbaz\n", 0);
    c.expect("grep no-match", "grep", &["zzz"], b"foo\nbar\n", b"", 1);
    c.expect("grep -c", "grep", &["-c", "ba"], b"foo\nbar\nbaz\n", b"2\n", 0);
    // grep reads a line at a time (coreutils::stream): a line longer than the
    // pipe and the read buffer is carried across reads, the last line needs no
    // newline, an empty input has no lines, and -l stops at the first match.
    let mut long = Vec::new();
    long.resize(9000, b'a');
    long.extend_from_slice(b"NEEDLE");
    long.resize(long.len() + 9000, b'b');
    long.extend_from_slice(b"\nnope\nNEEDLE tail");
    c.expect("grep -c long line", "grep", &["-c", "NEEDLE"], &long, b"2\n", 0);
    c.expect("grep -n last line", "grep", &["-n", "tail"], &long, b"3:NEEDLE tail\n", 0);
    c.expect("grep empty input", "grep", &["-v", "x"], b"", b"", 1);
    c.expect("grep -c empty input", "grep", &["-c", ""], b"", b"0\n", 1);
    c.expect("grep -l stdin", "grep", &["-l", "bar"], b"foo\nbar\nbaz\n", b"(standard input)\n", 0);
    // So do wc, cut, uniq and tail: a word spans reads, an empty input has no
    // lines, and tail keeps a window of the answer over input far longer.
    let mut word = Vec::new();
    word.resize(10000, b'x');
    word.push(b'\n');
    c.expect("wc -w long word", "wc", &["-w"], &word, b"      1\n", 0);
    c.expect("wc long input", "wc", &[], &long, b"      2      4  18023\n", 0);
    c.expect("cut empty input", "cut", &["-c1"], b"", b"", 0);
    c.expect("cut long line", "cut", &["-c1-3"], &long, b"aaa\nnop\nNEE\n", 0);
    c.expect("uniq empty input", "uniq", &[], b"", b"", 0);
    let mut numbered = Vec::new();
    for i in 0..5000 {
        numbered.extend_from_slice(format!("{}\n", i).as_bytes());
    }
    c.expect("tail -n window", "tail", &["-n", "3"], &numbered, b"4997\n4998\n4999\n", 0);
    c.expect("tail -c window", "tail", &["-c", "7"], &numbered, b"8\n4999\n", 0);
    c.expect("tail last line", "tail", &["-n", "1"], &long, b"NEEDLE tail", 0);
    // A line with no end is refused at LINE_MAX (64 MiB), never grown until a
    // page the system cannot back ends the tool -- exit 1, grep's "no match".
    c.expect("grep a line past LINE_MAX", "grep", &["x", "/dev/zero"], b"", b"", 2);
    c.expect("tail a line past LINE_MAX", "tail", &["-n", "1", "/dev/zero"], b"", b"", 1);
    // The last none of an input is nothing, so tail reads none of it.
    ends_quietly(&mut c, "tail -n 0 reads nothing", "tail", &["-n", "0", "/dev/zero"]);
    // +N counts from the start, and +0 is everything, as GNU tail counts.
    c.expect("tail -n +N from line N", "tail", &["-n", "+2"], b"a\nb\nc\nd\n", b"b\nc\nd\n", 0);
    c.expect("tail -n +0 is everything", "tail", &["-n", "+0"], b"a\nb\n", b"a\nb\n", 0);
    c.expect("tail -c +N from byte N", "tail", &["-c", "+3"], b"abcdef", b"cdef", 0);
    // POSIX's -N counts from the end, as no sign does; `--` ends the options.
    c.expect("tail -n -1 is the last line", "tail", &["-n", "-1"], b"a\nb\n", b"b\n", 0);
    c.expect("tail -- ends its options", "tail", &["-n", "1", "--"], b"a\nb\n", b"b\n", 0);
    c.expect("head -- ends its options", "head", &["-n", "1", "--"], b"a\nb\n", b"a\n", 0);
    // A count too large to hold is refused, not read as another count.
    c.expect("head refuses a count it cannot hold", "head", &["-99999999999999999999"], b"a\n", b"", 1);
    // A line's matches, fields and transforms, each shape once. These pin the
    // bytes, which the collecting code gave too: that nothing is collected is
    // held by the host's counting tests (coreutils::find, select), stream's
    // cat_lines tests and `cat -v streams a line with no end` below.
    let mut abab = Vec::new();
    let mut each_ab = Vec::new();
    for _ in 0..1000 {
        abab.extend_from_slice(b"ab");
        each_ab.extend_from_slice(b"ab\n");
    }
    abab.push(b'\n');
    c.expect("grep -o every match of a line", "grep", &["-o", "ab"], &abab, &each_ab, 0);
    c.expect("cut -f fields of a line", "cut", &["-f2,4-"], b"a\tb\tc\td\te\nno tab\n", b"b\td\te\nno tab\n", 0);
    c.expect("cat -n numbers every line", "cat", &["-n"], b"a\n\nb", b"     1\ta\n     2\t\n     3\tb", 0);
    c.expect("cat -b numbers nonblank lines", "cat", &["-b"], b"a\n\nb\n", b"     1\ta\n\n     2\tb\n", 0);
    c.expect("cat -s squeezes blank runs", "cat", &["-s"], b"a\n\n\n\nb\n", b"a\n\nb\n", 0);
    c.expect("cat -A shows everything", "cat", &["-A"], b"\ta\x01\x7f\xe9\n", b"^Ia^A^?M-i$\n", 0);
    // A reader that leaves had what it wanted: the filter stops reading, says
    // nothing and keeps its status (`yes | grep y | head -1` ends).
    reader_leaves(&mut c, "grep stops when its reader leaves", &["yes"], "grep", &["y"]);
    reader_leaves(&mut c, "cut stops when its reader leaves", &["yes"], "cut", &["-c1"]);
    reader_leaves(&mut c, "cat stops when its reader leaves", &["yes"], "cat", &[]);
    reader_leaves(&mut c, "tee stops when its reader leaves", &["yes"], "tee", &[]);
    reader_leaves(&mut c, "uniq stops when its reader leaves", &["seq", "1000000000000"], "uniq", &[]);
    reader_leaves(&mut c, "tail -n +1 streams and stops when its reader leaves", &["yes"], "tail", &["-n", "+1"]);
    // A run's line is shown as the run begins, not once a different line ends
    // it: `yes | uniq` shows its one line at once.
    shows_first(&mut c, "uniq shows a line as its run begins", &["yes"], "uniq", &[], b"y\n");
    // A line with no end streams through cat's transforms: it is shown as it
    // arrives, never held whole.
    reader_leaves(&mut c, "cat -v streams a line with no end", &["cat", "/dev/zero"], "cat", &["-v"]);
    // A reader gone before the first write: the tool stops at that write, a
    // banner too (the next input here has no end), and a verdict still gets
    // the search it needs.
    reader_gone_first(&mut c, "tail stops at a banner with no reader", "tail", &["-n", "1", "/dev/null", "/dev/random"], 0);
    // The premise of the next check: with a reader, the first write is a count
    // of none, before the operand that matches is searched.
    c.expect(
        "grep -c counts every operand",
        "grep",
        &["-c", "Thylacine", "/dev/null", "/bin/version"],
        b"",
        b"/dev/null:0\n/bin/version:1\n",
        0,
    );
    reader_gone_first(&mut c, "grep -c finds its verdict with no reader", "grep", &["-c", "Thylacine", "/dev/null", "/bin/version"], 0);
    // A write that fails for another reason is reported, a banner or raw text
    // as much as the payload, and the status says so.
    write_fails(&mut c, "head reports a banner it could not write", "head", &["-n", "1", "/dev/null", "/dev/null"]);
    write_fails(&mut c, "tail reports a banner it could not write", "tail", &["-n", "1", "/dev/null", "/dev/null"]);
    write_fails(&mut c, "ns reports raw text it could not write", "ns", &["--color=never"]);

    // --- LS-3c misc coreutils ---
    // (#91 landed: a child's real non-zero exit now survives verbatim -- cmp's
    // error-2 and env's 125 arrive as themselves. Neither is smoked here for its
    // error code: env only answers --help below, and cmp is smoked only for
    // equal/differ, a genuine 0/1.) `yes` is not captured by `expect`: it never
    // ends, so its check could only end at the bound. It feeds the capture's own
    // check and the reader-leaves checks above, which drop its reader and
    // require it to stop.
    c.expect("uname", "uname", &[], b"", b"Thylacine\n", 0);
    c.expect("uname -m", "uname", &["-m"], b"", b"aarch64\n", 0);
    c.expect("uname -a", "uname", &["-a"], b"", b"Thylacine (none) 1.0-dev #1-thylacine aarch64\n", 0);
    // A bare-name `env` resolves through the working directory, /bin, to the
    // coreutil. While the initrd was flat the same name met the /env device
    // (G15, ARCH 9.7), and the utility was unreachable before the pivot.
    c.expect_contains("env bare name", "env", &["--help"], b"", b"usage: env", 0);
    c.expect("realpath abs", "realpath", &["/a/b/../c"], b"", b"/a/c\n", 0);
    c.expect("realpath rel", "realpath", &["x/../y"], b"", b"/bin/y\n", 0); // cwd "/bin" -> "/bin/y"
    c.expect("sleep 0", "sleep", &["0"], b"", b"", 0);
    c.expect_contains("hexdump hex", "hexdump", &[], b"Hi", b"48 69", 0); // 'H'=0x48 'i'=0x69
    c.expect_contains("hexdump ascii", "hexdump", &[], b"Hi", b"|Hi|", 0);
    c.expect("which miss", "which", &["nope"], b"", b"", 1); // bare name, no PATH (G15)
    c.expect("which path", "which", &["/bin/version"], b"", b"/bin/version\n", 0);
    c.expect("cmp equal", "cmp", &["/bin/version", "/bin/version"], b"", b"", 0);
    // A binary is many 8 KiB buffers long: cmp streams it.
    c.expect("cmp equal large", "cmp", &["/bin/grep", "/bin/grep"], b"", b"", 0);
    c.expect_contains("cmp differ", "cmp", &["/bin/version", "/bin/welcome"], b"", b"differ", 1);

    // --- file read via File::open (devramfs /bin/version is read-only, present) ---
    c.expect("cat FILE", "cat", &["/bin/version"], b"", b"Thylacine v0.1-dev\n", 0);
    c.expect_contains("wc FILE", "wc", &["-l", "/bin/version"], b"", b"/bin/version", 0);

    // --- LS-K: identity + clock. This smoke runs as PRINCIPAL_SYSTEM
    // (joey-spawned), so uid == gid == 0xFFFFFFFE == 4294967294 -- exact-
    // matchable, proving the wrappers read the right field end-to-end. `date`
    // is time-varying; "UTC" proves it ran + formatted (the wall clock's 2020+
    // plausibility is the always-on kernel test clock.realtime_anchored). ---
    c.expect("whoami SYSTEM", "whoami", &[], b"", b"4294967294\n", 0);
    c.expect("id -u SYSTEM", "id", &["-u"], b"", b"4294967294\n", 0);
    c.expect("id SYSTEM", "id", &[], b"", b"uid=4294967294 gid=4294967294 groups=4294967294\n", 0);
    c.expect_contains("date UTC", "date", &[], b"", b"UTC", 0);

    // --- H-1c-2: the Beacon emitters + the --color=auto flip, end to end.
    // Every child here has a PIPED stdout, so SYS_FD_DEVCLASS answers '|'
    // (devpipe) in the child -- under Auto both gates resolve OFF. These are
    // the real spawns the crate's host tests cannot make.
    //
    // The ls subjects are explicit FILE operands, so each listing is known and
    // small: the boot ramfs is FLAT, so pre-pivot root lists every staged
    // binary and the other listable dirs are the empty synth mounts, whose
    // empty listing would satisfy the clean check vacuously (the #215
    // broken-fixture shape).

    // The flip's proof: `ls` (color now defaults to Auto) into a pipe is
    // BYTE-CLEAN -- no SGR, no frames. Fails on the pre-flip Always default
    // (file operands were SGR-wrapped too).
    beacon_clean(&mut c, "ls auto pipe clean", "ls", &["/bin/version", "/bin/welcome"]);
    beacon_clean(&mut c, "ls -l auto pipe clean", "ls", &["-l", "/bin/version", "/bin/welcome"]);

    beacon_clean(&mut c, "ps auto pipe clean", "ps", &[]);
    // `ps` piped = the verbatim /ctl/procs snapshot (parseable): the kernel
    // header + this very process family are in it.
    c.expect_contains("ps raw header", "ps", &[], b"", b"PID    PPID    NAME", 0);
    c.expect_contains("ps raw joey", "ps", &[], b"", b"joey", 0);

    // Force the rich tier down the REAL inheritance path: write our own
    // /env/BEACON (env_clone_into deep-copies it into every child we spawn),
    // then drive each emitter with --beacon=always (the flag trusts the
    // advertised tier; the pipe dc no longer gates). Asserts: the frames are
    // ON the wire, and stripping them yields the plain emission byte-exactly
    // (the P1 identity, in-guest).
    if write_env_beacon(b"rich") {
        c.pass("env BEACON=rich exported");
        beacon_rich_vs_plain(
            &mut c,
            "ls rich strips to plain",
            "ls",
            &["--beacon=always", "/bin/version", "/bin/welcome"],
            &["--beacon=never", "--color=never", "/bin/version", "/bin/welcome"],
            b"",
            b"\x1b]1936;v1;obj;type=path;ref=/bin/version",
        );
        beacon_rich_vs_plain(
            &mut c,
            "grep rich strips to plain",
            "grep",
            &["--beacon=always", "ba"],
            &["--beacon=never", "ba"],
            b"foo\nbar\nbaz\n",
            b"\x1b]1936;v1;em;class=strong",
        );
        beacon_rich_vs_plain(
            &mut c,
            "grep -o rich strips to plain",
            "grep",
            &["--beacon=always", "-o", "ba"],
            &["--beacon=never", "-o", "ba"],
            b"foo\nbar\nbaz\nbaba\n",
            b"\x1b]1936;v1;em;class=strong",
        );
        beacon_rich_vs_plain(
            &mut c,
            "stat rich strips to plain",
            "stat",
            &["--beacon=always", "/bin/version"],
            &["--beacon=never", "--color=never", "/bin/version"],
            b"",
            b"\x1b]1936;v1;obj;type=path;ref=/bin/version",
        );
        // ps at rich: the beacon table with obj pid cells. Its plain payload
        // is the STYLED aligned table (not the raw snapshot), so assert the
        // frame + content, not equality with the pass-through.
        match run_tool("ps", &["--beacon=always"], b"") {
            Some(Ran { code: Some(0), out, .. }) => {
                if window_contains(&out, b"\x1b]1936;v1;obj;type=pid;ref=")
                    && window_contains(&out, b"\x1b]1936;v1;table;cols=rrllrrrrr")
                    && window_contains(&beacon::wire::strip(&out), b"joey")
                {
                    c.pass("ps rich table + obj pid");
                } else {
                    c.fail("ps rich table + obj pid", "frames/payload missing");
                }
            }
            Some(r) => c.fail("ps rich table + obj pid", &ended(r.code)),
            None => c.fail("ps rich table + obj pid", "spawn failed"),
        }
        // PL-5: `ls -l` at rich emits a `pre` code-fence box (HALCYON.md
        // 14.13), not a table -- the box furniture is the pre payload, the
        // name cell staying affordant as an `obj`. A single-file operand
        // keeps the box to a header, one row and two borders. Assert the pre +
        // obj frames AND that stripping
        // yields the box itself (the top-left corner + a vertical rule):
        // ls -l's tiers differ by design (box at rich/cells, columns at a
        // pipe), so this is NOT a rich-vs-plain strip identity.
        match run_tool("ls", &["-l", "--beacon=always", "/bin/version"], b"") {
            Some(Ran { code: Some(0), out, .. }) => {
                let stripped = beacon::wire::strip(&out);
                if window_contains(&out, b"\x1b]1936;v1;pre")
                    && window_contains(&out, b"\x1b]1936;v1;obj;type=path;ref=")
                    && window_contains(&stripped, "\u{250c}".as_bytes())
                    && window_contains(&stripped, "\u{2502}".as_bytes())
                {
                    c.pass("ls -l rich pre-box (PL-5)");
                } else {
                    c.fail("ls -l rich pre-box (PL-5)", "pre/obj frame or box payload missing");
                }
            }
            Some(r) => c.fail("ls -l rich pre-box (PL-5)", &ended(r.code)),
            None => c.fail("ls -l rich pre-box (PL-5)", "spawn failed"),
        }
        // Reset our env so the tail checks (and anything after) see none.
        let _ = write_env_beacon(b"none");
    } else {
        c.fail("env BEACON=rich exported", "/env/BEACON write failed");
    }

    if c.fails == 0 {
        t_putstr(&format!("coreutil-smoke: all OK ({} checks)\n", c.checks));
        0
    } else {
        t_putstr(&format!("coreutil-smoke: {} of {} checks FAILED\n", c.fails, c.checks));
        1
    }
}
