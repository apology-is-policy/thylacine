// /bin/loom-death-probe -- programs that exit, exec or die with a private
// connection in flight (docs/ASYNC-SERVICE-LIFECYCLE.md section 9, AS-3). The
// kernel test loom.native_probe_death starts it beside /bin/loom-fixture and
// requires 72 from the default image, where setup refuses -EOPNOTSUPP, or 73 from
// the qualification image, where every victim below ended, its session ended at
// the server, and nothing was left behind. The test holds the kernel's counts:
// every ring retired, every connection freed.
//
// The driver has the usual fds: 0 the registry the fixture posts in, 1 the control
// pipe, 2 the notify pipe. It spawns this same program once per row of CASES, with
// the role as argv[1]. A victim has fd 0 a pipe it reads for its go-ahead, fd 1 a
// pipe it writes 'H' to once its operation is submitted, and fd 3 the registry. Its
// exit status names the path it took, so a victim that left some other way shows.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, Ordering};
use core::time::Duration;
use libthyla_rs::loom::native::{Connect, Fid, Payload, PoolMember, Read, Scope, Shot, Stream, Target};
use libthyla_rs::loom::{ServiceRing, ThylaKernel};
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::service_worker::{Control, ServiceWorker};
use libthyla_rs::{
    t_close, t_execve, t_exits, t_loom_enter, t_poll, t_postnote, t_putstr, t_read, t_wait_exitstatus,
    t_wait_if_exited, t_wait_if_signaled, t_wait_pid_for, t_wait_termsig, t_write, TPollFd, T_POLLIN,
    T_WAIT_PACKED, T_WAIT_WNOHANG,
};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

type K = ThylaKernel;

const PASS_DEFAULT: i64 = 72;
const PASS_QUAL: i64 = 73;
const REGISTRY: i32 = 0;
const NOTIFY: i64 = 2;
const VICTIM_GO: i64 = 0;
const VICTIM_READY: i64 = 1;
const VICTIM_REGISTRY: i32 = 3;

const E_BADF: i64 = 9;
const E_OPNOTSUPP: i32 = 95;
const E_NOSPC: i32 = 28;

const PAGE: u64 = 4096;
const ENTRIES: u32 = 2;
const BUFFERS: [usize; 2] = [PAGE as usize; 2];
const WAIT_NS: u64 = 5_000_000_000;
const SHOT: u64 = 512;
const SELF: &str = "/bin/loom-death-probe";
const AFTER_EXEC: &[u8] = b"/bin/loom-death-probe\0after-exec\0";

// Every descriptor a victim could hold is below this.
const FD_SCAN: i64 = 32;
// LOOM_ENTER_NONBLOCK (kernel/include/thylacine/loom.h): the ring's own image gets
// an answer to an ENTER of nothing with no wait, and what is not a ring -EBADF.
const ENTER_NONBLOCK: u64 = 1 << 1;

// The fixture's stats counters (usr/loom-fixture).
const ENDED: usize = 1;
const VIOLATIONS: usize = 2;
const STALLED: usize = 3;
const FAULTS: usize = 11;
const BACKLOG: usize = 12;
const STAT_COUNT: usize = 13;
const SIGKILL: i32 = 9;

const EXIT_PLAIN: i32 = 42;
const EXIT_GROUP: i32 = 43;
const EXIT_AFTER_EXEC: i32 = 44;
const EXIT_CLOSED: i32 = 45;
const EXIT_UNACCEPTED: i32 = 46;
const EXIT_LEASE: i32 = 47;

#[derive(Clone, Copy)]
enum Stop {
    Go,
    Kill,
}

struct Case {
    role: &'static str,
    // The counter that shows the victim's operation reached the server.
    reached: usize,
    // A session the server accepted, so its end shows at the server.
    ends: bool,
    stop: Stop,
    // None: killed, so the status is the kernel's.
    status: Option<i32>,
}

const CASES: [Case; 9] = [
    Case { role: "exit", reached: STALLED, ends: true, stop: Stop::Go, status: Some(EXIT_PLAIN) },
    Case { role: "group", reached: STALLED, ends: true, stop: Stop::Go, status: Some(EXIT_GROUP) },
    Case { role: "close", reached: STALLED, ends: true, stop: Stop::Go, status: Some(EXIT_CLOSED) },
    Case { role: "kill", reached: STALLED, ends: true, stop: Stop::Kill, status: None },
    Case { role: "exec", reached: STALLED, ends: true, stop: Stop::Go, status: Some(EXIT_AFTER_EXEC) },
    Case { role: "exec-sibling", reached: STALLED, ends: true, stop: Stop::Go, status: Some(EXIT_AFTER_EXEC) },
    Case { role: "connect", reached: FAULTS, ends: true, stop: Stop::Kill, status: None },
    Case { role: "unaccepted", reached: BACKLOG, ends: false, stop: Stop::Go, status: Some(EXIT_UNACCEPTED) },
    Case { role: "lease", reached: STALLED, ends: true, stop: Stop::Go, status: Some(EXIT_LEASE) },
];

struct Fail {
    line: u32,
    why: &'static str,
    got: i64,
}
type R<T> = core::result::Result<T, Fail>;

macro_rules! check {
    ($c:expr, $why:literal, $got:expr) => {
        if !$c {
            return Err(Fail { line: line!(), why: $why, got: $got as i64 });
        }
    };
}
macro_rules! ok {
    ($e:expr, $why:literal) => {
        match $e {
            Ok(v) => v,
            Err(e) => return Err(Fail { line: line!(), why: $why, got: e as i64 }),
        }
    };
}
macro_rules! done {
    ($ring:expr, $fut:expr, $why:literal) => {{
        let f = $fut;
        let until = Some($ring.now() + WAIT_NS);
        match $ring.block_on(f, until) {
            Ok(Ok(v)) => v,
            Ok(Err(e)) => return Err(Fail { line: line!(), why: $why, got: e as i64 }),
            Err(e) => {
                return Err(Fail { line: line!(), why: concat!($why, ": no completion"), got: e as i64 })
            }
        }
    }};
}

fn say(line: &str) {
    t_putstr(&format!("loom-death-probe: {}\n", line));
}

fn range(buffer: u32, offset: u64, length: u64) -> PoolMember {
    PoolMember { buffer_index: buffer, reserved: 0, offset, length }
}

fn soon(ring: &ServiceRing) -> Option<u64> {
    Some(ring.now() + WAIT_NS)
}

fn walk(ring: &ServiceRing, scope: &Scope<K>, name: &[u8]) -> R<Fid<K>> {
    Ok(done!(ring, ok!(scope.walk(name, soon(ring)), "submit a WALK"), "a WALK"))
}

fn open(ring: &ServiceRing, fid: &Fid<K>) -> R<()> {
    done!(ring, ok!(fid.open(0, soon(ring)), "submit an LOPEN"), "an LOPEN");
    Ok(())
}

fn connect(ring: &ServiceRing, target: &Target<K>) -> R<Scope<K>> {
    Ok(done!(ring, ok!(target.connect(soon(ring)), "submit a CONNECT"), "a CONNECT"))
}

// The lowest descriptor that answers ENTER as a ring does.
fn ring_fd() -> Option<i64> {
    (3..FD_SCAN).find(|fd| unsafe { t_loom_enter(*fd as u64, 0, 0, ENTER_NONBLOCK) } >= 0)
}

fn wait_go() {
    let mut b = [0u8; 1];
    let _ = unsafe { t_read(VICTIM_GO, b.as_mut_ptr(), 1) };
}

// ---- the victim ----

const ROLE_GROUP: u8 = 1;
const ROLE_CLOSE: u8 = 2;
const ROLE_EXEC: u8 = 3;

static TURNING: AtomicBool = AtomicBool::new(false);
static CLOSED: AtomicBool = AtomicBool::new(false);
// Long enough for a thread that has just said so to be parked in its turn.
const INTO_TURN: Duration = Duration::from_millis(10);

struct Sibling {
    role: u8,
    ring: i64,
}

fn exec_after() -> ! {
    let rc = unsafe { t_execve(SELF.as_bytes(), AFTER_EXEC, 2) };
    say(&format!("FAIL: exec ({})", rc));
    unsafe { t_exits(1) }
}

// Runs on its own thread while the main one sits in a ring turn.
fn sibling(s: &Sibling, control: &Control) -> libthyla_rs::err::Result<()> {
    control.ready()?;
    wait_go();
    while !TURNING.load(Ordering::Acquire) {
        let _ = libthyla_rs::time::sleep(Duration::from_millis(1));
    }
    let _ = libthyla_rs::time::sleep(INTO_TURN);
    match s.role {
        ROLE_GROUP => unsafe { t_exits(EXIT_GROUP as i64) },
        ROLE_EXEC => exec_after(),
        _ => {
            let rc = unsafe { t_close(s.ring) };
            if rc != 0 {
                say(&format!("FAIL: close the ring from a sibling ({})", rc));
                unsafe { t_exits(1) }
            }
            CLOSED.store(true, Ordering::Release);
        }
    }
    Ok(())
}

// The main thread's turn, until something ends the program. One long deadline, so a
// wake the kernel lost shows as a victim the driver's bound finds still here.
// `target` tells a turn that did not notice the close from one that did: a closed
// ring admits nothing.
fn turn_forever(ring: &ServiceRing, target: &Target<K>) -> ! {
    let until = ring.now() + 4 * WAIT_NS;
    TURNING.store(true, Ordering::Release);
    loop {
        let turned = ring.turn(Some(until));
        if CLOSED.load(Ordering::Acquire) && (turned.is_err() || target.connect(None).is_err()) {
            unsafe { t_exits(EXIT_CLOSED as i64) }
        }
        if ring.now() >= until {
            say("FAIL: nothing ended this victim");
            unsafe { t_exits(1) }
        }
    }
}

fn victim(role: &str) -> R<i64> {
    let ring = ok!(ServiceRing::setup(ThylaKernel, ENTRIES, &BUFFERS), "a victim's ring");
    let fd = ok!(ring_fd().ok_or(-1), "find the ring's descriptor");
    let service: &[u8] = match role {
        "connect" => b"hold-version",
        "unaccepted" => b"unaccepted",
        _ => b"fixture",
    };
    let target = ok!(ring.target(VICTIM_REGISTRY, service), "a victim's TARGET");
    let mut pending: Option<Connect<K>> = None;
    let mut scope: Option<Scope<K>> = None;
    let mut fids: Vec<Fid<K>> = Vec::new();
    let mut read: Option<Read<K>> = None;
    let mut stream: Option<Stream<K>> = None;
    let mut lease: Option<Payload<K>> = None;
    if role == "connect" || role == "unaccepted" {
        pending = Some(ok!(target.connect(None), "a victim's CONNECT"));
    } else {
        let s = connect(&ring, &target)?;
        if role == "lease" {
            let shots = walk(&ring, &s, b"stream")?;
            open(&ring, &shots)?;
            let pool = ok!(ring.pool(&[range(1, 0, SHOT), range(1, SHOT, SHOT)]), "a victim's pool");
            let mut st = ok!(shots.stream(&pool, 0, SHOT as u32, true, None), "a victim's stream");
            match done!(ring, st.next(), "a victim's shot") {
                Shot::Payload(p) => lease = Some(p),
                Shot::End(rc) => check!(false, "a stream ended before its shot", rc),
            }
            stream = Some(st);
            fids.push(shots);
            core::mem::forget(pool);
        }
        let held = walk(&ring, &s, b"stall")?;
        open(&ring, &held)?;
        read = Some(ok!(held.read(range(0, 0, PAGE), 0, None), "a READ the server holds"));
        fids.push(held);
        scope = Some(s);
    }
    let n = unsafe { t_write(VICTIM_READY, b"H".as_ptr(), 1) };
    check!(n == 1, "tell the driver the operation is submitted", n);

    // In flight when the program ends: nothing here is ever dropped.
    core::mem::forget((pending, scope, fids, read, stream, lease));

    match role {
        "exit" => {
            wait_go();
            unsafe { t_exits(EXIT_PLAIN as i64) }
        }
        "unaccepted" => {
            wait_go();
            unsafe { t_exits(EXIT_UNACCEPTED as i64) }
        }
        "lease" => {
            wait_go();
            unsafe { t_exits(EXIT_LEASE as i64) }
        }
        "exec" => {
            wait_go();
            exec_after()
        }
        "group" | "close" | "exec-sibling" => {
            let which = match role {
                "group" => ROLE_GROUP,
                "close" => ROLE_CLOSE,
                _ => ROLE_EXEC,
            };
            let _worker = ok!(ServiceWorker::new(Sibling { role: which, ring: fd }, sibling)
                                  .map_err(|e| e.as_errno()),
                              "a sibling thread");
            turn_forever(&ring, &target)
        }
        _ => turn_forever(&ring, &target),
    }
}

// The image an exec put here: exec closes a private ring whatever userspace asked
// for. A descriptor that outlived it would answer -EACCES, refused for this image
// before the kernel looks at the latch, so only -EBADF from every number shows the
// descriptor went. A new ring can be made.
fn after_exec() -> R<i64> {
    for fd in 3..FD_SCAN {
        let rc = unsafe { t_loom_enter(fd as u64, 0, 0, ENTER_NONBLOCK) };
        check!(rc == -E_BADF, "exec closed the private ring", rc);
    }
    let ring = ok!(ServiceRing::setup(ThylaKernel, ENTRIES, &BUFFERS), "a ring in the new image");
    core::mem::forget(ring);
    Ok(EXIT_AFTER_EXEC as i64)
}

// ---- the driver ----

// A dropped operation keeps its entry in the request book until a turn routes its
// terminal, so this may first have to make that room.
fn stats(ring: &ServiceRing, fid: &Fid<K>) -> R<[u32; STAT_COUNT]> {
    let until = ring.now() + WAIT_NS;
    let submitted = loop {
        match fid.read(range(0, 0, 64), 0, soon(ring)) {
            Err(e) if e == -E_NOSPC => {
                check!(ring.now() < until, "room for a stats read", e);
                ok!(ring.turn(Some(ring.now() + 10_000_000)), "a turn with the book full");
            }
            r => break ok!(r, "submit a stats read"),
        }
    };
    let read = done!(ring, submitted, "a stats read");
    let b = read.bytes();
    check!(b.len() == 4 * STAT_COUNT, "the stats file holds one u32 per counter", b.len());
    let mut v = [0u32; STAT_COUNT];
    for (i, w) in v.iter_mut().enumerate() {
        *w = u32::from_le_bytes([b[4 * i], b[4 * i + 1], b[4 * i + 2], b[4 * i + 3]]);
    }
    Ok(v)
}

fn stats_until(ring: &ServiceRing, fid: &Fid<K>, at: usize, want: u32) -> R<[u32; STAT_COUNT]> {
    let until = ring.now() + WAIT_NS;
    loop {
        let v = stats(ring, fid)?;
        if v[at] >= want {
            return Ok(v);
        }
        check!(ring.now() < until, "a fixture counter reaches its value", at);
        let _ = libthyla_rs::time::sleep(Duration::from_millis(5));
    }
}

fn wait_ready(child: &Child) -> R<()> {
    let Some(out) = child.stdout.as_ref() else {
        check!(false, "a piped stdout", 0);
        return Ok(());
    };
    let fd = out.as_raw_fd();
    let mut p = [TPollFd { fd, events: T_POLLIN, revents: 0 }];
    let n = unsafe { t_poll(p.as_mut_ptr(), 1, 5000) };
    check!(n == 1, "a victim gets its operation submitted", n);
    let mut b = [0u8; 1];
    let n = unsafe { t_read(fd as i64, b.as_mut_ptr(), 1) };
    check!(n == 1 && b[0] == b'H', "a victim says it is ready", n);
    Ok(())
}

// The point of the probe: however the server behaves, the program ends. The packed
// word, so a victim that exited 1 is not read as one that was killed.
fn reap(ring: &ServiceRing, child: &Child) -> R<i32> {
    let until = ring.now() + WAIT_NS;
    loop {
        let mut status = 0i32;
        let rc = unsafe { t_wait_pid_for(child.pid(), T_WAIT_WNOHANG | T_WAIT_PACKED, &mut status) };
        check!(rc >= 0, "reap a victim", rc);
        if rc != 0 {
            return Ok(status);
        }
        check!(ring.now() < until, "a victim ends although its server is silent", child.pid());
        let _ = libthyla_rs::time::sleep(Duration::from_millis(5));
    }
}

fn run_case(ring: &ServiceRing, st: &Fid<K>, case: &Case) -> R<()> {
    say(case.role);
    let base = stats(ring, st)?;
    let mut command = Command::new(SELF);
    command
        .arg(case.role)
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .inherit_fd(REGISTRY);
    let child = ok!(command.spawn().map_err(|e| e.as_errno()), "spawn a victim");
    wait_ready(&child)?;
    stats_until(ring, st, case.reached, base[case.reached] + 1)?;
    match case.stop {
        Stop::Go => {
            let Some(go) = child.stdin.as_ref() else {
                check!(false, "a piped stdin", 0);
                return Ok(());
            };
            let n = unsafe { t_write(go.as_raw_fd() as i64, b"G".as_ptr(), 1) };
            check!(n == 1, "tell a victim to go", n);
        }
        Stop::Kill => {
            let _ = libthyla_rs::time::sleep(INTO_TURN);
            let rc = unsafe { t_postnote(child.pid() as i64, b"kill".as_ptr(), 4) };
            check!(rc == 0, "kill a victim", rc);
        }
    }
    let status = reap(ring, &child)?;
    if t_wait_if_exited(status) {
        say(&format!("{} ended, status {}", case.role, t_wait_exitstatus(status)));
    } else {
        say(&format!("{} ended, signal {}", case.role, t_wait_termsig(status)));
    }
    match case.status {
        Some(want) => check!(t_wait_if_exited(status) && t_wait_exitstatus(status) == want,
                             "a victim took the path it named", status),
        None => check!(t_wait_if_signaled(status) && t_wait_termsig(status) == SIGKILL,
                       "a killed victim died of the kill", status),
    }
    let v = if case.ends {
        stats_until(ring, st, ENDED, base[ENDED] + 1)?
    } else {
        stats(ring, st)?
    };
    check!(v[VIOLATIONS] == base[VIOLATIONS], "the victim's end broke no protocol rule", v[VIOLATIONS]);
    Ok(())
}

fn drive() -> R<i64> {
    let mut b = [0u8; 1];
    let n = unsafe { t_read(NOTIFY, b.as_mut_ptr(), 1) };
    check!(n == 1 && b[0] == b'R', "the fixture has posted its service", n);
    let ring = match ServiceRing::setup(ThylaKernel, ENTRIES, &BUFFERS) {
        Err(e) if e == -E_OPNOTSUPP => {
            say("default image: setup refuses -EOPNOTSUPP");
            return Ok(PASS_DEFAULT);
        }
        r => ok!(r, "the driver's private ring"),
    };
    let target = ok!(ring.target(REGISTRY, b"fixture"), "TARGET of the fixture");
    let s = connect(&ring, &target)?;
    let st = walk(&ring, &s, b"stats")?;
    open(&ring, &st)?;
    for case in CASES.iter() {
        run_case(&ring, &st, case)?;
    }
    say("every victim ended and its session with it");
    Ok(PASS_QUAL)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = libthyla_rs::env::args();
    let result = match args.get_str(1) {
        None => drive(),
        Some("after-exec") => after_exec(),
        Some(role) => victim(role),
    };
    match result {
        Ok(code) => code,
        Err(f) => {
            say(&format!("FAIL line {}: {} (got {})", f.line, f.why, f.got));
            1
        }
    }
}
