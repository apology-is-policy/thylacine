//! The guest half of the paste latency record (ASYNC-SERVICE-LIFECYCLE, AS-4f;
//! HALCYON-INTERACTION 12), driven by tools/interactive/ls-halcyon-paste-latency.exp.
//! halcyond's test builds say each typing burst's latency themselves; this probe adds
//! what they cannot see from inside the loop: the session's wakeups while it is idle,
//! and a clipboard value of a chosen size to paste.
//!
//! `wakes <secs> <leg>` waits two seconds, reads /proc/<pid>/sched for every process
//! the caller may read (owner or CAP_HOSTOWNER), sleeps without printing, reads again
//! and says each process's dispatches and parks over the window. `fill <lines> <width> <leg>`
//! publishes `lines` numbered lines of `width` bytes, joined by LF.
use crate::clipboard_client;
use ::alloc::{format, string::String, vec::Vec};
use core::time::Duration;
use libthyla_rs::{fs::File, io::Read, procs, *};

const SECS_MAX: u64 = 600;
const SETTLE_SECS: u64 = 2;
const FILL_MAX: usize = 1 << 20;

fn number(n: usize, what: &'static str) -> Result<u64, &'static str> {
    env::args()
        .nth(n)
        .and_then(procs::decimal)
        .ok_or(what)
}

/// One process's counters: its name, threads, and the sums of `nsched` and
/// `parks` over its threads (columns 5 and 6 of the table under the header).
struct Sample {
    pid: u32,
    name: String,
    threads: u32,
    nsched: u64,
    parks: u64,
}

fn sched(pid: u32) -> Option<(u32, u64, u64)> {
    let mut f = File::open(format!("/proc/{}/sched", pid).as_str()).ok()?;
    let mut text = Vec::new();
    f.read_to_end(&mut text).ok()?;
    let at = text
        .windows(b"tid band cpu run_ns nsched parks nmig state".len())
        .position(|w| w == b"tid band cpu run_ns nsched parks nmig state")?;
    let mut threads = 0u32;
    let (mut nsched, mut parks) = (0u64, 0u64);
    for row in text[at..].split(|b| *b == b'\n').skip(1) {
        let cols: Vec<&[u8]> = row.split(|b| *b == b' ').filter(|c| !c.is_empty()).collect();
        if cols.len() != 8 {
            continue;
        }
        nsched += procs::decimal(cols[4])?;
        parks += procs::decimal(cols[5])?;
        threads += 1;
    }
    Some((threads, nsched, parks))
}

/// Every readable process but this one; `skipped` counts the ones the kernel
/// refused or that left mid-read.
fn snapshot(me: u32) -> Result<(Vec<Sample>, u32), &'static str> {
    let rows = procs::list().map_err(|_| "list /proc")?;
    let mut out = Vec::new();
    let mut skipped = 0;
    for r in rows {
        if r.pid == me {
            continue;
        }
        match sched(r.pid) {
            Some((threads, nsched, parks)) => out.push(Sample {
                pid: r.pid,
                name: r.name,
                threads,
                nsched,
                parks,
            }),
            None => skipped += 1,
        }
    }
    Ok((out, skipped))
}

fn wakes(leg: &str) -> Result<(), &'static str> {
    let secs = number(3, "no seconds")?;
    if secs == 0 || secs > SECS_MAX {
        return Err("seconds out of range");
    }
    let me = unsafe { t_getpid() } as u32;
    // The keys that ran this command opened a burst in halcyond's test builds; it
    // ends (and says its line) inside this pause, not inside the window.
    let _ = time::sleep(Duration::from_secs(SETTLE_SECS));
    let (before, _) = snapshot(me)?;
    if before.is_empty() {
        return Err("no process readable");
    }
    let _ = time::sleep(Duration::from_secs(secs));
    let (after, skipped) = snapshot(me)?;
    let mut said = 0;
    for a in &after {
        let Some(b) = before.iter().find(|b| b.pid == a.pid) else {
            continue;
        };
        t_putstr(&format!(
            "paste-latency: wakes {} {} pid={} threads={}->{} nsched=+{} parks=+{} secs={}\n",
            leg,
            a.name,
            a.pid,
            b.threads,
            a.threads,
            a.nsched.saturating_sub(b.nsched),
            a.parks.saturating_sub(b.parks),
            secs
        ));
        said += 1;
    }
    t_putstr(&format!(
        "paste-latency: wakes {} processes={} skipped={}\n",
        leg, said, skipped
    ));
    if said == 0 {
        return Err("no process lasted the window");
    }
    Ok(())
}

/// `lines` lines of `width` bytes: a five-digit number, a space, then the
/// alphabet over and over, so no two neighbouring lines read the same.
pub fn filled(lines: usize, width: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(lines * (width + 1));
    for n in 0..lines {
        if n > 0 {
            out.push(b'\n');
        }
        let head = format!("{:05} ", n % 100_000);
        for i in 0..width {
            out.push(match head.as_bytes().get(i) {
                Some(b) => *b,
                None => b'a' + ((i + n) % 26) as u8,
            });
        }
    }
    out
}

fn fill() -> Result<(), &'static str> {
    let lines = number(3, "no line count")? as usize;
    let width = number(4, "no width")? as usize;
    if lines == 0 || width == 0 || lines.saturating_mul(width + 1) > FILL_MAX {
        return Err("size out of range");
    }
    clipboard_client::publish(&filled(lines, width))
}

pub fn run() -> i64 {
    let which = env::args().nth(2).unwrap_or(b"");
    let leg_at = if which == b"fill" { 5 } else { 4 };
    let leg = env::args()
        .nth(leg_at)
        .map(String::from_utf8_lossy)
        .unwrap_or_default();
    let result = match which {
        b"wakes" => wakes(&leg),
        b"fill" => fill(),
        _ => Err("no such verb"),
    };
    match result {
        Ok(()) => {
            t_putstr(&format!("paste-latency: {} PASS\n", leg));
            0
        }
        Err(e) => {
            t_putstr(&format!("paste-latency: FAIL {} -- {}\n", leg, e));
            1
        }
    }
}
