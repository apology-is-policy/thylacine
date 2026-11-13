// The pointer devices' streams, read into what the compositor does with them.
//
// A device speaks in frames: the axes it moved, then EV_SYN. The tablet's
// axes are places (device units; the bin scales them to the display), the
// mouse's are motion, and either moves the pointer once a frame. A button or
// the wheel acts at once, at the place the frame has reached, so a click
// batched with its move lands where it was made. SYN_DROPPED is the stream
// saying it lost events (the broker's word when a client's undrained batch
// overflows), and a release may have been among them.
//
// The tablet keeps each axis as the device last gave it. QEMU's virtio
// tablet sends both in every frame: each front end queues X and Y, the device
// forwards each, and a frame reaches the guest whole or not at all. A frame
// cut short (a ring boundary, a drop, an episode) leaves at most one place
// half old, and the device's next frame replaces it. virtio-input cannot read
// the axes back (its ABS_INFO is the range alone), so a device that sent
// only the axis that changed would need another answer.
//
// Pure: the bin's pass hands each drain here, and the host tests run it.

/// evdev's codes as the broker passes them; the bin asserts each is the
/// broker's own.
pub const EV_SYN: u16 = 0;
pub const EV_KEY: u16 = 1;
pub const EV_REL: u16 = 2;
pub const EV_ABS: u16 = 3;
pub const SYN_DROPPED: u16 = 3;
pub const ABS_X: u16 = 0;
pub const ABS_Y: u16 = 1;
pub const REL_X: u16 = 0;
pub const REL_Y: u16 = 1;
pub const REL_WHEEL: u16 = 8;
pub const BTN_LEFT: u16 = 0x110;

/// What a stream asks of the compositor, in its order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ptr {
    /// The tablet's place, in device units.
    At(u32, u32),
    /// The mouse's motion over a frame.
    By(i32, i32),
    /// A button, down (true) or up.
    Button(u16, bool),
    /// The wheel.
    Scroll(i32),
    /// The stream lost events: every button still held is let go.
    Dropped,
}

/// The tablet's axes as the device last gave each.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Tablet {
    pub x: u32,
    pub y: u32,
}

impl Tablet {
    pub const fn new() -> Self {
        Tablet { x: 0, y: 0 }
    }

    /// One drain's events, as (type, code, value).
    pub fn read<I, F>(&mut self, events: I, mut act: F)
    where
        I: IntoIterator<Item = (u16, u16, u32)>,
        F: FnMut(Ptr),
    {
        let mut moved = false;
        for (etype, code, value) in events {
            match etype {
                EV_ABS if code == ABS_X => {
                    self.x = value;
                    moved = true;
                }
                EV_ABS if code == ABS_Y => {
                    self.y = value;
                    moved = true;
                }
                EV_SYN => {
                    self.commit(&mut moved, &mut act);
                    if code == SYN_DROPPED {
                        act(Ptr::Dropped);
                    }
                }
                EV_KEY if code >= BTN_LEFT => {
                    self.commit(&mut moved, &mut act);
                    act(Ptr::Button(code, value != 0));
                }
                EV_REL if code == REL_WHEEL => {
                    self.commit(&mut moved, &mut act);
                    act(Ptr::Scroll(value as i32));
                }
                _ => {}
            }
        }
        // A frame the ring split from its EV_SYN still moves the pointer; the
        // next drain's EV_SYN then has nothing to move.
        self.commit(&mut moved, &mut act);
    }

    fn commit<F: FnMut(Ptr)>(&self, moved: &mut bool, act: &mut F) {
        if core::mem::take(moved) {
            act(Ptr::At(self.x, self.y));
        }
    }
}

/// One drain of the mouse's events, as (type, code, value). REL values are
/// signed on the wire.
pub fn mouse<I, F>(events: I, mut act: F)
where
    I: IntoIterator<Item = (u16, u16, u32)>,
    F: FnMut(Ptr),
{
    fn commit<F: FnMut(Ptr)>(dx: &mut i32, dy: &mut i32, act: &mut F) {
        if *dx != 0 || *dy != 0 {
            act(Ptr::By(core::mem::take(dx), core::mem::take(dy)));
        }
    }
    let (mut dx, mut dy) = (0i32, 0i32);
    for (etype, code, value) in events {
        match etype {
            EV_REL if code == REL_X => dx = dx.saturating_add(value as i32),
            EV_REL if code == REL_Y => dy = dy.saturating_add(value as i32),
            EV_SYN => {
                commit(&mut dx, &mut dy, &mut act);
                if code == SYN_DROPPED {
                    act(Ptr::Dropped);
                }
            }
            EV_KEY if code >= BTN_LEFT => {
                commit(&mut dx, &mut dy, &mut act);
                act(Ptr::Button(code, value != 0));
            }
            EV_REL if code == REL_WHEEL => {
                commit(&mut dx, &mut dy, &mut act);
                act(Ptr::Scroll(value as i32));
            }
            _ => {}
        }
    }
    commit(&mut dx, &mut dy, &mut act);
}

/// A tablet axis on a display span: device units 0..=max onto 0..span.
pub fn place(value: u32, max: u32, span: u32) -> u32 {
    (value.min(max) as u64 * span.saturating_sub(1) as u64 / max.max(1) as u64) as u32
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::Ptr::{At, Button, By, Dropped, Scroll};
    use super::*;
    use std::vec::Vec;

    const SYN: (u16, u16, u32) = (EV_SYN, 0, 0);
    const DROP: (u16, u16, u32) = (EV_SYN, SYN_DROPPED, 0);
    const BTN_RIGHT: u16 = BTN_LEFT + 1;

    fn tablet(t: &mut Tablet, events: &[(u16, u16, u32)]) -> Vec<Ptr> {
        let mut out = Vec::new();
        t.read(events.iter().copied(), |p| out.push(p));
        out
    }

    fn mouse_of(events: &[(u16, u16, u32)]) -> Vec<Ptr> {
        let mut out = Vec::new();
        mouse(events.iter().copied(), |p| out.push(p));
        out
    }

    #[test]
    fn a_stream_that_lost_events_lets_every_button_go_after_the_frame_it_ends() {
        let mut t = Tablet::new();
        let got = tablet(&mut t, &[
            (EV_ABS, ABS_X, 10), (EV_ABS, ABS_Y, 20), SYN,
            (EV_KEY, BTN_LEFT, 1), SYN,
            (EV_ABS, ABS_X, 30), DROP,
        ]);
        assert_eq!(got, [At(10, 20), Button(BTN_LEFT, true), At(30, 20), Dropped]);
        let got = mouse_of(&[(EV_KEY, BTN_RIGHT, 1), SYN, (EV_REL, REL_X, 5), DROP]);
        assert_eq!(got, [Button(BTN_RIGHT, true), By(5, 0), Dropped]);
        // A frame's ordinary end is no loss.
        assert_eq!(tablet(&mut t, &[(EV_ABS, ABS_Y, 1), SYN]), [At(30, 1)]);
        assert_eq!(mouse_of(&[(EV_REL, REL_Y, 1), SYN]), [By(0, 1)]);
    }

    #[test]
    fn a_frame_moves_the_pointer_once_and_a_click_lands_where_it_was_made() {
        let mut t = Tablet::new();
        let up = (-1i32) as u32;
        assert_eq!(tablet(&mut t, &[(EV_ABS, ABS_X, 1), (EV_ABS, ABS_Y, 2), (EV_ABS, ABS_X, 3), SYN]), [At(3, 2)]);
        assert_eq!(
            tablet(&mut t, &[(EV_ABS, ABS_X, 4), (EV_KEY, BTN_LEFT, 1), (EV_ABS, ABS_Y, 5), (EV_KEY, BTN_LEFT, 0), SYN]),
            [At(4, 2), Button(BTN_LEFT, true), At(4, 5), Button(BTN_LEFT, false)]
        );
        assert_eq!(tablet(&mut t, &[(EV_ABS, ABS_Y, 6), (EV_REL, REL_WHEEL, up), SYN]), [At(4, 6), Scroll(-1)]);
        assert_eq!(
            mouse_of(&[(EV_REL, REL_X, 2), (EV_KEY, BTN_LEFT, 1), (EV_REL, REL_Y, 3), (EV_REL, REL_WHEEL, up), SYN]),
            [By(2, 0), Button(BTN_LEFT, true), By(0, 3), Scroll(-1)]
        );
        // A key is not a button, and a frame with nothing in it moves nothing.
        assert_eq!(tablet(&mut t, &[(EV_KEY, 30, 1), SYN, SYN]), []);
        assert_eq!(mouse_of(&[(EV_KEY, 30, 1), SYN]), []);
    }

    #[test]
    fn a_frame_split_from_its_end_still_moves_the_pointer() {
        let mut t = Tablet::new();
        assert_eq!(tablet(&mut t, &[(EV_ABS, ABS_X, 7)]), [At(7, 0)]);
        assert_eq!(tablet(&mut t, &[SYN]), []);
        assert_eq!(mouse_of(&[(EV_REL, REL_X, 7)]), [By(7, 0)]);
    }

    #[test]
    fn an_axis_the_device_did_not_send_keeps_its_last_value() {
        let mut t = Tablet::new();
        assert_eq!(tablet(&mut t, &[(EV_ABS, ABS_X, 1), (EV_ABS, ABS_Y, 2), SYN]), [At(1, 2)]);
        assert_eq!(tablet(&mut t, &[(EV_ABS, ABS_X, 9), SYN]), [At(9, 2)]);
        assert_eq!(t, Tablet { x: 9, y: 2 });
    }

    #[test]
    fn the_mouse_adds_a_frames_signed_motion_and_saturates() {
        let minus = |v: i32| v as u32;
        assert_eq!(mouse_of(&[(EV_REL, REL_X, 3), (EV_REL, REL_X, minus(-5)), (EV_REL, REL_Y, 2), SYN]), [By(-2, 2)]);
        assert_eq!(mouse_of(&[(EV_REL, REL_X, i32::MAX as u32), (EV_REL, REL_X, 1), SYN]), [By(i32::MAX, 0)]);
        // Motion that comes to nothing over its frame moves nothing.
        assert_eq!(mouse_of(&[(EV_REL, REL_X, 1), (EV_REL, REL_X, minus(-1)), SYN]), []);
    }

    #[test]
    fn a_place_spans_the_display_and_survives_an_empty_range() {
        assert_eq!(place(0, 32767, 1280), 0);
        assert_eq!(place(32767, 32767, 1280), 1279);
        assert_eq!(place(40000, 32767, 1280), 1279);
        assert_eq!(place(16384, 32767, 1281), 640);
        assert_eq!(place(5, 0, 1280), 0);
        assert_eq!(place(5, 10, 0), 0);
    }
}
