// /bin/srvfull-probe -- a server whose connection pool is full must wait, not
// run. Spawned by joey at boot as the system principal (the only reader of
// another Proc's CPU time besides a hostowner).
//
// A server that leaves its listener asked-about while it has no room to accept
// is woken by every poll for as long as one connect waits: the loop never
// sleeps and takes a core. Nothing a client can see changes, so the witness is
// the server's own CPU time (`cpu_ns` in /proc/<pid>/status, charged at every
// tick and every switch), read across a window that sits wholly inside one
// waiting connect.
//
// Per server, per attempt:
//   1. the probe opens one connection of its own (the slot it will give back);
//   2. a holder child (`srvfull-probe hold <srv> <quiet-ms>`, a pipe at its
//      fd 0) writes one byte when it starts and one per connect that returns;
//   3. when no byte has come for QUIET_MS the probe takes the holder to be
//      waiting on a full pool, and reads the server's CPU time twice,
//      SAMPLE_MS apart;
//   4. the probe closes its own connection. The server has room again and must
//      serve the waiting connect: the holder's open returns, it writes the
//      two instants its last connect began and returned, and exits 0.
//
// Step 3's inference is then checked, not trusted: the attempt counts only if
// that connect began before the first reading and returned after the probe
// began to free its slot. One that began later (the quiet was a slow connect, not a
// full pool) or returned earlier (someone else freed a slot) retakes the
// attempt with twice the quiet. The waiting connect waits about QUIET_MS +
// SAMPLE_MS however long the holder took to start, far inside the kernel's
// connect handshake deadline (5 s); only a connect that takes longer than the
// longest quiet while the pool has room can exhaust the attempts, and that is
// a defect worth a red boot.
//
// Prints one "srvfull-probe: <server> OK -- ..." line per server and exits 0,
// or "srvfull-probe: FAIL <server> <what> -- ..." and exits 1. netd is probed
// wherever /srv/net is posted and reported NOT PROBED where it is not; the
// boot harness requires its OK line on every boot where netd served /net.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::time::Duration;
use libthyla_rs::time::{monotonic_ns, sleep};
use libthyla_rs::{
    t_close, t_open, t_pipe, t_poll, t_putstr, t_read, t_spawn_full_argv, t_wait_pid_for, t_write,
    TPollFd, TSpawnArgs, T_OREAD, T_POLLIN, T_WALK_OPEN_FROM_ROOT,
};

/// (service path, the server's name in /ctl/procs, required). A server that
/// is not required is probed wherever its service is posted; the harness
/// requires its OK line wherever that server reports itself up.
const SERVERS: [(&str, &str, bool); 2] =
    [("/srv/corvus", "corvus", true), ("/srv/net", "netd", false)];
const SELF: &str = "/bin/srvfull-probe";

const SAMPLE_MS: u64 = 400;
/// No completed connect for this long reads as "the holder is waiting". It
/// doubles per attempt.
const QUIET_MS: u64 = 250;
const ATTEMPTS: u32 = 3;
/// How long the holder may take to start and fill before the first quiet.
const FILL_WAIT_MS: u64 = 20_000;
/// No pool here is this large; a fill that reaches it never met a full one.
const FILL_BOUND: usize = 32;
/// A waiting server's share of the window. A loop that never sleeps takes all
/// of it; one that parks takes microseconds.
const BUSY_DIVISOR: u64 = 4;
/// Both kernel files are rendered afresh on every read at an offset, so each
/// is read in one call; this is the kernel's own bound on either.
const CTL_BUF: usize = 4096;

const HOLD_SERVED: i32 = 0;
const HOLD_REFUSED: i32 = 2;
const HOLD_NEVER_FULL: i32 = 4;
const HOLD_USAGE: i32 = 5;

const STARTED: u8 = b's';
const CONNECTED: u8 = b'c';
const REPORT: u8 = b'R';
const REPORT_LEN: usize = 17;

const MS: u64 = 1_000_000;

fn say(s: &str) {
    t_putstr(s);
}

fn connect(path: &str) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) }
}

fn sleep_until(t_ns: u64) {
    loop {
        let now = monotonic_ns();
        if now >= t_ns {
            return;
        }
        let _ = sleep(Duration::from_nanos(t_ns - now));
    }
}

/// One read of a kernel text file. None: unreadable, or it filled the buffer
/// (the tail may be cut, and a second read would be of a different render).
fn read_once(path: &str) -> Option<Vec<u8>> {
    let fd = connect(path);
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; CTL_BUF];
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(fd) };
    if n <= 0 || n as usize >= CTL_BUF {
        return None;
    }
    Some(buf[..n as usize].to_vec())
}

fn parse_u64(b: &[u8]) -> Option<u64> {
    core::str::from_utf8(b).ok()?.parse::<u64>().ok()
}

enum Pid {
    Is(u64),
    Absent,
    Twice,
    Unreadable,
}

/// The PID of the one /ctl/procs row whose NAME is `name`.
fn server_pid(name: &str) -> Pid {
    let Some(table) = read_once("/ctl/procs") else {
        return Pid::Unreadable;
    };
    let mut found = Pid::Absent;
    for line in table.split(|&b| b == b'\n') {
        let mut cols = line.split(|&b| b == b' ').filter(|c| !c.is_empty());
        let (Some(pid), Some(_ppid), Some(n)) = (cols.next(), cols.next(), cols.next()) else {
            continue;
        };
        if n != name.as_bytes() {
            continue;
        }
        if matches!(found, Pid::Is(_)) {
            return Pid::Twice;
        }
        found = match parse_u64(pid) {
            Some(v) => Pid::Is(v),
            None => return Pid::Unreadable,
        };
    }
    found
}

enum Cpu {
    Ns(u64),
    Withheld,
    Unreadable,
}

fn field<'a>(status: &'a [u8], key: &str) -> Option<&'a [u8]> {
    for line in status.split(|&b| b == b'\n') {
        if let Some(rest) = line.strip_prefix(key.as_bytes()) {
            let start = rest.iter().position(|&b| b != b' ')?;
            return Some(&rest[start..]);
        }
    }
    None
}

/// `cpu_ns` of /proc/<pid>/status, and only while that pid is still `name`.
fn server_cpu(pid: u64, name: &str) -> Cpu {
    let Some(status) = read_once(&format!("/proc/{}/status", pid)) else {
        return Cpu::Unreadable;
    };
    if field(&status, "name:") != Some(name.as_bytes()) {
        return Cpu::Unreadable;
    }
    match field(&status, "cpu_ns:") {
        Some(b"-") => Cpu::Withheld,
        Some(v) => parse_u64(v).map(Cpu::Ns).unwrap_or(Cpu::Unreadable),
        None => Cpu::Unreadable,
    }
}

/// The holder. fd 0 is the write end of the probe's pipe. A connect that took
/// at least `quiet_ms` is the one the probe read as "waiting": the holder
/// reports it and stops.
fn hold(path: &str, quiet_ms: u64) -> i32 {
    let mut held: Vec<i64> = Vec::new();
    let _ = unsafe { t_write(0, &STARTED, 1) };
    let code = loop {
        if held.len() >= FILL_BOUND {
            say(&format!("srvfull-probe: hold {}: {} connects and none waited\n", path, held.len()));
            break HOLD_NEVER_FULL;
        }
        let p = monotonic_ns();
        let fd = connect(path);
        let e = monotonic_ns();
        if fd < 0 {
            say(&format!(
                "srvfull-probe: hold {}: connect {} failed (rc {} after {} ms)\n",
                path,
                held.len() + 1,
                fd,
                (e - p) / MS
            ));
            break HOLD_REFUSED;
        }
        held.push(fd);
        if e - p < quiet_ms * MS {
            let _ = unsafe { t_write(0, &CONNECTED, 1) };
            continue;
        }
        let mut report = [0u8; REPORT_LEN];
        report[0] = REPORT;
        report[1..9].copy_from_slice(&p.to_le_bytes());
        report[9..17].copy_from_slice(&e.to_le_bytes());
        let _ = unsafe { t_write(0, report.as_ptr(), report.len()) };
        say(&format!(
            "srvfull-probe: hold {}: {} connects filled the pool; one more waited {} ms and was served\n",
            path,
            held.len() - 1,
            (e - p) / MS
        ));
        break HOLD_SERVED;
    };
    for fd in held {
        unsafe { t_close(fd) };
    }
    code
}

/// joey starts this probe with no descriptors, so the holder is handed exactly
/// one: the pipe's write end, as its fd 0.
fn spawn_holder(path: &str, quiet_ms: u64, pipe_wr: i64) -> i64 {
    let mut argv: Vec<u8> = Vec::new();
    let wait = format!("{}", quiet_ms);
    for part in [SELF, "hold", path, wait.as_str()] {
        argv.extend_from_slice(part.as_bytes());
        argv.push(0);
    }
    let fds = [pipe_wr as u32];
    let req = TSpawnArgs {
        name_va: SELF.as_ptr() as u64,
        argv_data_va: argv.as_ptr() as u64,
        fd_list_va: fds.as_ptr() as u64,
        name_len: SELF.len() as u32,
        argv_data_len: argv.len() as u32,
        argc: 4,
        fd_count: 1,
        perm_flags: 0,
        ext_flags: 0,
        cap_mask: 0,
        principal_id: 0,
        primary_gid: 0,
        supp_gids_va: 0,
        supp_gid_count: 0,
        identity_flags: 0,
        allowance_va: 0,
        allowance_flags: 0,
        page_budget: 0,
        pheno_flags: 0,
        debug_flags: 0,
    };
    unsafe { t_spawn_full_argv(&req) }
}

enum Verdict {
    Ok(String),
    Fail(String),
    Retry(String),
}

enum Fill {
    Quiet,
    Ended,
    Slow,
}

/// Drain the pipe until it has been silent for `quiet_ms` (the holder waits),
/// it closes (the holder ended), or FILL_WAIT_MS passes.
fn await_quiet(rd: i64, quiet_ms: u64, got: &mut Vec<u8>) -> Fill {
    let deadline = monotonic_ns() + FILL_WAIT_MS * MS;
    loop {
        let mut pf = [TPollFd { fd: rd as i32, events: T_POLLIN, revents: 0 }];
        let rc = unsafe { t_poll(pf.as_mut_ptr(), 1, quiet_ms as i32) };
        if rc == 0 {
            // Silence counts only once the holder has shown it is running.
            if !got.is_empty() {
                return Fill::Quiet;
            }
        } else {
            let mut buf = [0u8; 64];
            let n = unsafe { t_read(rd, buf.as_mut_ptr(), buf.len()) };
            if n <= 0 {
                return Fill::Ended;
            }
            got.extend_from_slice(&buf[..n as usize]);
        }
        if monotonic_ns() >= deadline {
            return Fill::Slow;
        }
    }
}

fn drain(rd: i64, got: &mut Vec<u8>) {
    let mut buf = [0u8; 64];
    loop {
        let n = unsafe { t_read(rd, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            return;
        }
        got.extend_from_slice(&buf[..n as usize]);
    }
}

fn reap(child: i32) -> Option<i32> {
    let mut st: i32 = -1;
    let rc = unsafe { t_wait_pid_for(child, 0, &mut st as *mut i32) };
    if rc == child as i64 {
        Some(st)
    } else {
        None
    }
}

fn attempt(path: &str, name: &str, quiet_ms: u64) -> Verdict {
    let pid = match server_pid(name) {
        Pid::Is(p) => p,
        Pid::Absent => return Verdict::Fail(format!("CPU -- no /ctl/procs row is named {}", name)),
        Pid::Twice => return Verdict::Fail(format!("CPU -- two /ctl/procs rows are named {}", name)),
        Pid::Unreadable => {
            return Verdict::Fail(String::from("CPU -- /ctl/procs could not be read whole in one call"))
        }
    };
    let own = connect(path);
    if own < 0 {
        return Verdict::Fail(format!("CONNECT -- the probe's own connect failed (rc {})", own));
    }
    let (rd, wr) = unsafe { t_pipe() };
    if rd < 0 || wr < 0 {
        unsafe { t_close(own) };
        return Verdict::Fail(format!("PIPE -- no pipe for the holder (rc {} {})", rd, wr));
    }
    let child = spawn_holder(path, quiet_ms, wr);
    unsafe { t_close(wr) };
    if child <= 0 {
        unsafe {
            t_close(rd);
            t_close(own);
        }
        return Verdict::Fail(format!("SPAWN -- could not start the holder (rc {})", child));
    }
    let child = child as i32;

    let mut got: Vec<u8> = Vec::new();
    let fill = await_quiet(rd, quiet_ms, &mut got);
    let filled = got.iter().filter(|&&b| b == CONNECTED).count();

    // The readings. Taken even when the fill did not go quiet: the slot is
    // freed and the holder reaped on one path either way.
    let before = monotonic_ns();
    let c1 = server_cpu(pid, name);
    sleep_until(monotonic_ns() + SAMPLE_MS * MS);
    let c2 = server_cpu(pid, name);
    let after = monotonic_ns();

    let freeing = monotonic_ns();
    unsafe { t_close(own) };
    drain(rd, &mut got);
    unsafe { t_close(rd) };
    let status = reap(child);

    match fill {
        Fill::Quiet => {}
        Fill::Ended => {
            return match status {
                Some(HOLD_REFUSED) => Verdict::Fail(String::from(
                    "REFUSED -- a connect failed before any waited (the holder's line says which)",
                )),
                Some(HOLD_NEVER_FULL) => {
                    Verdict::Fail(format!("UNCONSTRUCTED -- {} connects met no full pool", FILL_BOUND))
                }
                other => Verdict::Fail(format!("HOLDER -- the holder ended early with {:?}", other)),
            };
        }
        Fill::Slow => {
            return Verdict::Fail(format!(
                "UNCONSTRUCTED -- the holder was still connecting after {} ms ({} connects)",
                FILL_WAIT_MS, filled
            ));
        }
    }
    match status {
        Some(HOLD_SERVED) => {}
        Some(HOLD_REFUSED) => {
            return Verdict::Fail(String::from(
                "REARM -- a connect waited at the full pool and was not served once a slot freed",
            ));
        }
        other => return Verdict::Fail(format!("HOLDER -- the holder ended with {:?}", other)),
    }
    let (began, returned) = match got.len().checked_sub(REPORT_LEN).map(|i| &got[i..]) {
        Some(r) if r[0] == REPORT => {
            let mut p = [0u8; 8];
            let mut e = [0u8; 8];
            p.copy_from_slice(&r[1..9]);
            e.copy_from_slice(&r[9..17]);
            (u64::from_le_bytes(p), u64::from_le_bytes(e))
        }
        _ => return Verdict::Fail(String::from("HOLDER -- the holder exited 0 without its report")),
    };
    if began > before {
        return Verdict::Retry(format!(
            "the connect that waited began {} us after the first reading",
            (began - before) / 1000
        ));
    }
    if returned < freeing {
        return Verdict::Retry(format!(
            "the waiting connect returned {} us before the probe freed its slot",
            (freeing - returned) / 1000
        ));
    }

    let (cpu1, cpu2) = match (c1, c2) {
        (Cpu::Ns(x), Cpu::Ns(y)) if y >= x => (x, y),
        (Cpu::Withheld, _) | (_, Cpu::Withheld) => {
            return Verdict::Fail(String::from("CPU -- the server's CPU time is withheld from this reader"));
        }
        _ => {
            return Verdict::Fail(format!(
                "CPU -- /proc/{}/status did not give {}'s cpu_ns twice",
                pid, name
            ))
        }
    };
    let window = after - before;
    let ran = cpu2 - cpu1;
    let figures = format!(
        "{} connects filled the pool; with one more waiting the server ran {} us of CPU in {} ms (cpu_ns {} -> {}); it was served {} us after the probe began to free its slot",
        filled,
        ran / 1000,
        window / MS,
        cpu1,
        cpu2,
        (returned - freeing) / 1000
    );
    if ran.saturating_mul(BUSY_DIVISOR) >= window {
        return Verdict::Fail(format!("SPIN -- {}", figures));
    }
    Verdict::Ok(figures)
}

fn probe(path: &str, name: &str) -> bool {
    let mut quiet = QUIET_MS;
    let mut last = String::new();
    for n in 1..=ATTEMPTS {
        match attempt(path, name, quiet) {
            Verdict::Ok(figures) => {
                say(&format!("srvfull-probe: {} OK -- {} (attempt {} of {})\n", name, figures, n, ATTEMPTS));
                return true;
            }
            Verdict::Fail(why) => {
                say(&format!("srvfull-probe: FAIL {} {}\n", name, why));
                return false;
            }
            Verdict::Retry(why) => {
                say(&format!(
                    "srvfull-probe: {} attempt {} of {} not judged (quiet {} ms): {}\n",
                    name, n, ATTEMPTS, quiet, why
                ));
                last = why;
                quiet *= 2;
            }
        }
    }
    say(&format!(
        "srvfull-probe: FAIL {} UNCONSTRUCTED -- no attempt sat inside one waiting connect: {}\n",
        name, last
    ));
    false
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args: Vec<&'static [u8]> = libthyla_rs::env::args().operands().collect();
    if let Some(&b"hold") = args.first() {
        let path = args.get(1).and_then(|p| core::str::from_utf8(p).ok());
        let wait = args.get(2).and_then(|p| parse_u64(p));
        return match (path, wait) {
            (Some(path), Some(wait)) => hold(path, wait) as i64,
            _ => HOLD_USAGE as i64,
        };
    }
    let mut ok = true;
    for (path, name, required) in SERVERS {
        if !required {
            let fd = connect(path);
            if fd < 0 {
                say(&format!("srvfull-probe: {} NOT PROBED -- {} is not posted (rc {})\n", name, path, fd));
                continue;
            }
            unsafe { t_close(fd) };
        }
        ok &= probe(path, name);
    }
    if ok {
        say("srvfull-probe: DONE\n");
        0
    } else {
        1
    }
}
