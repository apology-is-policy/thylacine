// A session tile's undelivered down-channel input (HALCYON 14.11.9; the
// KT-1 audit's A-F6 + round-2 B2-F3).
//
// halcyond is the pipe's only writer and must never block on it (natives
// cannot mark a pipe non-blocking; a whole-record write that does not fit
// parks the writer, and a parked compositor is a dead seat). So input is
// queued here and delivered one byte per ready POLLOUT by the session loop.
// Two record classes, two policies: KEYS are byte-bounded and the NEWEST
// drop past the cap (a terminal that stopped draining loses keystrokes, not
// the compositor); the GEOMETRY record is never dropped -- one waits
// (latest-wins) and goes out ahead of any further key, at a record boundary.

use alloc::collections::VecDeque;
use alloc::vec::Vec;

/// Bytes of encoded KEY input a tile may hold undelivered before the newest
/// are dropped. The geometry record is outside this cap.
pub const DOWN_PENDING_MAX: usize = 4096;

/// The queue. A dropped Resize would leave the terminal at the old size for
/// the rest of its life (no later CONFIGURE at the same size re-sends it,
/// and nothing acks it), which is why it has its own slot.
pub struct DownQueue {
    keys: VecDeque<Vec<u8>>,
    key_bytes: usize,
    resize: Option<Vec<u8>>,
    /// The record being written; `off` bytes of it are already out. A partial
    /// write never interleaves with another record.
    inflight: Vec<u8>,
    off: usize,
}

impl Default for DownQueue {
    fn default() -> Self {
        Self::new()
    }
}

impl DownQueue {
    pub fn new() -> DownQueue {
        DownQueue {
            keys: VecDeque::new(),
            key_bytes: 0,
            resize: None,
            inflight: Vec::new(),
            off: 0,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.off == self.inflight.len() && self.resize.is_none() && self.keys.is_empty()
    }

    /// Queue one encoded key record. False = dropped (the cap).
    pub fn push_key(&mut self, rec: &[u8]) -> bool {
        if self.key_bytes + rec.len() > DOWN_PENDING_MAX {
            return false;
        }
        self.key_bytes += rec.len();
        self.keys.push_back(rec.to_vec());
        true
    }

    /// Queue the encoded geometry record; a waiting one is replaced (only the
    /// latest size matters), an in-flight one completes first.
    pub fn push_resize(&mut self, rec: &[u8]) {
        self.resize = Some(rec.to_vec());
    }

    /// The next byte to deliver, loading the next record at a boundary: the
    /// waiting Resize first, else the oldest key.
    pub fn next_byte(&mut self) -> Option<u8> {
        if self.off == self.inflight.len() {
            self.inflight = match self.resize.take() {
                Some(r) => r,
                None => {
                    let k = self.keys.pop_front()?;
                    self.key_bytes -= k.len();
                    k
                }
            };
            self.off = 0;
        }
        self.inflight.get(self.off).copied()
    }

    /// The byte `next_byte` returned was written.
    pub fn advance(&mut self) {
        self.off += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    fn drain_all(q: &mut DownQueue) -> Vec<u8> {
        let mut out = Vec::new();
        while let Some(b) = q.next_byte() {
            out.push(b);
            q.advance();
        }
        assert!(q.is_empty());
        out
    }

    #[test]
    fn a_resize_survives_a_full_key_queue_and_goes_first() {
        let mut q = DownQueue::new();
        let key = [7u8; 8];
        let mut n = 0;
        while q.push_key(&key) {
            n += 1;
        }
        assert_eq!(n * 8, DOWN_PENDING_MAX, "keys fill exactly to the cap");
        assert!(!q.push_key(&key), "past the cap the newest key drops");
        let resize = [9u8, 1, 2, 3, 4, 5, 6, 7, 8];
        q.push_resize(&resize);
        let out = drain_all(&mut q);
        assert_eq!(
            &out[..9],
            &resize,
            "the geometry record is delivered, ahead of the keys"
        );
        assert_eq!(
            out.len(),
            9 + DOWN_PENDING_MAX,
            "no key already queued is lost"
        );
    }

    #[test]
    fn a_resize_never_splits_an_in_flight_key_record() {
        let mut q = DownQueue::new();
        q.push_key(&[1, 1, 1, 1]);
        q.push_key(&[2, 2]);
        // two bytes of the first key are out when the resize arrives
        assert_eq!(q.next_byte(), Some(1));
        q.advance();
        assert_eq!(q.next_byte(), Some(1));
        q.advance();
        q.push_resize(&[9, 9, 9]);
        let out = drain_all(&mut q);
        assert_eq!(out, alloc::vec![1, 1, 9, 9, 9, 2, 2]);
    }

    #[test]
    fn only_the_latest_waiting_resize_is_delivered() {
        let mut q = DownQueue::new();
        q.push_resize(&[1, 1]);
        q.push_resize(&[2, 2]);
        assert_eq!(drain_all(&mut q), alloc::vec![2, 2]);
        // an in-flight resize completes before its successor
        q.push_resize(&[3, 3]);
        assert_eq!(q.next_byte(), Some(3));
        q.advance();
        q.push_resize(&[4, 4]);
        assert_eq!(drain_all(&mut q), alloc::vec![3, 4, 4]);
    }

    #[test]
    fn empty_and_partial_states_are_reported_right() {
        let mut q = DownQueue::new();
        assert!(q.is_empty());
        assert_eq!(q.next_byte(), None);
        q.push_key(&[5]);
        assert!(!q.is_empty());
        assert_eq!(q.next_byte(), Some(5));
        assert!(!q.is_empty(), "loaded but unwritten is not empty");
        q.advance();
        assert!(q.is_empty());
    }
}
