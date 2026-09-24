//! The key -> action map and the navigation, both pure.
//!
//! Keys arrive already decoded by `kaua::input::Parser` -- the tree's VT input
//! parser, the one nora and prowl read their keys through. lantern adds no
//! second decoder; it only says what a decoded key MEANS to a deck.

use kaua::event::{KeyCode, KeyEvent, Mods};

/// What a keystroke asks for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Next,
    Prev,
    First,
    Last,
    /// Jump to a 0-based slide index (typed as the 1-based digit).
    Goto(usize),
    /// Paint the current slide again.
    Redraw,
    Quit,
    /// A key a deck has no meaning for. Deliberately the default: a presenter
    /// leaning on the keyboard must not lose the talk.
    Ignore,
}

/// The action a key asks for.
///
/// Quit is `q`, Ctrl-C and Ctrl-D -- and nothing else. Escape is NOT a quit on
/// purpose: it is the first byte of every arrow and function key, so resolving
/// a lone one needs a timing holdoff (`kaua::input::Parser::pending_escape`),
/// and a presenter whose deck vanishes because a holdoff guessed wrong is a
/// worse failure than one who has to press `q`.
///
/// Ctrl-C is spelled out because the raw-mode dance sets `-isig`: the byte
/// reaches the program instead of being cooked into an `interrupt` note, so
/// without this arm the habitual way to leave a terminal program would do
/// nothing at all.
pub fn action_for(k: KeyEvent) -> Action {
    if k.mods == Mods::CTRL {
        return match k.code {
            KeyCode::Char('c') | KeyCode::Char('d') => Action::Quit,
            KeyCode::Char('l') => Action::Redraw,
            _ => Action::Ignore,
        };
    }
    if k.mods != Mods::NONE && k.mods != Mods::SHIFT {
        return Action::Ignore;
    }
    match k.code {
        KeyCode::Char(' ')
        | KeyCode::Right
        | KeyCode::Down
        | KeyCode::PageDown
        | KeyCode::Enter
        | KeyCode::Char('n')
        | KeyCode::Char('j')
        | KeyCode::Char('l') => Action::Next,

        KeyCode::Left
        | KeyCode::Up
        | KeyCode::PageUp
        | KeyCode::Backspace
        | KeyCode::Char('p')
        | KeyCode::Char('k')
        | KeyCode::Char('h') => Action::Prev,

        KeyCode::Home | KeyCode::Char('g') => Action::First,
        KeyCode::End | KeyCode::Char('G') => Action::Last,

        KeyCode::Char('q') => Action::Quit,

        KeyCode::Char(c @ '1'..='9') => Action::Goto(c as usize - '1' as usize),

        _ => Action::Ignore,
    }
}

impl Action {
    /// The slide this action moves to, from slide `at` of `n`, or `None` when
    /// the view does not change.
    ///
    /// `Next` on the last slide and `Prev` on the first STAY. Neither wraps and
    /// neither exits: a deck's end is where a talk pauses for questions, and
    /// advancing off it should not clear the screen or quit. `Goto` past the
    /// end is refused rather than clamped -- a `7` in a six-slide deck is a
    /// mistype, and landing on the last slide would hide it.
    pub fn target(self, at: usize, n: usize) -> Option<usize> {
        if n == 0 {
            return None;
        }
        let to = match self {
            Action::Next => at.saturating_add(1).min(n - 1),
            Action::Prev => at.saturating_sub(1),
            Action::First => 0,
            Action::Last => n - 1,
            Action::Goto(i) if i < n => i,
            Action::Goto(_) | Action::Redraw | Action::Quit | Action::Ignore => return None,
        };
        if to == at {
            None
        } else {
            Some(to)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ch(c: char) -> KeyEvent {
        KeyEvent::new(KeyCode::Char(c))
    }

    #[test]
    fn the_advance_keys_all_advance() {
        for k in [
            ch(' '),
            ch('n'),
            ch('j'),
            ch('l'),
            KeyEvent::new(KeyCode::Right),
            KeyEvent::new(KeyCode::Down),
            KeyEvent::new(KeyCode::PageDown),
            KeyEvent::new(KeyCode::Enter),
        ] {
            assert_eq!(action_for(k), Action::Next, "{:?}", k);
        }
    }

    #[test]
    fn the_retreat_keys_all_retreat() {
        for k in [
            ch('p'),
            ch('k'),
            ch('h'),
            KeyEvent::new(KeyCode::Left),
            KeyEvent::new(KeyCode::Up),
            KeyEvent::new(KeyCode::PageUp),
            KeyEvent::new(KeyCode::Backspace),
        ] {
            assert_eq!(action_for(k), Action::Prev, "{:?}", k);
        }
    }

    #[test]
    fn quit_is_q_and_the_two_controls_and_nothing_else() {
        assert_eq!(action_for(ch('q')), Action::Quit);
        assert_eq!(
            action_for(KeyEvent::with(KeyCode::Char('c'), Mods::CTRL)),
            Action::Quit
        );
        assert_eq!(
            action_for(KeyEvent::with(KeyCode::Char('d'), Mods::CTRL)),
            Action::Quit
        );
        // Escape must NOT quit: it is every arrow key's first byte.
        assert_eq!(action_for(KeyEvent::new(KeyCode::Esc)), Action::Ignore);
        // Nor any other stray key.
        for k in [ch('x'), ch('Z'), ch('\t'), KeyEvent::new(KeyCode::F(1))] {
            assert_ne!(action_for(k), Action::Quit, "{:?}", k);
        }
    }

    #[test]
    fn ctrl_l_redraws_and_plain_l_advances() {
        assert_eq!(
            action_for(KeyEvent::with(KeyCode::Char('l'), Mods::CTRL)),
            Action::Redraw
        );
        assert_eq!(action_for(ch('l')), Action::Next);
    }

    #[test]
    fn the_ends_do_not_wrap_and_do_not_quit() {
        // Last slide of six, advancing.
        assert_eq!(Action::Next.target(5, 6), None);
        // First slide, retreating.
        assert_eq!(Action::Prev.target(0, 6), None);
        // And the ordinary moves still move.
        assert_eq!(Action::Next.target(0, 6), Some(1));
        assert_eq!(Action::Prev.target(5, 6), Some(4));
    }

    #[test]
    fn first_and_last_land_and_report_no_change_when_already_there() {
        assert_eq!(Action::First.target(3, 6), Some(0));
        assert_eq!(Action::Last.target(3, 6), Some(5));
        assert_eq!(Action::First.target(0, 6), None);
        assert_eq!(Action::Last.target(5, 6), None);
    }

    #[test]
    fn a_digit_jumps_one_based_and_past_the_end_is_refused() {
        assert_eq!(action_for(ch('1')), Action::Goto(0));
        assert_eq!(action_for(ch('9')), Action::Goto(8));
        // '0' is not a slide: there is no slide zero to a presenter.
        assert_eq!(action_for(ch('0')), Action::Ignore);
        assert_eq!(Action::Goto(2).target(0, 6), Some(2));
        // Past the end: refused, NOT clamped to the last slide, so a mistype
        // is visible instead of moving the talk somewhere plausible.
        assert_eq!(Action::Goto(6).target(0, 6), None);
        assert_eq!(Action::Goto(99).target(0, 6), None);
    }

    #[test]
    fn a_one_slide_deck_never_moves_and_an_empty_one_is_inert() {
        assert_eq!(Action::Next.target(0, 1), None);
        assert_eq!(Action::Prev.target(0, 1), None);
        assert_eq!(Action::Last.target(0, 1), None);
        for a in [Action::Next, Action::Prev, Action::First, Action::Last] {
            assert_eq!(a.target(0, 0), None, "{:?} on an empty deck", a);
        }
    }

    #[test]
    fn a_modified_navigation_key_is_ignored() {
        // Alt-Right is the compositor's or the terminal's business, not a
        // deck's; only CTRL has meanings here, and SHIFT rides 'G'.
        assert_eq!(
            action_for(KeyEvent::with(KeyCode::Right, Mods::ALT)),
            Action::Ignore
        );
        assert_eq!(
            action_for(KeyEvent::with(KeyCode::Char('G'), Mods::SHIFT)),
            Action::Last
        );
    }
}
