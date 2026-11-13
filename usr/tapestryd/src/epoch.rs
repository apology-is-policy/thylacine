// The seat as the compositor's pass reads it.
//
// The seat's owner takes the display and the input for a trusted-path
// episode and gives them back. What was held, pressed or pointed at before an
// episode is not so after it, and what the pointer did across one is no
// program's, so the first thing a pass does on the seat's return is put that
// state down (`Comp::seat_resumed`).
//
// A reading is the pair the broker answers: the generation, which the kernel
// moves once for an episode, and the phase, 0 while the seat is the
// compositor's. The broker answers it alone (`SeatState`) and with every
// drain of an input device, in the reply that carries the events and from
// the one thread that gathers them, so a drain's events are of the seat its
// reading tells: the broker gathers none while the phase is not 0 and lets go
// of what was not drained when an episode begins. A pass therefore deals with
// a drain's reading before it hands that drain's events on, and no episode
// fits between the two. Asking the seat and then draining is two requests,
// and a whole episode fits between two requests of a thread that is kept off
// the processor.
//
// Pure: the bin's pass calls it, and the host tests run it.

/// What a reading of the seat asks of the pass that took it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reading {
    /// The seat is as the pass last dealt with it.
    Standing,
    /// An episode has ended that the pass has not put its state down for.
    /// Told once for an episode.
    Resumed,
    /// An episode is on.
    Episode,
}

/// The generation the pass last put its state down for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SeatEpoch(u64);

impl SeatEpoch {
    pub const fn new() -> Self {
        SeatEpoch(0)
    }

    /// The broker answered `generation` and `phase`. An episode that is on
    /// is not yet one that has ended: its generation is taken only by the
    /// reading that finds the seat back.
    pub fn read(&mut self, generation: u64, phase: u32) -> Reading {
        if phase != 0 {
            Reading::Episode
        } else if generation != self.0 {
            self.0 = generation;
            Reading::Resumed
        } else {
            Reading::Standing
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_seat_that_stands_is_read_as_standing() {
        let mut epoch = SeatEpoch::new();
        // The first reading of a boot whose seat has changed hands before
        // the compositor started is a return like any other.
        assert_eq!(epoch.read(3, 0), Reading::Resumed);
        for _ in 0..4 {
            assert_eq!(epoch.read(3, 0), Reading::Standing);
        }
        // A boot whose seat never changed hands has nothing to put down.
        assert_eq!(SeatEpoch::new().read(0, 0), Reading::Standing);
    }

    #[test]
    fn an_episode_is_told_while_it_is_on_and_its_end_once() {
        let mut epoch = SeatEpoch::new();
        assert_eq!(epoch.read(1, 0), Reading::Resumed);
        // Every phase that is not 0 is an episode: beginning, exclusive,
        // restoring, failed.
        for phase in [1, 2, 3, 4] {
            assert_eq!(epoch.read(2, phase), Reading::Episode);
        }
        // Reading it while it was on did not deal with it.
        assert_eq!(epoch.read(2, 0), Reading::Resumed);
        assert_eq!(epoch.read(2, 0), Reading::Standing);
    }

    #[test]
    fn an_episode_that_began_and_ended_between_two_readings_is_told() {
        let mut epoch = SeatEpoch::new();
        assert_eq!(epoch.read(7, 0), Reading::Resumed);
        assert_eq!(epoch.read(7, 0), Reading::Standing);
        // Nothing was read while it was on: the generation alone says so.
        assert_eq!(epoch.read(8, 0), Reading::Resumed);
        assert_eq!(epoch.read(8, 0), Reading::Standing);
        // Two of them between two readings are one return.
        assert_eq!(epoch.read(10, 0), Reading::Resumed);
        assert_eq!(epoch.read(10, 0), Reading::Standing);
    }

    #[test]
    fn a_phase_that_comes_back_with_no_new_generation_is_no_return() {
        // A chord the kernel refused: the broker reads a failed phase for a
        // pass and the generation does not move. The keyboard's stream says
        // what was lost (SYN_DROPPED); the seat did not change hands.
        let mut epoch = SeatEpoch::new();
        assert_eq!(epoch.read(5, 0), Reading::Resumed);
        assert_eq!(epoch.read(5, 4), Reading::Episode);
        assert_eq!(epoch.read(5, 0), Reading::Standing);
    }
}
