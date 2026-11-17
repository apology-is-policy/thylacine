//! The guest half of the paste latency record (ASYNC-SERVICE-LIFECYCLE, AS-4f;
//! HALCYON-INTERACTION 12), driven by tools/interactive/ls-halcyon-paste-latency.exp.
//! halcyond's test builds say each typing burst's latency themselves; this probe adds
//! what they cannot see from inside the loop: the session's wakeups while it is idle,
//! and a clipboard value of a chosen size to paste.
//!
//! `wakes <secs> <leg>` waits two seconds, reads /proc/<pid>/sched for every process
//! the caller may read (owner or CAP_HOSTOWNER), sleeps without printing, reads again
//! and says each process's dispatches and parks over the window. `fill <lines> <width> <leg>`
//! publishes `lines` numbered lines of `width` bytes, joined by LF. `fill-cmd <bytes>
//! <mark> <leg>` publishes one line of `bytes` bytes that is this probe's `mark <mark>`
//! command padded with letters, so a paste into a shell shows it landed when it runs.
use crate::clipboard_client;
use ::alloc::{format, string::String, vec, vec::Vec};
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

/// One process's counters: its name and, per thread, `tid`, `nsched` and `parks`
/// (columns 1, 5 and 6 of the table under the header).
struct Sample {
    pid: u32,
    name: String,
    threads: Vec<(u64, u64, u64)>,
}

impl Sample {
    fn sums(&self) -> (u64, u64) {
        self.threads.iter().fold((0, 0), |(n, p), t| (n + t.1, p + t.2))
    }
}

/// The kernel renders the file afresh on every read, so it is read in ONE call: a
/// row split across two reads could join two renders.
const SCHED_BUF: usize = 8192;

fn sched(pid: u32) -> Option<Vec<(u64, u64, u64)>> {
    let mut f = File::open(format!("/proc/{}/sched", pid).as_str()).ok()?;
    let mut buf = vec![0u8; SCHED_BUF];
    let n = f.read(&mut buf).ok()?;
    if n == 0 || n == SCHED_BUF {
        return None;
    }
    let text = &buf[..n];
    let at = text
        .windows(b"tid band cpu run_ns nsched parks nmig state".len())
        .position(|w| w == b"tid band cpu run_ns nsched parks nmig state")?;
    let mut threads = Vec::new();
    for row in text[at..].split(|b| *b == b'\n').skip(1) {
        let cols: Vec<&[u8]> = row.split(|b| *b == b' ').filter(|c| !c.is_empty()).collect();
        if cols.len() != 8 {
            continue;
        }
        threads.push((procs::decimal(cols[0])?, procs::decimal(cols[4])?, procs::decimal(cols[5])?));
    }
    Some(threads)
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
            Some(threads) => out.push(Sample {
                pid: r.pid,
                name: r.name,
                threads,
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
        // Each thread that woke in the window, by tid; a thread that came or went
        // inside it is in the counts below only through the sums.
        for t in &a.threads {
            if let Some(bt) = b.threads.iter().find(|bt| bt.0 == t.0) {
                if t.1 > bt.1 || t.2 > bt.2 {
                    t_putstr(&format!(
                        "paste-latency: thread {} {} pid={} tid={} nsched=+{} parks=+{}\n",
                        leg,
                        a.name,
                        a.pid,
                        t.0,
                        t.1.saturating_sub(bt.1),
                        t.2.saturating_sub(bt.2)
                    ));
                }
            }
        }
        let ((an, ap), (bn, bp)) = (a.sums(), b.sums());
        t_putstr(&format!(
            "paste-latency: wakes {} {} pid={} threads={}->{} nsched=+{} parks=+{} secs={}\n",
            leg,
            a.name,
            a.pid,
            b.threads.len(),
            a.threads.len(),
            an.saturating_sub(bn),
            ap.saturating_sub(bp),
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
    let total = width.checked_add(1).and_then(|w| w.checked_mul(lines));
    if lines == 0 || width == 0 || total.is_none_or(|t| t > FILL_MAX) {
        return Err("size out of range");
    }
    clipboard_client::publish(&filled(lines, width))
}

fn fill_cmd() -> Result<(), &'static str> {
    let bytes = number(3, "no size")? as usize;
    let mark = env::args().nth(4).ok_or("no mark")?;
    let mut line = Vec::new();
    line.extend_from_slice(b"kaua-term-probe --paste-latency mark ");
    line.extend_from_slice(mark);
    line.push(b' ');
    if bytes < line.len() + 1 || bytes > FILL_MAX || mark.iter().any(|b| !b.is_ascii_graphic()) {
        return Err("size out of range");
    }
    let mut i = 0usize;
    while line.len() < bytes {
        line.push(b'a' + (i % 26) as u8);
        i += 1;
    }
    clipboard_client::publish(&line)
}

pub fn run() -> i64 {
    let which = env::args().nth(2).unwrap_or(b"");
    let leg_at = match which {
        b"fill" | b"fill-cmd" => 5,
        b"mark" => 3,
        _ => 4,
    };
    let leg = env::args()
        .nth(leg_at)
        .map(String::from_utf8_lossy)
        .unwrap_or_default();
    let result = match which {
        b"wakes" => wakes(&leg),
        b"fill" => fill(),
        b"fill-cmd" => fill_cmd(),
        b"mark" => Ok(()),
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
