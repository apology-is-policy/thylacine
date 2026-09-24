//! The reader's resource bounds (MANUAL-DESIGN.md 8.1). Sections built to be
//! expensive are read, checked and rendered as `manual` does, on the heap the
//! guest runs it on, and that heap's peak footprint must stay within the
//! reader's working set, which is half of `HEAP_BYTES`; the same sections must
//! take time, and write output, linear in their size.
//!
//! The heap is libthyla-rs's (thyla-heap: dlmalloc for small blocks, a mapping of
//! its own for a large one). Its footprint is everything it holds from the
//! system -- what dlmalloc has carved, in use or free, and the direct blocks --
//! so what fragmentation costs counts, not only the bytes in use. That heap, over
//! an arena standing in for the kernel, is this test binary's allocator for the
//! thread being measured; every other allocation, and every other test, uses the
//! system allocator.

extern crate std;

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::cell::Cell;
use core::ptr::{self, addr_of_mut};
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::alloc::System;
use std::time::{Duration, Instant};

use beacon::Tier;
use thyla_heap::{Backend, Heap, PAGE, RESERVATION_LEN};

use crate::format::{self, Problem, TABLE_CELL_MAX, TABLE_COLUMNS_MAX};
use crate::render::{render, CHUNK};
use crate::{HEAP_BYTES, SECTION_MAX};

std::thread_local! {
    static MEASURING: Cell<bool> = const { Cell::new(false) };
}

/// The address space the kernel stand-in hands out: the heap's first
/// reservation, and room for direct blocks far past the working set, so
/// exceeding the bound is a measurement rather than an abort.
const ARENA: usize = RESERVATION_LEN + 4 * HEAP_BYTES;

const MAPPINGS_MAX: usize = 64;

/// The stand-in's mappings, `live[..n]` as `(base, len)` sorted by base. Nothing
/// at or above `touched` has been handed out, so it is still zero.
struct Mappings {
    live: [(usize, usize); MAPPINGS_MAX],
    n: usize,
    touched: usize,
}

static LOCK: AtomicBool = AtomicBool::new(false);
static mut MAPPINGS: Mappings = Mappings {
    live: [(0, 0); MAPPINGS_MAX],
    n: 0,
    touched: 0,
};
static BOTTOM: AtomicUsize = AtomicUsize::new(0);

fn mappings<R>(f: impl FnOnce(&mut Mappings) -> R) -> R {
    while LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
    let r = f(unsafe { &mut *addr_of_mut!(MAPPINGS) });
    LOCK.store(false, Ordering::Release);
    r
}

fn mapping_of(m: &Mappings, va: usize, len: usize) -> Option<usize> {
    m.live[..m.n]
        .iter()
        .position(|&(b, l)| va >= b && va + len <= b + l)
}

/// The kernel's three calls over one arena, a reservation placed first fit as
/// the kernel places it. The heap makes them from inside the global allocator,
/// so they never allocate.
#[derive(Clone, Copy)]
struct Arena;

impl Backend for Arena {
    fn reserve(&self, len: usize, align_log2: u32) -> Option<usize> {
        let align = PAGE.max(1 << align_log2);
        let bottom = BOTTOM.load(Ordering::Relaxed);
        mappings(|m| {
            if m.n == MAPPINGS_MAX {
                return None;
            }
            let (mut at, mut i) = (bottom, 0);
            while i < m.n && at.next_multiple_of(align) + len > m.live[i].0 {
                at = m.live[i].0 + m.live[i].1;
                i += 1;
            }
            let at = at.next_multiple_of(align);
            if at + len > bottom + ARENA {
                return None;
            }
            m.live.copy_within(i..m.n, i + 1);
            m.live[i] = (at, len);
            m.n += 1;
            if at < m.touched {
                unsafe { ptr::write_bytes(at as *mut u8, 0, m.touched.min(at + len) - at) };
            }
            m.touched = m.touched.max(at + len);
            Some(at)
        })
    }

    fn decommit(&self, va: usize, len: usize) -> bool {
        mappings(|m| {
            let inside = mapping_of(m, va, len).is_some();
            if inside {
                unsafe { ptr::write_bytes(va as *mut u8, 0, len) };
            }
            inside
        })
    }

    fn detach(&self, va: usize, len: usize) -> bool {
        mappings(|m| {
            let Some(i) = mapping_of(m, va, len) else {
                return false;
            };
            let (b, l) = m.live[i];
            let pieces = [(b, va - b), (va + len, b + l - (va + len))];
            let kept = pieces.iter().filter(|p| p.1 > 0).count();
            if m.n - 1 + kept > MAPPINGS_MAX {
                return false;
            }
            m.live.copy_within(i + 1..m.n, i + kept);
            for (j, p) in pieces.into_iter().filter(|p| p.1 > 0).enumerate() {
                m.live[i + j] = p;
            }
            m.n = m.n - 1 + kept;
            true
        })
    }
}

static HEAP: Heap<Arena> = Heap::new(Arena);

/// Blocks the measured work holds in the heap.
static LIVE: AtomicUsize = AtomicUsize::new(0);

fn measuring() -> bool {
    MEASURING.with(|m| m.get())
}

fn in_arena(p: *mut u8) -> bool {
    let bottom = BOTTOM.load(Ordering::Relaxed);
    bottom != 0 && p as usize >= bottom && (p as usize) < bottom + ARENA
}

fn took(p: *mut u8) -> *mut u8 {
    if !p.is_null() {
        LIVE.fetch_add(1, Ordering::Relaxed);
    }
    p
}

struct GuestHeap;

unsafe impl GlobalAlloc for GuestHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if measuring() {
            took(HEAP.alloc(layout))
        } else {
            System.alloc(layout)
        }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if measuring() {
            took(HEAP.alloc_zeroed(layout))
        } else {
            System.alloc_zeroed(layout)
        }
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        if in_arena(p) {
            HEAP.dealloc(p, layout);
            LIVE.fetch_sub(1, Ordering::Relaxed);
        } else {
            System.dealloc(p, layout);
        }
    }

    // A block in the heap is resized by the heap, as in the guest, which may
    // grow it where it lies.
    unsafe fn realloc(&self, p: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if in_arena(p) {
            HEAP.realloc(p, layout, new_size)
        } else if measuring() {
            let q = self.alloc(Layout::from_size_align_unchecked(new_size, layout.align()));
            if !q.is_null() {
                ptr::copy_nonoverlapping(p, q, layout.size().min(new_size));
                System.dealloc(p, layout);
            }
            q
        } else {
            System.realloc(p, layout, new_size)
        }
    }
}

#[global_allocator]
static ALLOCATOR: GuestHeap = GuestHeap;

/// What the reader may hold at once: the section, and the holes its buffer left
/// behind while it grew; a few copies of its largest block (the block joined for
/// scanning, an emphasis or code span's text, and on a console wider than the
/// block a wrapped line and the word pending on it); and a chunk of output
/// (MANUAL-DESIGN.md 4.4). The heap leaves room above it.
const WORKING_SET_MAX: usize = 8 * SECTION_MAX;
const _: () = assert!(WORKING_SET_MAX <= HEAP_BYTES / 2);

/// The two bounds tests run one at a time: they share the heap, and neither
/// should time the other.
static SERIAL: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Run `f` on the heap, trimmed first, and return its peak footprint while `f`
/// ran. `f` must free everything it allocates.
fn peak_footprint(f: impl FnOnce()) -> usize {
    if BOTTOM.load(Ordering::Relaxed) == 0 {
        // calloc's fresh pages from the system: zero until touched.
        let arena = Layout::from_size_align(ARENA + PAGE, 16).unwrap();
        let p = unsafe { System.alloc_zeroed(arena) };
        assert!(!p.is_null());
        BOTTOM.store((p as usize).next_multiple_of(PAGE), Ordering::Relaxed);
    }
    HEAP.trim();
    HEAP.reset_peak();
    MEASURING.with(|m| m.set(true));
    f();
    MEASURING.with(|m| m.set(false));
    assert_eq!(
        LIVE.load(Ordering::Relaxed),
        0,
        "the measured work left memory allocated"
    );
    HEAP.peak()
}

/// How `read_capped` sizes its buffer: from the length `fstat` reports, or, when
/// it reports none, from nothing, so the buffer grows as the file is read.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Read {
    Hinted,
    Unhinted,
}

/// What `manual <file>` does with a file's bytes (main.rs `show`): it holds the
/// path and the copy it prints, reads the file 8 KiB at a time into a buffer
/// sized as `read` says, checks it, printing each problem, and renders it when it
/// passes. Returns the bytes written, or `None` when the check fails.
fn show(bytes: &[u8], read: Read, tier: Tier, width: Option<usize>) -> Option<usize> {
    let path = String::from("/manual/05-x.md");
    let shown = crate::sanitize(&path, false);
    let hint = match read {
        Read::Hinted => bytes.len(),
        Read::Unhinted => 0,
    };
    let mut v = Vec::with_capacity(hint.min(SECTION_MAX));
    for chunk in bytes.chunks(8 * 1024) {
        v.extend_from_slice(chunk);
    }
    let src = String::from_utf8(v).unwrap();
    let mut printed = 0usize;
    let problems = format::check(Some("05-x.md"), &src, &mut |line, p: Problem| {
        // libthyla-rs's `eprintln!` is `eprint!("{}\n", ..)`, and its
        // `Stderr::write_fmt` formats the whole line into a new String first.
        let line = alloc::fmt::format(format_args!(
            "{}\n",
            format_args!("manual: {}:{}: {}", shown, line, p)
        ));
        printed += 1;
        drop(line);
    });
    assert_eq!(problems, printed);
    if problems > 0 {
        return None;
    }
    let mut written = 0usize;
    render(&src, tier, width, &mut |chunk| written += chunk.len());
    assert!(written > 0);
    Some(written)
}

fn fill(prefix: &str, unit: &str, bytes: usize) -> String {
    let mut s = String::from(prefix);
    while s.len() + unit.len() <= bytes {
        s.push_str(unit);
    }
    s
}

/// Sections of `bytes` built to be expensive, each named, with whether it
/// passes the check.
fn expensive(bytes: usize) -> Vec<(&'static str, String, bool)> {
    let row16 = "|a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|\n";
    let delim16 = "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    let mut v: Vec<(&'static str, String, bool)> = Vec::new();
    let mut add = |name, s: String, passes| v.push((name, s, passes));
    add("blank lines", fill("# T\n", "\n", bytes), true);
    add("one-word paragraphs", fill("# T\n", "\nw\n", bytes), true);
    add(
        "one paragraph of one-character lines",
        fill("# T\n\n", "a\n", bytes),
        true,
    );
    add(
        "one paragraph of one long line",
        fill("# T\n\nx", "a", bytes),
        true,
    );
    add(
        "one paragraph of short words",
        fill("# T\n\nx", " ab", bytes),
        true,
    );
    add("headings", fill("# T\n", "\n## h\n", bytes), true);
    add("bulleted items", fill("# T\n\n", "- a\n", bytes), true);
    add(
        "one item of continuation lines",
        fill("# T\n\n- a\n", "  b\n", bytes),
        true,
    );
    add(
        "numbered items",
        {
            let mut s = String::from("# T\n\n");
            let mut n = 1;
            while s.len() + 16 <= bytes {
                s.push_str(&alloc::format!("{}. a\n", n));
                n += 1;
            }
            s
        },
        true,
    );
    add(
        "one-cell table rows",
        fill("# T\n\n|a|\n|---|\n", "|a|\n", bytes),
        true,
    );
    add(
        "sixteen-cell table rows",
        fill(&alloc::format!("# T\n\n{}{}", row16, delim16), row16, bytes),
        true,
    );
    add(
        "code block lines",
        {
            let mut s = fill("# T\n\n```\n", "x\n", bytes - 4);
            s.push_str("```\n");
            s
        },
        true,
    );
    add("code spans", fill("# T\n\nx", " `a`", bytes), true);
    add("emphasis", fill("# T\n\nx", " *a*", bytes), true);
    add("strong emphasis", fill("# T\n\nx", " **a**", bytes), true);
    add("escapes", fill("# T\n\nx", "\\*", bytes), true);
    add(
        "underscores that open nothing",
        fill("# T\n\nx", " _y", bytes),
        true,
    );
    add(
        "brackets that close nothing",
        fill("# T\n\nx", "[", bytes),
        true,
    );
    add(
        "ampersands that begin no reference",
        fill("# T\n\nx", " a&b", bytes),
        true,
    );
    add(
        "one line of character references",
        fill("# T\n\nx", "&#1;", bytes),
        false,
    );
    add(
        "benign prose",
        fill(
            "# T\n",
            "\nThe quick brown fox jumps over the lazy dog near the river.\n",
            bytes,
        ),
        true,
    );
    add(
        "a problem in every paragraph",
        fill("# T\n", "\n<\n", bytes),
        false,
    );
    add(
        "a problem on every line of one paragraph",
        fill("# T\n\na\n", "<\n", bytes),
        false,
    );
    add(
        "three problems on every line",
        fill("# T\n\na\n", "\t<\n", bytes),
        false,
    );
    add(
        "one line of the same problem",
        fill("# T\n\nx", "<", bytes),
        false,
    );
    add(
        "one line of alternating problems",
        fill("# T\n\nx", "<~~", bytes),
        false,
    );
    add(
        "control characters",
        fill("# T\n", "\n\x01\n", bytes),
        false,
    );
    add(
        "stars that close nothing",
        fill("# T\n\nx", " *a", bytes),
        false,
    );
    add(
        "backticks that close nothing",
        fill("# T\n\nx", " `` ```", bytes),
        false,
    );
    add(
        "footnote-like brackets",
        fill("# T\n\nx", "[^a", bytes),
        true,
    );
    add("hard breaks", fill("# T\n\na\n", "a  \n", bytes), false);
    add("setext underlines", fill("# T\n\na\n", "=\n", bytes), false);
    add("deep headings", fill("# T\n", "\n#### h\n", bytes), false);
    add(
        "one row of many cells",
        fill("# T\n\n|", "a|", bytes),
        false,
    );
    add(
        "unseparated blocks",
        fill("# T\n\na\n", "- b\nc\n", bytes),
        false,
    );
    add(
        "one emphasis across a paragraph",
        {
            let mut s = fill("# T\n\n*a", "\na", bytes - 2);
            s.push_str("*\n");
            s
        },
        true,
    );
    add(
        "one code span across a paragraph",
        {
            let mut s = fill("# T\n\n`a", "\na", bytes - 2);
            s.push_str("`\n");
            s
        },
        true,
    );
    add(
        "paragraphs of doubling length",
        {
            // Each paragraph outgrows the buffers the one before it grew.
            let mut s = String::from("# T\n");
            let mut lines = 1;
            loop {
                let mut p = String::from("\n`a");
                for _ in 0..lines {
                    p.push_str("\na");
                }
                p.push_str("` *b");
                for _ in 0..lines {
                    p.push_str("\nb");
                }
                p.push_str("*\n");
                if s.len() + p.len() > bytes {
                    break;
                }
                s.push_str(&p);
                lines *= 2;
            }
            while s.len() < bytes {
                s.push('\n');
            }
            s
        },
        true,
    );
    // Wider than a cell may be (3.2), so only its check is measured: the row is
    // still split, unescaped and counted as one cell.
    add(
        "one table cell of escaped pipes",
        {
            let mut s = fill("# T\n\n| a", "\\|", bytes - 11);
            s.push_str(" |\n|---|\n");
            s
        },
        false,
    );
    // Every cell pads to its column's widest, so the widest cells the format
    // allows, over as many short rows as fit, are the most output per byte.
    let limit = |c: char| -> String { core::iter::repeat_n(c, TABLE_CELL_MAX).collect() };
    add(
        "rows of escaped pipes at the width limit",
        {
            let row = alloc::format!("| {} |\n", "\\|".repeat(TABLE_CELL_MAX));
            let mut s = fill("# T\n\n| h |\n|---|\n", &row, bytes);
            while s.len() < bytes {
                s.push('\n');
            }
            s
        },
        true,
    );
    add(
        "short rows under cells at the width limit",
        fill(
            &alloc::format!("# T\n\n| {} | {} |\n|---|---|\n", limit('a'), limit('b')),
            "|c|d|\n",
            bytes,
        ),
        true,
    );
    add(
        "empty rows under sixteen cells at the width limit",
        {
            let mut head = String::from("# T\n\n|");
            for _ in 0..TABLE_COLUMNS_MAX {
                head.push_str(&limit('h'));
                head.push('|');
            }
            head.push('\n');
            head.push_str(delim16);
            fill(&head, "|||||||||||||||||\n", bytes)
        },
        true,
    );
    v
}

/// A section at the size limit, in each of its most expensive shapes, is read,
/// checked and rendered at every tier and with wrapping within the working set.
#[test]
fn the_heap_bounds_hold() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let mut worst = (0usize, "");
    let wide = Some(2 * SECTION_MAX);
    // A section that fails the check is never rendered, so only the first two
    // settings, which differ in how the file is read, apply to it.
    let settings = [
        (Read::Unhinted, Tier::None, wide),
        (Read::Hinted, Tier::Rich, None),
        (Read::Hinted, Tier::None, None),
        (Read::Hinted, Tier::None, Some(80)),
        (Read::Hinted, Tier::None, wide),
    ];
    for (name, src, passes) in expensive(SECTION_MAX) {
        assert!(src.len() <= SECTION_MAX && src.len() > SECTION_MAX - 64);
        for (i, &(read, tier, width)) in settings.iter().enumerate() {
            if !passes && i >= 2 {
                break;
            }
            let mut passed = false;
            let peak = peak_footprint(|| passed = show(src.as_bytes(), read, tier, width).is_some());
            assert_eq!(passed, passes, "{}: whether it passes the check", name);
            std::eprintln!(
                "bounds: {:<50} {:?}/{:?}/{:?}: peak footprint {:>6} KiB ({:.2} bytes per byte)",
                name,
                read,
                tier,
                width,
                peak / 1024,
                peak as f64 / src.len() as f64
            );
            // The reader holds the whole section, so a smaller peak means the
            // heap never saw its allocations.
            assert!(
                peak >= src.len(),
                "{}: a peak footprint of {} bytes is less than the section",
                name,
                peak
            );
            assert!(
                peak <= WORKING_SET_MAX,
                "{} at {:?}/{:?}/{:?}: a peak footprint of {} bytes exceeds the {}-byte working set",
                name,
                read,
                tier,
                width,
                peak,
                WORKING_SET_MAX
            );
            if peak > worst.0 {
                worst = (peak, name);
            }
        }
    }
    std::eprintln!(
        "bounds: worst peak footprint {} KiB ({})",
        worst.0 / 1024,
        worst.1
    );
}

/// Every expensive shape takes time, and writes output, linear in its size: the
/// same work on a section four times larger takes well under sixteen times as
/// long and writes well under sixteen times as much. Each size is timed as the
/// best of several runs, which discards interference rather than averaging it
/// in.
#[test]
fn the_time_bounds_hold() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    fn best(src: &str) -> (Duration, usize) {
        let mut written = 0;
        let t = (0..3)
            .map(|_| {
                let t = Instant::now();
                written = [(Tier::Rich, None), (Tier::None, Some(80))]
                    .into_iter()
                    .map(|(tier, width)| {
                        show(src.as_bytes(), Read::Hinted, tier, width).unwrap_or(0)
                    })
                    .sum();
                t.elapsed()
            })
            .min()
            .unwrap();
        (t, written)
    }
    let small = expensive(SECTION_MAX / 16);
    let large = expensive(SECTION_MAX / 4);
    for ((name, s, _), (_, l, _)) in small.iter().zip(&large) {
        let ((ts, os), (tl, ol)) = (best(s), best(l));
        std::eprintln!(
            "time: {:<50} {:>9.3?} and {:>6} KiB out at {:>4} KiB, {:>9.3?} and {:>6} KiB out at {:>4} KiB",
            name,
            ts,
            os / 1024,
            s.len() / 1024,
            tl,
            ol / 1024,
            l.len() / 1024
        );
        // Linear work is four times as long and as large; quadratic is sixteen.
        // The floors keep timer resolution, and output that does not grow with
        // the section, from deciding a case.
        assert!(
            tl <= ts * 8 + Duration::from_millis(20),
            "{}: {:?} at {} bytes, {:?} at {} bytes",
            name,
            ts,
            s.len(),
            tl,
            l.len()
        );
        assert!(
            ol <= os * 8 + CHUNK,
            "{}: {} bytes written at {} bytes, {} at {} bytes",
            name,
            os,
            s.len(),
            ol,
            l.len()
        );
    }
}
