// chrome -- the per-leaf Daylight tag bar, the thinking half (HALCYON.md
// 13.6, RATIFIED PER-LEAF; HALCYON-VISUAL section 4). Pure: the `layout`
// text and the per-pane file texts come in as strings, a display list goes
// out. The syscalling half -- surfaces, fds, the event pump -- is the bin's
// `chromeset` (the 13.1 split: the lib thinks and never syscalls, so every
// rule here is host-testable; H-3b-3 first put both halves in the lib and
// broke the lib's host-test build, which is how the split got enforced).
//
// halcyond owns one Role::Chrome surface per visible leaf that carries a
// tag-bar strip, paints the whole strip (bg + separator + name + trail),
// and the compositor PLACES it at the leaf's `tagbar` rect (H-3b-2). Strip
// rects and statuses come from the pane 9P tree (`layout`, `pane/<id>/
// tagbar`, `pane/<id>/status`) -- the section 13.7 file-walk bias, no new
// read verb. The NAME is the tile's program and the TRAIL its status
// (section 4.1): the host that spawned the tile supplies both (a shell
// tile: `ut` + its working directory, the operator's mockups); a leaf
// nobody describes shows its `tag` text. Pills are commands and commands
// are H-3c.
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
use libhalcyon::tag::argv_of;
use libhalcyon::theme::{Argb, DAYLIGHT};

use crate::layout::Sheet;
use crate::raster::{GlyphSource, FACE_BODY};

/// The name typeface size (section 4.3: 10.5px, proportional), LOGICAL --
/// the sheet scales it (HALCYON-SCALE 6).
pub const NAME_PX: f32 = 10.5;
/// The trail typeface size (section 4.3: pills and the trail at 9.5px).
pub const TRAIL_PX: f32 = 9.5;
/// The gap between the bar's elements (section 4.3: 5px).
const GAP: i32 = 5;

/// The tile's program from its command line (section 4.1: the name is the
/// tile's PROGRAM, never the whole command line): the first word's
/// basename. Empty for an empty or blank tag -- the host names the shell.
pub fn program_name(cmdline: &str) -> String {
    let first = argv_of(cmdline).into_iter().next().unwrap_or("");
    let base = first.rsplit('/').next().unwrap_or(first);
    String::from(if base.is_empty() { first } else { base })
}

/// `path` with the home directory folded to `~` (the shell tile's trail
/// reads `~/kernel/sched`, the operator's mockups): the exact home, or a
/// path under it; any other path, and a path under a mere prefix of home
/// (`/home/mx` for `/home/m`), verbatim.
pub fn abbrev_home(path: &str, home: Option<&str>) -> String {
    let home = home.unwrap_or("").trim_end_matches('/');
    if home.is_empty() {
        return String::from(path);
    }
    if path == home {
        return String::from("~");
    }
    match path.strip_prefix(home) {
        Some(rest) if rest.starts_with('/') => {
            let mut s = String::from("~");
            s.push_str(rest);
            s
        }
        _ => String::from(path),
    }
}

/// Fit a trail into `avail` pixels without truncating it (section 4.1: the
/// trail never ellipsises): a PATH gives up its leading components --
/// `~/a/b/c` -> `…/b/c` -> `…/c` -- until it fits or only its last
/// component is left; anything else is returned whole (a program's status
/// is its own to keep short). `measure` is the width of a candidate.
pub fn fit_trail(trail: &str, avail: i32, mut measure: impl FnMut(&str) -> i32) -> String {
    if measure(trail) <= avail || !trail.contains('/') {
        return String::from(trail);
    }
    let parts: Vec<&str> = trail.split('/').collect();
    let mut out = String::from(trail);
    for keep in (1..parts.len()).rev() {
        let tail = parts[parts.len() - keep..].join("/");
        let mut cand = String::from("\u{2026}/");
        cand.push_str(&tail);
        out = cand;
        if measure(&out) <= avail {
            break;
        }
    }
    out
}

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

/// The trail's ink per key (the mockups' `.hal-tag-trail`): the dim step of
/// the strip's own ink family, so the trail recedes behind the name.
pub fn trail_ink(key: Key) -> Argb {
    let d = &DAYLIGHT;
    match key {
        Key::Resting => d.fg_dim,
        Key::Sage => d.sage.fg_dim,
        Key::Cinnabar => d.cinnabar.fg_dim,
    }
}

/// A run of `text` in `face` at `px`: its glyphs and their whole width, at
/// the sub-pixel pen (HALCYON-TYPE 4.3) -- the same shaper the status bar
/// and the menu use, so every chrome surface places type the way the
/// transcript does.
fn shape(gs: &mut GlyphSource, face: u8, px: f32, text: &str) -> (Vec<GlyphRef>, i32) {
    gs.shape_run(face, px, text.chars())
}

/// The baseline that centres a face's line box in the strip above the
/// separator (`sep_h` tall).
fn centred_baseline(gs: &mut GlyphSource, px: f32, h: u32, sep_h: i32) -> i32 {
    let (asc, desc) = gs
        .line_metrics(FACE_BODY, px)
        .map(|m| (m.ascent, m.descent))
        .unwrap_or((8, 2));
    ((h as i32 - sep_h) - (asc + desc)) / 2 + asc
}

/// The strip display list (section 4.1/4.2, 4.3 metrics, at the sheet's
/// scale): the ground, the hairline separator on the bottom edge, the name
/// at the left in the proportional face, and the trail -- the tile's
/// status, right-aligned in its dim ink -- each vertically centred in the
/// strip above the separator. No pills yet (H-3c), so no rule. The trail
/// is never cut: a path gives up leading components to fit beside the name
/// (`fit_trail`); a trail that still does not fit starts after the name and
/// runs off the strip's edge (the mockups' `flex-shrink: 0`). A zero-sized
/// strip yields an empty list.
pub fn strip_list(
    key: Key,
    name: &str,
    trail: &str,
    w: u32,
    h: u32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> Cartoon {
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    let (bg, sep, ink) = key_colors(key);
    let hair = sheet.hairline;
    cart.ops.push(Op::Clear { color: bg });
    cart.ops.push(Op::Rect {
        x: 0,
        y: (h as i32 - hair).max(0),
        w,
        h: hair as u32,
        color: sep,
    });
    let pad = sheet.metrics.tag_pad_x;
    let gap = sheet.ipx(GAP);
    let (name_px, trail_px) = (sheet.px(NAME_PX), sheet.px(TRAIL_PX));
    let mut name_end = pad;
    if !name.is_empty() {
        let baseline = centred_baseline(gs, name_px, h, hair);
        let (refs, width) = shape(gs, FACE_BODY, name_px, name);
        if !refs.is_empty() {
            cart.push_glyphs(gs.gen(), pad, baseline, ink, &refs);
            name_end = pad + width + gap;
        }
    }
    if !trail.is_empty() {
        let avail = w as i32 - pad - name_end;
        let text = fit_trail(trail, avail, |s| shape(gs, FACE_BODY, trail_px, s).1);
        let baseline = centred_baseline(gs, trail_px, h, hair);
        let (refs, width) = shape(gs, FACE_BODY, trail_px, &text);
        if !refs.is_empty() {
            let x = (w as i32 - pad - width).max(name_end);
            cart.push_glyphs(gs.gen(), x, baseline, trail_ink(key), &refs);
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

    fn sheet() -> Sheet {
        crate::layout::daylight_sheet(100)
    }

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
        let c = strip_list(Key::Cinnabar, "halcyon", "", 300, 20, &sheet(), &mut gs);
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
        let empty = strip_list(Key::Sage, "", "", 300, 20, &sheet(), &mut gs);
        assert_eq!(empty.ops.len(), 2, "no name, no glyph run");
        assert!(strip_list(Key::Sage, "x", "", 0, 20, &sheet(), &mut gs).ops.is_empty());
    }

    /// The glyph runs of a list: (x, ink, glyph count, width), in order.
    fn runs(c: &Cartoon) -> Vec<(i32, Argb, usize, i32)> {
        c.ops
            .iter()
            .filter_map(|op| match *op {
                Op::Glyphs {
                    baseline_x,
                    color,
                    start,
                    count,
                    ..
                } => {
                    let g = &c.runs[start as usize..(start + count) as usize];
                    Some((baseline_x, color, g.len(), g.iter().map(|r| r.advance).sum()))
                }
                _ => None,
            })
            .collect()
    }

    // Section 4.1: the trail is right-aligned at the strip's padding, in the
    // key's dim ink, after the name -- and never cut.
    #[test]
    fn the_trail_sits_at_the_right_in_the_dim_ink() {
        let mut gs = GlyphSource::new_vendored(64);
        let c = strip_list(Key::Sage, "ut", "~/kernel/sched", 300, 20, &sheet(), &mut gs);
        let r = runs(&c);
        assert_eq!(r.len(), 2, "the name run then the trail run: {:?}", r);
        assert_eq!(r[0].0, sheet().metrics.tag_pad_x, "the name at the left pad");
        assert_eq!(r[0].1, DAYLIGHT.sage.fg, "the name in the key's ink");
        assert_eq!(r[1].1, DAYLIGHT.sage.fg_dim, "the trail in the key's dim ink");
        assert_eq!(r[1].2, "~/kernel/sched".chars().count(), "the whole trail");
        assert_eq!(
            r[1].0 + r[1].3,
            300 - sheet().metrics.tag_pad_x,
            "right-aligned at the pad"
        );
        assert!(r[1].0 > r[0].0, "the trail after the name");
        // Resting: the theme's own dim ink.
        let rest = strip_list(Key::Resting, "ut", "~", 300, 20, &sheet(), &mut gs);
        assert_eq!(runs(&rest)[1].1, DAYLIGHT.fg_dim);
        // No name: the trail alone, still right-aligned.
        let alone = strip_list(Key::Sage, "", "idle", 300, 20, &sheet(), &mut gs);
        assert_eq!(runs(&alone).len(), 1);
    }

    // A strip too narrow for name + path: the path drops its leading
    // components (never a mid-glyph cut), keeping at least its last one;
    // what still does not fit starts after the name and runs off the edge
    // rather than over the name.
    #[test]
    fn a_narrow_strip_elides_the_paths_head_and_never_covers_the_name() {
        let mut gs = GlyphSource::new_vendored(64);
        let c = strip_list(Key::Sage, "ut", "~/thylacine/kernel/sched", 90, 20, &sheet(), &mut gs);
        let r = runs(&c);
        assert_eq!(r.len(), 2);
        assert!(
            r[1].0 >= r[0].0 + r[0].3 + GAP,
            "the trail starts after the name + gap: {:?}",
            r
        );
        assert!(
            r[1].2 < "~/thylacine/kernel/sched".chars().count(),
            "the path was shortened"
        );
        assert!(
            r[1].2 >= "\u{2026}/sched".chars().count(),
            "its last component survives"
        );
    }

    #[test]
    fn fit_trail_drops_leading_components_only_for_paths() {
        let m = |s: &str| s.chars().count() as i32 * 6;
        assert_eq!(fit_trail("~/a/b/c", 100, m), "~/a/b/c", "fits: whole");
        assert_eq!(fit_trail("~/a/b/c", 6 * 5, m), "\u{2026}/b/c");
        assert_eq!(fit_trail("~/a/b/c", 6 * 3, m), "\u{2026}/c");
        assert_eq!(fit_trail("~/a/b/c", 1, m), "\u{2026}/c", "never past the last");
        assert_eq!(fit_trail("/usr/lib", 6 * 4, m), "\u{2026}/lib");
        assert_eq!(fit_trail("NORMAL \u{b7} 221", 1, m), "NORMAL \u{b7} 221", "not a path: whole");
        assert_eq!(fit_trail("", 1, m), "");
    }

    #[test]
    fn program_name_is_the_first_words_basename() {
        assert_eq!(program_name("/bin/ut --home /home/m"), "ut");
        assert_eq!(program_name("ut"), "ut");
        assert_eq!(program_name("  nora /etc/x  "), "nora");
        assert_eq!(program_name("/goroot/bin/go build"), "go");
        assert_eq!(program_name(""), "");
        assert_eq!(program_name("   "), "");
        assert_eq!(program_name("/bin/"), "/bin/", "a trailing slash names nothing: verbatim");
    }

    #[test]
    fn abbrev_home_folds_home_and_only_home() {
        assert_eq!(abbrev_home("/home/m", Some("/home/m")), "~");
        assert_eq!(abbrev_home("/home/m/", Some("/home/m")), "~/");
        assert_eq!(abbrev_home("/home/m/src/x", Some("/home/m")), "~/src/x");
        assert_eq!(abbrev_home("/home/mx", Some("/home/m")), "/home/mx");
        assert_eq!(abbrev_home("/etc", Some("/home/m")), "/etc");
        assert_eq!(abbrev_home("/etc", None), "/etc");
        assert_eq!(abbrev_home("/etc", Some("")), "/etc");
        assert_eq!(abbrev_home("", Some("/home/m")), "");
        assert_eq!(abbrev_home("/home/m/x", Some("/home/m/")), "~/x", "a trailing slash on home");
    }

    // HALCYON-SCALE 6: the strip at 200% -- the compositor carves a 40 px
    // bar (Metrics::at), the separator is the 2 px hairline on its bottom
    // edge, the name sits at the doubled pad in the doubled size, and the
    // trail right-aligns at the doubled pad; the 100% strip is unchanged.
    #[test]
    fn the_strip_at_200_is_the_scaled_bar() {
        let mut gs = GlyphSource::new_vendored(64);
        gs.set_scale(200);
        let s2 = crate::layout::daylight_sheet(200);
        let h = s2.metrics.header_h as u32;
        assert_eq!(h, 40);
        let c = strip_list(Key::Sage, "ut", "~/kernel", 600, h, &s2, &mut gs);
        assert!(matches!(c.ops[1], Op::Rect { x: 0, y: 38, w: 600, h: 2, .. }), "a 2 px separator at the bottom");
        let r = runs(&c);
        assert_eq!(r[0].0, 12, "the name at the doubled pad");
        assert_eq!(r[1].0 + r[1].3, 600 - 12, "the trail right-aligned at the doubled pad");
        // The name run is wider at 2.0 than at 1.0 (the size doubled).
        let mut g1 = GlyphSource::new_vendored(64);
        let s1 = crate::layout::daylight_sheet(100);
        let c1 = strip_list(Key::Sage, "ut", "~/kernel", 300, 20, &s1, &mut g1);
        let r1 = runs(&c1);
        assert!(matches!(c1.ops[1], Op::Rect { y: 19, h: 1, .. }), "1.0: the 1 px separator at y 19");
        assert!(r[0].3 > r1[0].3 * 3 / 2, "the 2.0 name ({}) is wider than the 1.0 name ({})", r[0].3, r1[0].3);
        // The baselines centre in the strip above the separator.
        let base = |c: &Cartoon| c.ops.iter().find_map(|o| match *o { Op::Glyphs { baseline_y, .. } => Some(baseline_y), _ => None }).unwrap();
        assert!(base(&c) > 20 && base(&c) < 38, "the 2.0 baseline sits in the 40 px strip: {}", base(&c));
        assert!(base(&c1) > 8 && base(&c1) < 19);
    }
}
