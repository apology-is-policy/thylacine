//! Pure split-virtqueue index bookkeeping.
//!
//! The avail/used ring counters and their wrap / back-pressure invariants,
//! factored out of the device glue (`virtio.rs`) so the bug-prone arithmetic --
//! the exact class the virtio audits prosecute (RW-7/RW-8) -- is host-testable
//! in isolation.
//!
//! These structs touch NO memory and NO device. They track the u16 counters a
//! split-virtqueue driver maintains per queue (per VIRTIO 1.2 section 2.6 the
//! avail/used `idx` fields are u16, monotonic with implicit wraparound at
//! 65536) and yield the ring SLOT the driver should read/write. Every slot they
//! return is `counter % size`, hence always in `0..size` -- so a slot can never
//! index out of the ring. The driver still bounds-checks the device-controlled
//! `desc_id` it reads OUT of the used ring before scaling it into a buffer pool
//! (that `desc_id` is device-attacker-controlled and is NOT a ring slot; see
//! `VirtioNet::poll_rx`). The anti-DoS bound on a drain LOOP (a hostile device
//! racing `used.idx` far ahead) is the CALLER's responsibility -- `take_used`
//! yields one entry per call; the driver's poll loop caps its iterations.

/// TX submission ring. The driver advances `avail_idx` as it posts frames and
/// `seen_used` as it reaps completions; `in_flight = avail_idx - seen_used`.
#[derive(Clone, Copy, Debug)]
pub struct TxRing {
    size: u16,
    avail_idx: u16,
    seen_used: u16,
}

impl TxRing {
    #[inline]
    pub const fn new(size: u16) -> Self {
        Self { size, avail_idx: 0, seen_used: 0 }
    }

    /// Frames posted to the device but not yet reaped.
    #[inline]
    pub fn in_flight(&self) -> u16 {
        self.avail_idx.wrapping_sub(self.seen_used)
    }

    /// True iff a new frame can be posted without reusing a descriptor still in
    /// flight (the back-pressure gate: never exceed `size` outstanding, so a
    /// descriptor's buffer is never overwritten while the device may read it).
    #[inline]
    pub fn can_post(&self) -> bool {
        self.in_flight() < self.size
    }

    /// The descriptor + buffer slot the next post uses (`avail_idx % size`).
    /// Caller MUST have checked `can_post()` first.
    #[inline]
    pub fn next_slot(&self) -> usize {
        (self.avail_idx % self.size) as usize
    }

    /// Commit a post: advance `avail_idx` (wrapping). Returns the new
    /// `avail_idx` the driver writes into the avail-ring `idx` field.
    #[inline]
    pub fn commit_post(&mut self) -> u16 {
        self.avail_idx = self.avail_idx.wrapping_add(1);
        self.avail_idx
    }

    /// Reap completions up to the device's `cur_used` index, bounded by `cap`
    /// iterations (defense vs a desynced/hostile device that never matches).
    /// Returns the count reaped (frees that many in-flight slots).
    #[inline]
    pub fn reap(&mut self, cur_used: u16, cap: u16) -> u16 {
        let mut n = 0u16;
        while self.seen_used != cur_used && n < cap {
            self.seen_used = self.seen_used.wrapping_add(1);
            n = n.wrapping_add(1);
        }
        n
    }

    #[inline]
    pub fn avail_idx(&self) -> u16 {
        self.avail_idx
    }
}

/// RX completion + recycle ring. Descriptors are pre-posted (so `avail_idx`
/// starts at `size`), drained one-per-`take_used` as the device fills them, and
/// recycled back to the avail ring. `seen_used` is the driver's cursor into the
/// used ring; `avail_idx` extends past `size` as buffers recycle.
#[derive(Clone, Copy, Debug)]
pub struct RxRing {
    size: u16,
    avail_idx: u16, // starts at `size`: all buffers pre-posted at init
    seen_used: u16,
}

impl RxRing {
    #[inline]
    pub const fn new(size: u16) -> Self {
        Self { size, avail_idx: size, seen_used: 0 }
    }

    /// True iff the device has delivered an unseen frame (`used.idx` moved past
    /// our cursor).
    #[inline]
    pub fn has_used(&self, cur_used: u16) -> bool {
        self.seen_used != cur_used
    }

    /// Consume one used entry: return its used-ring slot (`seen_used % size`)
    /// and advance the cursor. `None` once caught up to `cur_used`. The caller
    /// bounds its drain loop (anti-DoS vs a device racing `used.idx` ahead).
    #[inline]
    pub fn take_used(&mut self, cur_used: u16) -> Option<usize> {
        if self.seen_used == cur_used {
            return None;
        }
        let slot = (self.seen_used % self.size) as usize;
        self.seen_used = self.seen_used.wrapping_add(1);
        Some(slot)
    }

    /// Recycle a drained descriptor: return the avail-ring slot to write the
    /// reclaimed `desc_id` into (`avail_idx % size`) and advance `avail_idx`
    /// (wrapping). Pair each `take_used` that yields a frame with one
    /// `recycle_slot` so the RX queue never runs dry.
    #[inline]
    pub fn recycle_slot(&mut self) -> usize {
        let slot = (self.avail_idx % self.size) as usize;
        self.avail_idx = self.avail_idx.wrapping_add(1);
        slot
    }

    #[inline]
    pub fn avail_idx(&self) -> u16 {
        self.avail_idx
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SZ: u16 = 16;

    #[test]
    fn tx_backpressure_blocks_at_size() {
        let mut tx = TxRing::new(SZ);
        for i in 0..SZ {
            assert!(tx.can_post(), "should accept post {i}");
            assert!(tx.next_slot() < SZ as usize);
            tx.commit_post();
        }
        assert_eq!(tx.in_flight(), SZ);
        assert!(!tx.can_post(), "must back-pressure at `size` in flight");
        let reaped = tx.reap(2, SZ);
        assert_eq!(reaped, 2);
        assert_eq!(tx.in_flight(), SZ - 2);
        assert!(tx.can_post());
    }

    #[test]
    fn tx_reap_is_capped() {
        let mut tx = TxRing::new(SZ);
        for _ in 0..SZ {
            tx.commit_post();
        }
        let reaped = tx.reap(SZ, 4);
        assert_eq!(reaped, 4, "drain loop honors the cap");
        assert_eq!(tx.in_flight(), SZ - 4);
    }

    #[test]
    fn tx_slot_stays_in_range_across_u16_wrap() {
        // Post + reap ~5 full u16 cycles; slot must never leave 0..size and
        // in_flight returns to 0 each lap -- exercises wrapping_add rollover at
        // 65536 on BOTH avail_idx and seen_used.
        let mut tx = TxRing::new(SZ);
        for i in 0u32..(65536 * 5 + 7) {
            assert!(tx.can_post(), "iter {i}");
            let slot = tx.next_slot();
            assert!(slot < SZ as usize, "slot {slot} OOB at iter {i}");
            tx.commit_post();
            assert_eq!(tx.reap(tx.avail_idx(), SZ), 1);
            assert_eq!(tx.in_flight(), 0);
        }
    }

    #[test]
    fn rx_take_used_stops_at_cur_used() {
        let mut rx = RxRing::new(SZ);
        assert!(rx.has_used(3));
        assert_eq!(rx.take_used(3), Some(0));
        assert_eq!(rx.take_used(3), Some(1));
        assert_eq!(rx.take_used(3), Some(2));
        assert!(!rx.has_used(3));
        assert_eq!(rx.take_used(3), None, "caught up to cur_used");
    }

    #[test]
    fn rx_recycle_slot_in_range_and_advances() {
        let mut rx = RxRing::new(SZ);
        // avail_idx starts at size; first recycle slot is size % size == 0.
        assert_eq!(rx.recycle_slot(), 0);
        assert_eq!(rx.recycle_slot(), 1);
        assert_eq!(rx.avail_idx(), SZ + 2);
    }

    #[test]
    fn rx_drain_recycle_steady_state_across_wrap() {
        // Drain + recycle one frame per "tick" for > 5 u16 cycles; every used
        // slot and every recycle slot stays in 0..size and the cursors wrap
        // without desyncing.
        let mut rx = RxRing::new(SZ);
        let mut device_used: u16 = 0;
        for i in 0u32..(65536 * 5 + 9) {
            device_used = device_used.wrapping_add(1);
            let used_slot = rx.take_used(device_used).expect("one frame ready");
            assert!(used_slot < SZ as usize, "used slot {used_slot} OOB at {i}");
            let avail_slot = rx.recycle_slot();
            assert!(avail_slot < SZ as usize, "avail slot {avail_slot} OOB at {i}");
            assert_eq!(rx.take_used(device_used), None, "exactly one per tick");
        }
    }
}
