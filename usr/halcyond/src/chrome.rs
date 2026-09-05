// chrome -- the per-leaf Daylight tag bar, the thinking half (HALCYON.md
// 13.6, RATIFIED PER-LEAF; HALCYON-VISUAL section 4). Pure: the `layout`
// text and the per-pane file texts come in as strings, a display list goes
// out. The syscalling half -- surfaces, fds, the event pump -- is the bin's
// `chromeset` (the 13.1 split: the lib thinks and never syscalls, so every
// rule here is host-testable; H-3b-3 first put both halves in the lib and
// broke the lib's host-test build, which is how the split got enforced).
//
// halcyond owns one Role::Chrome surface per visible leaf that carries a
// tag-bar strip, paints the whole strip (bg + separator + name), and the
// compositor PLACES it at the leaf's `tagbar` rect (H-3b-2). Strip rects,
// names and statuses come from the pane 9P tree (`layout`, `pane/<id>/
// tagbar`, `pane/<id>/tag`, `pane/<id>/status`) -- the section 13.7
// file-walk bias, no new read verb. Pills are commands and commands are
// H-3c.
//
// STATES (section 4.2; H-3b-4): the LIVE tile -- the focused leaf, the one
// tile holding input -- takes the sage or cinnabar key by the exit of its
// last command; every other leaf is a resting pane's sole tile, "the tile
// a resting pane would return to", and carries the theme's ember (deep) on
// its separator with the name in full ink. The plain Resting row (border
// separator, muted name) belongs to the collapsed tiles of a stack, which
// do not exist before tile stacking lands. The status is READ from the
// pane's `status` file -- the compositor's record, the same one its live
// hairline reads -- never from a private copy, so strip and hairline can
// never disagree; the console tile WRITES it through the gated verb.

use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use libhalcyon::theme::{Argb, DAYLIGHT, METRICS};

use crate::raster::{GlyphSource, FACE_BODY};

/// The name typeface size (section 4.3: 10.5px, proportional).
pub const NAME_PX: f32 = 10.5;

/// One visible leaf as the `layout` text reports it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Leaf {
    pub id: u32,
    pub focused: bool,
    pub surface: Option<u32>,
    /// Not laid out this pass (a zoom or a tab hides it). Still a leaf, still
    /// hosted: absent from the chrome, present in the tree.
    pub hidden: bool,
}

/// Parse the leaf lines of the `layout` text: "<id>[*] leaf surface=<n>|empty
/// [x,y,w,h][ hidden]" (tapestryd pane.rs render_pane), visible leaves only
/// -- the chrome's input (a hidden leaf carves no strip). Containers and the
/// epoch header are not leaves; a malformed id is skipped, never guessed.
pub fn parse_leaves(layout: &str) -> Vec<Leaf> {
    parse_leaves_all(layout)
        .into_iter()
        .filter(|l| !l.hidden)
        .collect()
}

/// Every leaf line, hidden ones included, with `hidden` set. The session
/// compositor's input: a hidden leaf is still hosted and must never read as
/// vanished (dropping it would kill the tile's shell on a zoom).
pub fn parse_leaves_all(layout: &str) -> Vec<Leaf> {
    let mut out = Vec::new();
    for line in layout.lines() {
        let line = line.trim();
        if !line.contains(" leaf ") {
            continue;
        }
        let hidden = line.ends_with("hidden");
        let mut it = line.split_ascii_whitespace();
        let idtok = match it.next() {
            Some(t) => t,
            None => continue,
        };
        let focused = idtok.ends_with('*');
        let id: u32 = match idtok.trim_end_matches('*').parse() {
            Ok(v) => v,
            Err(_) => continue,
        };
        let surface = it
            .find_map(|t| t.strip_prefix("surface="))
            .and_then(|s| s.parse().ok());
        out.push(Leaf {
            id,
            focused,
            surface,
            hidden,
        });
    }
    out
}

/// "x y w h" (the `tagbar` / `geometry` file text).
pub fn parse_rect(s: &str) -> Option<(u32, u32, u32, u32)> {
    let mut it = s.split_ascii_whitespace();
    let x = it.next()?.parse().ok()?;
    let y = it.next()?.parse().ok()?;
    let w = it.next()?.parse().ok()?;
    let h = it.next()?.parse().ok()?;
    Some((x, y, w, h))
}

/// The display key of one tile (section 4.2 rows).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Key {
    /// Not live: "resting, active tile" (ember_deep separator, full ink).
    Resting,
    /// Live, last exit 0 (or nothing has run yet).
    Sage,
    /// Live, last exit non-zero.
    Cinnabar,
}

/// The key from the compositor's two facts: focus (the layout's `*`) and
/// the pane's recorded status (the `status` file text: resting|ok|err).
/// Only `err` promotes a live tile to cinnabar -- section 1.4's "two states
/// only": anything else, including an unreadable file, is sage.
pub fn key_for(focused: bool, status: &str) -> Key {
    if !focused {
        Key::Resting
    } else if status.trim() == "err" {
        Key::Cinnabar
    } else {
        Key::Sage
    }
}

/// The strip's colours per key: (ground, separator, name ink).
pub fn key_colors(key: Key) -> (Argb, Argb, Argb) {
    let d = &DAYLIGHT;
    match key {
        Key::Resting => (d.header, d.ember_deep, d.fg),
        Key::Sage => (d.sage.tint, d.sage.key, d.sage.fg),
        Key::Cinnabar => (d.cinnabar.tint, d.cinnabar.key, d.cinnabar.fg),
    }
}

/// The strip display list (section 4.1/4.2, 4.3 metrics): the ground, the
/// 1px separator on the bottom edge, and the name in the proportional face
/// vertically centred in the strip above the separator. No pills yet
/// (H-3c), no trail. A zero-sized strip yields an empty list.
pub fn strip_list(key: Key, name: &str, w: u32, h: u32, gs: &mut GlyphSource) -> Cartoon {
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    let (bg, sep, ink) = key_colors(key);
    cart.ops.push(Op::Clear { color: bg });
    cart.ops.push(Op::Rect {
        x: 0,
        y: h as i32 - 1,
        w,
        h: 1,
        color: sep,
    });
    if !name.is_empty() {
        let (asc, desc) = gs
            .line_metrics(FACE_BODY, NAME_PX)
            .map(|m| (m.ascent, m.descent))
            .unwrap_or((8, 2));
        let box_h = asc + desc;
        let baseline = ((h as i32 - 1) - box_h) / 2 + asc;
        let mut refs: Vec<GlyphRef> = Vec::new();
        for ch in name.chars() {
            if let Some(g) = gs.glyph(FACE_BODY, NAME_PX, ch) {
                refs.push(g);
            }
        }
        if !refs.is_empty() {
            cart.push_glyphs(gs.gen(), METRICS.tag_pad_x, baseline, ink, &refs);
        }
    }
    cart
}

/// A name for the tile the transcript lives in (section 4.1: the name is
/// the tile's program).
pub fn console_name() -> String {
    String::from("halcyon")
}

#[cfg(test)]
mod tests {
    use super::*;

    const LAYOUT: &str = "epoch 7 focused 3\n\
1 splith n=2 active=1 [0,0,1280,800]\n\
  2 leaf surface=0 [4,24,632,772]\n\
  3* leaf empty [644,24,632,772]\n\
  4 leaf surface=2 [0,0,0,0] hidden\n\
  x leaf empty [0,0,0,0]\n";

    #[test]
    fn leaves_parse_focus_surface_and_skip_hidden() {
        let l = parse_leaves(LAYOUT);
        assert_eq!(
            l.len(),
            2,
            "the container, the hidden leaf and the malformed id are not leaves"
        );
        assert_eq!(
            l[0],
            Leaf {
                id: 2,
                focused: false,
                surface: Some(0),
                hidden: false,
            }
        );
        assert_eq!(
            l[1],
            Leaf {
                id: 3,
                focused: true,
                surface: None,
                hidden: false,
            }
        );
    }

    #[test]
    fn rect_parses_four_fields_or_nothing() {
        assert_eq!(parse_rect("4 4 632 20\n"), Some((4, 4, 632, 20)));
        assert_eq!(parse_rect("0 0 0 0\n"), Some((0, 0, 0, 0)));
        assert_eq!(parse_rect("4 4 632"), None);
        assert_eq!(parse_rect("a b c d"), None);
    }

    // Section 1.4 / 4.2: the key is (focus x last exit); only a live `err`
    // is cinnabar, a live anything-else is sage, and no status is ever
    // shown where input is not.
    #[test]
    fn key_is_focus_times_last_exit() {
        assert_eq!(key_for(true, "ok\n"), Key::Sage);
        assert_eq!(key_for(true, "resting\n"), Key::Sage);
        assert_eq!(key_for(true, ""), Key::Sage);
        assert_eq!(key_for(true, "err\n"), Key::Cinnabar);
        assert_eq!(key_for(false, "err\n"), Key::Resting);
        assert_eq!(key_for(false, "ok\n"), Key::Resting);
    }

    // The 4.2 table, pinned: ground / separator / name per row.
    #[test]
    fn strip_colors_match_the_scripture() {
        assert_eq!(
            key_colors(Key::Resting),
            (0xFFCEC4B6, 0xFFC86030, 0xFF1A120A)
        );
        assert_eq!(key_colors(Key::Sage), (0xFFB8CCC4, 0xFF1E5844, 0xFF0C2820));
        assert_eq!(
            key_colors(Key::Cinnabar),
            (0xFFDCB8B0, 0xFF982818, 0xFF3C1008)
        );
    }

    #[test]
    fn strip_list_is_ground_separator_then_name() {
        let mut gs = GlyphSource::new_vendored(64);
        let c = strip_list(Key::Cinnabar, "halcyon", 300, 20, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFFDCB8B0 }));
        assert!(matches!(
            c.ops[1],
            Op::Rect {
                x: 0,
                y: 19,
                w: 300,
                h: 1,
                color: 0xFF982818
            }
        ));
        assert!(c.ops.len() > 2, "the name produced glyph ops");
        let empty = strip_list(Key::Sage, "", 300, 20, &mut gs);
        assert_eq!(empty.ops.len(), 2, "no name, no glyph run");
        assert!(strip_list(Key::Sage, "x", 0, 20, &mut gs).ops.is_empty());
    }
}
