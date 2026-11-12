// The motion made up for a device that only says where the pointer is.
//
// A relative device (the mouse) says how far the pointer went, and that is
// handed on as it is. An absolute one (the tablet) says a position, so its
// motion is the difference between two of its positions that follow each
// other; a position with none before it is only a place.
//
// "Follow each other" is the whole rule. One pointer can be moved through
// both devices: a frontend that captures the pointer sends its motion through
// the mouse, and when it lets go it sends positions again, from where the
// mouse left the pointer. Counted from the tablet's position before the
// capture, the first of those would hand a program all of the captured motion
// a second time. So motion from the relative device ends the run, and the
// next position is a place again. A stylus and a mouse used together lose one
// report's worth of the stylus's motion at each turn, which is the cheaper
// error by the width of the screen.
//
// Pure: the bin's drains call it, and the host tests run it.

/// Where the absolute device last said the pointer was, while nothing else
/// has moved the pointer since.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AbsBase(Option<(u32, u32)>);

impl AbsBase {
    pub const fn new() -> Self {
        AbsBase(None)
    }

    /// The absolute device says the pointer is at `(x, y)`: the motion that
    /// makes, if it makes any.
    pub fn at(&mut self, x: u32, y: u32) -> Option<(i32, i32)> {
        let from = self.0.replace((x, y))?;
        let d = (span(from.0, x), span(from.1, y));
        (d != (0, 0)).then_some(d)
    }

    /// Something else moved the pointer.
    pub fn forget(&mut self) {
        self.0 = None;
    }
}

fn span(from: u32, to: u32) -> i32 {
    (to as i64 - from as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_first_position_is_a_place_and_each_one_after_it_its_own_motion() {
        let mut base = AbsBase::new();
        assert_eq!(base.at(639, 399), None);
        assert_eq!(base.at(799, 399), Some((160, 0)));
        // The same position again is no motion.
        assert_eq!(base.at(799, 399), None);
        assert_eq!(base.at(790, 410), Some((-9, 11)));
    }

    #[test]
    fn a_position_after_motion_from_the_mouse_is_a_place_again() {
        let mut base = AbsBase::new();
        assert_eq!(base.at(639, 399), None);
        assert_eq!(base.at(799, 399), Some((160, 0)));
        // The mouse moves the pointer, and the tablet then says where that
        // left it. Counted from 799 this would be -640.
        base.forget();
        assert_eq!(base.at(159, 399), None);
        // The position after it makes exactly its own motion.
        assert_eq!(base.at(319, 399), Some((160, 0)));
    }

    #[test]
    fn the_widest_motion_there_is_does_not_overflow() {
        let mut base = AbsBase::new();
        assert_eq!(base.at(0, u32::MAX), None);
        assert_eq!(base.at(u32::MAX, 0), Some((i32::MAX, i32::MIN)));
    }
}
