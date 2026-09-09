// WEAVE-SKEIN (docs/WEAVE-SKEIN-DESIGN.md): the guest-side segment arithmetic.
//
// A weave above SKEIN_BLOCK is backed by N physically-contiguous runs rather
// than one span, because a single span of a 48.8 MiB weave means an order-14
// (64 MiB, naturally aligned) buddy allocation that fails with 1889 MiB free.
// The kernel hands the list over via t_dma_segments; virtio-gpu's
// RESOURCE_ATTACH_BACKING takes exactly that array.
//
// The one piece of arithmetic that is not a straight copy is here: a weave's
// per-slot resources are each backed by a SUBRANGE of the weave (slot i lives
// at i * slot_stride), and a slot boundary does not land on a block boundary.
// So the driver must slice the weave's list down to the slot's byte range.
//
// This module is PURE -- no syscalls, no device -- so it is exercised on the
// host, where the boundary cases that matter (a slot starting mid-block,
// ending mid-block, spanning several) can be constructed directly instead of
// waiting for a display geometry that happens to produce them.

/// One physically-contiguous run of a buffer's backing.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Seg {
    pub pa: u64,
    pub len: u64,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SubErr {
    /// The requested range is empty, overflows, or runs past the buffer.
    OutOfRange,
    /// The range needs more entries than the output holds. REFUSED rather
    /// than truncated: a short list attached as a whole backing would have
    /// the device read a prefix and whatever follows it in physical memory.
    TooMany,
    /// The input list is not a well-formed backing (a zero-length run).
    Malformed,
}

/// Slice a buffer's segment list down to the byte range `[off, off + len)`,
/// writing the covering runs into `out` in ascending order and returning how
/// many were written.
///
/// The returned runs sum to exactly `len` -- that is the property the caller
/// depends on, because it is what makes "this resource is backed by these
/// pages and no others" true (I-45).
pub fn subrange(segs: &[Seg], off: u64, len: u64, out: &mut [Seg]) -> Result<usize, SubErr> {
    if len == 0 {
        return Err(SubErr::OutOfRange);
    }
    let end = off.checked_add(len).ok_or(SubErr::OutOfRange)?;

    // Total first, so a range past the end is refused before any output is
    // written -- a partially-filled `out` plus an error is the shape a caller
    // can misread as success.
    let mut total: u64 = 0;
    for s in segs {
        if s.len == 0 {
            return Err(SubErr::Malformed);
        }
        total = total.checked_add(s.len).ok_or(SubErr::Malformed)?;
    }
    if end > total {
        return Err(SubErr::OutOfRange);
    }

    let mut n = 0usize;
    let mut pos: u64 = 0; // buffer offset at which the current run starts
    for s in segs {
        let s_end = pos + s.len; // no overflow: bounded by `total` above
        if s_end > off && pos < end {
            let from = if off > pos { off - pos } else { 0 };
            let to = if end < s_end { end - pos } else { s.len };
            if n == out.len() {
                return Err(SubErr::TooMany);
            }
            out[n] = Seg {
                pa: s.pa + from,
                len: to - from,
            };
            n += 1;
        }
        pos = s_end;
        if pos >= end {
            break;
        }
    }
    Ok(n)
}

#[cfg(test)]
mod tests {
    use super::*;

    // A three-block skein: 2 MiB + 2 MiB + 1 MiB, at deliberately
    // NON-ADJACENT physical addresses. Adjacent test PAs would let an
    // off-by-one in the block walk still produce the right answer by
    // accident -- the scatter is the whole point, so the fixture scatters.
    const M: u64 = 1024 * 1024;
    fn skein() -> [Seg; 3] {
        [
            Seg { pa: 0x4000_0000, len: 2 * M },
            Seg { pa: 0x9000_0000, len: 2 * M },
            Seg { pa: 0x2000_0000, len: M },
        ]
    }

    fn total(v: &[Seg]) -> u64 {
        v.iter().map(|s| s.len).sum()
    }

    #[test]
    fn whole_buffer_reproduces_the_list() {
        let s = skein();
        let mut out = [Seg::default(); 8];
        let n = subrange(&s, 0, 5 * M, &mut out).unwrap();
        assert_eq!(n, 3);
        assert_eq!(&out[..n], &s[..]);
    }

    #[test]
    fn range_inside_one_block_yields_one_run() {
        let s = skein();
        let mut out = [Seg::default(); 8];
        let n = subrange(&s, 4096, 8192, &mut out).unwrap();
        assert_eq!(n, 1);
        assert_eq!(out[0], Seg { pa: 0x4000_0000 + 4096, len: 8192 });
    }

    // The case a display geometry produces and a hand-picked test usually
    // misses: a slot that starts mid-block and ends mid-block.
    #[test]
    fn range_straddling_a_boundary_splits_at_it() {
        let s = skein();
        let mut out = [Seg::default(); 8];
        let n = subrange(&s, 2 * M - 4096, 8192, &mut out).unwrap();
        assert_eq!(n, 2);
        assert_eq!(out[0], Seg { pa: 0x4000_0000 + 2 * M - 4096, len: 4096 });
        assert_eq!(out[1], Seg { pa: 0x9000_0000, len: 4096 });
        assert_eq!(total(&out[..n]), 8192);
    }

    #[test]
    fn range_spanning_every_block() {
        let s = skein();
        let mut out = [Seg::default(); 8];
        let n = subrange(&s, M, 4 * M - 4096, &mut out).unwrap();
        assert_eq!(n, 3);
        assert_eq!(out[0], Seg { pa: 0x4000_0000 + M, len: M });
        assert_eq!(out[1], Seg { pa: 0x9000_0000, len: 2 * M });
        assert_eq!(out[2], Seg { pa: 0x2000_0000, len: M - 4096 });
        assert_eq!(total(&out[..n]), 4 * M - 4096);
    }

    // The three slots of a triple-buffered weave, walked exactly as the
    // driver walks them. Every byte of the weave must be covered once: this
    // is the end-to-end shape, where a per-slot test would not catch a slot
    // whose start offset was computed wrong.
    #[test]
    fn three_slots_tile_the_weave_exactly() {
        let s = skein();
        let slot = 5 * M / 3 / 4096 * 4096; // page-aligned slot stride
        let mut seen: u64 = 0;
        for i in 0..3u64 {
            let mut out = [Seg::default(); 8];
            let n = subrange(&s, i * slot, slot, &mut out).unwrap();
            assert!(n >= 1);
            assert_eq!(total(&out[..n]), slot, "slot {i} must be fully backed");
            seen += total(&out[..n]);
        }
        assert_eq!(seen, 3 * slot);
    }

    #[test]
    fn past_the_end_refuses() {
        let s = skein();
        let mut out = [Seg::default(); 8];
        assert_eq!(subrange(&s, 5 * M, 1, &mut out), Err(SubErr::OutOfRange));
        assert_eq!(subrange(&s, 0, 5 * M + 1, &mut out), Err(SubErr::OutOfRange));
        assert_eq!(subrange(&s, 0, 0, &mut out), Err(SubErr::OutOfRange));
        assert_eq!(subrange(&s, u64::MAX, 2, &mut out), Err(SubErr::OutOfRange));
        // The last byte still resolves -- a bound that refused it would be
        // off by one in the safe-looking direction.
        assert!(subrange(&s, 5 * M - 1, 1, &mut out).is_ok());
    }

    // REFUSES, never truncates. The failure this forbids is silent: a
    // truncated list attached as a whole backing leaves the device reading
    // a prefix and then whatever physical memory follows it.
    #[test]
    fn too_small_an_output_refuses_rather_than_truncating() {
        let s = skein();
        let mut out = [Seg::default(); 2];
        assert_eq!(subrange(&s, 0, 5 * M, &mut out), Err(SubErr::TooMany));
        // ... and exactly-enough still works, so the refusal is on the real
        // bound and not one entry early.
        let mut out3 = [Seg::default(); 3];
        assert_eq!(subrange(&s, 0, 5 * M, &mut out3), Ok(3));
    }

    #[test]
    fn a_zero_length_run_is_malformed() {
        let bad = [Seg { pa: 0x4000_0000, len: 0 }];
        let mut out = [Seg::default(); 4];
        assert_eq!(subrange(&bad, 0, 1, &mut out), Err(SubErr::Malformed));
    }

    // A single contiguous object (nblk == 1) is not a special case here --
    // the same walk must produce the same answer, because the driver runs one
    // code path for both and only the kernel knows which it got.
    #[test]
    fn a_contiguous_buffer_takes_the_same_path() {
        let one = [Seg { pa: 0x8000_0000, len: 16 * M }];
        let mut out = [Seg::default(); 4];
        let n = subrange(&one, 4 * M, 2 * M, &mut out).unwrap();
        assert_eq!(n, 1);
        assert_eq!(out[0], Seg { pa: 0x8000_0000 + 4 * M, len: 2 * M });
    }
}
