// /bin/loom-native-probe -- the Rust native Loom adapter driven from EL0
// against /bin/loom-fixture (docs/ASYNC-SERVICE-ABI.md, NA-4d). The kernel test
// loom.native_probe_rust starts both and requires 72 from the default image,
// where setup refuses -EOPNOTSUPP, or 73 from the qualification image, where
// every leg below passed.
//
// fd 0 is the registry the fixture posts in, fd 1 the control pipe (closed at
// exit, which ends the fixture) and fd 2 the notify pipe. Every leg reads the
// fixture's stats file for what the server saw, so a refusal the kernel made
// locally cannot pass for one the server answered, and the reverse.

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

const PASS_DEFAULT: i64 = 72;
const PASS_QUAL: i64 = 73;
const REGISTRY: i32 = 0;
const NOTIFY: i64 = 2;

// The kernel's errno registry (kernel/include/thylacine/errno.h).
const E_NOENT: i32 = 2;
const E_ACCES: i32 = 13;
const E_OPNOTSUPP: i32 = 95;
const E_TIMEDOUT: i32 = 110;
const E_CANCELED: i32 = 125;

const PAGE: u64 = 4096;
const ENTRIES: u32 = 2;
const CQ_ENTRIES: usize = 4;
// 0: fixed data and sink I/O; 1: pool A; 2: stats; 3: pool B; 4: stalled reads.
const BUFFERS: [usize; 5] = [PAGE as usize; 5];
const WAIT_NS: u64 = 5_000_000_000;

// The fixture's files and its stats counters (usr/loom-fixture).
const DATA_LEN: usize = 3 * 4096 + 1000;
const STREAM_LEN: usize = 2600;
const SHOT: u64 = 512;
const ACCEPTED: usize = 0;
const ENDED: usize = 1;
const VIOLATIONS: usize = 2;
const STALLED: usize = 3;
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
                        drop(p);
                        drop(older);
                    }
                    None if swap => held = Some(p),
                    None => got.extend_from_slice(p.bytes()),
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
    }
    Ok(got)
}

fn shot(ring: &ServiceRing, st: &mut Stream<K>) -> R<Payload<K>> {
    match done!(ring, st.next(), "a shot") {
        Shot::Payload(p) => Ok(p),
        Shot::End(rc) => Err(Fail { line: line!(), why: "a stream ended before its shot", got: rc as i64 }),
    }
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
                  "the server's Rlerror for a missing name passes through");
    let data = walk(&ring, &s1, b"data")?;
    done_refused!(ring, ok!(data.open(1, soon(&ring)), "submit an LOPEN"), E_ACCES,
                  "the kernel's own stat check refuses writing a 0444 file");
    open(&ring, &data, 0)?;
    let sink = walk(&ring, &s1, b"sink")?;
    open(&ring, &sink, 2)?;

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
    let mut st = ok!(second.stream(&pool_b, 0, SHOT as u32, true, None), "a multishot stream");
    // Submitting entered the SQE; the executor runs without a turn.
    let until = ring.now() + WAIT_NS;
    while ring.backlog() < CQ_ENTRIES {
        check!(ring.now() < until, "the CQ fills while no turn reaps it", ring.backlog());
        let _ = libthyla_rs::time::sleep(Duration::from_millis(1));
    }
    check!(ring.backlog() == CQ_ENTRIES, "a full CQ holds exactly its entries", ring.backlog());
    let routed = ok!(ring.turn(None), "a turn over the full CQ");
    check!(routed == CQ_ENTRIES, "one turn routes the whole CQ", routed);
    let got = drain(&ring, &mut st, false)?;
    check!(got.len() == STREAM_LEN && matches(&got, 0), "no shot lost behind the full CQ", got.len());
    drop(st);

    say("a deadline aborts its scope");
    let s2 = connect(&ring, &target)?;
    let near = walk(&ring, &s2, b"data")?;
    open(&ring, &near, 0)?;
    let stall = walk(&ring, &s2, b"stall")?;
    open(&ring, &stall, 0)?;
    let base = stats(&ring, &stats_fid)?;
    let deadline = ring.now() + 1_000_000_000;
    let rd = ok!(stall.read(range(4, 0, PAGE), 0, Some(deadline)), "submit a READ the server holds");
    stats_until(&ring, &stats_fid, STALLED, base[STALLED] + 1)?;
    match ring.block_on(rd, Some(deadline + WAIT_NS)) {
        Ok(Err(e)) => check!(e == -E_TIMEDOUT, "the deadline ends the held READ", e),
        Ok(Ok(_)) => check!(false, "a READ the server never answered completed", 0),
        Err(e) => check!(false, "the held READ ended", e),
    }
    // The abort latched the scope out of READY: admission refuses a sibling's
    // operation, and no FID reserves under the scope.
    done_refused!(ring, ok!(near.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), E_CANCELED,
                  "the deadline aborted the open sibling too");
    refused!(s2.walk(b"data", soon(&ring)), E_NOENT, "no FID reserves under the scope the deadline aborted");
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;

    say("cancellation by drop");
    let s3 = connect(&ring, &target)?;
    let near = walk(&ring, &s3, b"data")?;
    open(&ring, &near, 0)?;
    let stall = walk(&ring, &s3, b"stall")?;
    open(&ring, &stall, 0)?;
    let base = stats(&ring, &stats_fid)?;
    let rd = ok!(stall.read(range(4, 0, PAGE), 0, None), "submit a READ the server holds");
    stats_until(&ring, &stats_fid, STALLED, base[STALLED] + 1)?;
    drop(rd);
    done_refused!(ring, ok!(near.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), E_CANCELED,
                  "dropping a pending READ aborted the open sibling too");
    refused!(s3.walk(b"data", soon(&ring)), E_NOENT, "no FID reserves under the scope the drop aborted");
    stats_until(&ring, &stats_fid, ENDED, base[ENDED] + 1)?;
    let r = done!(ring, ok!(data.read(range(0, 0, PAGE), PAGE, soon(&ring)), "submit a READ"), "a READ");
    check!(r.bytes().len() == PAGE as usize && matches(r.bytes(), PAGE as usize),
           "a scope beside the aborted ones still reads", r.bytes().len());
    drop(r);

    say("closing the ring with leases held");
    let v = stats(&ring, &stats_fid)?;
    check!(v[VIOLATIONS] == 0, "the fixture saw no protocol violation", v[VIOLATIONS]);
    let third = walk(&ring, &s1, b"stream")?;
    open(&ring, &third, 0)?;
    let mut st = ok!(third.stream(&pool_a, 0, SHOT as u32, true, None), "a multishot stream");
    let p1 = shot(&ring, &mut st)?;
    let p2 = shot(&ring, &mut st)?;
    let held = done!(ring, ok!(data.read(range(0, 0, PAGE), 0, soon(&ring)), "submit a READ"), "a READ");
    ring.close();
    check!(matches(p1.bytes(), 0) && matches(p2.bytes(), SHOT as usize) && matches(held.bytes(), 0),
           "leases keep their bytes after the ring closes", p1.bytes().len());
    refused!(ring.target(REGISTRY, b"fixture"), E_CANCELED, "a closed ring admits no TARGET");
    refused!(s1.walk(b"data", None), E_CANCELED, "a closed ring admits no WALK");
    drop(p1);
    drop(p2);
    drop(held);
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
