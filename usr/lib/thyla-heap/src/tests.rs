// Host tests: the real dlmalloc over the real Reservations, with a backend that
// models the kernel's three calls on host memory.
//
// Run: cd usr && cargo test -p thyla-heap --target aarch64-apple-darwin
//
// The backend poisons every page it takes back -- decommitted or detached -- so a
// trim that reached a live block fails that block's pattern check, and a write
// after a detach shows up in the dead range at the end. It refuses a decommit or a
// detach outside a single live mapping, which the heap never needs; a refusal
// there is a bookkeeping bug surfacing, not a kernel rule being modelled.

extern crate std;

use core::sync::atomic::{AtomicUsize, Ordering};
use std::boxed::Box;
use std::sync::Mutex;
use std::vec::Vec;

use super::*;

const POISON: u8 = 0xDB;
const KIB: usize = 1024;
const MIB: usize = 1024 * KIB;

struct Map {
    base: usize,
    len: usize,
    live: bool,
}

struct Mock {
    // Every reservation is placed first-fit in one arena, as the kernel's
    // `vma_find_gap` places them in the burrow window, so a reservation lands
    // against its neighbour whenever the gap allows -- the adjacency dlmalloc
    // would merge across if a carve could fill its reservation.
    arena: usize,
    maps: Mutex<Vec<Map>>,
    decommitted: AtomicUsize,
    reserves: AtomicUsize,
}

const ARENA: usize = 1 << 30;

impl Mock {
    fn new() -> &'static Mock {
        let layout = std::alloc::Layout::from_size_align(ARENA, MIB).unwrap();
        let arena = unsafe { std::alloc::alloc_zeroed(layout) } as usize;
        assert!(arena != 0);
        Box::leak(Box::new(Mock {
            arena,
            maps: Mutex::new(Vec::new()),
            decommitted: AtomicUsize::new(0),
            reserves: AtomicUsize::new(0),
        }))
    }

    fn live_maps(&self) -> usize {
        self.maps.lock().unwrap().iter().filter(|m| m.live).count()
    }

    // Nothing may write a range after it was detached (until it is reserved
    // again, which zeroes it).
    fn assert_dead_untouched(&self) {
        for m in self.maps.lock().unwrap().iter().filter(|m| !m.live) {
            let bytes = unsafe { core::slice::from_raw_parts(m.base as *const u8, m.len) };
            assert!(bytes.iter().all(|&b| b == POISON), "a write after detach at {:#x}", m.base);
        }
    }
}

impl Backend for Mock {
    fn reserve(&self, len: usize, align_log2: u32) -> Option<usize> {
        if len == 0 || !len.is_multiple_of(PAGE) {
            return None;
        }
        let align = if align_log2 == 0 { PAGE } else { 1usize.checked_shl(align_log2)? };
        let round = |v: usize| (v + align - 1) & !(align - 1);
        let mut maps = self.maps.lock().unwrap();
        let mut live: Vec<(usize, usize)> =
            maps.iter().filter(|m| m.live).map(|m| (m.base, m.base + m.len)).collect();
        live.sort();
        let mut cand = round(self.arena);
        for (b, e) in live {
            if e <= cand {
                continue;
            }
            if b >= cand && b - cand >= len {
                break;
            }
            cand = round(e);
        }
        if cand + len > self.arena + ARENA {
            return None;
        }
        // A reused range reads as zero, and is no longer dead.
        let (lo, hi) = (cand, cand + len);
        let mut kept = Vec::new();
        for m in maps.drain(..) {
            let (b, e) = (m.base, m.base + m.len);
            if m.live || e <= lo || b >= hi {
                kept.push(m);
                continue;
            }
            let (ob, oe) = (b.max(lo), e.min(hi));
            unsafe { ptr::write_bytes(ob as *mut u8, 0, oe - ob) };
            if b < lo {
                kept.push(Map { base: b, len: lo - b, live: false });
            }
            if e > hi {
                kept.push(Map { base: hi, len: e - hi, live: false });
            }
        }
        *maps = kept;
        maps.push(Map { base: cand, len, live: true });
        self.reserves.fetch_add(1, Ordering::SeqCst);
        Some(cand)
    }

    fn decommit(&self, va: usize, len: usize) -> bool {
        if len == 0 || !va.is_multiple_of(PAGE) || !len.is_multiple_of(PAGE) {
            return false;
        }
        let maps = self.maps.lock().unwrap();
        if !maps.iter().any(|m| m.live && va >= m.base && va + len <= m.base + m.len) {
            return false;
        }
        unsafe { ptr::write_bytes(va as *mut u8, POISON, len) };
        self.decommitted.fetch_add(len, Ordering::SeqCst);
        true
    }

    fn detach(&self, va: usize, len: usize) -> bool {
        if len == 0 || !va.is_multiple_of(PAGE) || !len.is_multiple_of(PAGE) {
            return false;
        }
        let mut maps = self.maps.lock().unwrap();
        let i = match maps.iter().position(|m| m.live && va >= m.base && va + len <= m.base + m.len) {
            Some(i) => i,
            None => return false,
        };
        unsafe { ptr::write_bytes(va as *mut u8, POISON, len) };
        let (base, end) = (maps[i].base, maps[i].base + maps[i].len);
        maps[i] = Map { base: va, len, live: false };
        if va > base {
            maps.push(Map { base, len: va - base, live: true });
        }
        if va + len < end {
            maps.push(Map { base: va + len, len: end - (va + len), live: true });
        }
        true
    }
}

fn heap(first_len: usize) -> (&'static Mock, Heap<&'static Mock>) {
    let m = Mock::new();
    (m, Heap::with_reservation_len(m, first_len))
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> usize {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as usize
    }
}

// A block's pattern: its first and last 64 bytes and every 1024th byte, so every
// page a block covers holds a checked byte.
fn marks(n: usize) -> impl Iterator<Item = usize> {
    (0..n.min(64)).chain((0..n).step_by(1024)).chain(n.saturating_sub(64)..n)
}

fn byte(k: usize, i: usize) -> u8 {
    (k as u8) ^ ((i >> 3) as u8).wrapping_mul(29) ^ 0x5A
}

unsafe fn fill(p: *mut u8, n: usize, k: usize) {
    for i in marks(n) {
        *p.add(i) = byte(k, i);
    }
}

unsafe fn check(p: *mut u8, n: usize, k: usize) {
    check_upto(p, n, n, k);
}

// A block filled at `filled` bytes and since resized to `upto`: its marks below
// `upto`.
unsafe fn check_upto(p: *mut u8, filled: usize, upto: usize, k: usize) {
    for i in marks(filled).filter(|&i| i < upto) {
        assert_eq!(*p.add(i), byte(k, i), "block {} corrupted at byte {}", k, i);
    }
}

// The head segment dlmalloc keeps after a trim: the top chunk under one 64 KiB
// granule plus its foot, on a granule boundary.
const TRIMMED: usize = 128 * KIB;

#[test]
fn small_blocks_rise_and_fall() {
    let (m, h) = heap(RESERVATION_LEN);
    let mut rng = Rng(1);
    let mut blocks = Vec::new();
    for k in 0..20_000 {
        let size = 16 + rng.next() % 2048;
        let l = Layout::from_size_align(size, 8).unwrap();
        let p = unsafe { h.alloc(l) };
        assert!(!p.is_null());
        unsafe { fill(p, size, k) };
        blocks.push((p, l, k));
    }
    let high = h.footprint();
    assert!(high > 16 * MIB, "footprint {} after ~20 MiB of blocks", high);
    for i in (1..blocks.len()).rev() {
        blocks.swap(i, rng.next() % (i + 1));
    }
    for (p, l, k) in blocks {
        unsafe {
            check(p, l.size(), k);
            h.dealloc(p, l);
        }
    }
    // No explicit trim: the free that left a top chunk past 2 MiB trimmed it.
    assert!(h.footprint() <= TRIMMED, "footprint {} after freeing everything", h.footprint());
    assert!(h.peak() >= high, "the peak {} fell with the footprint", h.peak());
    assert!(m.decommitted.load(Ordering::SeqCst) >= high - TRIMMED);
    assert_eq!(h.reservations(), 1);
    assert_eq!(m.live_maps(), 1);
    m.assert_dead_untouched();
}

#[test]
fn a_refused_length_is_asked_for_again_at_half_down_to_the_carve() {
    let (m, h) = heap(RESERVATION_LEN);
    // One direct block leaves 4 MiB of the arena.
    let wall = Layout::from_size_align(ARENA - 4 * MIB, PAGE).unwrap();
    let w = unsafe { h.alloc(wall) };
    assert!(!w.is_null());
    // 256 MiB is refused, and every half down to the 4 MiB left is asked for;
    // once that is carved, the halving ends at the carve's size in a refusal.
    let l = Layout::from_size_align(64 * KIB, 8).unwrap();
    let mut blocks = Vec::new();
    loop {
        let p = unsafe { h.alloc(l) };
        if p.is_null() {
            break;
        }
        blocks.push(p);
        assert!(blocks.len() <= 64, "more than the 4 MiB left was handed out");
    }
    assert!(blocks.len() >= 56, "{} blocks from the 4 MiB left", blocks.len());
    assert_eq!(h.reservations(), 1);
    assert_eq!(m.reserves.load(Ordering::SeqCst), 2);
    for p in blocks {
        unsafe { h.dealloc(p, l) };
    }
    unsafe { h.dealloc(w, wall) };
    h.trim();
    assert!(h.footprint() <= TRIMMED);
    m.assert_dead_untouched();
}

#[test]
fn trim_moves_the_bump_back_and_the_next_carve_reuses_it() {
    let (m, h) = heap(RESERVATION_LEN);
    let l = Layout::from_size_align(64 * KIB, 16).unwrap();
    let round = |h: &Heap<&'static Mock>| -> Vec<usize> {
        let ps: Vec<*mut u8> = (0..128).map(|_| unsafe { h.alloc(l) }).collect();
        let addrs = ps.iter().map(|&p| p as usize).collect();
        for p in ps {
            unsafe { h.dealloc(p, l) };
        }
        addrs
    };
    let first = round(&h);
    let after_first = m.decommitted.load(Ordering::SeqCst);
    assert!(after_first > 0, "freeing 8 MiB trimmed nothing");
    let second = round(&h);
    assert_eq!(first, second, "the second round was not carved where the first was");
    assert_eq!(m.reserves.load(Ordering::SeqCst), 1, "a trim-then-grow opened a reservation");
    assert_eq!(m.live_maps(), 1);
}

#[test]
fn reservations_switch_and_the_emptied_ones_are_detached() {
    // A 1 MiB first reservation, doubling: ~12 MiB of small blocks crosses four.
    let (m, h) = heap(MIB);
    let l = Layout::from_size_align(100 * KIB, 16).unwrap();
    let mut ps = Vec::new();
    for k in 0..120 {
        let p = unsafe { h.alloc(l) };
        assert!(!p.is_null());
        unsafe { fill(p, l.size(), k) };
        ps.push((p, k));
    }
    let crossed = h.reservations();
    assert!(crossed >= 4, "only {} reservations for 12 MiB from a 1 MiB start", crossed);
    for (p, k) in ps {
        unsafe {
            check(p, l.size(), k);
            h.dealloc(p, l);
        }
    }
    h.trim();
    assert_eq!(h.reservations(), 1, "an emptied reservation was kept");
    assert_eq!(m.live_maps(), 1, "a reservation was dropped from the table but left mapped");
    assert!(h.footprint() <= TRIMMED);
    m.assert_dead_untouched();
}

// The defect the reservations exist to prevent: under a fresh-attach-per-call
// platform with decommit as free_part, every cycle below would leave one more
// mapping behind.
#[test]
fn cycles_leave_no_mapping_behind() {
    let (m, h) = heap(MIB);
    let l = Layout::from_size_align(200 * KIB, 16).unwrap();
    for _ in 0..50 {
        let ps: Vec<*mut u8> = (0..40).map(|_| unsafe { h.alloc(l) }).collect();
        assert!(ps.iter().all(|p| !p.is_null()));
        for p in ps {
            unsafe { h.dealloc(p, l) };
        }
        h.trim();
        assert_eq!(h.reservations(), 1);
        assert_eq!(m.live_maps(), 1, "a mapping outlived its cycle");
    }
    m.assert_dead_untouched();
}

#[test]
fn a_direct_block_is_its_own_mapping_and_goes_back_whole() {
    let (m, h) = heap(RESERVATION_LEN);
    let l = Layout::from_size_align(DIRECT_MIN, 8).unwrap();
    let p = unsafe { h.alloc(l) };
    assert!(!p.is_null());
    assert_eq!(h.footprint(), DIRECT_MIN);
    assert_eq!(h.reservations(), 0, "a direct block went through dlmalloc");
    unsafe { fill(p, l.size(), 7) };
    unsafe { check(p, l.size(), 7) };
    unsafe { h.dealloc(p, l) };
    assert_eq!(h.footprint(), 0);
    assert_eq!(m.live_maps(), 0);

    // Zeroed: a fresh reservation, read without a memset.
    let z = Layout::from_size_align(3 * MIB + 5, 8).unwrap();
    let p = unsafe { h.alloc_zeroed(z) };
    assert!(unsafe { core::slice::from_raw_parts(p, z.size()) }.iter().all(|&b| b == 0));
    unsafe { h.dealloc(p, z) };

    // Aligned to DIRECT_MIN or more: direct, at the alignment asked.
    let a = Layout::from_size_align(100, DIRECT_MIN).unwrap();
    let p = unsafe { h.alloc(a) };
    assert_eq!(p as usize % DIRECT_MIN, 0);
    assert_eq!(h.footprint(), PAGE);
    unsafe { h.dealloc(p, a) };
    assert_eq!(h.footprint(), 0);
    assert_eq!(m.live_maps(), 0);
    m.assert_dead_untouched();
}

#[test]
fn a_small_block_aligned_past_a_page_is_dlmallocs() {
    let (m, h) = heap(RESERVATION_LEN);
    let mut held = Vec::new();
    for (k, align) in [2 * PAGE, 64 * KIB, DIRECT_MIN / 2].into_iter().enumerate() {
        let l = Layout::from_size_align(100 + k, align).unwrap();
        let p = unsafe { h.alloc(l) };
        assert!(!p.is_null());
        assert_eq!(p as usize % align, 0, "aligned to {}", align);
        unsafe { fill(p, l.size(), k) };
        held.push((p, l, k));
    }
    assert_eq!(h.reservations(), 1, "an aligned small block was not dlmalloc's");
    assert_eq!(m.live_maps(), 1, "an aligned small block was mapped on its own");
    for (p, l, k) in held {
        unsafe {
            check(p, l.size(), k);
            h.dealloc(p, l);
        }
    }
    h.trim();
    assert!(h.footprint() <= TRIMMED);
    m.assert_dead_untouched();
}

#[test]
fn direct_realloc_keeps_shrinks_in_place_and_moves_to_grow() {
    let (m, h) = heap(RESERVATION_LEN);
    let l = Layout::from_size_align(300_000, 8).unwrap();
    let p = unsafe { h.alloc(l) };
    unsafe { fill(p, 300_000, 1) };

    // Same page count: the same block.
    let q = unsafe { h.realloc(p, l, 300_100) };
    assert_eq!(q, p);
    let l = Layout::from_size_align(300_100, 8).unwrap();

    // Grow: a new mapping, the bytes carried, the old one detached.
    h.reset_peak();
    let q = unsafe { h.realloc(p, l, 900_000) };
    assert!(!q.is_null() && q != p);
    unsafe { check(q, 300_000, 1) };
    assert_eq!(h.footprint(), page_round(900_000).unwrap());
    assert_eq!(
        h.peak(),
        page_round(300_100).unwrap() + page_round(900_000).unwrap(),
        "the moment the grow held both blocks was not counted"
    );
    assert_eq!(m.live_maps(), 1);
    let l = Layout::from_size_align(900_000, 8).unwrap();

    // Shrink: in place, the tail detached.
    let r = unsafe { h.realloc(q, l, 400_000) };
    assert_eq!(r, q);
    assert_eq!(h.footprint(), page_round(400_000).unwrap());
    unsafe { check(r, 300_000, 1) };
    unsafe { h.dealloc(r, Layout::from_size_align(400_000, 8).unwrap()) };
    assert_eq!(h.footprint(), 0);
    h.reset_peak();
    assert_eq!(h.peak(), 0);
    assert_eq!(m.live_maps(), 0);
    m.assert_dead_untouched();
}

// A backend that reads the footprint, as a peer thread would, at each direct
// block's reserve and detach: the heap's lock must be free there, and the block
// counted.
#[derive(Clone, Copy)]
struct Watcher;

static WATCHED: Heap<Watcher> = Heap::new(Watcher);
static WATCHED_MOCK: std::sync::OnceLock<&'static Mock> = std::sync::OnceLock::new();
const LOCK_HELD: usize = usize::MAX;
static AT_RESERVE: AtomicUsize = AtomicUsize::new(0);
static AT_DETACH: AtomicUsize = AtomicUsize::new(0);

fn watched_footprint() -> usize {
    match WATCHED.small.try_lock() {
        Some(d) => d.allocator().footprint(),
        None => LOCK_HELD,
    }
}

impl Backend for Watcher {
    fn reserve(&self, len: usize, align_log2: u32) -> Option<usize> {
        AT_RESERVE.store(watched_footprint(), Ordering::SeqCst);
        WATCHED_MOCK.get().unwrap().reserve(len, align_log2)
    }
    fn decommit(&self, va: usize, len: usize) -> bool {
        WATCHED_MOCK.get().unwrap().decommit(va, len)
    }
    fn detach(&self, va: usize, len: usize) -> bool {
        AT_DETACH.store(watched_footprint(), Ordering::SeqCst);
        WATCHED_MOCK.get().unwrap().detach(va, len)
    }
}

#[test]
fn a_direct_block_is_counted_for_as_long_as_it_is_held() {
    let m = *WATCHED_MOCK.get_or_init(Mock::new);
    let l = Layout::from_size_align(DIRECT_MIN, 8).unwrap();
    let p = unsafe { WATCHED.alloc(l) };
    assert!(!p.is_null());
    assert_eq!(AT_RESERVE.load(Ordering::SeqCst), DIRECT_MIN, "the block was reserved before it was counted");
    unsafe { WATCHED.dealloc(p, l) };
    assert_eq!(AT_DETACH.load(Ordering::SeqCst), DIRECT_MIN, "the block was uncounted before it was detached");
    assert_eq!(WATCHED.footprint(), 0);

    // Refused: counted while it was asked for, and not after.
    let huge = Layout::from_size_align(2 * ARENA, 8).unwrap();
    assert!(unsafe { WATCHED.alloc(huge) }.is_null());
    assert_eq!(AT_RESERVE.load(Ordering::SeqCst), 2 * ARENA);
    assert_eq!(WATCHED.footprint(), 0, "a refused block stayed counted");
    assert_eq!(m.live_maps(), 0);
}

// A backend that asks for a second block while the first is being reserved,
// as a peer thread could, and grants neither.
#[derive(Clone, Copy)]
struct Nester;

static NESTED: Heap<Nester> = Heap::new(Nester);
static NESTED_ASKS: AtomicUsize = AtomicUsize::new(0);

// Half the address space, less an alignment's worth: a valid layout, twice
// more than a usize can count.
fn half_of_everything() -> Layout {
    Layout::from_size_align(isize::MAX as usize - 7, 8).unwrap()
}

impl Backend for Nester {
    fn reserve(&self, _len: usize, _align_log2: u32) -> Option<usize> {
        if NESTED_ASKS.fetch_add(1, Ordering::SeqCst) == 0 {
            assert!(unsafe { NESTED.alloc(half_of_everything()) }.is_null());
        }
        None
    }
    fn decommit(&self, _va: usize, _len: usize) -> bool {
        false
    }
    fn detach(&self, _va: usize, _len: usize) -> bool {
        false
    }
}

#[test]
fn a_block_no_system_could_hold_is_refused_before_it_is_counted() {
    assert!(unsafe { NESTED.alloc(half_of_everything()) }.is_null());
    assert_eq!(NESTED.footprint(), 0);
    assert_eq!(NESTED.peak(), 0, "a block refused before it was asked for raised the peak");
}

#[test]
fn realloc_across_the_threshold_carries_the_bytes() {
    let (m, h) = heap(RESERVATION_LEN);
    let small = Layout::from_size_align(100 * KIB, 8).unwrap();
    let p = unsafe { h.alloc(small) };
    unsafe { fill(p, small.size(), 3) };
    let q = unsafe { h.realloc(p, small, 600 * KIB) };
    unsafe { check(q, small.size(), 3) };
    let big = Layout::from_size_align(600 * KIB, 8).unwrap();
    assert!(h.footprint() >= big.size());
    let r = unsafe { h.realloc(q, big, 10 * KIB) };
    unsafe { check_upto(r, small.size(), 10 * KIB, 3) };
    unsafe { h.dealloc(r, Layout::from_size_align(10 * KIB, 8).unwrap()) };
    h.trim();
    assert!(h.footprint() <= TRIMMED);
    assert_eq!(m.live_maps(), 1, "the direct block outlived its move back");
    m.assert_dead_untouched();
}

#[test]
fn churn_keeps_every_block_intact_and_gives_everything_back() {
    let (m, h) = heap(4 * MIB);
    let mut rng = Rng(0x7419);
    let mut live: Vec<(*mut u8, Layout, usize)> = Vec::new();
    for k in 0..30_000 {
        // At most 2000 blocks live, so the arena (1 GiB) holds the direct ones.
        let op = if live.len() >= 2000 { 6 + rng.next() % 4 } else { rng.next() % 10 };
        if op < 6 || live.is_empty() {
            let size = match rng.next() % 20 {
                0 => DIRECT_MIN + rng.next() % MIB,
                1..=4 => 2 * KIB + rng.next() % (120 * KIB),
                _ => 1 + rng.next() % 512,
            };
            let align = if rng.next().is_multiple_of(50) { 8 * KIB } else { 8 };
            let l = Layout::from_size_align(size, align).unwrap();
            let p = unsafe { h.alloc(l) };
            assert!(!p.is_null());
            assert_eq!(p as usize % align, 0);
            unsafe { fill(p, size, k) };
            live.push((p, l, k));
        } else if op < 9 {
            let i = rng.next() % live.len();
            let (p, l, key) = live.swap_remove(i);
            unsafe {
                check(p, l.size(), key);
                h.dealloc(p, l);
            }
        } else {
            let i = rng.next() % live.len();
            let (p, l, key) = live[i];
            let new = 1 + rng.next() % (DIRECT_MIN * 2);
            let q = unsafe { h.realloc(p, l, new) };
            assert!(!q.is_null());
            unsafe { check_upto(q, l.size(), new, key) };
            let nl = Layout::from_size_align(new, l.align()).unwrap();
            unsafe { fill(q, new, k) };
            live[i] = (q, nl, k);
        }
    }
    for (p, l, key) in live.drain(..) {
        unsafe {
            check(p, l.size(), key);
            h.dealloc(p, l);
        }
    }
    h.trim();
    assert!(h.footprint() <= TRIMMED, "footprint {} after the churn drained", h.footprint());
    assert_eq!(h.reservations(), 1);
    assert_eq!(m.live_maps(), 1);
    m.assert_dead_untouched();
}
