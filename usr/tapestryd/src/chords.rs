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
    /// HALCYON-INSTRUMENT 9.3 (I-7): the picker and help chords. The
    /// compositor does not act on these -- they live in the environment --
    /// it delivers TEV_CHORD to the registered rail's owner (server.rs).
    Picker,
    Help,
    SplitToggle,
    TabCycle(bool), // true = forward
    Close,
    /// HALCYON-SCALE 6: one 25% step of the display scale (+1 / -1).
    ScaleStep(i8),
    /// Back to the EDID-derived scale (`scale auto`).
    ScaleReset,
    /// HALCYON-WORKSPACES 4 (ratified): switch to workspace n, creating it
    /// when n is the next free number (i3). `n` is ONE-BASED, 1..=9 by
    /// construction -- the defaults table and `action_of` are its only
    /// constructors and both bound it.
    Workspace(u8),
    /// Move the focused tile to workspace n, ownership-preserving.
    MoveToWorkspace(u8),
    /// HALCYON-INSTRUMENT 6.1 (2026-09-16): a new empty tile in the focused
    /// pane -- joining its stack, or making a lone tile a stack of two -- for
    /// the session to fill with a shell. The kit has no such control (its
    /// tiles are fixtures); Super+N is Halcyon's.
    NewTile,
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
// HALCYON-WORKSPACES 4: the nine workspace digits. evdev numbers the top row
// 1..9 as 2..10 with `0` at 11 -- all nine were unbound before this.
const KEY_1: u16 = 2;
const KEY_2: u16 = 3;
const KEY_3: u16 = 4;
const KEY_4: u16 = 5;
const KEY_5: u16 = 6;
const KEY_6: u16 = 7;
const KEY_7: u16 = 8;
const KEY_8: u16 = 9;
const KEY_9: u16 = 10;
const KEY_0: u16 = 11;
const KEY_MINUS: u16 = 12;
const KEY_EQUAL: u16 = 13;
const KEY_TAB: u16 = 15;
const KEY_Q: u16 = 16;
const KEY_E: u16 = 18;
const KEY_T: u16 = 20;
const KEY_SLASH: u16 = 53;
const KEY_S: u16 = 31;
const KEY_F: u16 = 33;
const KEY_H: u16 = 35;
const KEY_N: u16 = 49;
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
        "a" => 30,
        "b" => 48,
        "c" => 46,
        "d" => 32,
        "e" => 18,
        "f" => 33,
        "g" => 34,
        "h" => 35,
        "i" => 23,
        "j" => 36,
        "k" => 37,
        "l" => 38,
        "m" => 50,
        "n" => 49,
        "o" => 24,
        "p" => 25,
        "q" => 16,
        "r" => 19,
        "s" => 31,
        "t" => 20,
        "u" => 22,
        "v" => 47,
        "w" => 17,
        "x" => 45,
        "y" => 21,
        "z" => 44,
        "tab" => KEY_TAB,
        "up" => KEY_UP,
        "left" => KEY_LEFT,
        "right" => KEY_RIGHT,
        "down" => KEY_DOWN,
        "1" => KEY_1,
        "2" => KEY_2,
        "3" => KEY_3,
        "4" => KEY_4,
        "5" => KEY_5,
        "6" => KEY_6,
        "7" => KEY_7,
        "8" => KEY_8,
        "9" => KEY_9,
        "0" => KEY_0,
        "minus" => KEY_MINUS,
        "equal" => KEY_EQUAL,
        "slash" => KEY_SLASH,
        _ => return None,
    })
}

/// The inverse of `key_code`: an evdev code -> its config NAME (the one
/// `key_code` accepts for it). None for a code the grammar cannot name.
pub fn key_name(code: u16) -> Option<&'static str> {
    Some(match code {
        30 => "a",
        48 => "b",
        46 => "c",
        32 => "d",
        18 => "e",
        33 => "f",
        34 => "g",
        35 => "h",
        23 => "i",
        36 => "j",
        37 => "k",
        38 => "l",
        50 => "m",
        49 => "n",
        24 => "o",
        25 => "p",
        16 => "q",
        19 => "r",
        31 => "s",
        20 => "t",
        22 => "u",
        47 => "v",
        17 => "w",
        45 => "x",
        21 => "y",
        44 => "z",
        KEY_TAB => "tab",
        KEY_UP => "up",
        KEY_LEFT => "left",
        KEY_RIGHT => "right",
        KEY_DOWN => "down",
        KEY_1 => "1",
        KEY_2 => "2",
        KEY_3 => "3",
        KEY_4 => "4",
        KEY_5 => "5",
        KEY_6 => "6",
        KEY_7 => "7",
        KEY_8 => "8",
        KEY_9 => "9",
        KEY_0 => "0",
        KEY_MINUS => "minus",
        KEY_EQUAL => "equal",
        KEY_SLASH => "slash",
        _ => return None,
    })
}

/// The inverse of `action_of`: an action -> its config NAME.
pub fn action_name(a: ChordAction) -> &'static str {
    match a {
        ChordAction::FocusDir(Dir::Left) => "focus-left",
        ChordAction::FocusDir(Dir::Right) => "focus-right",
        ChordAction::FocusDir(Dir::Up) => "focus-up",
        ChordAction::FocusDir(Dir::Down) => "focus-down",
        ChordAction::MoveDir(Dir::Left) => "move-left",
        ChordAction::MoveDir(Dir::Right) => "move-right",
        ChordAction::MoveDir(Dir::Up) => "move-up",
        ChordAction::MoveDir(Dir::Down) => "move-down",
        ChordAction::Split(Mode::SplitH) => "split-h",
        ChordAction::Split(Mode::SplitV) => "split-v",
        ChordAction::Split(_) => "split-h",
        ChordAction::SplitToggle => "split-toggle",
        ChordAction::Zoom => "zoom",
        ChordAction::Picker => "picker",
        ChordAction::Help => "help",
        ChordAction::SetMode(Mode::Tabbed) => "tab",
        ChordAction::SetMode(_) => "stack",
        ChordAction::TabCycle(true) => "cycle",
        ChordAction::TabCycle(false) => "cycle-back",
        ChordAction::Close => "close",
        ChordAction::ScaleStep(s) if s > 0 => "scale-up",
        ChordAction::ScaleStep(_) => "scale-down",
        ChordAction::ScaleReset => "scale-reset",
        ChordAction::NewTile => "new-tile",
        // n is 1..=9 BY CONSTRUCTION (see the variant's doc). The clamp is
        // there because `u8` is not, and is unreachable; a table keeps this
        // direction and `action_of` from drifting as 18 arms would.
        ChordAction::Workspace(n) => WORKSPACE_NAMES[(n.clamp(1, 9) - 1) as usize],
        ChordAction::MoveToWorkspace(n) => MOVE_TO_NAMES[(n.clamp(1, 9) - 1) as usize],
    }
}

/// HALCYON-WORKSPACES 4: the nine switch names and the nine move names.
const WORKSPACE_NAMES: [&str; 9] = [
    "workspace-1",
    "workspace-2",
    "workspace-3",
    "workspace-4",
    "workspace-5",
    "workspace-6",
    "workspace-7",
    "workspace-8",
    "workspace-9",
];
const MOVE_TO_NAMES: [&str; 9] = [
    "move-to-1",
    "move-to-2",
    "move-to-3",
    "move-to-4",
    "move-to-5",
    "move-to-6",
    "move-to-7",
    "move-to-8",
    "move-to-9",
];

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
        "picker" => ChordAction::Picker,
        "help" => ChordAction::Help,
        "tab" => ChordAction::SetMode(Mode::Tabbed),
        "stack" => ChordAction::SetMode(Mode::Stacked),
        "cycle" => ChordAction::TabCycle(true),
        "cycle-back" => ChordAction::TabCycle(false),
        "close" => ChordAction::Close,
        "scale-up" => ChordAction::ScaleStep(1),
        "scale-down" => ChordAction::ScaleStep(-1),
        "scale-reset" => ChordAction::ScaleReset,
        "new-tile" => ChordAction::NewTile,
        "none" => return Some(None), // the unbind token
        // HALCYON-WORKSPACES 4: `workspace-1`..`-9` and `move-to-1`..`-9`,
        // PARSED rather than listed, so this direction and `action_name`
        // cannot drift apart the way eighteen hand-written arms would. The
        // 1..=9 bound here is half of what makes the payload's invariant
        // hold (the defaults table is the other half).
        other => {
            let (rest, mk): (&str, fn(u8) -> ChordAction) =
                if let Some(r) = other.strip_prefix("workspace-") {
                    (r, ChordAction::Workspace)
                } else if let Some(r) = other.strip_prefix("move-to-") {
                    (r, ChordAction::MoveToWorkspace)
                } else {
                    return None;
                };
            match rest.parse::<u8>() {
                Ok(n) if (1..=9).contains(&n) => mk(n),
                _ => return None,
            }
        }
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
                d(KEY_T, false, Picker),
                d(KEY_T, true, SetMode(Mode::Tabbed)),
                d(KEY_S, false, SetMode(Mode::Stacked)),
                d(KEY_N, false, NewTile),
                d(KEY_SLASH, false, Help),
                d(KEY_E, false, SplitToggle),
                d(KEY_TAB, false, TabCycle(true)),
                d(KEY_TAB, true, TabCycle(false)),
                d(KEY_Q, true, Close),
                // HALCYON-SCALE 6: the universal zoom keys (browsers,
                // terminals); free in this table; remappable like the rest.
                d(KEY_EQUAL, false, ScaleStep(1)),
                d(KEY_MINUS, false, ScaleStep(-1)),
                d(KEY_0, false, ScaleReset),
                // HALCYON-WORKSPACES 4 (ratified 2026-09-15): Super+1..9
                // switches (creating the next free number, i3);
                // Super+Shift+1..9 moves the focused tile there. All
                // eighteen keycodes were free in this table.
                d(KEY_1, false, Workspace(1)),
                d(KEY_2, false, Workspace(2)),
                d(KEY_3, false, Workspace(3)),
                d(KEY_4, false, Workspace(4)),
                d(KEY_5, false, Workspace(5)),
                d(KEY_6, false, Workspace(6)),
                d(KEY_7, false, Workspace(7)),
                d(KEY_8, false, Workspace(8)),
                d(KEY_9, false, Workspace(9)),
                d(KEY_1, true, MoveToWorkspace(1)),
                d(KEY_2, true, MoveToWorkspace(2)),
                d(KEY_3, true, MoveToWorkspace(3)),
                d(KEY_4, true, MoveToWorkspace(4)),
                d(KEY_5, true, MoveToWorkspace(5)),
                d(KEY_6, true, MoveToWorkspace(6)),
                d(KEY_7, true, MoveToWorkspace(7)),
                d(KEY_8, true, MoveToWorkspace(8)),
                d(KEY_9, true, MoveToWorkspace(9)),
            ],
            gaps: 1,
        }
    }

    /// Restore the default BINDINGS (the environment's reset-first push: a
    /// removed config chord reverts to its default). cfg-4 audit F3:
    /// DECOUPLED from `gaps` -- `chord-reset` resets bindings ONLY, never
    /// the inset (which has its own verb + reconcile; a silent gaps reset
    /// here left a stale visible inset with no triggering command).
    pub fn reset(&mut self) {
        let gaps = self.gaps;
        *self = Chords::new();
        self.gaps = gaps;
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
            self.binds.push(Bind {
                key,
                shift,
                action: a,
            });
        }
        Ok(())
    }

    /// HALCYON-INSTRUMENT 8.2: the table as text, one binding per line in
    /// the config grammar (`super+[shift+]<key> <action>`), in table order
    /// -- what the `chords` file publishes, so an environment derives its
    /// chord hints from the bindings in force and never from a literal. A
    /// binding on a key the grammar cannot name is skipped (none exists in
    /// the default table; a config line cannot make one).
    pub fn render(&self) -> alloc::string::String {
        let mut out = alloc::string::String::new();
        for b in &self.binds {
            let Some(key) = key_name(b.key) else { continue };
            out.push_str("super+");
            if b.shift {
                out.push_str("shift+");
            }
            out.push_str(key);
            out.push(' ');
            out.push_str(action_name(b.action));
            out.push('\n');
        }
        out
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
        // The key must be the FINAL token: any token after it (a second
        // key OR a modifier) is malformed (cfg-4 audit F4: the strict
        // grammar the comment states -- modifiers must precede the key).
        if key.is_some() {
            return Err(());
        }
        match tok {
            "super" => super_seen = true,
            "shift" => shift = true,
            other => key = Some(key_code(other).ok_or(())?),
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
        assert!(matches!(
            c.lookup(KEY_LEFT, false),
            Some(ChordAction::FocusDir(Dir::Left))
        ));
        assert!(matches!(
            c.lookup(KEY_LEFT, true),
            Some(ChordAction::MoveDir(Dir::Left))
        ));
        assert!(matches!(
            c.lookup(KEY_H, false),
            Some(ChordAction::Split(Mode::SplitH))
        ));
        assert!(matches!(
            c.lookup(KEY_TAB, false),
            Some(ChordAction::TabCycle(true))
        ));
        assert!(matches!(
            c.lookup(KEY_TAB, true),
            Some(ChordAction::TabCycle(false))
        ));
        assert!(matches!(c.lookup(KEY_Q, true), Some(ChordAction::Close)));
        // HALCYON-SCALE 6: the scale chords (Super+= / Super+- / Super+0),
        // shift-free, and their config names.
        assert!(matches!(c.lookup(KEY_EQUAL, false), Some(ChordAction::ScaleStep(1))));
        assert!(matches!(c.lookup(KEY_MINUS, false), Some(ChordAction::ScaleStep(-1))));
        assert!(matches!(c.lookup(KEY_0, false), Some(ChordAction::ScaleReset)));
        assert!(matches!(action_of("scale-up"), Some(Some(ChordAction::ScaleStep(1)))));
        assert!(matches!(action_of("scale-reset"), Some(Some(ChordAction::ScaleReset))));
        assert_eq!(key_code("equal"), Some(KEY_EQUAL));
        assert_eq!(key_code("minus"), Some(KEY_MINUS));
        assert_eq!(key_code("0"), Some(KEY_0));
        // I-7 (ruling 13): Super+T opens the picker, Super+Shift+T is tabbed,
        // Super+/ is help.
        assert!(matches!(c.lookup(KEY_T, false), Some(ChordAction::Picker)));
        assert!(matches!(c.lookup(KEY_T, true), Some(ChordAction::SetMode(Mode::Tabbed))));
        assert!(matches!(c.lookup(KEY_SLASH, false), Some(ChordAction::Help)));
        // HALCYON-INSTRUMENT 6.1 (2026-09-16): Super+N opens a new tile.
        assert!(matches!(c.lookup(KEY_N, false), Some(ChordAction::NewTile)));
        assert!(matches!(action_of("new-tile"), Some(Some(ChordAction::NewTile))));
        assert_eq!(action_name(ChordAction::NewTile), "new-tile");
        assert_eq!(key_code("n"), Some(KEY_N));
        assert!(matches!(action_of("picker"), Some(Some(ChordAction::Picker))));
        assert!(matches!(action_of("help"), Some(Some(ChordAction::Help))));
        assert_eq!(key_code("slash"), Some(KEY_SLASH));
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
        assert!(matches!(
            c.lookup(KEY_LEFT, true),
            Some(ChordAction::MoveDir(Dir::Left))
        ));
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
        assert!(
            c.bind("super+f+shift", "zoom").is_err(),
            "F4: modifier after key"
        );
        assert!(c.bind("super+nope", "zoom").is_err(), "unknown key");
        assert!(c.bind("super+f", "fly").is_err(), "unknown action");
        assert!(
            c.bind("ctrl+f", "zoom").is_err(),
            "non-super modifier only, no super"
        );
        // shift+super order is accepted (modifiers commute; key is last).
        assert!(c.bind("super+shift+right", "move-right").is_ok());
        assert!(matches!(
            c.lookup(KEY_RIGHT, true),
            Some(ChordAction::MoveDir(Dir::Right))
        ));
    }

    /// HALCYON-INSTRUMENT 8.2: the rendered table round-trips through the
    /// grammar it is written in -- every line re-binds to the same action --
    /// and the two name maps are each other's inverse over the whole
    /// vocabulary, so a hint derived from the file names the binding in
    /// force.
    #[test]
    fn render_is_the_grammar_and_the_names_invert() {
        let c = Chords::new();
        let text = c.render();
        assert!(text.contains("super+left focus-left\n"));
        assert!(text.contains("super+shift+left move-left\n"));
        assert!(text.contains("super+tab cycle\n"));
        assert!(text.contains("super+shift+tab cycle-back\n"));
        assert!(text.contains("super+shift+q close\n"));
        assert!(text.contains("super+t picker\n"));
        assert!(text.contains("super+shift+t tab\n"));
        assert!(text.contains("super+slash help\n"));
        assert!(text.contains("super+equal scale-up\n"));
        assert!(text.contains("super+n new-tile\n"));
        assert!(text.contains("super+1 workspace-1\n"));
        assert!(text.contains("super+shift+9 move-to-9\n"));
        // DERIVED, not a literal: this pinned 22 by hand and went stale the
        // moment the workspace chords landed. The claim is "render emits one
        // line per bind, none dropped and none duplicated" -- `render` is
        // what is under test and `binds` is its input, so this stays a real
        // assertion while becoming one that cannot rot.
        assert_eq!(
            text.lines().count(),
            c.binds.len(),
            "every default binding, one line each"
        );
        let mut d = Chords::new();
        d.binds.clear();
        for line in text.lines() {
            let (combo, action) = line.split_once(' ').expect("two words");
            assert!(d.bind(combo, action).is_ok(), "{} re-binds", line);
        }
        assert_eq!(d.render(), text, "a round trip is the identity");
        // A rebind shows up; an unbind disappears.
        assert!(d.bind("super+g", "zoom").is_ok());
        assert!(d.render().contains("super+g zoom\n"));
        assert!(d.bind("super+f", "none").is_ok());
        assert!(!d.render().contains("super+f "));
        // The name maps invert over the vocabulary.
        for name in [
            "a", "z", "tab", "up", "left", "right", "down", "0", "minus", "equal", "1", "5", "9",
        ] {
            assert_eq!(key_name(key_code(name).unwrap()), Some(name));
        }
        for name in [
            "focus-left", "focus-right", "focus-up", "focus-down", "move-left", "move-right",
            "move-up", "move-down", "split-h", "split-v", "split-toggle", "zoom", "tab", "stack",
            "cycle", "cycle-back", "close", "scale-up", "scale-down", "scale-reset",
            // HALCYON-WORKSPACES 4: the parsed pair must invert too -- this
            // is the only check that the table direction and the parse
            // direction agree about all eighteen.
            "workspace-1", "workspace-5", "workspace-9", "move-to-1", "move-to-5", "move-to-9",
        ] {
            let a = action_of(name).unwrap().unwrap();
            assert_eq!(action_name(a), name);
        }
        assert_eq!(key_name(1), None, "a code the grammar cannot name");
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
