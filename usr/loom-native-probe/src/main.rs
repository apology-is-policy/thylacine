// /bin/loom-native-probe -- the Rust native Loom adapter driven from EL0
// against /bin/loom-fixture (docs/ASYNC-SERVICE-ABI.md, NA-4d). The kernel test
// loom.native_probe_rust starts both and requires 78 from the default image,
// where setup refuses -EOPNOTSUPP, or 79 from the qualification image, where
// every leg below passed. No other program exits with either code.
//
// fd 0 is the registry the fixture posts in, fd 1 the control pipe (closed at
// exit, which ends the fixture) and fd 2 the notify pipe. The legs read the
// fixture's stats file for what the server saw: the sessions it accepted and
// ended, its stalled reads, Twalks and Tclunks, and the violations it counted.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::vec::Vec;
use core::time::Duration;
use libthyla_rs::loom::native::{Fid, Payload, PoolMember, Scope, Shot, Stream, Target};
use libthyla_rs::loom::{ServiceRing, ThylaKernel};
use libthyla_rs::{t_putstr, t_read};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

type K = ThylaKernel;

const PASS_DEFAULT: i64 = 78;
const PASS_QUAL: i64 = 79;
const REGISTRY: i32 = 0;
const NOTIFY: i64 = 2;

// The kernel's errno registry (kernel/include/thylacine/errno.h).
const E_NOENT: i32 = 2;
const E_ACCES: i32 = 13;
const E_NOSPC: i32 = 28;
const E_OPNOTSUPP: i32 = 95;
const E_TIMEDOUT: i32 = 110;
const E_CANCELED: i32 = 125;

const PAGE: u64 = 4096;
const ENTRIES: u32 = 2;
const CQ_ENTRIES: usize = 4;
// 0: fixed data and sink I/O; 1: pool A; 2: stats; 3: pool B; 4: stalled reads.
const BUFFERS: [usize; 5] = [PAGE as usize; 5];
const WAIT_NS: u64 = 5_000_000_000;
// Targets, scopes, fids and pools share the kernel's LOOM_SERVICE_SLOT_COUNT
// slots (kernel/include/thylacine/loom_service_abi.h).
const SLOTS: usize = 64;

// The fixture's files and its stats counters (usr/loom-fixture).
const DATA_LEN: usize = 3 * 4096 + 1000;
const LEAF_LEN: usize = 600;
const STREAM_LEN: usize = 2600;
const SHOT: u64 = 512;
const ACCEPTED: usize = 0;
const ENDED: usize = 1;
const VIOLATIONS: usize = 2;
const STALLED: usize = 3;
const CLUNKS: usize = 5;
const WALKS: usize = 7;
const STAT_COUNT: usize = 11;

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
// An adapter call that must succeed.
macro_rules! ok {
    ($e:expr, $why:literal) => {
        match $e {
            Ok(v) => v,
            Err(e) => return Err(Fail { line: line!(), why: $why, got: e as i64 }),
        }
    };
}
// An adapter call that must answer exactly -$code.
macro_rules! refused {
    ($e:expr, $code:expr, $why:literal) => {
        match $e {
            Ok(_) => return Err(Fail { line: line!(), why: $why, got: 0 }),
            Err(e) => check!(e == -$code, $why, e),
        }
    };
}
// Turn the ring until a future completes; its result must succeed.
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
// Turn the ring until a future completes; it must answer exactly -$code.
macro_rules! done_refused {
    ($ring:expr, $fut:expr, $code:expr, $why:literal) => {{
        let f = $fut;
        let until = Some($ring.now() + WAIT_NS);
        match $ring.block_on(f, until) {
            Ok(Ok(_)) => return Err(Fail { line: line!(), why: $why, got: 0 }),
            Ok(Err(e)) => check!(e == -$code, $why, e),
            Err(e) => {
                return Err(Fail { line: line!(), why: concat!($why, ": no completion"), got: e as i64 })
            }
        }
    }};
}
// Return a lease now; the kernel must accept it.
macro_rules! returned {
    ($p:expr, $why:literal) => {
        if let Err((e, _)) = $p.try_return() {
            return Err(Fail { line: line!(), why: $why, got: e as i64 });
        }
    };
}

fn say(line: &str) {
    t_putstr(&format!("loom-native-probe: {}\n", line));
}

// The fixture's bytes: a period of 251 does not divide a page or a shot.
fn matches(bytes: &[u8], from: usize) -> bool {
    bytes.iter().enumerate().all(|(i, b)| *b == ((from + i) % 251) as u8)
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

fn open(ring: &ServiceRing, fid: &Fid<K>, mode: u32) -> R<()> {
    done!(ring, ok!(fid.open(mode, soon(ring)), "submit an LOPEN"), "an LOPEN");
    Ok(())
}

fn connect(ring: &ServiceRing, target: &Target<K>) -> R<Scope<K>> {
    Ok(done!(ring, ok!(target.connect(soon(ring)), "submit a CONNECT"), "a CONNECT"))
}

fn stats(ring: &ServiceRing, fid: &Fid<K>) -> R<[u32; STAT_COUNT]> {
    let read = done!(ring, ok!(fid.read(range(2, 0, 64), 0, soon(ring)), "submit a stats read"), "a stats read");
    let b = read.bytes();
    check!(b.len() == 4 * STAT_COUNT, "the stats file holds one u32 per counter", b.len());
    let mut v = [0u32; STAT_COUNT];
    for (i, w) in v.iter_mut().enumerate() {
        *w = u32::from_le_bytes([b[4 * i], b[4 * i + 1], b[4 * i + 2], b[4 * i + 3]]);
    }
    Ok(v)
}

// Reads the stats until counter `at` reaches `want`.
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

// Every byte a multishot stream yields, in arrival order, after checking it
// ends with the 0 of its EOF read. With `swap` it holds one lease and returns
// the newer of each pair first, the opposite order to their arrival.
fn drain(ring: &ServiceRing, st: &mut Stream<K>, swap: bool) -> R<Vec<u8>> {
    let mut got = Vec::new();
    let mut held: Option<Payload<K>> = None;
    loop {
        match done!(ring, st.next(), "the next shot") {
            Shot::Payload(p) => {
                check!(p.bytes().len() as u64 <= SHOT, "a shot fits its member", p.bytes().len());
                match held.take() {
                    Some(older) => {
                        got.extend_from_slice(older.bytes());
                        got.extend_from_slice(p.bytes());
                        returned!(p, "return the newer lease first");
                        returned!(older, "then the older one");
                    }
                    None if swap => held = Some(p),
                    None => {
                        got.extend_from_slice(p.bytes());
                        returned!(p, "return a lease");
                    }
                }
            }
            Shot::End(rc) => {
                check!(rc == 0, "a multishot stream ends with its EOF read's 0", rc);
                break;
            }
        }
    }
    if let Some(p) = held.take() {
        got.extend_from_slice(p.bytes());
        returned!(p, "return the last lease");
    }
    Ok(got)
}

fn shot(ring: &ServiceRing, st: &mut Stream<K>) -> R<Payload<K>> {
    match done!(ring, st.next(), "a shot") {
        Shot::Payload(p) => Ok(p),
        Shot::End(rc) => Err(Fail { line: line!(), why: "a stream ended before its shot", got: rc as i64 }),
    }
}

// Walks `data` under `scope` until the slot table is full, which refuses the
// next WALK -ENOSPC at submit, then drops every fid it walked. Returns how many
// fit.
fn fill(ring: &ServiceRing, scope: &Scope<K>) -> R<usize> {
    let mut held = Vec::new();
    loop {
        match scope.walk(b"data", soon(ring)) {
            Ok(w) => held.push(done!(ring, w, "a WALK into a free slot")),
            Err(e) => {
                check!(e == -E_NOSPC, "a full slot table refuses the WALK at submit", e);
                break;
            }
        }
        check!(held.len() <= SLOTS, "the slot table holds at most its slots", held.len());
    }
    Ok(held.len())
}

fn run() -> R<i64> {
    let mut b = [0u8; 1];
    let n = unsafe { t_read(NOTIFY, b.as_mut_ptr(), 1) };
    check!(n == 1 && b[0] == b'R', "the fixture has posted its service", n);
    let ring = match ServiceRing::setup(ThylaKernel, ENTRIES, &BUFFERS) {
        Err(e) if e == -E_OPNOTSUPP => {
            say("default image: setup refuses -EOPNOTSUPP");
            return Ok(PASS_DEFAULT);
        }
        r => ok!(r, "a private ring in the qualification image"),
    };

    say("connect, walk, open");
    refused!(ring.target(REGISTRY, b"nosuch"), E_NOENT, "TARGET of a name nobody posted");
    let target = ok!(ring.target(REGISTRY, b"fixture"), "TARGET of the fixture");
    let s1 = connect(&ring, &target)?;
    let stats_fid = walk(&ring, &s1, b"stats")?;
    open(&ring, &stats_fid, 0)?;
    let base = stats(&ring, &stats_fid)?;
    check!(base[ACCEPTED] == 1 && base[ENDED] == 0, "the fixture holds one session", base[ACCEPTED]);
    done_refused!(ring, ok!(s1.walk(b"nosuch", soon(&ring)), "submit a WALK"), E_NOENT,
                  "a missing name's WALK is refused");
    let v = stats(&ring, &stats_fid)?;
    check!(v[WALKS] == base[WALKS] + 1 && v[VIOLATIONS] == base[VIOLATIONS],
           "the server answered that Twalk itself", v[WALKS]);
    let data = walk(&ring, &s1, b"data")?;
    done_refused!(ring, ok!(data.open(1, soon(&ring)), "submit an LOPEN"), E_ACCES,
                  "the kernel's own stat check refuses writing a 0444 file");
    open(&ring, &data, 0)?;
    let sink = walk(&ring, &s1, b"sink")?;
    open(&ring, &sink, 2)?;

    say("a walk from a fid below the root");
    let dir = walk(&ring, &s1, b"dir")?;
    let leaf = done!(ring, ok!(dir.walk(b"leaf", soon(&ring)), "submit a WALK from a directory fid"),
                     "a WALK from a directory fid");
    open(&ring, &leaf, 0)?;
    let r = done!(ring, ok!(leaf.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), "a READ");
    check!(r.bytes().len() == LEAF_LEN && matches(r.bytes(), 0), "the file below a directory", r.bytes().len());
    drop(r);

    say("fixed reads and writes");
    let r = done!(ring, ok!(data.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), "a READ");
    check!(r.bytes().len() == PAGE as usize && matches(r.bytes(), 0), "a fixed READ's bytes", r.bytes().len());
    drop(r);
    let tail = (3 * PAGE) as usize;
    let r = done!(ring, ok!(data.read(range(0, 0, PAGE), tail as u64, soon(&ring)), "submit a READ"), "a READ");
    check!(r.bytes().len() == DATA_LEN - tail && matches(r.bytes(), tail), "a short READ at the end", r.bytes().len());
    drop(r);
    let mut msg = [0u8; 100];
    for (i, m) in msg.iter_mut().enumerate() {
        *m = (i * 7 + 1) as u8;
    }
    let w = done!(ring, ok!(sink.write(range(0, 0, 100), 0, &msg, soon(&ring)), "submit a WRITE"), "a WRITE");
    check!(w == 100, "a fixed WRITE's count", w);
    let r = done!(ring, ok!(sink.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), "a READ");
    check!(r.bytes() == &msg[..], "the sink reads back what was written", r.bytes().len());
    drop(r);

    say("a multishot stream with out-of-order returns");
    let pool_a = ok!(ring.pool(&[range(1, 0, SHOT), range(1, SHOT, SHOT)]), "pool A");
    let first = walk(&ring, &s1, b"stream")?;
    open(&ring, &first, 0)?;
    let mut st = ok!(first.stream(&pool_a, 0, SHOT as u32, true, None), "a multishot stream");
    let got = drain(&ring, &mut st, true)?;
    check!(got.len() == STREAM_LEN && matches(&got, 0), "every shot, in order", got.len());
    drop(st);

    say("a full CQ");
    let members: Vec<PoolMember> = (0..8).map(|i| range(3, i * SHOT, SHOT)).collect();
    let pool_b = ok!(ring.pool(&members), "pool B");
    let second = walk(&ring, &s1, b"stream")?;
    open(&ring, &second, 0)?;
    check!(ring.backlog() == 0, "the CQ is empty before the stream", ring.backlog());
    let mut st = ok!(second.stream(&pool_b, 0, SHOT as u32, true, None), "a multishot stream");
    // Submitting entered the SQE; the executor runs without a turn.
    let until = ring.now() + WAIT_NS;
    while ring.backlog() < CQ_ENTRIES {
        check!(ring.now() < until, "the CQ fills while no turn reaps it", ring.backlog());
        let _ = libthyla_rs::time::sleep(Duration::from_millis(1));
    }
    // Long enough for an engine that ignored the full CQ to post past it.
    let _ = libthyla_rs::time::sleep(Duration::from_millis(100));
    check!(ring.backlog() == CQ_ENTRIES, "a full CQ holds exactly its entries", ring.backlog());
    let routed = ok!(ring.turn(None), "a turn over the full CQ");
    check!(routed == CQ_ENTRIES, "one turn routes the whole CQ", routed);
    let got = drain(&ring, &mut st, false)?;
    check!(got.len() == STREAM_LEN && matches(&got, 0), "no shot lost behind the full CQ", got.len());
    drop(st);

    say("a deadline aborts its scope");
    let s2 = connect(&ring, &target)?;
    let near2 = walk(&ring, &s2, b"data")?;
    open(&ring, &near2, 0)?;
    let stall2 = walk(&ring, &s2, b"stall")?;
    open(&ring, &stall2, 0)?;
    let base = stats(&ring, &stats_fid)?;
    let deadline = ring.now() + 3_000_000_000;
    let rd = ok!(stall2.read(range(4, 0, PAGE), 0, Some(deadline)), "submit a READ the server holds");
    stats_until(&ring, &stats_fid, STALLED, base[STALLED] + 1)?;
    match ring.block_on(rd, Some(deadline + WAIT_NS)) {
        Ok(Err(e)) => check!(e == -E_TIMEDOUT, "the deadline ends the held READ", e),
        Ok(Ok(_)) => check!(false, "a READ the server never answered completed", 0),
        Err(e) => check!(false, "the held READ ended", e),
    }
    // The abort latched the scope out of READY: admission refuses a sibling's
    // operation, and no FID reserves under the scope.
    done_refused!(ring, ok!(near2.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), E_CANCELED,
                  "the deadline aborted the open sibling too");
    refused!(s2.walk(b"data", soon(&ring)), E_NOENT, "no FID reserves under the scope the deadline aborted");
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;

    say("cancellation by drop");
    let s3 = connect(&ring, &target)?;
    let near3 = walk(&ring, &s3, b"data")?;
    open(&ring, &near3, 0)?;
    let stall3 = walk(&ring, &s3, b"stall")?;
    open(&ring, &stall3, 0)?;
    let base = stats(&ring, &stats_fid)?;
    let rd = ok!(stall3.read(range(4, 0, PAGE), 0, None), "submit a READ the server holds");
    stats_until(&ring, &stats_fid, STALLED, base[STALLED] + 1)?;
    drop(rd);
    done_refused!(ring, ok!(near3.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), E_CANCELED,
                  "dropping a pending READ aborted the open sibling too");
    refused!(s3.walk(b"data", soon(&ring)), E_NOENT, "no FID reserves under the scope the drop aborted");
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;
    let r = done!(ring, ok!(data.read(range(0, 0, PAGE), PAGE, soon(&ring)), "submit a READ"), "a READ");
    check!(r.bytes().len() == PAGE as usize && matches(r.bytes(), PAGE as usize),
           "a scope beside the aborted ones still reads", r.bytes().len());
    drop(r);

    say("a stream dropped before its end");
    let s4 = connect(&ring, &target)?;
    let fourth = walk(&ring, &s4, b"stream")?;
    open(&ring, &fourth, 0)?;
    let base = stats(&ring, &stats_fid)?;
    let mut st = ok!(fourth.stream(&pool_a, 0, SHOT as u32, true, None), "a multishot stream");
    let p = shot(&ring, &mut st)?;
    drop(st);
    check!(p.bytes().len() == SHOT as usize && matches(p.bytes(), 0), "a lease outlives its dropped stream",
           p.bytes().len());
    returned!(p, "a dropped stream's lease returns");
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;
    // Pool A has two members: a stream drains only if both came back.
    let fifth = walk(&ring, &s1, b"stream")?;
    open(&ring, &fifth, 0)?;
    let mut st = ok!(fifth.stream(&pool_a, 0, SHOT as u32, true, None), "a multishot stream");
    let got = drain(&ring, &mut st, false)?;
    check!(got.len() == STREAM_LEN && matches(&got, 0), "the dropped stream's pool streams again", got.len());
    drop(st);

    say("an explicit abort");
    let s5 = connect(&ring, &target)?;
    let near5 = walk(&ring, &s5, b"data")?;
    open(&ring, &near5, 0)?;
    let base = stats(&ring, &stats_fid)?;
    s5.abort();
    done_refused!(ring, ok!(near5.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), E_CANCELED,
                  "an explicit abort refuses the open fid");
    refused!(s5.walk(b"data", soon(&ring)), E_NOENT, "no FID reserves under the scope abort ended");
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;

    say("dropped fids, scopes, pools and targets are reaped");
    let t2 = ok!(ring.target(REGISTRY, b"fixture"), "a second TARGET of the fixture");
    // Held: two targets; s1 and its eight fids; pools A and B; s2, s3 and s5
    // with their two, two and one fids; s4 and its one.
    const HELD: usize = 2 + 1 + 8 + 2 + 3 + 3 + 2 + 2;
    let base = stats(&ring, &stats_fid)?;
    let f1 = fill(&ring, &s1)?;
    check!(f1 == SLOTS - HELD, "the free slots are those no live handle holds", f1);
    stats_until(&ring, &stats_fid, CLUNKS, base[CLUNKS] + f1 as u32)?;
    drop((near2, stall2, s2, near3, stall3, s3, fourth, s4, near5, s5, pool_b, t2));
    const DROPPED: usize = 12;
    let f2 = fill(&ring, &s1)?;
    check!(f2 == f1 + DROPPED, "every dropped handle's slot came back", f2);
    let v = stats_until(&ring, &stats_fid, CLUNKS, base[CLUNKS] + (f1 + f2) as u32)?;
    check!(v[CLUNKS] == base[CLUNKS] + (f1 + f2) as u32 && v[VIOLATIONS] == base[VIOLATIONS],
           "a Tclunk for each live scope's dropped fid, and no other", v[CLUNKS]);

    say("closing a ring with leases held");
    let ring2 = ok!(ServiceRing::setup(ThylaKernel, ENTRIES, &BUFFERS), "a second private ring");
    let target2 = ok!(ring2.target(REGISTRY, b"fixture"), "TARGET from the second ring");
    let base = stats(&ring, &stats_fid)?;
    let s6 = connect(&ring2, &target2)?;
    let third = walk(&ring2, &s6, b"stream")?;
    open(&ring2, &third, 0)?;
    let data6 = walk(&ring2, &s6, b"data")?;
    open(&ring2, &data6, 0)?;
    let pool_c = ok!(ring2.pool(&[range(1, 0, SHOT), range(1, SHOT, SHOT)]), "pool C");
    let mut st = ok!(third.stream(&pool_c, 0, SHOT as u32, true, None), "a multishot stream");
    let p1 = shot(&ring2, &mut st)?;
    let p2 = shot(&ring2, &mut st)?;
    let held = done!(ring2, ok!(data6.read(range(0, 0, PAGE), 0, soon(&ring2)), "submit a READ"), "a READ");
    ring2.close();
    check!(p1.bytes().len() == SHOT as usize && p2.bytes().len() == SHOT as usize &&
           held.bytes().len() == PAGE as usize && matches(p1.bytes(), 0) &&
           matches(p2.bytes(), SHOT as usize) && matches(held.bytes(), 0),
           "leases keep their bytes after the ring closes", p1.bytes().len());
    refused!(ring2.target(REGISTRY, b"fixture"), E_CANCELED, "a closed ring admits no TARGET");
    refused!(s6.walk(b"data", None), E_CANCELED, "a closed ring admits no WALK");
    // The close reached the kernel: the server saw the session end while this
    // program still runs.
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;
    drop(p1);
    drop(p2);
    drop(held);

    say("retiring a ring");
    let v = stats(&ring, &stats_fid)?;
    check!(v[VIOLATIONS] == 0, "the fixture saw no protocol violation", v[VIOLATIONS]);
    ring.retire();
    let until = ring.now() + WAIT_NS;
    while !ring.retired() {
        check!(ring.now() < until, "every scope retires and every terminal is routed", 0);
        ok!(ring.turn(Some(ring.now() + 10_000_000)), "a turn while the ring retires");
    }
    refused!(ring.target(REGISTRY, b"fixture"), E_CANCELED, "a retired ring admits no TARGET");
    say("qualification image drives every leg through the Rust adapter");
    Ok(PASS_QUAL)
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    match run() {
        Ok(code) => code,
        Err(f) => {
            say(&format!("FAIL line {}: {} (got {})", f.line, f.why, f.got));
            1
        }
    }
}
