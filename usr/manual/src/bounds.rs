//! The reader's resource bounds (MANUAL-DESIGN.md 8.1). Sections built to be
//! expensive are read, checked and rendered as `manual` does, under the
//! allocator the guest heap uses, and the heap's high-water mark must stay
//! within the reader's working set, which is half of `HEAP_BYTES`; the same
//! sections must take time, and write output, linear in their size.
//!
//! The high-water mark is what a lazily committed `ThylaAllocN` heap touches:
//! the furthest address `linked_list_allocator` has handed out, so the space a
//! first-fit heap loses to fragmentation counts, not only the bytes in use.
//! That heap is this test binary's allocator for the thread being measured; every
//! other allocation, and every other test, uses the system allocator.

extern crate std;

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::cell::Cell;
use core::ptr::{addr_of_mut, NonNull};
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::alloc::System;
use std::time::{Duration, Instant};

use linked_list_allocator::Heap;

use beacon::Tier;

use crate::format::{self, Problem, TABLE_CELL_MAX, TABLE_COLUMNS_MAX};
use crate::render::{render, CHUNK};
use crate::{HEAP_BYTES, SECTION_MAX};

std::thread_local! {
    static MEASURING: Cell<bool> = const { Cell::new(false) };
}

/// The guest-shaped heap spans more than `HEAP_BYTES`, so exceeding the bound is
/// a measurement rather than an abort; with first fit, a placement below
/// `HEAP_BYTES` is the placement a heap of exactly that size would make.
const ARENA: usize = 4 * HEAP_BYTES;

static LOCK: AtomicBool = AtomicBool::new(false);
static mut HEAP: Heap = Heap::empty();
static BOTTOM: AtomicUsize = AtomicUsize::new(0);
static HIGH_WATER: AtomicUsize = AtomicUsize::new(0);

fn lock() {
    while LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn unlock() {
    LOCK.store(false, Ordering::Release);
}

struct GuestHeap;

unsafe impl GlobalAlloc for GuestHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if !MEASURING.with(|m| m.get()) {
            return System.alloc(layout);
        }
        lock();
        let r = (*addr_of_mut!(HEAP)).allocate_first_fit(layout);
        unlock();
        match r {
            Ok(p) => {
                // The heap rounds a block up to two words and to word alignment.
                let size = (layout.size().max(16) + 7) & !7;
                let end = p.as_ptr() as usize + size - BOTTOM.load(Ordering::Relaxed);
                HIGH_WATER.fetch_max(end, Ordering::Relaxed);
                p.as_ptr()
            }
            Err(()) => core::ptr::null_mut(),
        }
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        let bottom = BOTTOM.load(Ordering::Relaxed);
        let a = p as usize;
        if bottom != 0 && a >= bottom && a < bottom + ARENA {
            lock();
            (*addr_of_mut!(HEAP)).deallocate(NonNull::new_unchecked(p), layout);
            unlock();
        } else {
            System.dealloc(p, layout);
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

/// The two bounds tests run one at a time: they share the arena, and neither
/// should time the other.
static SERIAL: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Run `f` on a fresh guest-shaped heap and return its high-water mark. `f`
/// must free everything it allocates.
fn high_water(f: impl FnOnce()) -> usize {
    unsafe {
        if BOTTOM.load(Ordering::Relaxed) == 0 {
            let p = System.alloc(Layout::from_size_align(ARENA, 16).unwrap());
            assert!(!p.is_null());
            BOTTOM.store(p as usize, Ordering::Relaxed);
        }
        lock();
        let heap = &mut *addr_of_mut!(HEAP);
        *heap = Heap::empty();
        heap.init(BOTTOM.load(Ordering::Relaxed) as *mut u8, ARENA);
        unlock();
    }
    HIGH_WATER.store(0, Ordering::Relaxed);
    MEASURING.with(|m| m.set(true));
    f();
    MEASURING.with(|m| m.set(false));
    let used = unsafe { (*addr_of_mut!(HEAP)).used() };
    assert_eq!(used, 0, "the measured work left memory allocated");
    HIGH_WATER.load(Ordering::Relaxed)
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
            let hw = high_water(|| passed = show(src.as_bytes(), read, tier, width).is_some());
            assert_eq!(passed, passes, "{}: whether it passes the check", name);
            std::eprintln!(
                "bounds: {:<50} {:?}/{:?}/{:?}: high water {:>6} KiB ({:.2} bytes per byte)",
                name,
                read,
                tier,
                width,
                hw / 1024,
                hw as f64 / src.len() as f64
            );
            assert!(
                hw <= WORKING_SET_MAX,
                "{} at {:?}/{:?}/{:?}: a high-water mark of {} bytes exceeds the {}-byte working set",
                name,
                read,
                tier,
                width,
                hw,
                WORKING_SET_MAX
            );
            if hw > worst.0 {
                worst = (hw, name);
            }
        }
    }
    std::eprintln!(
        "bounds: worst high water {} KiB ({})",
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
