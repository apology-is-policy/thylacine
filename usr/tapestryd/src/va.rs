// The compositor's mapping window: the one range of its own address space in
// which it chooses where each DMA object lands.
//
// SYS_DMA_MAP takes a caller-chosen VA, so every weave generation, screen
// buffer, GPU BO, ring and probe page tapestryd maps needs an address, and
// the kernel places its OWN regions for this process elsewhere: the exec
// stack under 2 GiB, the vDSO page at 3 GiB, every lazy or eager burrow
// first-fit from 4 GiB up. The window therefore sits entirely BELOW the
// stack guard, which nothing the kernel chooses can enter (the bin pins the
// constants at compile time), and it must REUSE what is freed: a bump
// allocator's consumption is the session's HISTORY -- a divider drag reweaves
// every visible surface at frame rate -- where this one's is the LIVE set, so
// an exhausted window is a refusal about what exists now, never a collision
// with a region that was never the compositor's to hand out.
//
// Pure (no syscalls): the bin maps and detaches; this only decides addresses.

use alloc::vec::Vec;

pub const PAGE: u64 = 4096;

/// A first-fit range allocator over `[base, top)`, page-granular.
///
/// `free` holds the unallocated ranges as `(start, len)`, sorted by start,
/// pairwise disjoint and never touching (a free coalesces with both
/// neighbours), so its length is at most the live allocation count + 1.
#[derive(Debug)]
pub struct VaWindow {
    base: u64,
    top: u64,
    free: Vec<(u64, u64)>,
    live: u64,
    peak: u64,
}

fn page_round(n: u64) -> Option<u64> {
    n.checked_add(PAGE - 1).map(|v| v & !(PAGE - 1))
}

impl VaWindow {
    /// The window `[base, top)`. Both ends must be page-aligned with
    /// `base < top`; anything else yields an empty window that refuses
    /// every allocation (a constant pinned wrong fails closed).
    pub fn new(base: u64, top: u64) -> VaWindow {
        let mut free = Vec::new();
        if base % PAGE == 0 && top % PAGE == 0 && base < top {
            free.push((base, top - base));
        }
        VaWindow {
            base,
            top,
            free,
            live: 0,
            peak: 0,
        }
    }

    /// An address for `size` bytes (rounded up to whole pages), first fit.
    /// None for a zero size, a size past the window, or no range that fits.
    pub fn alloc(&mut self, size: u64) -> Option<u64> {
        if size == 0 {
            return None;
        }
        let need = page_round(size)?;
        let i = self.free.iter().position(|&(_, len)| len >= need)?;
        let (start, len) = self.free[i];
        if len == need {
            self.free.remove(i);
        } else {
            self.free[i] = (start + need, len - need);
        }
        self.live += need;
        if self.live > self.peak {
            self.peak = self.live;
        }
        Some(start)
    }

    /// Return `[va, va + size)` (page-rounded). Refused -- nothing changes
    /// and false is returned -- when the range is empty, misaligned, leaves
    /// the window, or overlaps any free range: a double free or a free of an
    /// address this window never handed out must not put a live mapping's
    /// address back in circulation.
    pub fn free(&mut self, va: u64, size: u64) -> bool {
        if size == 0 || va % PAGE != 0 {
            return false;
        }
        let Some(len) = page_round(size) else {
            return false;
        };
        let Some(end) = va.checked_add(len) else {
            return false;
        };
        if va < self.base || end > self.top {
            return false;
        }
        // The first free range starting at or after `va`.
        let i = self.free.partition_point(|&(s, _)| s < va);
        if let Some(&(s, _)) = self.free.get(i) {
            if s < end {
                return false;
            }
        }
        if i > 0 {
            let (ps, pl) = self.free[i - 1];
            if ps + pl > va {
                return false;
            }
        }
        // Coalesce with the successor, then the predecessor.
        let mut start = va;
        let mut total = len;
        if let Some(&(s, l)) = self.free.get(i) {
            if s == end {
                total += l;
                self.free.remove(i);
            }
        }
        if i > 0 {
            let (ps, pl) = self.free[i - 1];
            if ps + pl == va {
                start = ps;
                total += pl;
                self.free[i - 1] = (start, total);
                self.live -= len;
                return true;
            }
        }
        self.free.insert(i, (start, total));
        self.live -= len;
        true
    }

    /// Bytes handed out and not yet returned.
    pub fn live(&self) -> u64 {
        self.live
    }

    /// The most bytes ever handed out at once.
    pub fn peak(&self) -> u64 {
        self.peak
    }

    /// The largest single allocation that would succeed now.
    pub fn largest_free(&self) -> u64 {
        self.free.iter().map(|&(_, l)| l).max().unwrap_or(0)
    }

    /// Free ranges currently held (the fragmentation witness).
    pub fn free_ranges(&self) -> usize {
        self.free.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const B: u64 = 0x0240_0000;
    const T: u64 = 0x0240_0000 + 64 * PAGE;

    #[test]
    fn allocations_are_page_rounded_first_fit_and_disjoint() {
        let mut w = VaWindow::new(B, T);
        let a = w.alloc(1).unwrap();
        let b = w.alloc(PAGE + 1).unwrap();
        let c = w.alloc(PAGE).unwrap();
        assert_eq!(a, B);
        assert_eq!(b, B + PAGE, "one byte takes a whole page");
        assert_eq!(c, B + 3 * PAGE, "PAGE + 1 took two");
        assert_eq!(w.live(), 4 * PAGE);
        assert_eq!(w.alloc(0), None);
    }

    #[test]
    fn a_freed_range_is_handed_out_again() {
        let mut w = VaWindow::new(B, T);
        let a = w.alloc(8 * PAGE).unwrap();
        let _b = w.alloc(PAGE).unwrap();
        assert!(w.free(a, 8 * PAGE));
        assert_eq!(w.alloc(8 * PAGE), Some(a), "first fit reuses the hole");
    }

    #[test]
    fn frees_coalesce_with_both_neighbours() {
        let mut w = VaWindow::new(B, T);
        let a = w.alloc(PAGE).unwrap();
        let b = w.alloc(PAGE).unwrap();
        let c = w.alloc(PAGE).unwrap();
        let _d = w.alloc(PAGE).unwrap();
        assert!(w.free(a, PAGE));
        assert!(w.free(c, PAGE));
        assert_eq!(w.free_ranges(), 3, "a, c and the tail are apart");
        assert!(w.free(b, PAGE));
        assert_eq!(w.free_ranges(), 2, "a..c is one range again");
        assert_eq!(w.alloc(3 * PAGE), Some(a));
    }

    #[test]
    fn an_exhausted_window_refuses_rather_than_overrunning_its_top() {
        let mut w = VaWindow::new(B, T);
        assert!(w.alloc(64 * PAGE).is_some());
        assert_eq!(w.alloc(PAGE), None);
        let mut w = VaWindow::new(B, T);
        assert_eq!(w.alloc(65 * PAGE), None, "past the window");
        assert_eq!(w.alloc(u64::MAX), None, "the round-up cannot wrap");
    }

    #[test]
    fn a_double_free_or_a_foreign_range_changes_nothing() {
        let mut w = VaWindow::new(B, T);
        let a = w.alloc(2 * PAGE).unwrap();
        let _b = w.alloc(PAGE).unwrap();
        assert!(w.free(a, 2 * PAGE));
        let before = (w.live(), w.free_ranges(), w.largest_free());
        assert!(!w.free(a, 2 * PAGE), "double free");
        assert!(!w.free(a + PAGE, PAGE), "inside a free range");
        assert!(!w.free(B - PAGE, PAGE), "below the window");
        assert!(!w.free(T, PAGE), "at the top");
        assert!(!w.free(a + 1, PAGE), "misaligned");
        assert!(!w.free(a, 0), "empty");
        assert!(!w.free(u64::MAX & !(PAGE - 1), 2 * PAGE), "wraps");
        assert_eq!((w.live(), w.free_ranges(), w.largest_free()), before);
    }

    #[test]
    fn a_misconfigured_window_refuses_everything() {
        assert_eq!(VaWindow::new(B + 1, T).alloc(PAGE), None);
        assert_eq!(VaWindow::new(T, B).alloc(PAGE), None);
        assert_eq!(VaWindow::new(B, B).alloc(PAGE), None);
    }

    /// THE witness for the drag: two tiles and their headers reweave every
    /// frame, one growing and one shrinking, each new generation allocated
    /// BEFORE the old one is freed (the bin's swap order). A bump allocator
    /// needs the sum over the whole drag; this window's use never exceeds
    /// what the largest pair of generations alive at once can need.
    #[test]
    fn a_long_divider_drag_stays_bounded_by_the_live_set() {
        let top = B + (2u64 << 30); // 2 GiB, the real window's order
        let mut w = VaWindow::new(B, top);
        let gen = |px: u64| page_round(px * 4 * 800).unwrap() * 3;
        let (mut wa, mut wb) = (640u64, 640u64);
        let mut a = (w.alloc(gen(wa)).unwrap(), gen(wa));
        let mut b = (w.alloc(gen(wb)).unwrap(), gen(wb));
        let mut hdr = (w.alloc(gen(1)).unwrap(), gen(1));
        let mut bump_total = a.1 + b.1 + hdr.1;
        for frame in 0..20_000u64 {
            let step = if (frame / 500) % 2 == 0 { 1 } else { -1i64 };
            wa = (wa as i64 + step).clamp(300, 980) as u64;
            wb = 1280 - wa;
            for (slot, px) in [(&mut a, wa), (&mut b, wb), (&mut hdr, 1)] {
                let size = gen(px);
                let va = w.alloc(size).expect("the live set always fits");
                assert!(w.free(slot.0, slot.1));
                *slot = (va, size);
                bump_total += size;
            }
        }
        let max_pair = 2 * (gen(980) + gen(300) + gen(1));
        assert!(w.peak() <= max_pair, "peak {} vs {}", w.peak(), max_pair);
        assert!(bump_total > top - B, "a bump allocator would have left the window");
        assert!(w.free_ranges() <= 4, "fragmentation stays bounded: {}", w.free_ranges());
    }
}
