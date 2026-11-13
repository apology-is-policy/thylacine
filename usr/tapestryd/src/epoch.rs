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
// What a drain brings while the phase is not 0 is handed to no program. That
// can be the stream's own word for a loss (SYN_DROPPED, after a chord the
// kernel refused, which moves no generation), so the reading that finds
// phase 0 again is a return whether or not the generation moved: the state
// is put down on what the pass did with the events, not on an argument about
// when the broker can be read at such a phase.
//
// Pure: the bin's pass calls it, and the host tests run it.

/// What a reading of the seat asks of the pass that took it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reading {
    /// The seat is as the pass last dealt with it.
    Standing,
    /// The seat is the compositor's again and the pass has not put its state
    /// down for it: an episode ended, or a phase that was not 0 was read.
    /// Told once.
    Resumed,
    /// An episode is on.
    Episode,
}

/// The generation the pass last put its state down for, and whether a phase
/// that was not 0 has been read since.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SeatEpoch {
    generation: u64,
    away: bool,
}

impl SeatEpoch {
    pub const fn new() -> Self {
        SeatEpoch { generation: 0, away: false }
    }

    /// The broker answered `generation` and `phase`. An episode that is on
    /// is not yet one that has ended: its generation is taken only by the
    /// reading that finds the seat back.
    pub fn read(&mut self, generation: u64, phase: u32) -> Reading {
        if phase != 0 {
            self.away = true;
            Reading::Episode
        } else if generation != self.generation || self.away {
            self.generation = generation;
            self.away = false;
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
        // A generation is a name, not a count: one that differs is another.
        assert_eq!(epoch.read(4, 0), Reading::Resumed);
        assert_eq!(epoch.read(4, 0), Reading::Standing);
    }

    #[test]
    fn a_phase_that_comes_back_with_no_new_generation_is_a_return_too() {
        // A chord the kernel refused moves no generation, and the stream's
        // word for what was lost (SYN_DROPPED) is in the reply the broker
        // gives next. Read at a phase that is not 0, that reply is handed to
        // no program, so the word goes with it: the state is put down by the
        // reading that finds phase 0 again.
        let mut epoch = SeatEpoch::new();
        assert_eq!(epoch.read(5, 0), Reading::Resumed);
        assert_eq!(epoch.read(5, 4), Reading::Episode);
        assert_eq!(epoch.read(5, 0), Reading::Resumed);
        assert_eq!(epoch.read(5, 0), Reading::Standing);
        // Once for it, however many readings found the phase away.
        for phase in [4, 4, 1] {
            assert_eq!(epoch.read(5, phase), Reading::Episode);
        }
        assert_eq!(epoch.read(5, 0), Reading::Resumed);
        assert_eq!(epoch.read(5, 0), Reading::Standing);
        // A generation that moved while the phase was away is the same one
        // return, not two.
        assert_eq!(epoch.read(6, 2), Reading::Episode);
        assert_eq!(epoch.read(6, 0), Reading::Resumed);
        assert_eq!(epoch.read(6, 0), Reading::Standing);
    }
}
