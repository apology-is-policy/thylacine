// The runtime chord binding table (AURORA-CONFIG.md section 3.5, cfg-4).
//
// The Super chord PLANE (G-6c) is unchanged: while Super is held EVERY
// non-modifier key is compositor input and none reaches a surface -- that
// reservation is `super_held`, structurally INDEPENDENT of any binding, so
// an unbound (or just-rebound) key still swallows its own release. Only the
// (key, shift) -> ACTION mapping becomes data here: a table seeded with the
// stage-0 i3-flavored defaults, overridable by config `chord` lines the
// environment PUSHES through the gated global ctl (section 3.3). A live
// rebind therefore mutates only this table (`self.chords`) and never the
// swallow-set (`chord_down`), so no half key-pair can ever leak across a
// remap -- the cfg-4 prosecution obligation.

use crate::pane::{Dir, Mode};
use alloc::vec::Vec;

/// One dispatched chord action -- the layout operation a bound (key, shift)
/// performs. The flat vocabulary folds the old shift-modifies-the-action
/// cases into distinct (key, shift) table entries (Super+Left = FocusDir,
/// Super+Shift+Left = MoveDir), so the table is a pure lookup.
#[derive(Clone, Copy, PartialEq)]
pub enum ChordAction {
    FocusDir(Dir),
    MoveDir(Dir),
    Split(Mode),   // SplitH | SplitV
    SetMode(Mode), // Tabbed | Stacked
    Zoom,
    SplitToggle,
    TabCycle(bool), // true = forward
    Close,
}

#[derive(Clone, Copy)]
struct Bind {
    key: u16,
    shift: bool,
    action: ChordAction,
}

/// The v1.0 inter-pane gap cap: a leaf CONTENT inset (px). A gap wider than
/// this would shrink small panes to nothing; the default is 1 (the stage-0
/// 1px frame inset).
pub const GAPS_MAX: u32 = 32;

pub struct Chords {
    binds: Vec<Bind>, // at most one entry per (key, shift)
    pub gaps: u32,
}

// evdev key codes (linux/input-event-codes.h). Only the codes the default
// chord set + the config grammar name.
const KEY_TAB: u16 = 15;
const KEY_Q: u16 = 16;
const KEY_E: u16 = 18;
const KEY_T: u16 = 20;
const KEY_S: u16 = 31;
const KEY_F: u16 = 33;
const KEY_H: u16 = 35;
const KEY_V: u16 = 47;
const KEY_UP: u16 = 103;
const KEY_LEFT: u16 = 105;
const KEY_RIGHT: u16 = 106;
const KEY_DOWN: u16 = 108;

/// A key NAME (config grammar) -> its evdev code (US-QWERTY, matching
/// keymap.rs). The full letter set plus the arrows and tab -- any letter is
/// bindable to a chord; a name outside this set is rejected.
fn key_code(name: &str) -> Option<u16> {
    Some(match name {
        // Letters (evdev codes per linux/input-event-codes.h + keymap.rs).
        "a" => 30, "b" => 48, "c" => 46, "d" => 32, "e" => 18,
        "f" => 33, "g" => 34, "h" => 35, "i" => 23, "j" => 36,
        "k" => 37, "l" => 38, "m" => 50, "n" => 49, "o" => 24,
        "p" => 25, "q" => 16, "r" => 19, "s" => 31, "t" => 20,
        "u" => 22, "v" => 47, "w" => 17, "x" => 45, "y" => 21,
        "z" => 44,
        "tab" => KEY_TAB,
        "up" => KEY_UP,
        "left" => KEY_LEFT,
        "right" => KEY_RIGHT,
        "down" => KEY_DOWN,
        _ => return None,
    })
}

/// An action NAME (config grammar) -> the action, or `None` for the special
/// `none` unbind token (the caller removes the binding).
fn action_of(name: &str) -> Option<Option<ChordAction>> {
    Some(Some(match name {
        "focus-left" => ChordAction::FocusDir(Dir::Left),
        "focus-right" => ChordAction::FocusDir(Dir::Right),
        "focus-up" => ChordAction::FocusDir(Dir::Up),
        "focus-down" => ChordAction::FocusDir(Dir::Down),
        "move-left" => ChordAction::MoveDir(Dir::Left),
        "move-right" => ChordAction::MoveDir(Dir::Right),
        "move-up" => ChordAction::MoveDir(Dir::Up),
        "move-down" => ChordAction::MoveDir(Dir::Down),
        "split-h" => ChordAction::Split(Mode::SplitH),
        "split-v" => ChordAction::Split(Mode::SplitV),
        "split-toggle" => ChordAction::SplitToggle,
        "zoom" => ChordAction::Zoom,
        "tab" => ChordAction::SetMode(Mode::Tabbed),
        "stack" => ChordAction::SetMode(Mode::Stacked),
        "cycle" => ChordAction::TabCycle(true),
        "cycle-back" => ChordAction::TabCycle(false),
        "close" => ChordAction::Close,
        "none" => return Some(None), // the unbind token
        _ => return None,
    }))
}

impl Chords {
    /// The stage-0 default table (the pre-cfg-4 hardcoded `match`, as data).
    pub fn new() -> Chords {
        use ChordAction::*;
        let d = |key, shift, action| Bind { key, shift, action };
        Chords {
            binds: alloc::vec![
                d(KEY_LEFT, false, FocusDir(Dir::Left)),
                d(KEY_RIGHT, false, FocusDir(Dir::Right)),
                d(KEY_UP, false, FocusDir(Dir::Up)),
                d(KEY_DOWN, false, FocusDir(Dir::Down)),
                d(KEY_LEFT, true, MoveDir(Dir::Left)),
                d(KEY_RIGHT, true, MoveDir(Dir::Right)),
                d(KEY_UP, true, MoveDir(Dir::Up)),
                d(KEY_DOWN, true, MoveDir(Dir::Down)),
                d(KEY_H, false, Split(Mode::SplitH)),
                d(KEY_V, false, Split(Mode::SplitV)),
                d(KEY_F, false, Zoom),
                d(KEY_T, false, SetMode(Mode::Tabbed)),
                d(KEY_S, false, SetMode(Mode::Stacked)),
                d(KEY_E, false, SplitToggle),
                d(KEY_TAB, false, TabCycle(true)),
                d(KEY_TAB, true, TabCycle(false)),
                d(KEY_Q, true, Close),
            ],
            gaps: 1,
        }
    }

    /// Restore the defaults (the environment's reset-first push: a removed
    /// config line reverts to its default, never lingers).
    pub fn reset(&mut self) {
        *self = Chords::new();
    }

    /// The dispatch lookup: what action does this (key, shift) bind?
    pub fn lookup(&self, key: u16, shift: bool) -> Option<ChordAction> {
        self.binds
            .iter()
            .find(|b| b.key == key && b.shift == shift)
            .map(|b| b.action)
    }

    /// Apply one `chord <combo> <action>` binding. `combo` is
    /// `super+[shift+]<key>` (Super REQUIRED -- the plane modifier); `action`
    /// is a vocabulary name or `none` (unbind). Returns Err on any malformed
    /// token (the gated ctl caller maps it to E_INVAL); a well-formed rebind
    /// replaces any existing (key, shift) entry.
    pub fn bind(&mut self, combo: &str, action: &str) -> Result<(), ()> {
        let (key, shift) = parse_combo(combo)?;
        let act = action_of(action).ok_or(())?;
        // Drop any existing entry for this (key, shift) first.
        self.binds.retain(|b| !(b.key == key && b.shift == shift));
        if let Some(a) = act {
            self.binds.push(Bind { key, shift, action: a });
        }
        Ok(())
    }

    pub fn set_gaps(&mut self, px: u32) -> Result<(), ()> {
        if px > GAPS_MAX {
            return Err(());
        }
        self.gaps = px;
        Ok(())
    }
}

/// Parse `super+[shift+]<key>` -> (evdev code, shift). Super is mandatory
/// (the whole plane is Super-reserved, so a non-Super combo is meaningless);
/// only `shift` is an accepted extra modifier; the LAST token is the key.
fn parse_combo(combo: &str) -> Result<(u16, bool), ()> {
    let mut super_seen = false;
    let mut shift = false;
    let mut key: Option<u16> = None;
    for tok in combo.split('+') {
        if tok.is_empty() {
            return Err(());
        }
        match tok {
            "super" => super_seen = true,
            "shift" => shift = true,
            other => {
                // The key must be the final token: a token after the key
                // (a second key, or a modifier after the key) is malformed.
                if key.is_some() {
                    return Err(());
                }
                key = Some(key_code(other).ok_or(())?);
            }
        }
    }
    match (super_seen, key) {
        (true, Some(k)) => Ok((k, shift)),
        _ => Err(()),
    }
}

// DORMANT host-harness tests (the G-4f named seam: tapestryd is no_std +
// aarch64-asm, so `cargo test` cannot host-build it -- these document the
// grammar + the rebind/swallow-set invariant; the in-guest witness is
// ls-gfx-chords.exp).
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_match_the_stage0_vocabulary() {
        let c = Chords::new();
        // The i3-flavored defaults (the pre-cfg-4 hardcoded match).
        assert!(matches!(c.lookup(KEY_F, false), Some(ChordAction::Zoom)));
        assert!(matches!(c.lookup(KEY_LEFT, false), Some(ChordAction::FocusDir(Dir::Left))));
        assert!(matches!(c.lookup(KEY_LEFT, true), Some(ChordAction::MoveDir(Dir::Left))));
        assert!(matches!(c.lookup(KEY_H, false), Some(ChordAction::Split(Mode::SplitH))));
        assert!(matches!(c.lookup(KEY_TAB, false), Some(ChordAction::TabCycle(true))));
        assert!(matches!(c.lookup(KEY_TAB, true), Some(ChordAction::TabCycle(false))));
        assert!(matches!(c.lookup(KEY_Q, true), Some(ChordAction::Close)));
        // An unbound key (no default) -> plane-reserved, no action.
        assert!(c.lookup(34 /* g */, false).is_none());
        assert_eq!(c.gaps, 1);
    }

    #[test]
    fn rebind_add_replace_unbind() {
        let mut c = Chords::new();
        // Add a NEW binding on an unbound key.
        assert!(c.bind("super+g", "zoom").is_ok());
        assert!(matches!(c.lookup(34, false), Some(ChordAction::Zoom)));
        // Replace an existing (key, shift) with a different action.
        assert!(c.bind("super+g", "close").is_ok());
        assert!(matches!(c.lookup(34, false), Some(ChordAction::Close)));
        // Unbind: `none` removes the (key, shift) entry (drops to
        // plane-reserved, no action).
        assert!(c.bind("super+f", "none").is_ok());
        assert!(c.lookup(KEY_F, false).is_none());
        // shift is part of the (key, shift) identity: unbinding super+left
        // leaves super+shift+left intact.
        assert!(c.bind("super+left", "none").is_ok());
        assert!(c.lookup(KEY_LEFT, false).is_none());
        assert!(matches!(c.lookup(KEY_LEFT, true), Some(ChordAction::MoveDir(Dir::Left))));
        // reset restores the full default table.
        c.reset();
        assert!(matches!(c.lookup(KEY_F, false), Some(ChordAction::Zoom)));
        assert!(c.lookup(34, false).is_none());
    }

    #[test]
    fn bind_rejects_malformed() {
        let mut c = Chords::new();
        assert!(c.bind("f", "zoom").is_err(), "super required");
        assert!(c.bind("super+", "zoom").is_err(), "empty key token");
        assert!(c.bind("super+f+g", "zoom").is_err(), "two keys");
        assert!(c.bind("super+nope", "zoom").is_err(), "unknown key");
        assert!(c.bind("super+f", "fly").is_err(), "unknown action");
        assert!(c.bind("ctrl+f", "zoom").is_err(), "non-super modifier only, no super");
        // shift+super order is accepted (modifiers commute; key is last).
        assert!(c.bind("super+shift+right", "move-right").is_ok());
        assert!(matches!(c.lookup(KEY_RIGHT, true), Some(ChordAction::MoveDir(Dir::Right))));
    }

    #[test]
    fn gaps_bounds() {
        let mut c = Chords::new();
        assert!(c.set_gaps(8).is_ok());
        assert_eq!(c.gaps, 8);
        assert!(c.set_gaps(0).is_ok(), "0 = borderless even split");
        assert_eq!(c.gaps, 0);
        assert!(c.set_gaps(GAPS_MAX).is_ok());
        assert!(c.set_gaps(GAPS_MAX + 1).is_err(), "over-cap rejected");
        assert_eq!(c.gaps, GAPS_MAX, "a rejected set leaves the prior value");
    }
}
