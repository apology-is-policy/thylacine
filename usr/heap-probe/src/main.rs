// /heap-probe -- B-1c (the native heap): libthyla-rs's heap driven from EL0 (ARCH
// 6.5 "Capacity"; dec-2026-09-24-native-heap-large-blocks). Spawned by joey at
// boot; prints one "heap-probe: <leg> OK" line per leg, a "heap-probe: time"
// line per measured cost and "heap-probe: ALL OK", and exits 0. A failed leg
// prints "heap-probe: FAIL <leg> got=<n> want<op><m>"; the legs after it still
// run, and the probe exits 1.
//
// thyla-heap's host tests prove the heap's policy against a model of the kernel.
// What only the guest shows is the memory bar through the real calls: the census
// the kernel publishes (/proc/<pid>/status) rises past the old fixed heap's 4 MiB
// as a program allocates and falls again as it frees -- small blocks through
// dlmalloc's trim, a large block through its own mapping while a small one stays
// live, and a reservation dlmalloc emptied through its release. The times are
// the cost of giving memory back eagerly: the churn of a heap that swings past
// dlmalloc's trim threshold, beside one that stays under it.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::alloc::{alloc, dealloc, Layout};
use alloc::vec::Vec;
use core::ptr::{read_volatile, write_volatile};
use libthyla_rs::alloc as heap;
use libthyla_rs::time::monotonic_ns;
use libthyla_rs::{t_close, t_getpid, t_open, t_putstr, t_read, T_OREAD, T_WALK_OPEN_FROM_ROOT};

const PAGE: usize = 4096;
const KIB: usize = 1024;
const MIB: usize = 1024 * KIB;

/// dlmalloc trims once its top chunk passes 2 MiB, keeping under one 64 KiB
/// granule of it, so a heap that has freed everything holds at most this many
/// pages more than a trimmed one.
const TRIM_KEEP: u64 = ((2 * MIB + 64 * KIB) / PAGE) as u64;

/// Pages a census may carry that no block explains: pagemap nodes (one per 512
/// pages mapped, and the roots) and the probe's own stack.
const SLACK: u64 = 32;

/// Sixteen times the old fixed heap.
const SMALL_TOTAL: usize = 64 * MIB;
const BLOCKS_MAX: usize = 8192;
const LARGE: usize = 32 * MIB;

fn put_num(mut n: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    if n == 0 {
        i -= 1;
        buf[i] = b'0';
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    // The digits are ASCII by construction.
    if let Ok(s) = core::str::from_utf8(&buf[i..]) {
        t_putstr(s);
    }
}

fn fail(leg: &str) -> bool {
    t_putstr("heap-probe: FAIL ");
    t_putstr(leg);
    t_putstr("\n");
    false
}

fn fail_num(leg: &str, got: u64, op: &str, want: u64) -> bool {
    t_putstr("heap-probe: FAIL ");
    t_putstr(leg);
    t_putstr(" got=");
    put_num(got);
    t_putstr(" want");
    t_putstr(op);
    put_num(want);
    t_putstr("\n");
    false
}

/// `got <= most`, else a named failure.
fn at_most(leg: &str, got: u64, most: u64) -> bool {
    got <= most || fail_num(leg, got, "<=", most)
}

/// `got >= least`, else a named failure.
fn at_least(leg: &str, got: u64, least: u64) -> bool {
    got >= least || fail_num(leg, got, ">=", least)
}

fn ok(leg: &str, pages: u64, what: &str) {
    t_putstr("heap-probe: ");
    t_putstr(leg);
    t_putstr(" OK (");
    put_num(pages);
    t_putstr(" pages ");
    t_putstr(what);
    t_putstr(")\n");
}

fn time(what: &str, ns: u64, pages: u64, per: &str) {
    t_putstr("heap-probe: time ");
    t_putstr(what);
    t_putstr(" ");
    put_num(ns / pages.max(1));
    t_putstr(" ns per page ");
    t_putstr(per);
    t_putstr("\n");
}

// The decimal after `key` in `buf`, or None.
fn field(buf: &[u8], key: &[u8]) -> Option<u64> {
    let mut i = 0;
    while i + key.len() <= buf.len() {
        if &buf[i..i + key.len()] == key {
            let mut j = i + key.len();
            while j < buf.len() && buf[j] == b' ' {
                j += 1;
            }
            let mut v: u64 = 0;
            let mut got = false;
            while j < buf.len() && buf[j].is_ascii_digit() {
                v = v * 10 + (buf[j] - b'0') as u64;
                j += 1;
                got = true;
            }
            return if got { Some(v) } else { None };
        }
        i += 1;
    }
    None
}

// The data view of /proc/<pid>/status, as capacity-probe reads it: `pages` less
// the page tables and the file pages of this program's own image, which move
// with where things landed and what the code touched, not with the heap.
fn census() -> Option<u64> {
    let pid = unsafe { t_getpid() };
    if pid <= 0 {
        return None;
    }
    let mut path = [0u8; 40];
    let mut n = 0usize;
    for &c in b"/proc/" {
        path[n] = c;
        n += 1;
    }
    let mut digits = [0u8; 20];
    let mut d = 0usize;
    let mut v = pid as u64;
    loop {
        digits[d] = b'0' + (v % 10) as u8;
        d += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    while d > 0 {
        d -= 1;
        path[n] = digits[d];
        n += 1;
    }
    for &c in b"/status" {
        path[n] = c;
        n += 1;
    }
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), n, T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = [0u8; 1024];
    let mut total = 0usize;
    while total < buf.len() {
        let n = unsafe { t_read(fd, buf[total..].as_mut_ptr(), buf.len() - total) };
        if n <= 0 {
            break;
        }
        total += n as usize;
    }
    let _ = unsafe { t_close(fd) };
    let pages = field(&buf[..total], b"pages:")?;
    let tables = field(&buf[..total], b"tables:")?;
    let file = field(&buf[..total], b"file:")?;
    pages.checked_sub(tables)?.checked_sub(file)
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> usize {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as usize
    }
}

/// Write `tag` on every page `[p, p + size)` spans, and its complement on the
/// last byte when that byte starts no page of its own.
unsafe fn touch(p: *mut u8, size: usize, tag: u8) {
    let mut off = 0;
    while off < size {
        write_volatile(p.add(off), tag);
        off += PAGE;
    }
    if (size - 1) % PAGE != 0 {
        write_volatile(p.add(size - 1), !tag);
    }
}

unsafe fn intact(p: *mut u8, size: usize, tag: u8) -> bool {
    let mut off = 0;
    while off < size {
        if read_volatile(p.add(off)) != tag {
            return false;
        }
        off += PAGE;
    }
    (size - 1) % PAGE == 0 || read_volatile(p.add(size - 1)) == !tag
}

fn layout(size: usize) -> Layout {
    Layout::from_size_align(size, 8).unwrap()
}

/// A small block, patterned on every byte, that stays live while the blocks
/// above it are freed and trimmed away: a trim that reached below the top chunk
/// would find it.
struct Sentinel(*mut u8);

const SENTINEL: usize = PAGE;

impl Sentinel {
    fn new() -> Self {
        let p = unsafe { alloc(layout(SENTINEL)) };
        if !p.is_null() {
            for i in 0..SENTINEL {
                unsafe { write_volatile(p.add(i), (i % 251) as u8) };
            }
        }
        Sentinel(p)
    }

    fn intact(&self, leg: &str) -> bool {
        if self.0.is_null() {
            return fail(leg);
        }
        for i in 0..SENTINEL {
            if unsafe { read_volatile(self.0.add(i)) } != (i % 251) as u8 {
                return fail_num(leg, i as u64, "=", SENTINEL as u64);
            }
        }
        true
    }
}

impl Drop for Sentinel {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { dealloc(self.0, layout(SENTINEL)) };
        }
    }
}

/// 64 MiB of blocks under the direct threshold, sized from 16 bytes to 256 KiB,
/// raise the census by at least their pages and by no more than the heap says
/// it holds; freed in an order that fragments the heap before it coalesces, they
/// give all but dlmalloc's kept top back without a trim being asked for.
fn small_blocks(rng: &mut Rng) -> bool {
    let mut blocks: Vec<(usize, usize)> = Vec::with_capacity(BLOCKS_MAX);
    // The records' own pages belong to the baseline.
    blocks.resize(BLOCKS_MAX, (0, 0));
    blocks.clear();
    heap::trim();
    // Live below every block, so the frees' trims end just above it.
    let sentinel = Sentinel::new();
    let (Some(base), fp0) = (census(), heap::footprint()) else {
        return fail("census-readable");
    };

    let t0 = monotonic_ns();
    let mut total = 0usize;
    while total < SMALL_TOTAL && blocks.len() < BLOCKS_MAX {
        let class = rng.next() % 14;
        let size = (16 << class) + rng.next() % (16 << class);
        let p = unsafe { alloc(layout(size)) };
        if p.is_null() {
            return fail_num("small-blocks-alloc", total as u64, ">=", SMALL_TOTAL as u64);
        }
        unsafe { touch(p, size, blocks.len() as u8) };
        blocks.push((p as usize, size));
        total += size;
    }
    let t1 = monotonic_ns();
    let Some(high) = census() else {
        return fail("census-readable");
    };
    let rise = high.saturating_sub(base);
    let held = (heap::footprint().saturating_sub(fp0) / PAGE) as u64;
    let mut good = at_least("small-blocks-rise", rise, (SMALL_TOTAL / PAGE) as u64)
        && at_most("small-blocks-within-footprint", rise, held + held / 256 + SLACK);
    if good {
        ok("small-blocks-rise", rise, "over the base, 64 MiB in blocks under 256 KiB");
    }

    for (i, &(p, size)) in blocks.iter().enumerate() {
        if !unsafe { intact(p as *mut u8, size, i as u8) } {
            good = fail_num("small-blocks-intact", i as u64, "=", blocks.len() as u64);
            break;
        }
    }
    let t2 = monotonic_ns();
    for start in [1, 0] {
        for &(p, size) in blocks.iter().skip(start).step_by(2) {
            unsafe { dealloc(p as *mut u8, layout(size)) };
        }
    }
    let t3 = monotonic_ns();
    blocks.clear();
    let Some(after) = census() else {
        return fail("census-readable");
    };
    if at_most("small-blocks-fall", after.saturating_sub(base), TRIM_KEEP + SLACK) {
        ok("small-blocks-fall", after.saturating_sub(base), "over the base, no trim asked");
    } else {
        good = false;
    }
    good &= sentinel.intact("small-blocks-sentinel");
    time("small-alloc-and-touch", t1 - t0, rise, "(allocated, then faulted in)");
    time("small-free", t3 - t2, high.saturating_sub(after), "(freed, trimmed and decommitted)");
    good
}

/// A top chunk between one 64 KiB granule and dlmalloc's 2 MiB trim threshold is
/// what no free trims, so its pages stay until a trim is asked for; asked, the
/// trim returns them. Once frees have passed the threshold they have trimmed
/// already, and an explicit trim has nothing left to show.
fn trim_returns_the_rest() -> bool {
    const BLOCK: usize = 64 * KIB;
    const BLOCKS: usize = 16;
    // The records live on the stack, so the heap holds only the blocks.
    let mut v = [core::ptr::null_mut::<u8>(); BLOCKS];
    heap::trim();
    let sentinel = Sentinel::new();
    let Some(base) = census() else {
        return fail("census-readable");
    };
    for (i, slot) in v.iter_mut().enumerate() {
        let p = unsafe { alloc(layout(BLOCK)) };
        if p.is_null() {
            return fail("trim-alloc");
        }
        unsafe { touch(p, BLOCK, i as u8) };
        *slot = p;
    }
    for &p in &v {
        unsafe { dealloc(p, layout(BLOCK)) };
    }
    let Some(kept) = census() else {
        return fail("census-readable");
    };
    let kept = kept.saturating_sub(base);
    // The premise: the frees gave nothing back, so there is a rest to return.
    let mut good = at_least("trim-premise-kept", kept, (BLOCKS * BLOCK / PAGE) as u64 * 3 / 4);
    heap::trim();
    let Some(trimmed) = census() else {
        return fail("census-readable");
    };
    let trimmed = trimmed.saturating_sub(base);
    good &= at_most("trim-returns-the-rest", trimmed, SLACK);
    good &= sentinel.intact("trim-sentinel");
    if good {
        ok("trim-returns-the-rest", kept - trimmed, "kept by the frees, returned by the trim");
    }
    good
}

/// A 32 MiB block is a mapping of its own: freed with a small block allocated
/// after it still live, its pages all go back -- in a heap made only of
/// segments they would stay until everything above them was freed.
fn large_block() -> bool {
    heap::trim();
    let (Some(base), fp0) = (census(), heap::footprint()) else {
        return fail("census-readable");
    };
    let t0 = monotonic_ns();
    let big = unsafe { alloc(layout(LARGE)) };
    if big.is_null() {
        return fail("large-block-alloc");
    }
    unsafe { touch(big, LARGE, 0xB1) };
    let t1 = monotonic_ns();
    let small = unsafe { alloc(layout(KIB)) };
    if small.is_null() {
        return fail("large-block-small-alloc");
    }
    unsafe { touch(small, KIB, 0x5A) };
    let Some(high) = census() else {
        return fail("census-readable");
    };
    let rise = high.saturating_sub(base);
    let mut good = at_least("large-block-rise", rise, (LARGE / PAGE) as u64)
        && at_least("large-block-in-footprint", heap::footprint().saturating_sub(fp0) as u64, LARGE as u64);
    if good {
        ok("large-block-rise", rise, "over the base, one 32 MiB block and a small one");
    }
    let t2 = monotonic_ns();
    unsafe { dealloc(big, layout(LARGE)) };
    let t3 = monotonic_ns();
    let Some(after) = census() else {
        return fail("census-readable");
    };
    if at_most("large-block-returns-under-a-live-block", after.saturating_sub(base), SLACK) {
        ok("large-block-returns-under-a-live-block", after.saturating_sub(base), "over the base");
    } else {
        good = false;
    }
    if !unsafe { intact(small, KIB, 0x5A) } {
        good = fail("large-block-small-intact");
    }
    unsafe { dealloc(small, layout(KIB)) };
    time("large-touch", t1 - t0, (LARGE / PAGE) as u64, "(reserved, then faulted in)");
    time("large-free", t3 - t2, high.saturating_sub(after), "(detached)");
    good
}

/// Blocks carved past the first reservation's end open a second; freed, the
/// first is emptied and released whole by dlmalloc's own trim -- the new
/// reservation holds past the 2 MiB threshold, so its last free trims -- and the
/// census falls back. A live block in the first would pin it (dlmalloc returns a
/// segment only when it is wholly free), so the records are a direct block,
/// outside the reservations.
fn reservation_switch() -> bool {
    const BLOCK: usize = 128 * KIB;
    const CAP: usize = 4096;
    const PAST_THRESHOLD: usize = 3 * MIB / BLOCK;
    const RECORDS: usize = 256 * KIB / core::mem::size_of::<usize>();
    let mut blocks: Vec<usize> = Vec::with_capacity(RECORDS);
    blocks.resize(RECORDS, 0);
    blocks.clear();
    heap::trim();
    let (Some(base), before) = (census(), heap::reservations()) else {
        return fail("census-readable");
    };
    while heap::reservations() == before && blocks.len() < CAP {
        let p = unsafe { alloc(layout(BLOCK)) };
        if p.is_null() {
            return fail_num("reservation-switch-alloc", blocks.len() as u64, "<", CAP as u64);
        }
        blocks.push(p as usize);
    }
    let crossed = heap::reservations();
    let mut good = at_least("reservation-switch", crossed as u64, before as u64 + 1);
    for _ in 0..PAST_THRESHOLD {
        let p = unsafe { alloc(layout(BLOCK)) };
        if p.is_null() {
            return fail("reservation-switch-alloc");
        }
        blocks.push(p as usize);
    }
    for &p in &blocks {
        unsafe { dealloc(p as *mut u8, layout(BLOCK)) };
    }
    let carved = blocks.len();
    good &= at_most("reservation-released-by-a-free", heap::reservations() as u64, before as u64);
    heap::trim();
    let Some(after) = census() else {
        return fail("census-readable");
    };
    // The records' pages were in the base, so they stay in until the census.
    drop(blocks);
    if at_most("reservation-switch-gives-back", after.saturating_sub(base), SLACK) && good {
        ok("reservation-switch", carved as u64 * (BLOCK / PAGE) as u64, "carved across two reservations, the first released");
    } else {
        good = false;
    }
    good
}

/// The eager release's cost: a heap that swings 4 MiB past its trim threshold
/// decommits and re-faults every cycle; one that swings 1.5 MiB never trims.
fn churn() -> bool {
    const BLOCK: usize = 64 * KIB;
    const CYCLES: u64 = 16;
    let mut good = true;
    for (what, blocks, per) in [
        ("churn-4mib", 64usize, "per cycle (decommitted and faulted in again)"),
        ("churn-1.5mib", 24usize, "per cycle (kept, under the trim threshold)"),
    ] {
        let mut v: Vec<usize> = Vec::with_capacity(blocks);
        v.resize(blocks, 0);
        v.clear();
        heap::trim();
        let Some(base) = census() else {
            return fail("census-readable");
        };
        let t0 = monotonic_ns();
        for c in 0..CYCLES {
            for _ in 0..blocks {
                let p = unsafe { alloc(layout(BLOCK)) };
                if p.is_null() {
                    return fail("churn-alloc");
                }
                unsafe { touch(p, BLOCK, c as u8) };
                v.push(p as usize);
            }
            for &p in &v {
                unsafe { dealloc(p as *mut u8, layout(BLOCK)) };
            }
            v.clear();
        }
        let t1 = monotonic_ns();
        time(what, t1 - t0, CYCLES * (blocks * BLOCK / PAGE) as u64, per);
        let Some(after) = census() else {
            return fail("census-readable");
        };
        good &= at_most("churn-gives-back", after.saturating_sub(base), TRIM_KEEP + SLACK);
    }
    good
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // The census is read once before any baseline, so the stack pages a read
    // needs are resident already.
    if census().is_none() {
        fail("census-readable");
        return 1;
    }
    let mut rng = Rng(0x7_1ac1_de5e);
    let mut good = small_blocks(&mut rng);
    good &= trim_returns_the_rest();
    good &= large_block();
    good &= reservation_switch();
    good &= churn();
    if !good {
        return 1;
    }
    t_putstr("heap-probe: ALL OK\n");
    0
}
