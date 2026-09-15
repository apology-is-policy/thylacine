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
use libhalcyon::theme::{Argb, Theme};

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

/// The `layout` header's workspace pair -- `workspaces N active K`, ONE-BASED
/// exactly as the compositor writes it (HALCYON-WORKSPACES 4, the ratified
/// channel: there is no `workspace/` subtree). Returned with K still
/// one-based; `StatusModel.active` is ZERO-based, so the caller subtracts at
/// the point it fills the model, and that conversion lives in exactly one
/// place.
///
/// None when the first line carries neither token -- a compositor older than
/// W-1a, or a malformed header -- so the caller keeps its own default rather
/// than painting a guess. Only the FIRST line is read: a container row says
/// `active=1` with an equals sign and could never be mistaken for the
/// header's bare `active`, but reading one line makes that structural rather
/// than a property of the spelling.
pub fn parse_workspaces(layout: &str) -> Option<(u8, u8)> {
    let head = layout.lines().next()?;
    let (mut n, mut k) = (None, None);
    let mut it = head.split_ascii_whitespace();
    while let Some(tok) = it.next() {
        match tok {
            "workspaces" => n = it.next().and_then(|t| t.parse::<u8>().ok()),
            "active" => k = it.next().and_then(|t| t.parse::<u8>().ok()),
            _ => {}
        }
    }
    let (n, k) = (n?, k?);
    // A pair that cannot describe a tree is refused whole: the bar would
    // otherwise light a chip with no workspace behind it.
    if n == 0 || k == 0 || k > n {
        return None;
    }
    Some((n, k))
}

#[cfg(test)]
mod workspace_header_tests {
    use super::parse_workspaces;

    #[test]
    fn reads_the_pair_one_based() {
        let l = "epoch 8 focused 5 workspaces 3 active 2\n1 splith n=2 active=1 [0,0,1,1]\n";
        assert_eq!(parse_workspaces(l), Some((3, 2)));
    }

    #[test]
    fn a_pre_w1a_header_is_none_not_a_guess() {
        let l = "epoch 7 focused 3\n1 splith n=2 active=1 [0,0,1,1]\n";
        assert_eq!(parse_workspaces(l), None);
    }

    /// THE CONTROL: a container row's `active=1` must never be read as the
    /// header's `active`. Without the first-line-only rule this returns
    /// Some and the bar lights a chip off a pane row.
    #[test]
    fn container_rows_are_not_the_header() {
        let l = "epoch 7 focused 3\n1 splith n=2 active=1 [0,0,1,1]\n  2 leaf surface=0 [0,0,1,1]\n";
        assert_eq!(parse_workspaces(l), None);
    }

    #[test]
    fn a_malformed_number_is_refused() {
        assert_eq!(parse_workspaces("epoch 1 focused 1 workspaces x active 1\n"), None);
        assert_eq!(parse_workspaces("epoch 1 focused 1 workspaces 2 active y\n"), None);
    }

    #[test]
    fn an_impossible_pair_is_refused_whole() {
        assert_eq!(parse_workspaces("epoch 1 focused 1 workspaces 1 active 2\n"), None);
        assert_eq!(parse_workspaces("epoch 1 focused 1 workspaces 0 active 0\n"), None);
    }

    #[test]
    fn the_single_workspace_default_reads_one_one() {
        let l = "epoch 2 focused 1 workspaces 1 active 1\n1 leaf surface=0 [0,0,1,1]\n";
        assert_eq!(parse_workspaces(l), Some((1, 1)));
    }
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
    parse_tree(layout).into_iter().map(|t| t.leaf).collect()
}

/// One leaf with its place in the tree (HALCYON-INSTRUMENT 6.1 / 6.4): the
/// header's index and its stack's size, whether it is the open tile, and
/// whether it is the stack's last (no separator under its header).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TileInfo {
    pub leaf: Leaf,
    /// The 1-based position among the parent's children and their count,
    /// when the parent is a stack or a tab container; (1, 1) for a leaf
    /// that is a stack of one (a split's child, or the root).
    pub index: u32,
    pub count: u32,
    /// The parent's OPEN (active) child; true for a stack of one.
    pub open: bool,
    /// The last tile of its stack.
    pub last: bool,
    /// No surface: an empty leaf -- with `count == 1`, the 14.6 placard.
    pub empty: bool,
}

/// Parse the `layout` text into its leaves with their stack facts. The
/// dump's grammar (tapestryd `render_pane`): two spaces per depth; a
/// container row `<id>[*] <mode> n=<k> active=<a> [rect]`, a leaf row
/// `<id>[*] leaf surface=<n>|empty [rect][ w=<n>][ hidden]`. The depth
/// names the parent: a leaf at depth d belongs to the nearest container at
/// depth d-1 above it. A malformed id is skipped, never guessed; a row
/// whose depth names no container is a stack of one.
pub fn parse_tree(layout: &str) -> Vec<TileInfo> {
    struct Cont {
        depth: usize,
        stacked: bool,
        n: u32,
        active: u32,
        seen: u32,
    }
    let mut stack: Vec<Cont> = Vec::new();
    let mut out = Vec::new();
    for raw in layout.lines() {
        let depth = raw.len().saturating_sub(raw.trim_start().len()) / 2;
        let line = raw.trim();
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
        let kind = match it.next() {
            Some(k) => k,
            None => continue,
        };
        // The parent: pop containers at or below this depth.
        while stack.last().is_some_and(|c| c.depth >= depth) {
            stack.pop();
        }
        let place = match stack.last_mut() {
            Some(c) if c.depth + 1 == depth => {
                let i = c.seen;
                c.seen += 1;
                Some((c.stacked, i, c.n, c.active))
            }
            _ => None,
        };
        if kind == "leaf" {
            let hidden = line.ends_with("hidden");
            let surface = it
                .clone()
                .find_map(|t| t.strip_prefix("surface="))
                .and_then(|s| s.parse().ok());
            let (index, count, open, last) = match place {
                Some((true, i, n, active)) => (i + 1, n.max(1), i == active, i + 1 >= n),
                _ => (1, 1, true, true),
            };
            out.push(TileInfo {
                leaf: Leaf {
                    id,
                    focused,
                    surface,
                    hidden,
                },
                index,
                count,
                open,
                last,
                empty: surface.is_none(),
            });
            continue;
        }
        // A container row: its mode, n and active.
        let stacked = kind == "stacked" || kind == "tabbed";
        let mut n = 0u32;
        let mut active = 0u32;
        for t in it {
            if let Some(v) = t.strip_prefix("n=") {
                n = v.parse().unwrap_or(0);
            } else if let Some(v) = t.strip_prefix("active=") {
                active = v.parse().unwrap_or(0);
            }
        }
        stack.push(Cont {
            depth,
            stacked,
            n,
            active,
            seen: 0,
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
pub fn key_colors(d: &Theme, key: Key) -> (Argb, Argb, Argb) {
    match key {
        Key::Resting => (d.header, d.ember_deep, d.fg),
        Key::Sage => (d.sage.tint, d.sage.key, d.sage.fg),
        Key::Cinnabar => (d.cinnabar.tint, d.cinnabar.key, d.cinnabar.fg),
    }
}

/// The trail's ink per key (the mockups' `.hal-tag-trail`): the dim step of
/// the strip's own ink family, so the trail recedes behind the name.
pub fn trail_ink(d: &Theme, key: Key) -> Argb {
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
    // The source follows the sheet in force at every painter entry (r2 A-F2).
    gs.set_kerning(sheet.kerning);
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    let (bg, sep, ink) = key_colors(&sheet.theme, key);
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
            cart.push_glyphs(gs.gen(), x, baseline, trail_ink(&sheet.theme, key), &refs);
        }
    }
    cart
}

/// A name for the tile the transcript lives in (section 4.1: the name is
/// the tile's program).
pub fn console_name() -> String {
    String::from("halcyon")
}

// =============================================================================
// HALCYON-INSTRUMENT 6.4 / 7.3 / 14.6 -- the Instrument header, the tile
// states and the empty pane's placard (I-3). Pure, like the legacy strip
// above: the state comes in, a display list goes out; the syscalling half
// (surfaces, the pointer, the verbs) is the bin's `chromeset`.
// =============================================================================

/// What became of a tile's process (14.6). `Live` is the ordinary state;
/// the other three are RETAINED tiles -- header, order, body and title
/// kept -- distinguished by their metadata word.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Fate {
    Live,
    /// The tile's program exited with this status (`EXIT n`).
    Ended(i32),
    /// The tile's connection went without a word (`DISCONNECTED`).
    Disconnected,
    /// The parser or the stream broke (`CRASHED`).
    Crashed,
}

/// A host's word on a leaf it hosts (the session's per-tile facts), read
/// once per reconcile: the name (the program, or its declared title), the
/// trail (the shell's directory, a program's status), the fate, whether a
/// command runs now, the last command's exit, and a program-reported
/// dirty flag (no program reports one yet; the hook is the header's).
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Described {
    pub name: String,
    pub trail: String,
    pub fate: Fate,
    pub running: bool,
    pub last_exit: Option<i64>,
    pub dirty: bool,
}

impl Described {
    /// A live tile with a name and a trail, nothing else known.
    pub fn plain(name: String, trail: String) -> Described {
        Described {
            name,
            trail,
            fate: Fate::Live,
            running: false,
            last_exit: None,
            dirty: false,
        }
    }
}

/// The metadata's ink role (6.4 / 7.3).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MetaInk {
    Dim,
    Secondary,
    Amber,
    Error,
    Success,
}

/// The header's metadata: OUR trail (6.4), uppercase (7.2). Precedence,
/// most decisive first: a retained tile's word (`EXIT n` in `success` /
/// `error`, `DISCONNECTED`, `CRASHED` in `error`); a running command
/// (`RUNNING` in `secondary`, 14.3); a failed last command (`EXIT n` in
/// `error` -- this is where the legacy sage / cinnabar key lives under
/// Instrument); else the trail in `dim`, `amber` when the program reports
/// the document modified. Focus and failure are two facts: nothing here
/// reads focus.
pub fn metadata_for(d: &Described) -> (String, MetaInk) {
    let exit_word = |n: i64| {
        let mut s = String::from("EXIT ");
        let _ = core::fmt::write(&mut s, format_args!("{}", n));
        s
    };
    match d.fate {
        Fate::Ended(n) => {
            return (
                exit_word(n as i64),
                if n == 0 { MetaInk::Success } else { MetaInk::Error },
            )
        }
        Fate::Disconnected => return (String::from("DISCONNECTED"), MetaInk::Error),
        Fate::Crashed => return (String::from("CRASHED"), MetaInk::Error),
        Fate::Live => {}
    }
    if d.running {
        return (String::from("RUNNING"), MetaInk::Secondary);
    }
    if let Some(n) = d.last_exit.filter(|&n| n != 0) {
        return (exit_word(n), MetaInk::Error);
    }
    (
        d.trail.to_uppercase(),
        if d.dirty { MetaInk::Amber } else { MetaInk::Dim },
    )
}

/// The ink for a metadata role, from the sheet's Instrument palette.
pub fn meta_ink(sheet: &Sheet, ink: MetaInk) -> Argb {
    meta_ink_of(&sheet.inst, ink)
}

/// The state a header paints from (6.4 / 7.3; the independent facts of
/// 6.2 #7).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct HeaderState {
    /// The globally focused tile (the layout's `*`).
    pub focused: bool,
    /// The stack's open tile (the leaf is laid out; not `hidden`).
    pub expanded: bool,
    /// The pointer is over the header.
    pub hovered: bool,
    /// The pointer is over the action box (`x`).
    pub hover_close: bool,
    /// The tile's 1-based index in its stack.
    pub index: u32,
    /// The stack's last tile: no separator row under a collapsed header.
    pub last: bool,
}

/// The header's regions at (`w`, `h`) in surface px (6.4, at the sheet's
/// scale): the index box, where the name starts, where the metadata ends,
/// and the action box -- RESERVED whether or not the `x` shows, so the
/// metadata never moves when it appears (the golden: the meta ends 35 px
/// from the right edge on every header, collapsed ones included).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct HeaderRegions {
    pub index_w: i32,
    pub name_x: i32,
    pub meta_right: i32,
    /// (x, y, w, h)
    pub action: (i32, i32, i32, i32),
}

/// The action box's inner height (6.4: "28 (inner 24 tall)"), logical.
const ACTION_H: i32 = 24;
/// The `x` glyph's size, logical. 6.4's table says "mono 15"; the kit's
/// CSS says otherwise and wins (`.tile-action { font-size: 15px }` inherits
/// the header's Plex Sans; nothing sets a mono family on it), so the `x`
/// is the body face at 15.
const ACTION_PX: f32 = 15.0;
/// The name's size and tracking (7.2: Sans 13 / 500, +.01 em), logical.
const NAME_INST_PX: f32 = 13.0;
const NAME_TRACK_EM: f32 = 0.01;
/// The metadata's tracking (7.2: mono 10 / 400, +.04 em, uppercase).
const META_TRACK_EM: f32 = 0.04;

pub fn header_regions(w: u32, h: u32, sheet: &Sheet) -> HeaderRegions {
    let m = &sheet.metrics;
    let (wi, hi) = (w as i32, h as i32);
    let gap = m.header_gap;
    let aw = m.action_w.min(wi);
    let ah = sheet.ipx(ACTION_H).min(hi);
    HeaderRegions {
        index_w: m.index_w.min(wi),
        name_x: (m.index_w + gap).min(wi),
        meta_right: (wi - aw - gap).max(0),
        action: (wi - aw, (hi - ah) / 2, aw, ah),
    }
}

/// What a press at (`x`, `y`) on a header means (9.1): the action box, or
/// the tile (the index, the name, the metadata -- "the header's primary
/// click still selects or opens the tile", 14.9).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum HeaderHit {
    Action,
    Tile,
}

pub fn header_hit(x: i32, y: i32, w: u32, h: u32, sheet: &Sheet) -> HeaderHit {
    let (ax, ay, aw, ah) = header_regions(w, h, sheet).action;
    if x >= ax && x < ax + aw && y >= ay && y < ay + ah {
        HeaderHit::Action
    } else {
        HeaderHit::Tile
    }
}

/// `text` cut from its end to fit `avail`, with an ellipsis, measured by
/// the same shaper that paints it; whole when it fits, empty when not even
/// the ellipsis does.
fn fit_end(gs: &mut GlyphSource, face: u8, px: f32, text: &str, avail: i32) -> String {
    fit_end_tracked(gs, face, px, 0.0, text, avail)
}

/// `fit_end` for a run painted with `tracking` px of letter-spacing: the
/// measure is the painter's (`shape_run_spaced`), so the cut lands where
/// the tracked run actually ends.
fn fit_end_tracked(gs: &mut GlyphSource, face: u8, px: f32, tracking: f32, text: &str, avail: i32) -> String {
    let width = |gs: &mut GlyphSource, s: &str| gs.shape_run_spaced(face, px, tracking, s.chars()).1;
    if width(gs, text) <= avail {
        return String::from(text);
    }
    let ell = '\u{2026}';
    let mut chars: Vec<char> = text.chars().collect();
    // The candidate is measured WITH its ellipsis, as one run -- the run
    // the painter shapes, whose last pair kerns (r2 A-F6: two runs summed
    // put a name a pixel past `avail` after a `Z`, `A`, `L`; `rail::fit_end`
    // had the one-run form all along).
    while let Some(_) = chars.pop() {
        let w = gs
            .shape_run_spaced(face, px, tracking, chars.iter().copied().chain(core::iter::once(ell)))
            .1;
        if w <= avail {
            let mut out: String = chars.iter().collect();
            out.push(ell);
            return out;
        }
    }
    String::new()
}

/// `fit_end` for the other chrome painters (the menu's title label).
pub fn fit_end_pub(gs: &mut GlyphSource, face: u8, px: f32, text: &str, avail: i32) -> String {
    fit_end(gs, face, px, text, avail)
}

/// A run's baseline that centres its line box in `h` rows.
fn centred_in(gs: &mut GlyphSource, face: u8, px: f32, h: i32) -> i32 {
    let (asc, desc) = gs
        .line_metrics(face, px)
        .map(|m| (m.ascent, m.descent))
        .unwrap_or((8, 2));
    (h - (asc + desc)) / 2 + asc
}

/// The Instrument header's display list (6.4, the 7.3 matrix, the golden's
/// rows): the ground -- `open_header` expanded, `hover` on a hovered
/// collapsed header, `header` otherwise; a collapsed non-last header's 1 px
/// `separator` bottom row; the index box's 1 px `separator` right rule; the
/// 2 x 20 `amber` focus mark on the focused expanded header; the two-digit
/// index (`amber` focused + expanded, else `dim`); the name (`text`
/// expanded or hovered, else `secondary`), end-ellipsised before the
/// metadata; the metadata right-aligned before the action box in its ink;
/// the `x` on the expanded or hovered header (`dim`; `error` under the
/// pointer, with a 1 px `structure` rule at the box's left edge). The type
/// is the sheet's (7.2, since I-5): the index and the metadata in
/// `face_mono_text` at `chrome_mono_px` (10; the metadata tracked .04 em),
/// the name in `face_medium` at 13 tracked .01 em, the `x` in `face_body`
/// at 15.
pub fn header_list(
    st: HeaderState,
    name: &str,
    meta: &str,
    meta_ink: MetaInk,
    w: u32,
    h: u32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> Cartoon {
    // The source follows the sheet in force at every painter entry (r2 A-F2).
    gs.set_kerning(sheet.kerning);
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return cart;
    }
    let i = &sheet.inst;
    let hi = h as i32;
    let hair = sheet.hairline.max(1);
    let r = header_regions(w, h, sheet);
    let ground = if st.expanded {
        sheet.derived.open_header
    } else if st.hovered {
        i.hover
    } else {
        i.header
    };
    cart.ops.push(Op::Clear { color: ground });
    if !st.expanded && !st.last {
        cart.ops.push(Op::Rect {
            x: 0,
            y: (hi - hair).max(0),
            w,
            h: hair as u32,
            color: i.separator,
        });
    }
    // The index box's right rule.
    if r.index_w > 0 {
        cart.ops.push(Op::Rect {
            x: (r.index_w - hair).max(0),
            y: 0,
            w: hair as u32,
            h,
            color: i.separator,
        });
    }
    // The focus mark.
    if st.focused && st.expanded {
        let m = &sheet.metrics;
        let inset = m.mark_inset_y.max(0);
        let mh = (hi - 2 * inset).max(0);
        if mh > 0 {
            cart.ops.push(Op::Rect {
                x: 0,
                y: inset,
                w: m.mark_w.max(1) as u32,
                h: mh as u32,
                color: i.amber,
            });
        }
    }
    let gen = gs.gen();
    let mono = sheet.face_mono_text;
    let mono_px = sheet.chrome_mono_px;
    // The index, centred in its box.
    {
        let mut num = String::new();
        let _ = core::fmt::write(&mut num, format_args!("{:02}", st.index));
        let (refs, width) = gs.shape_run(mono, mono_px, num.chars());
        if !refs.is_empty() && r.index_w > 0 {
            let x = ((r.index_w - hair - width) / 2).max(0);
            let base = centred_in(gs, mono, mono_px, hi);
            let ink = if st.focused && st.expanded { i.amber } else { i.dim };
            cart.push_glyphs(gen, x, base, ink, &refs);
        }
    }
    // The metadata, right-aligned before the action box, tracked .04 em.
    let mut meta_x = r.meta_right;
    if !meta.is_empty() {
        let (refs, width) = gs.shape_run_spaced(mono, mono_px, META_TRACK_EM * mono_px, meta.chars());
        let x = r.meta_right - width;
        if !refs.is_empty() && x >= r.name_x {
            let base = centred_in(gs, mono, mono_px, hi);
            cart.push_glyphs(gen, x, base, meta_ink_of(i, meta_ink), &refs);
            meta_x = x;
        }
    }
    // The name, in what is left, cut from its end: Sans 500 at 13, +.01 em.
    if !name.is_empty() {
        let face = sheet.face_medium;
        let px = sheet.px(NAME_INST_PX);
        let track = NAME_TRACK_EM * px;
        let avail = meta_x - sheet.metrics.header_gap - r.name_x;
        if avail > 0 {
            let text = fit_end_tracked(gs, face, px, track, name, avail);
            let (refs, _) = gs.shape_run_spaced(face, px, track, text.chars());
            if !refs.is_empty() {
                let base = centred_in(gs, face, px, hi);
                let ink = if st.expanded || st.hovered { i.text } else { i.secondary };
                cart.push_glyphs(gen, r.name_x, base, ink, &refs);
            }
        }
    }
    // The action.
    if st.expanded || st.hovered {
        let (ax, ay, aw, ah) = r.action;
        let face = sheet.face_body;
        let px = sheet.px(ACTION_PX);
        let (refs, width) = gs.shape_run(face, px, "\u{d7}".chars());
        if !refs.is_empty() && aw > 0 {
            if st.hover_close {
                cart.ops.push(Op::Rect {
                    x: ax,
                    y: ay,
                    w: hair as u32,
                    h: ah.max(0) as u32,
                    color: i.structure,
                });
            }
            let base = ay + centred_in(gs, face, px, ah);
            let ink = if st.hover_close { i.error } else { i.dim };
            cart.push_glyphs(gen, ax + (aw - width) / 2, base, ink, &refs);
        }
    }
    cart
}

fn meta_ink_of(i: &libhalcyon::instrument::InstrumentTheme, ink: MetaInk) -> Argb {
    match ink {
        MetaInk::Dim => i.dim,
        MetaInk::Secondary => i.secondary,
        MetaInk::Amber => i.amber,
        MetaInk::Error => i.error,
        MetaInk::Success => i.success,
    }
}

/// The empty pane's placard (14.6), logical: the text inset, the gap
/// between the two lines, the action's height and its horizontal padding.
const PLACARD_PAD: i32 = 20;
const PLACARD_GAP: i32 = 8;
const PLACARD_ACTION_H: i32 = 28;
const PLACARD_ACTION_PAD: i32 = 8;
const PLACARD_TITLE_PX: f32 = 17.0;
const PLACARD_BODY_PX: f32 = 13.0;
pub const PLACARD_TITLE: &str = "Empty pane";
pub const PLACARD_HINT: &str = "Open a shell to start here.";
pub const PLACARD_ACTION: &str = "Open shell";

/// The empty pane's placard display list (14.6): `pane` ground; at the
/// content's top-left, padding 20, `Empty pane` in Sans 17 `text`, gap 8,
/// `Open a shell to start here.` in Sans 13 `secondary`, then -- only
/// where the owner may spawn (`can_spawn`) -- the text-style action `Open
/// shell` in `amber`, 28 tall, horizontal padding 8, `hover` ground under
/// the pointer. Returns the action's rect (x, y, w, h) when it shows: the
/// hit test's input. No header, no mark, no index: a focused empty pane
/// gets its frame from the compositor and nothing else.
pub fn placard_list(
    can_spawn: bool,
    hover_action: bool,
    w: u32,
    h: u32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> (Cartoon, Option<(i32, i32, i32, i32)>) {
    // The source follows the sheet in force at every painter entry (r2 A-F2).
    gs.set_kerning(sheet.kerning);
    let mut cart = Cartoon::new();
    if w == 0 || h == 0 {
        return (cart, None);
    }
    let i = &sheet.inst;
    cart.ops.push(Op::Clear { color: i.pane });
    let gen = gs.gen();
    let pad = sheet.ipx(PLACARD_PAD);
    let gap = sheet.ipx(PLACARD_GAP);
    let (tpx, bpx) = (sheet.px(PLACARD_TITLE_PX), sheet.px(PLACARD_BODY_PX));
    let face = sheet.face_body;
    let mut y = pad;
    let line = |gs: &mut GlyphSource, px: f32| {
        gs.line_metrics(face, px)
            .map(|m| (m.ascent, m.ascent + m.descent))
            .unwrap_or((12, 16))
    };
    let (asc, lh) = line(gs, tpx);
    let (refs, _) = gs.shape_run(face, tpx, PLACARD_TITLE.chars());
    if !refs.is_empty() {
        cart.push_glyphs(gen, pad, y + asc, i.text, &refs);
    }
    y += lh + gap;
    let (asc, lh) = line(gs, bpx);
    let (refs, _) = gs.shape_run(face, bpx, PLACARD_HINT.chars());
    if !refs.is_empty() {
        cart.push_glyphs(gen, pad, y + asc, i.secondary, &refs);
    }
    y += lh + gap;
    if !can_spawn {
        return (cart, None);
    }
    let ah = sheet.ipx(PLACARD_ACTION_H);
    let apad = sheet.ipx(PLACARD_ACTION_PAD);
    let (refs, width) = gs.shape_run(face, bpx, PLACARD_ACTION.chars());
    let rect = (pad, y, width + 2 * apad, ah);
    if hover_action {
        cart.ops.push(Op::Rect {
            x: rect.0,
            y: rect.1,
            w: rect.2.max(0) as u32,
            h: rect.3.max(0) as u32,
            color: i.hover,
        });
    }
    if !refs.is_empty() {
        let base = y + centred_in(gs, face, bpx, ah);
        cart.push_glyphs(gen, pad + apad, base, i.amber, &refs);
    }
    (cart, Some(rect))
}

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::theme::DAYLIGHT;

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
            key_colors(&DAYLIGHT, Key::Resting),
            (0xFFCEC4B6, 0xFFC86030, 0xFF1A120A)
        );
        assert_eq!(
            key_colors(&DAYLIGHT, Key::Sage),
            (0xFFB8CCC4, 0xFF1E5844, 0xFF0C2820)
        );
        assert_eq!(
            key_colors(&DAYLIGHT, Key::Cinnabar),
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
    // ---- HALCYON-INSTRUMENT 6.4 / 7.3 / 14.6 (I-3) ----------------------------

    fn carbon() -> Sheet {
        crate::layout::sheet_for(
            &libhalcyon::instrument::Bundle::builtin(libhalcyon::instrument::Profile::Instrument),
            100,
            crate::layout::TEST_DISPLAY_W,
        )
    }

    /// The compositor's dump for [A | stack{b1, b2*, b3}] with a zoom-hidden
    /// leaf beside it: the lone leaf is a stack of one; the stack's children
    /// carry their index, count, open and last; a hidden-under-zoom leaf
    /// keeps `hidden` and reads as a stack of one of its own parent.
    // (No `\`-continued lines: a continuation strips the next line's
    // leading spaces, and the DEPTH is what this text is about.)
    const TREE: &str = "epoch 9 focused 5\n1 splith n=2 active=1 [0,0,1280,800]\n  2 leaf surface=0 [4,38,600,700]\n  3 stacked n=3 active=1 [0,0,0,0]\n    4 leaf surface=1 [0,0,0,0] hidden\n    5* leaf surface=2 [700,70,500,600]\n    6 leaf empty [0,0,0,0] w=3 hidden\n";

    #[test]
    fn parse_tree_reads_each_leafs_place_in_its_stack() {
        let t = parse_tree(TREE);
        assert_eq!(t.len(), 4);
        assert_eq!((t[0].leaf.id, t[0].index, t[0].count, t[0].open, t[0].last, t[0].empty), (2, 1, 1, true, true, false));
        assert_eq!((t[1].leaf.id, t[1].index, t[1].count, t[1].open, t[1].last), (4, 1, 3, false, false));
        assert!(t[1].leaf.hidden, "a collapsed tile is hidden");
        assert_eq!((t[2].leaf.id, t[2].index, t[2].count, t[2].open, t[2].last), (5, 2, 3, true, false));
        assert!(t[2].leaf.focused && !t[2].leaf.hidden);
        assert_eq!((t[3].leaf.id, t[3].index, t[3].count, t[3].open, t[3].last, t[3].empty), (6, 3, 3, false, true, true));
        // The legacy views are unchanged over the same text.
        assert_eq!(parse_leaves_all(TREE).len(), 4);
        assert_eq!(parse_leaves(TREE).len(), 2, "the two laid-out leaves");
        // A split's children are stacks of one even under a container.
        let t = parse_tree("epoch 1 focused 2\n1 splitv n=2 active=0 [0,0,1,1]\n  2* leaf empty [0,0,1,1]\n  3 leaf surface=4 [0,0,1,1]\n");
        assert!(t.iter().all(|x| x.index == 1 && x.count == 1 && x.open && x.last));
        assert!(t[0].empty && !t[1].empty);
    }

    /// 6.4 at 100 %, a 732 x 32 header: the index box 32, the name at 39
    /// (32 + the 7 gap), the metadata ending 35 from the right edge (the
    /// action's 28 + the gap -- reserved on every header, the golden's
    /// 658.70 + 43.20 = 701.9 of 736.91), the action box 28 x 24 at y 4.
    #[test]
    fn header_regions_reserve_the_action_box_and_hit_it() {
        let s = carbon();
        let r = header_regions(732, 32, &s);
        assert_eq!(r.index_w, 32);
        assert_eq!(r.name_x, 39);
        assert_eq!(r.meta_right, 697);
        assert_eq!(r.action, (704, 4, 28, 24));
        assert_eq!(header_hit(710, 10, 732, 32, &s), HeaderHit::Action);
        assert_eq!(header_hit(704, 4, 732, 32, &s), HeaderHit::Action);
        assert_eq!(header_hit(703, 10, 732, 32, &s), HeaderHit::Tile, "the gap before the box is the tile");
        assert_eq!(header_hit(710, 2, 732, 32, &s), HeaderHit::Tile, "above the 24 tall box");
        assert_eq!(header_hit(10, 10, 732, 32, &s), HeaderHit::Tile, "the index");
        assert_eq!(header_hit(300, 10, 732, 32, &s), HeaderHit::Tile, "the name");
        // A header narrower than its regions never yields a negative box.
        let r = header_regions(20, 32, &s);
        assert!(r.action.0 <= 0 && r.meta_right == 0 && r.index_w == 20);
    }

    /// 6.4 + 14.3 + 14.6: the metadata word and its ink, most decisive
    /// first: a retained tile's fate, then a running command, then a failed
    /// last command, then the trail (dim; amber when dirty).
    #[test]
    fn metadata_precedence_is_fate_then_running_then_failure_then_trail() {
        let mut d = Described::plain(String::from("ut"), String::from("~/src"));
        assert_eq!(metadata_for(&d), (String::from("~/SRC"), MetaInk::Dim));
        d.dirty = true;
        assert_eq!(metadata_for(&d).1, MetaInk::Amber);
        d.last_exit = Some(2);
        assert_eq!(metadata_for(&d), (String::from("EXIT 2"), MetaInk::Error), "failure over dirty");
        d.last_exit = Some(0);
        assert_eq!(metadata_for(&d).0, "~/SRC", "a clean exit is not a word");
        d.running = true;
        d.last_exit = Some(3);
        assert_eq!(metadata_for(&d), (String::from("RUNNING"), MetaInk::Secondary), "running over a stale failure");
        d.fate = Fate::Ended(0);
        assert_eq!(metadata_for(&d), (String::from("EXIT 0"), MetaInk::Success));
        d.fate = Fate::Ended(127);
        assert_eq!(metadata_for(&d), (String::from("EXIT 127"), MetaInk::Error));
        d.fate = Fate::Disconnected;
        assert_eq!(metadata_for(&d), (String::from("DISCONNECTED"), MetaInk::Error));
        d.fate = Fate::Crashed;
        assert_eq!(metadata_for(&d), (String::from("CRASHED"), MetaInk::Error));
        let s = carbon();
        assert_eq!(meta_ink(&s, MetaInk::Error), 0xFFBD_7770);
        assert_eq!(meta_ink(&s, MetaInk::Success), 0xFF81_9B85);
        assert_eq!(meta_ink(&s, MetaInk::Dim), 0xFF73_7A76);
    }

    /// The 7.3 matrix on Carbon, pinned against the golden's pixels: a
    /// collapsed header rests on `header` (8,10,11) with a `separator`
    /// (41,45,43) last row unless last in its stack and the index rule at
    /// x 31; the expanded focused header rests on `open_header` (21,24,25)
    /// with the 2 x 20 `amber` mark at (0, 6), the index in `amber`, the
    /// name in `text` and the `x` in `dim`; a hovered collapsed header
    /// rests on `hover` (25,28,29), lights its name and shows the `x`; the
    /// `x` under the pointer is `error` with a `structure` rule at the
    /// box's left edge.
    #[test]
    fn the_header_paints_the_state_matrix() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let st = |focused, expanded, hovered, hover_close, last| HeaderState {
            focused,
            expanded,
            hovered,
            hover_close,
            index: 2,
            last,
        };
        let rects = |c: &Cartoon| -> Vec<(i32, i32, u32, u32, Argb)> {
            c.ops
                .iter()
                .filter_map(|op| match *op {
                    Op::Rect { x, y, w, h, color } => Some((x, y, w, h, color)),
                    _ => None,
                })
                .collect()
        };
        // Collapsed, not last, resting.
        let c = header_list(st(false, false, false, false, false), "ut", "~/SRC", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF08_0A0B }));
        let rs = rects(&c);
        assert!(rs.contains(&(0, 31, 732, 1, 0xFF29_2D2B)), "the separator last row: {:?}", rs);
        assert!(rs.contains(&(31, 0, 1, 32, 0xFF29_2D2B)), "the index rule: {:?}", rs);
        assert!(!rs.iter().any(|r| r.4 == 0xFFC7_B98B), "no mark on a collapsed header");
        let r = runs(&c);
        assert_eq!(r.len(), 3, "index, meta, name -- no x: {:?}", r);
        assert!(r.iter().any(|x| x.1 == 0xFFAF_B4B0), "the name in secondary");
        assert!(r.iter().any(|x| x.1 == 0xFF73_7A76 && x.2 == 2), "the index in dim");
        // The metadata's type against the golden's `.tile-meta` box (7.2,
        // I-5): an 8-character run in mono 10 tracked .04 em is 43.203 --
        // 5.4 px a glyph, Cornucopia's 5 plus the .4 px -- so `MODIFIED`
        // lays 43 wide (the pen's truncation), where the 12 px island
        // cell laid it 48. Read back through the run's advances.
        let c8 = header_list(st(false, false, false, false, false), "ut", "MODIFIED", MetaInk::Dim, 732, 32, &s, &mut gs);
        let meta = runs(&c8).into_iter().find(|x| x.2 == 8).expect("the 8-glyph metadata run");
        let meta_w: i32 = {
            let start = c8
                .ops
                .iter()
                .find_map(|op| match *op {
                    Op::Glyphs { start, count, baseline_x, .. } if count == 8 && baseline_x == meta.0 => Some(start),
                    _ => None,
                })
                .unwrap();
            c8.runs[start as usize..start as usize + 8].iter().map(|g| g.advance).sum()
        };
        assert!((meta_w - 43).abs() <= 1, "MODIFIED at mono 10 + .04 em: {meta_w} vs 43.203");
        // Collapsed and LAST: no separator row.
        let c = header_list(st(false, false, false, false, true), "ut", "", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(!rects(&c).contains(&(0, 31, 732, 1, 0xFF29_2D2B)));
        // Expanded + focused.
        let c = header_list(st(true, true, false, false, false), "renderer.rs", "SRC", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF15_1819 }));
        let rs = rects(&c);
        assert!(rs.contains(&(0, 6, 2, 20, 0xFFC7_B98B)), "the focus mark: {:?}", rs);
        assert!(!rs.contains(&(0, 31, 732, 1, 0xFF29_2D2B)), "no separator on an expanded header");
        let r = runs(&c);
        assert_eq!(r.len(), 4, "index, meta, name, x: {:?}", r);
        assert!(r.iter().any(|x| x.1 == 0xFFC7_B98B && x.2 == 2), "the index in amber");
        assert!(r.iter().any(|x| x.1 == 0xFFF2_F3EF), "the name in text");
        let close = r.iter().find(|x| x.2 == 1 && x.0 >= 704).expect("the x inside the action box");
        assert_eq!(close.1, 0xFF73_7A76, "the x in dim");
        // Expanded, NOT focused: no mark, index dim.
        let c = header_list(st(false, true, false, false, false), "a", "", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(!rects(&c).contains(&(0, 6, 2, 20, 0xFFC7_B98B)));
        assert!(runs(&c).iter().any(|x| x.1 == 0xFF73_7A76 && x.2 == 2));
        // Hovered collapsed: `hover` ground, name lit, x shown in dim.
        let c = header_list(st(false, false, true, false, false), "ut", "", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF19_1C1D }));
        let r = runs(&c);
        assert!(r.iter().any(|x| x.1 == 0xFFF2_F3EF), "the hovered name in text");
        assert!(r.iter().any(|x| x.2 == 1 && x.0 >= 704 && x.1 == 0xFF73_7A76), "the x appears");
        // Hovered expanded: the ground does not change.
        let c = header_list(st(true, true, true, false, false), "ut", "", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF15_1819 }));
        // The x under the pointer: error ink + the structure rule.
        let c = header_list(st(true, true, true, true, false), "ut", "", MetaInk::Dim, 732, 32, &s, &mut gs);
        assert!(runs(&c).iter().any(|x| x.2 == 1 && x.0 >= 704 && x.1 == 0xFFBD_7770));
        assert!(rects(&c).contains(&(704, 4, 1, 24, 0xFF45_4B48)), "the rule at the box's left edge");
        // The metadata ink follows its role; the name gives way to it.
        let c = header_list(st(false, false, false, false, false), "a-very-long-program-name-that-will-not-fit", "EXIT 1", MetaInk::Error, 200, 32, &s, &mut gs);
        let r = runs(&c);
        let meta = r.iter().find(|x| x.1 == 0xFFBD_7770).expect("the metadata in error");
        let name = r.iter().find(|x| x.1 == 0xFFAF_B4B0).expect("the name");
        assert!(name.0 + name.3 <= meta.0 - 7, "the name ends before the gap: {:?}", r);
        assert!(name.2 < "a-very-long-program-name-that-will-not-fit".chars().count(), "the name was cut");
        // Degenerate sizes yield nothing.
        assert!(header_list(st(true, true, false, false, false), "ut", "", MetaInk::Dim, 0, 32, &s, &mut gs).ops.is_empty());
    }

    /// 14.6: the placard's ground is `pane`, its two lines `text` then
    /// `secondary` at the 20 px inset, and the `Open shell` action shows only
    /// where the owner may spawn (a console renderer never), `amber`, on a
    /// `hover` band under the pointer.
    #[test]
    fn the_placard_offers_a_shell_only_where_it_can_spawn() {
        let s = carbon();
        let mut gs = GlyphSource::new_vendored(64);
        let (c, act) = placard_list(false, false, 600, 400, &s, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF0B_0D0E }));
        assert!(act.is_none());
        let r = runs(&c);
        assert_eq!(r.len(), 2);
        assert_eq!((r[0].0, r[0].1), (20, 0xFFF2_F3EF), "the title at the inset in text");
        assert_eq!((r[1].0, r[1].1), (20, 0xFFAF_B4B0), "the hint in secondary");
        let (c, act) = placard_list(true, false, 600, 400, &s, &mut gs);
        let (ax, ay, aw, ah) = act.expect("the action shows");
        assert_eq!((ax, ah), (20, 28));
        assert!(ay > r[1].0 && aw > 16, "below the hint, wider than its padding: {:?}", act);
        let r = runs(&c);
        assert_eq!(r.len(), 3);
        assert_eq!(r[2].1, 0xFFC7_B98B, "the action in amber");
        assert_eq!(r[2].0, 28, "the label at the action's padding");
        assert!(!c.ops.iter().any(|op| matches!(op, Op::Rect { color: 0xFF19_1C1D, .. })));
        let (c, _) = placard_list(true, true, 600, 400, &s, &mut gs);
        assert!(c.ops.iter().any(|op| matches!(op, Op::Rect { x: 20, h: 28, color: 0xFF19_1C1D, .. })), "the hover band");
        assert!(placard_list(true, true, 0, 0, &s, &mut gs).0.ops.is_empty());
    }
}
