//! Aggregate userspace cancellation is an additional condition, never a
//! replacement for GPU/key quiescence. Only the designated compositor may
//! supply an acknowledgement; the native dispatcher authenticates that peer.
#[derive(Default)]
pub struct Gate {
    generation: u64,
    phase: u32,
    acknowledged: bool,
    sampled: bool,
}
impl Gate {
    pub fn observe(&mut self, generation: u64, phase: u32) {
        if !self.sampled || self.generation != generation || self.phase != phase {
            self.generation = generation;
            self.phase = phase;
            self.acknowledged = false;
            self.sampled = true;
        }
    }
    pub fn acknowledge(&mut self, generation: u64) -> bool {
        if !self.sampled || self.phase != 1 || generation != self.generation {
            return false;
        }
        self.acknowledged = true;
        true
    }
    pub fn ready(&self, generation: u64) -> bool {
        self.sampled && self.phase == 1 && generation == self.generation && self.acknowledged
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_generation_and_quiescing_required() {
        let mut g = Gate::default();
        assert!(!g.acknowledge(0));
        g.observe(0, 0);
        assert!(!g.acknowledge(0));
        g.observe(1, 1);
        assert!(!g.ready(1));
        assert!(!g.acknowledge(0));
        assert!(!g.acknowledge(2));
        assert!(g.acknowledge(1));
        assert!(g.ready(1));
        assert!(!g.ready(2));
        g.observe(1, 1);
        assert!(g.ready(1));
    }
    #[test]
    fn no_replay_across_episode_failure_or_restoration() {
        let mut g = Gate::default();
        for phase in [2, 3, 4, 0] {
            g.observe(1, 1);
            assert!(g.acknowledge(1));
            g.observe(1, phase);
            assert!(!g.ready(1));
            assert!(!g.acknowledge(1));
        }
        g.observe(2, 1);
        assert!(!g.ready(2));
        assert!(!g.acknowledge(1));
    }
}
