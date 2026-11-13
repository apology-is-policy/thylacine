// The stall lever's display half (`thylacine.test-latch-dead`), as state.
//
// A display event for a host of one of three heights aims one shot at a step
// of the mode change it leads to: the change's first bind, the bind of the
// switch to direct that follows it, or the EDID query it ends with. The step
// takes the shot just before it runs. One shot a size, so the asking that
// follows a stall is not stalled too; and a shot never outlives the display
// it was aimed by, so one the change did not reach is spent on nothing.
//
// Pure: the compositor holds one under test-mode, and the host tests run it.

pub const MODE_H: u32 = 797;
pub const SWITCH_H: u32 = 787;
pub const EDID_H: u32 = 1277;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    Mode,
    Switch,
    Edid,
}

#[derive(Debug)]
pub struct Aim {
    step: Option<Step>,
    host: (u32, u32),
}

impl Aim {
    pub const fn new() -> Self {
        Aim { step: None, host: (0, 0) }
    }

    /// A mode change for the display of `host` begins, under the armed
    /// latch or not.
    pub fn aim(&mut self, host: (u32, u32), armed: bool) {
        // A shot aimed for the display that was, which found no step.
        self.step = None;
        if !armed || self.host == host {
            return;
        }
        self.step = match host.1 {
            MODE_H => Some(Step::Mode),
            SWITCH_H => Some(Step::Switch),
            EDID_H => Some(Step::Edid),
            _ => return,
        };
        self.host = host;
    }

    /// The host's display was asked, and is `host`.
    pub fn followed(&mut self, host: (u32, u32)) {
        if host != self.host {
            self.step = None;
        }
    }

    /// The mode change ended. One that failed is followed by no switch and no
    /// EDID, and a shot its bind did not take is not carried to a later bind.
    pub fn changed(&mut self, ok: bool) {
        if !ok || self.step == Some(Step::Mode) {
            self.step = None;
        }
    }

    /// Whether `step`, about to run, takes the shot; taking it spends it.
    pub fn take(&mut self, step: Step) -> bool {
        if self.step != Some(step) {
            return false;
        }
        self.step = None;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_shot_aimed_by_one_display_is_not_taken_at_another() {
        for (h, step) in [(SWITCH_H, Step::Switch), (EDID_H, Step::Edid)] {
            let mut a = Aim::new();
            a.aim((1920, h), true);
            a.changed(true);
            a.followed((1024, 768));
            assert!(!a.take(step), "a shot for {} survived another display", h);
            // The same display asked again keeps it.
            let mut a = Aim::new();
            a.aim((1920, h), true);
            a.changed(true);
            a.followed((1920, h));
            assert!(a.take(step));
        }
    }

    #[test]
    fn one_shot_a_size() {
        let mut a = Aim::new();
        a.aim((1280, MODE_H), true);
        assert!(a.take(Step::Mode));
        a.aim((1280, MODE_H), true);
        assert!(!a.take(Step::Mode));
        // A display of no lever height between the two is not a new size.
        a.aim((1280, 600), true);
        a.aim((1280, MODE_H), true);
        assert!(!a.take(Step::Mode));
        a.aim((1281, MODE_H), true);
        assert!(a.take(Step::Mode));
    }

    #[test]
    fn nothing_is_aimed_without_the_latch() {
        let mut a = Aim::new();
        a.aim((1280, MODE_H), false);
        assert!(!a.take(Step::Mode));
        // Nor is the size spent.
        a.aim((1280, MODE_H), true);
        assert!(a.take(Step::Mode));
    }

    #[test]
    fn a_change_spends_what_it_did_not_reach() {
        let mut a = Aim::new();
        a.aim((1, SWITCH_H), true);
        a.changed(false);
        assert!(!a.take(Step::Switch));
        a.aim((2, EDID_H), true);
        a.changed(false);
        assert!(!a.take(Step::Edid));
        a.aim((3, MODE_H), true);
        a.changed(true);
        assert!(!a.take(Step::Mode));
        a.aim((4, SWITCH_H), true);
        a.changed(true);
        assert!(a.take(Step::Switch));
    }

    #[test]
    fn the_shot_is_taken_by_its_own_step_once() {
        let mut a = Aim::new();
        a.aim((5, EDID_H), true);
        assert!(!a.take(Step::Mode));
        assert!(!a.take(Step::Switch));
        assert!(a.take(Step::Edid));
        assert!(!a.take(Step::Edid));
        // A new aiming spends a shot the display before left.
        a.aim((6, EDID_H), true);
        a.aim((7, 600), true);
        assert!(!a.take(Step::Edid));
    }
}
