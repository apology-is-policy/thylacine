//! Single outstanding cursor chain. Completion is a used-entry proof, never an
//! IRQ. Poison is permanent until device reset: after a malformed entry nothing
//! says what the device did, so late DMA could reach reused descriptors or freed
//! backing, and cannot qualify a trusted handover. A chain that only outlived its
//! bound is abandoned instead: a well-formed entry, however late, proves the device
//! is finished with it, so the lane comes back.
#[derive(Default)]
pub struct Queue {
    pub published: u16,
    pub retired: u16,
    failed: bool,
    abandoned: bool,
}
impl Queue {
    pub fn idle(&self) -> bool {
        !self.failed && !self.abandoned && self.published == self.retired
    }
    pub fn poison(&mut self) {
        self.failed = true;
    }
    pub fn poisoned(&self) -> bool {
        self.failed
    }
    pub fn abandon(&mut self) {
        self.abandoned = true;
    }
    pub fn abandoned(&self) -> bool {
        self.abandoned && !self.failed
    }
    pub fn begin(&mut self) -> Result<u16, ()> {
        if !self.idle() {
            return Err(());
        }
        let slot = self.published;
        self.published = self.published.wrapping_add(1);
        Ok(slot)
    }
    pub fn complete(&mut self, index: u16, head: u32, len: u32, reply_ok: bool) -> Result<(), ()> {
        if self.failed
            || self.published == self.retired
            || index != self.published
            || head != 0
            || !(len == 0 || (len == 24 && reply_ok))
        {
            self.poison();
            return Err(());
        }
        self.retired = index;
        self.abandoned = false;
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn no_reuse_before_completion_including_wrap() {
        let mut q = Queue {
            published: u16::MAX,
            retired: u16::MAX,
            failed: false,
            abandoned: false,
        };
        assert_eq!(q.begin(), Ok(u16::MAX));
        assert!(!q.idle());
        assert!(q.begin().is_err());
        assert!(q.complete(0, 0, 0, false).is_ok());
        assert!(q.idle());
        assert_eq!(q.begin(), Ok(0));
        assert!(q.complete(1, 0, 24, true).is_ok());
    }
    #[test]
    fn malformed_and_late_entries_never_release_the_lane() {
        for (idx, head, len, ok) in [
            (0, 0, 0, true),
            (2, 0, 0, true),
            (1, 1, 0, true),
            (1, 0, 23, true),
            (1, 0, 25, true),
            (1, 0, 24, false),
        ] {
            let mut q = Queue::default();
            q.begin().unwrap();
            assert!(q.complete(idx, head, len, ok).is_err());
            assert!(!q.idle());
            assert!(q.complete(1, 0, 0, true).is_err());
            assert!(q.begin().is_err());
        }
        let mut q = Queue::default();
        q.begin().unwrap();
        q.poison();
        assert!(q.complete(1, 0, 0, true).is_err());
        assert!(!q.idle());
    }
    #[test]
    fn an_abandoned_chain_frees_the_lane_only_by_retiring_well_formed() {
        let mut q = Queue::default();
        q.begin().unwrap();
        q.abandon();
        assert!(!q.idle());
        assert!(q.abandoned());
        assert!(q.begin().is_err());
        assert!(q.complete(1, 0, 24, true).is_ok());
        assert!(q.idle());
        assert!(!q.abandoned());
        assert_eq!(q.begin(), Ok(1));

        let mut q = Queue::default();
        q.begin().unwrap();
        q.abandon();
        assert!(q.complete(1, 1, 24, true).is_err());
        assert!(q.poisoned());
        assert!(!q.abandoned());
        assert!(!q.idle());
    }
}
