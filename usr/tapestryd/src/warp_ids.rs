// The seam's public warp context ids. A pub is the only name the broker's
// released-stream record (GPU-DESIGN 4.5.13) and a client's paths know a
// context by, and the 32-bit sequence wraps, so a pub is reissued only once
// nothing that carried it can still be named.

/// The pub after `seq`: never 0 (`warp_ctx_vindicate`'s "no condemned slot"
/// sentinel) and never one `held` names. `held` names at most a slot's worth
/// of values, so the search ends.
pub fn next_ctx_pub(seq: u32, held: impl Fn(u32) -> bool) -> u32 {
    let mut p = seq;
    loop {
        p = p.wrapping_add(1);
        if p != 0 && !held(p) {
            return p;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_sequence_counts_up_and_skips_zero_at_the_wrap() {
        assert_eq!(next_ctx_pub(0, |_| false), 1);
        assert_eq!(next_ctx_pub(41, |_| false), 42);
        assert_eq!(next_ctx_pub(u32::MAX, |_| false), 1);
    }

    #[test]
    fn a_held_pub_is_never_reissued_even_across_the_wrap() {
        let held = [1u32, 2, 5];
        let h = |p: u32| held.contains(&p);
        assert_eq!(next_ctx_pub(u32::MAX, h), 3, "0, 1 and 2 are skipped");
        assert_eq!(next_ctx_pub(4, h), 6);
        assert_eq!(next_ctx_pub(u32::MAX - 1, |p| p == u32::MAX), 1);
    }
}
