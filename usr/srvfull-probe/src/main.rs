// /bin/srvfull-probe -- a server whose connection pool is full must wait, not
// run. Spawned by joey at boot as the system principal (the only reader of
// another Proc's CPU time besides a hostowner).
//
// A server that leaves its listener asked-about while it has no room to accept
// is woken by every poll for as long as one connect waits: the loop never
// sleeps and takes a core. Nothing a client can see changes, so the witness is
// the server's own CPU time, read from /ctl/procs across a window that sits
// wholly inside one waiting connect.
//
// Per server, per attempt:
//   1. the probe opens one connection of its own (the slot it will give back);
//   2. a child (`srvfull-probe hold <srv> <a>`) opens connections until one
//      does not return: the pool is full and that connect waits;
//   3. from the instant `a` the probe reads the server's CPU time twice,
//      SAMPLE_MS apart, and checks the child is still alive afterwards;
//   4. the probe closes its own connection. The server has room again and must
//      serve the waiting connect: the child's open returns and it exits 0.
//
// The child vouches for the window: it exits 0 only if the connect that waited
// began at or before `a` and returned at or after `a + SAMPLE_MS`. A fill that
// was still running at `a` exits 3 and the attempt is taken again with twice
// the settle, so a slow boot costs time and never a verdict. What the child
// cannot vouch for is the few milliseconds between its open returning and its
// exit; the alive check after the second reading bounds that to its own
// teardown, far below the quarter of the window the verdict needs.
//
// Prints one "srvfull-probe: <server> OK -- ..." line per server and exits 0,
// or "srvfull-probe: FAIL <server> <what> -- ..." and exits 1. netd is probed
// wherever /srv/net is posted and reported NOT PROBED where it is not; the
// boot harness requires its OK line on every boot where netd reports itself up.

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
    t_close, t_open, t_putstr, t_read, t_spawn_full_argv, t_wait_pid_for, TSpawnArgs, T_OREAD,
    T_WAIT_WNOHANG, T_WALK_OPEN_FROM_ROOT,
};

/// (service path, the server's name in /ctl/procs, required). A server that
/// is not required is probed wherever its service is posted; the harness
/// requires its OK line wherever that server reports itself up.
const SERVERS: [(&str, &str, bool); 2] =
    [("/srv/corvus", "corvus", true), ("/srv/net", "netd", false)];
const SELF: &str = "/bin/srvfull-probe";

const SAMPLE_MS: u64 = 400;
const SETTLE_MS: u64 = 500;
/// The settle doubles per attempt, and the last one's settle plus the window
/// must stay well inside the kernel's connect handshake deadline (5 s), or
/// the waiting connect would fail on its own before the slot is freed.
const ATTEMPTS: u32 = 3;
/// No pool here is this large; a fill that reaches it never met a full one.
const FILL_BOUND: usize = 32;
/// A waiting server's share of the window. A loop that never sleeps takes all
/// of it; one that parks takes microseconds.
const BUSY_DIVISOR: u64 = 4;

const HOLD_SERVED: i32 = 0;
const HOLD_REFUSED: i32 = 2;
const HOLD_LATE: i32 = 3;
const HOLD_NEVER_FULL: i32 = 4;
const HOLD_USAGE: i32 = 5;

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

fn read_all(path: &str) -> Option<Vec<u8>> {
    let fd = connect(path);
    if fd < 0 {
        return None;
    }
    let mut out = Vec::new();
    let mut buf = [0u8; 1024];
    loop {
        let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
        if n < 0 {
            unsafe { t_close(fd) };
            return None;
        }
        if n == 0 {
            break;
        }
        out.extend_from_slice(&buf[..n as usize]);
    }
    unsafe { t_close(fd) };
    Some(out)
}

enum Cpu {
    Ns(u64),
    Absent,
    Withheld,
    Unreadable,
}

/// The CPU_NS column of the one /ctl/procs row whose NAME is `name`.
fn server_cpu(name: &str) -> Cpu {
    let Some(table) = read_all("/ctl/procs") else {
        return Cpu::Unreadable;
    };
    let mut found = Cpu::Absent;
    for line in table.split(|&b| b == b'\n') {
        let mut cols = line.split(|&b| b == b' ').filter(|c| !c.is_empty());
        let (Some(_pid), Some(_ppid), Some(n)) = (cols.next(), cols.next(), cols.next()) else {
            continue;
        };
        if n != name.as_bytes() {
            continue;
        }
        let Some(cpu) = cols.next_back() else { continue };
        if matches!(found, Cpu::Ns(_)) {
            // Two rows of one name: the reading would be of an unknown one.
            return Cpu::Unreadable;
        }
        found = match core::str::from_utf8(cpu).ok().and_then(|s| s.parse::<u64>().ok()) {
            Some(v) => Cpu::Ns(v),
            None => Cpu::Withheld,
        };
    }
    found
}

fn hold(path: &str, a_ns: u64) -> i32 {
    let mut held: Vec<i64> = Vec::new();
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
        if e <= a_ns {
            continue;
        }
        if p <= a_ns && e >= a_ns + SAMPLE_MS * MS {
            say(&format!(
                "srvfull-probe: hold {}: {} connects filled the pool; one more waited {} ms and was served\n",
                path,
                held.len() - 1,
                (e - p) / MS
            ));
            break HOLD_SERVED;
        }
        break HOLD_LATE;
    };
    for fd in held {
        unsafe { t_close(fd) };
    }
    code
}

/// joey starts this probe with no descriptors, so the holder is started with
/// none either (the library's Command hands down fds 0..2, which do not exist).
fn spawn_holder(path: &str, a_ns: u64) -> i64 {
    let mut argv: Vec<u8> = Vec::new();
    let a = format!("{}", a_ns);
    for part in [SELF, "hold", path, a.as_str()] {
        argv.extend_from_slice(part.as_bytes());
        argv.push(0);
    }
    let req = TSpawnArgs {
        name_va: SELF.as_ptr() as u64,
        argv_data_va: argv.as_ptr() as u64,
        fd_list_va: 0,
        name_len: SELF.len() as u32,
        argv_data_len: argv.len() as u32,
        argc: 4,
        fd_count: 0,
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
    Retry,
}

fn attempt(path: &str, name: &str, settle_ms: u64) -> Verdict {
    let own = connect(path);
    if own < 0 {
        return Verdict::Fail(format!("CONNECT -- the probe's own connect failed (rc {})", own));
    }
    let a = monotonic_ns() + settle_ms * MS;
    let child = spawn_holder(path, a);
    if child <= 0 {
        unsafe { t_close(own) };
        return Verdict::Fail(format!("SPAWN -- could not start the holder (rc {})", child));
    }
    let child = child as i32;

    sleep_until(a);
    let c1 = server_cpu(name);
    let t1 = monotonic_ns();
    sleep_until(t1 + SAMPLE_MS * MS);
    let c2 = server_cpu(name);
    let t2 = monotonic_ns();
    let mut early: i32 = -1;
    let reaped = unsafe { t_wait_pid_for(child, T_WAIT_WNOHANG, &mut early as *mut i32) };
    let alive = reaped == 0;

    unsafe { t_close(own) };
    let freed = monotonic_ns();
    let status = if alive {
        let mut st: i32 = -1;
        let rc = unsafe { t_wait_pid_for(child, 0, &mut st as *mut i32) };
        if rc == child as i64 { Some(st) } else { None }
    } else if reaped == child as i64 {
        Some(early)
    } else {
        None
    };
    let served_ms = (monotonic_ns() - freed) / MS;

    let (cpu1, cpu2) = match (c1, c2) {
        (Cpu::Ns(x), Cpu::Ns(y)) if y >= x => (x, y),
        (Cpu::Withheld, _) | (_, Cpu::Withheld) => {
            return Verdict::Fail(String::from("CPU -- the server's CPU time is withheld from this reader"));
        }
        (Cpu::Absent, _) | (_, Cpu::Absent) => {
            return Verdict::Fail(format!("CPU -- no /ctl/procs row is named {}", name));
        }
        _ => return Verdict::Fail(String::from("CPU -- /ctl/procs could not be read as one row of numbers")),
    };

    match status {
        Some(HOLD_SERVED) => {}
        Some(HOLD_LATE) => return Verdict::Retry,
        Some(HOLD_REFUSED) => {
            return Verdict::Fail(format!(
                "REARM -- a connect at the full pool failed: {}",
                if alive { "it waited, and was not served once a slot freed" } else { "refused before the slot freed" }
            ));
        }
        Some(HOLD_NEVER_FULL) => {
            return Verdict::Fail(format!("UNCONSTRUCTED -- {} connects met no full pool", FILL_BOUND));
        }
        other => return Verdict::Fail(format!("HOLDER -- the holder ended with {:?}", other)),
    }
    if !alive {
        return Verdict::Fail(String::from(
            "UNCONSTRUCTED -- the waiting connect was served before the probe freed its slot",
        ));
    }

    let window = t2 - t1;
    let ran = cpu2 - cpu1;
    let figures = format!(
        "with a connect waiting at the full pool the server ran {} us of CPU in {} ms (CPU_NS {} -> {}); the connect was served {} ms after a slot freed",
        ran / 1000,
        window / MS,
        cpu1,
        cpu2,
        served_ms
    );
    if ran.saturating_mul(BUSY_DIVISOR) >= window {
        return Verdict::Fail(format!("SPIN -- {}", figures));
    }
    Verdict::Ok(figures)
}

fn probe(path: &str, name: &str) -> bool {
    let mut settle = SETTLE_MS;
    for n in 1..=ATTEMPTS {
        match attempt(path, name, settle) {
            Verdict::Ok(figures) => {
                say(&format!("srvfull-probe: {} OK -- {} (attempt {} of {})\n", name, figures, n, ATTEMPTS));
                return true;
            }
            Verdict::Fail(why) => {
                say(&format!("srvfull-probe: FAIL {} {}\n", name, why));
                return false;
            }
            Verdict::Retry => settle *= 2,
        }
    }
    say(&format!(
        "srvfull-probe: FAIL {} UNCONSTRUCTED -- the pool was not full {} ms after the holder started\n",
        name,
        settle / 2
    ));
    false
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args: Vec<&'static [u8]> = libthyla_rs::env::args().operands().collect();
    if let Some(&b"hold") = args.first() {
        let path = args.get(1).and_then(|p| core::str::from_utf8(p).ok());
        let a = args
            .get(2)
            .and_then(|p| core::str::from_utf8(p).ok())
            .and_then(|s| s.parse::<u64>().ok());
        return match (path, a) {
            (Some(path), Some(a)) => hold(path, a) as i64,
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
