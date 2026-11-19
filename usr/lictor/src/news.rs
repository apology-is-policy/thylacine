//! The compositor's news: which broker answers have changed since a reader last
//! heard. A read of the broker's `news` file parks until one has, so the
//! compositor asks only when an answer would differ (operator vote 147).
//!
//! Every fact is one the client may already ask for, read at the end of a pass,
//! where the requests of that pass were answered: the seat pair (`SeatState`),
//! the engine's state and stall count, the display's event count and the
//! restores that ended blank (the `Stats` every reply carries), and per
//! queue whether a take would return something (`InputDrain`, `TakeCompletions`,
//! `TakeVindications`). News therefore tells the client
//! nothing a request could not, and nothing about input the seat kept back: an
//! event pressed at a trusted screen is gone from the queue before the pass ends.
//! No queue is news during an episode, only one in a normal seat being reported:
//! the fence takes are parked then, and a late fence retired mid-episode would
//! say when (I-27); the input takes are admitted, but the seat queues nothing
//! while the phase is not 0. It is news after.
//!
//! A queue is news once per take. Told, the client takes; the take, not the
//! reply, rearms it. A queue nobody takes is told once and then stays quiet, so
//! an undrained device cannot spin a reader.

pub const SEAT: u32 = 1;
pub const INPUT: u32 = 2;
/// The fence lists, the engine's state, or the display's counts.
pub const GPU: u32 = 4;
pub const ALL: u32 = SEAT | INPUT | GPU;
use crate::gpu_api::Engine;

/// Input devices with a queue of their own (`backend::intake::MAX_DEVICES`).
pub const INPUTS: usize = 3;
pub const COMPLETIONS: usize = INPUTS;
pub const VINDICATIONS: usize = INPUTS + 1;
pub const QUEUES: usize = INPUTS + 2;

/// The broker's state at the end of a pass.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Facts {
    pub seat: (u64, u32),
    /// The engine's state and its stall count, which change between requests.
    pub engine: (Engine, u32),
    /// The display's event count and the restores that ended blank: `Stats`
    /// the compositor follows the host's display and rebinds by (vote M37).
    pub display: (u32, u32),
    /// Per queue: a take would return something now.
    pub ready: [bool; QUEUES],
    /// Per queue: the takes it has served.
    pub takes: [u64; QUEUES],
}

/// What one reader has been told.
#[derive(Clone, Copy, Debug, Default)]
pub struct Heard {
    seat: Option<(u64, u32)>,
    engine: Option<(Engine, u32)>,
    display: Option<(u32, u32)>,
    takes: [Option<u64>; QUEUES],
}

fn class(queue: usize) -> u32 {
    if queue < INPUTS { INPUT } else { GPU }
}

/// A queue's readiness is reported only in a normal seat.
fn reportable(facts: &Facts, queue: usize) -> bool {
    facts.ready[queue] && facts.seat.1 == 0
}

impl Heard {
    /// The classes of `interest` with news for this reader.
    pub fn pending(&self, facts: &Facts, interest: u32) -> u32 {
        let mut bits = 0;
        if interest & SEAT != 0 && self.seat != Some(facts.seat) {
            bits |= SEAT;
        }
        if interest & GPU != 0 && (self.engine != Some(facts.engine) || self.display != Some(facts.display)) {
            bits |= GPU;
        }
        for q in 0..QUEUES {
            if interest & class(q) != 0 && reportable(facts, q) && self.takes[q] != Some(facts.takes[q]) {
                bits |= class(q);
            }
        }
        bits
    }
    /// The reader has been told `bits`. Only a queue that was reported is
    /// marked: marking any other would silence it before any take.
    pub fn told(&mut self, facts: &Facts, bits: u32) {
        if bits & SEAT != 0 {
            self.seat = Some(facts.seat);
        }
        if bits & GPU != 0 {
            self.engine = Some(facts.engine);
            self.display = Some(facts.display);
        }
        for q in 0..QUEUES {
            if bits & class(q) != 0 && reportable(facts, q) {
                self.takes[q] = Some(facts.takes[q]);
            }
        }
    }
}

/// A news Tread's offset names its interest. Zero, or any bit outside `ALL`, is
/// refused, so a future class is never read as part of a mask.
pub fn interest(offset: u64) -> Option<u32> {
    if offset == 0 || offset & !(ALL as u64) != 0 { None } else { Some(offset as u32) }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts() -> Facts { Facts { seat: (1, 0), ..Facts::default() } }
    /// A reader told the seat and the engine as they stand, and nothing queued.
    fn settled(f: &Facts) -> Heard {
        let mut h = Heard::default();
        let mut quiet = *f;
        quiet.ready = [false; QUEUES];
        h.told(&quiet, SEAT | GPU);
        h
    }

    #[test]
    fn a_new_reader_hears_the_seat_and_every_ready_queue() {
        let mut f = facts();
        f.ready[1] = true;
        f.ready[VINDICATIONS] = true;
        let h = Heard::default();
        assert_eq!(h.pending(&f, ALL), ALL);
        assert_eq!(h.pending(&f, SEAT), SEAT);
        assert_eq!(h.pending(&f, INPUT | GPU), INPUT | GPU);
        f.ready = [false; QUEUES];
        assert_eq!(h.pending(&f, INPUT), 0, "an empty queue is no news");
        assert_eq!(h.pending(&f, GPU), GPU, "but the engine it has not heard is");
    }

    #[test]
    fn the_seat_is_news_until_told_and_again_on_any_change() {
        let mut f = facts();
        let mut h = settled(&f);
        assert_eq!(h.pending(&f, ALL), 0);
        f.seat = (1, 1);
        assert_eq!(h.pending(&f, ALL), SEAT, "a phase change");
        h.told(&f, SEAT);
        f.seat = (2, 1);
        assert_eq!(h.pending(&f, ALL), SEAT, "a generation change");
        h.told(&f, INPUT | GPU);
        assert_eq!(h.pending(&f, ALL), SEAT, "telling other classes does not tell the seat");
    }

    #[test]
    fn a_queue_told_stays_quiet_until_a_take_then_speaks_on_refill() {
        let mut f = facts();
        let mut h = settled(&f);
        f.ready[0] = true;
        assert_eq!(h.pending(&f, ALL), INPUT);
        h.told(&f, INPUT);
        assert_eq!(h.pending(&f, ALL), 0, "more input before the take is not news again");
        // The take empties it; nothing is news while it is empty.
        f.ready[0] = false;
        f.takes[0] += 1;
        assert_eq!(h.pending(&f, ALL), 0);
        // A refill after the take is news.
        f.ready[0] = true;
        assert_eq!(h.pending(&f, ALL), INPUT);
    }

    #[test]
    fn a_take_that_leaves_the_queue_ready_is_news_at_once() {
        // Events queued between the reply and the take stay behind it.
        let mut f = facts();
        let mut h = settled(&f);
        f.ready[COMPLETIONS] = true;
        h.told(&f, GPU);
        f.takes[COMPLETIONS] += 1;
        assert_eq!(h.pending(&f, ALL), GPU);
    }

    #[test]
    fn an_undrained_queue_never_spins_a_reader() {
        // Device 2 is never taken: told once, then silent, while the others work.
        let mut f = facts();
        let mut h = settled(&f);
        f.ready[2] = true;
        assert_eq!(h.pending(&f, ALL), INPUT);
        h.told(&f, INPUT);
        for _ in 0..4 {
            f.ready[0] = true;
            assert_eq!(h.pending(&f, ALL), INPUT, "device 0 arrives");
            h.told(&f, INPUT);
            f.takes[0] += 1;
            f.takes[1] += 1;
            f.ready[0] = false;
            assert_eq!(h.pending(&f, ALL), 0, "device 2's backlog is not news again");
        }
    }

    #[test]
    fn telling_an_empty_queue_does_not_silence_its_next_arrival() {
        let mut f = facts();
        let mut h = Heard::default();
        f.ready[0] = true;
        h.told(&f, ALL);
        // Queue 1 was empty when INPUT was told; it fills with no take between.
        f.ready[1] = true;
        assert_eq!(h.pending(&f, ALL), INPUT);
    }

    #[test]
    fn the_engine_is_gpu_news_on_any_change_of_state_or_stalls() {
        let mut f = facts();
        let mut h = settled(&f);
        assert_eq!(h.pending(&f, ALL), 0);
        f.engine = (Engine::Stalled, 1);
        assert_eq!(h.pending(&f, ALL), GPU, "a stall began");
        assert_eq!(h.pending(&f, SEAT | INPUT), 0, "not news for a reader without GPU interest");
        h.told(&f, GPU);
        assert_eq!(h.pending(&f, ALL), 0);
        f.engine = (Engine::Live, 1);
        assert_eq!(h.pending(&f, ALL), GPU, "it ended");
        h.told(&f, GPU);
        f.engine = (Engine::Live, 2);
        assert_eq!(h.pending(&f, ALL), GPU, "a stall begun and ended between passes still moves the count");
    }

    #[test]
    fn the_display_is_gpu_news_on_an_event_or_a_blank_restore() {
        let mut f = facts();
        let mut h = settled(&f);
        assert_eq!(h.pending(&f, ALL), 0);
        f.display = (1, 0);
        assert_eq!(h.pending(&f, ALL), GPU, "the host's display changed");
        assert_eq!(h.pending(&f, SEAT | INPUT), 0, "not news for a reader without GPU interest");
        h.told(&f, GPU);
        assert_eq!(h.pending(&f, ALL), 0, "told, it is quiet");
        f.display = (1, 1);
        assert_eq!(h.pending(&f, ALL), GPU, "a restore ended blank");
        f.engine = (Engine::Stalled, 1);
        h.told(&f, GPU);
        assert_eq!(h.pending(&f, ALL), 0, "telling GPU tells the engine and the display as they stand");
        f.display = (2, 1);
        assert_eq!(h.pending(&f, ALL), GPU, "and a later event is news again");
    }

    #[test]
    fn no_queue_is_news_in_an_episode_and_each_is_news_after_it() {
        let mut f = Facts { seat: (2, 2), ..Facts::default() };
        f.ready[VINDICATIONS] = true;
        f.ready[0] = true;
        let mut h = settled(&f);
        assert_eq!(h.pending(&f, ALL), 0, "a fence retired mid-episode is not news yet");
        // The engine moving mid-episode is news (SeatState's Stats say as much),
        // and telling it must not mark the withheld queues heard.
        f.engine = (Engine::Stalled, 1);
        assert_eq!(h.pending(&f, ALL), GPU);
        h.told(&f, GPU);
        f.display = (1, 0);
        assert_eq!(h.pending(&f, ALL), GPU, "so is the display moving mid-episode");
        h.told(&f, GPU);
        f.seat = (2, 0);
        assert_eq!(h.pending(&f, ALL), SEAT | INPUT | GPU);
    }

    #[test]
    fn interest_names_a_class_and_refuses_everything_else() {
        assert_eq!(interest(0), None);
        assert_eq!(interest(8), None);
        assert_eq!(interest(1 << 40), None);
        assert_eq!(interest(SEAT as u64), Some(SEAT));
        assert_eq!(interest(ALL as u64), Some(ALL));
    }
}
