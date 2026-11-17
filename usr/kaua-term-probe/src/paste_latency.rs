//! The guest half of the paste latency record (ASYNC-SERVICE-LIFECYCLE, AS-4f;
//! HALCYON-INTERACTION 12), driven by tools/interactive/ls-halcyon-paste-latency.exp.
//! halcyond's test builds say each typing burst's latency themselves; this probe adds
//! what they cannot see from inside the loop: the session's wakeups while it is idle,
//! and a clipboard value of a chosen size to paste.
//!
//! `wakes <secs> <leg>` waits two seconds, reads /proc/<pid>/sched for every process
//! the caller may read (owner or CAP_HOSTOWNER), sleeps without printing, reads again
//! and says each process's dispatches and parks over the window. `fill <lines> <width> <leg>`
//! publishes `lines` numbered lines of `width` bytes, joined by LF. `fill-pad <bytes>
//! <leg>` publishes `bytes` capitals, A to Z over and over. `mark <leg> <pad> <typed>
//! <token>` checks a shell line made of a typed prefix, that paste and keys typed around
//! it: the token must be the alphabet's first `typed` lowercase letters with the whole
//! `pad`-capital run inside them, and it says after how many typed keys the paste landed.
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

/// The kernel renders the file afresh on every read, so it is read in ONE call: a
/// row split across two reads could join two renders.
const SCHED_BUF: usize = 8192;

fn header<'a>(text: &'a [u8], key: &[u8]) -> Option<&'a [u8]> {
    let line = text.split(|b| *b == b'\n').find(|l| l.starts_with(key))?;
    Some(trim(&line[key.len()..]))
}

fn trim(mut s: &[u8]) -> &[u8] {
    while let [b' ', rest @ ..] = s {
        s = rest;
    }
    while let [rest @ .., b' '] = s {
        s = rest;
    }
    s
}

/// The file's name and threads, or None when it is unreadable or its table is not
/// whole: the kernel commits whole rows into a fixed buffer, so a heavily threaded
/// process's tail can be missing, and a thread can come or go between the
/// `threads:` count and the rows.
fn sched(pid: u32) -> Option<(Vec<u8>, Vec<(u64, u64, u64)>)> {
    let mut f = File::open(format!("/proc/{}/sched", pid).as_str()).ok()?;
    let mut buf = vec![0u8; SCHED_BUF];
    let n = f.read(&mut buf).ok()?;
    if n == 0 || n == SCHED_BUF {
        return None;
    }
    let text = &buf[..n];
    let name = header(text, b"name:")?.to_vec();
    let count = procs::decimal(header(text, b"threads:")?)?;
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
    if threads.len() as u64 != count {
        return None;
    }
    Some((name, threads))
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
            Some((name, threads)) => out.push(Sample {
                pid: r.pid,
                name: String::from_utf8_lossy(&name).into_owned(),
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
        // The same pid under another name is a reused pid, not the same process.
        let Some(b) = before.iter().find(|b| b.pid == a.pid && b.name == a.name) else {
            continue;
        };
        // Each thread that woke in the window, by tid. The sums run over the threads
        // in the second snapshot: one seen in both by its growth, one born in the
        // window by its whole count; one that left in the window is not counted.
        let (mut dn, mut dp) = (0u64, 0u64);
        for t in &a.threads {
            let bt = b.threads.iter().find(|bt| bt.0 == t.0).copied().unwrap_or((t.0, 0, 0));
            dn += t.1.saturating_sub(bt.1);
            dp += t.2.saturating_sub(bt.2);
            {
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
        t_putstr(&format!(
            "paste-latency: wakes {} {} pid={} threads={}->{} nsched=+{} parks=+{} secs={}\n",
            leg,
            a.name,
            a.pid,
            b.threads.len(),
            a.threads.len(),
            dn,
            dp,
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

fn fill_pad() -> Result<(), &'static str> {
    let bytes = number(3, "no size")? as usize;
    if bytes == 0 || bytes > FILL_MAX {
        return Err("size out of range");
    }
    clipboard_client::publish(&(0..bytes).map(|i| b'A' + (i % 26) as u8).collect::<Vec<u8>>())
}

/// Where in `token` a `pad`-capital paste sits among `typed` typed keys, or why
/// the line is not that.
pub fn landed(token: &[u8], pad: usize, typed: usize) -> Result<usize, &'static str> {
    let at = token.iter().position(|b| b.is_ascii_uppercase()).ok_or("the paste is not in the line")?;
    let run = token[at..].iter().take_while(|b| b.is_ascii_uppercase()).count();
    if run != pad || token[at..at + run].iter().enumerate().any(|(i, b)| *b != b'A' + (i % 26) as u8) {
        return Err("the paste is not whole");
    }
    let keys = token[..at].iter().chain(&token[at + run..]);
    if token.len() - run != typed || keys.enumerate().any(|(i, b)| *b != b'a' + (i % 26) as u8) {
        return Err("typed keys missing, extra or out of order");
    }
    Ok(at)
}

fn mark(leg: &str) -> Result<(), &'static str> {
    let pad = number(4, "no paste size")? as usize;
    let typed = number(5, "no typed count")? as usize;
    let token = env::args().nth(6).ok_or("no line")?;
    let after = landed(token, pad, typed)?;
    t_putstr(&format!(
        "paste-latency: {}: the paste landed after {} of {} typed keys\n",
        leg, after, typed
    ));
    Ok(())
}

pub fn run() -> i64 {
    let which = env::args().nth(2).unwrap_or(b"");
    let leg_at = match which {
        b"fill" => 5,
        b"fill-pad" => 4,
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
        b"fill-pad" => fill_pad(),
        b"mark" => mark(&leg),
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
