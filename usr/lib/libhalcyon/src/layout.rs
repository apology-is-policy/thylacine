// layout -- the `halcyon-layout v1` / `v2` save format (HALCYON.md 13.7,
// H-4; HALCYON-INSTRUMENT 5.3).
//
// The pure half of layout save/restore: a bounded, no-panic serializer +
// parser for the pane tree's SHAPE (container modes + active child; per-leaf
// tag = the command line; since I-2 each child's WEIGHT in its parent).
// Surface ids are runtime, never saved -- a restored leaf gets a fresh
// surface from the respawned program. Shared by halcyond (the device-tier
// restore + the gesture) and the user-authority session tool (the
// session-tier save/restore, the D decision), so it lives here in libhalcyon
// rather than in either.
//
// v2 = v1 plus ` w=<weight>` on a row whose node carries a non-default
// weight. The writer emits the v2 header only when some weight is
// non-default, so a tree with equal weights is byte-identical v1 and an old
// reader keeps reading it; the v1 reader stays and a v1 file loads with equal
// weights. A ` w=` under a v1 header is refused: the header says what the
// rows may carry.
//
// The parser reads UNTRUSTED input (a layout file in the user's $home): every
// path is bounded and fail-closed -- a malformed or oversize file returns an
// Err the caller degrades on (geometry-only, or no restore), NEVER a panic
// (a panic in a no_std tool is a silent exit(1)).

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;

use crate::carve::DEFAULT_WEIGHT;

/// How many workspaces may exist (HALCYON-WORKSPACES 4). It lives HERE for
/// the reason this module already lives here: the compositor ENFORCES the
/// bound and the renderer must not accept a header that exceeds it, and both
/// link libhalcyon while neither links the other.
///
/// It was two hand-copied consts until 2026-09-15 (r3 F4). That drift is
/// fail-closed but INVISIBLE: had the compositor's bound risen alone, the
/// reader would have rejected legal headers and the bar would have silently
/// kept its default -- wrong at the only place a user can see it. A comment
/// naming the hazard does not prevent it; one definition does.
pub const MAX_WORKSPACES: usize = 9;

/// The format's first line (exact match required): v1, every weight default.
pub const FMT_HEADER: &str = "halcyon-layout v1";
/// The v2 header: rows may carry ` w=<weight>` (HALCYON-INSTRUMENT 5.3).
pub const FMT_HEADER_V2: &str = "halcyon-layout v2";
/// Container nesting cap (a hostile file cannot exhaust the parse stack; the
/// real tree is far shallower -- a handful of splits).
pub const MAX_DEPTH: usize = 32;
/// Total node cap (leaves + containers), sized at the compositor's pane cap.
pub const MAX_NODES: usize = 256;
/// Per-leaf tag cap (the command line), the Beacon VALUE_MAX order.
pub const MAX_TAG_LEN: usize = 1024;

/// A container's layout mode -- the same tokens tapestryd's `pane::Mode`
/// serializes (`splith`/`splitv`/`tabbed`/`stacked`), so the two interoperate
/// through the format string without libhalcyon depending on tapestryd.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LayoutMode {
    SplitH,
    SplitV,
    Tabbed,
    Stacked,
}

impl LayoutMode {
    pub fn name(self) -> &'static str {
        match self {
            LayoutMode::SplitH => "splith",
            LayoutMode::SplitV => "splitv",
            LayoutMode::Tabbed => "tabbed",
            LayoutMode::Stacked => "stacked",
        }
    }
    pub fn parse(s: &str) -> Option<LayoutMode> {
        match s {
            "splith" => Some(LayoutMode::SplitH),
            "splitv" => Some(LayoutMode::SplitV),
            "tabbed" => Some(LayoutMode::Tabbed),
            "stacked" => Some(LayoutMode::Stacked),
            _ => None,
        }
    }
}

/// A node of the saved tree: a leaf carrying its program's command line (the
/// tag; empty = an empty pane), or a container with its mode, active-child
/// index, and children. Either carries its `weight` in its parent (5.2:
/// `u16`, sum-normalised, `DEFAULT_WEIGHT` when equal; the root's is
/// meaningless and written only if someone set it).
///
/// `env` marks a leaf whose tile was NOT the saving session's at save time --
/// the environment's (the console, a transcript pane) or another principal's.
/// A session restore never respawns such a leaf (it is not the session's to
/// provide); the marker keeps the saved file a faithful picture of the whole
/// screen, so a future environment-driven restore can still place it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum LayoutNode {
    Leaf {
        tag: String,
        env: bool,
        weight: u16,
    },
    Container {
        mode: LayoutMode,
        active: u32,
        children: Vec<LayoutNode>,
        weight: u16,
    },
}

impl LayoutNode {
    /// The node's weight in its parent.
    pub fn weight(&self) -> u16 {
        match self {
            LayoutNode::Leaf { weight, .. } | LayoutNode::Container { weight, .. } => *weight,
        }
    }
}

/// Does any node carry a non-default weight (the v2 header's condition)?
fn has_weights(node: &LayoutNode) -> bool {
    match node {
        LayoutNode::Leaf { weight, .. } => *weight != DEFAULT_WEIGHT,
        LayoutNode::Container {
            weight, children, ..
        } => *weight != DEFAULT_WEIGHT || children.iter().any(has_weights),
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ParseError {
    /// The first line was not exactly `halcyon-layout v1`.
    BadHeader,
    /// A row's indent was odd, or did not follow its parent by one level.
    BadIndent,
    /// A row was neither a `leaf` nor a `<mode> n=.. active=..` container.
    BadRow,
    /// Nesting exceeded MAX_DEPTH.
    TooDeep,
    /// Node count exceeded MAX_NODES.
    TooMany,
    /// A tag exceeded MAX_TAG_LEN (decoded).
    TagTooLong,
    /// No node at all (only a header, or empty input).
    Empty,
    /// A second depth-0 row (a layout has exactly one root).
    Trailing,
    /// A container's declared `n=` did not match its actual child count, or a
    /// container had zero children.
    BadChildCount,
    /// A ` w=` was not `1..=65535`, or appeared under the v1 header.
    BadWeight,
}

/// Serialize a tree to the `halcyon-layout` format: the header (v1, or v2
/// iff some weight is non-default), then one pre-order row per node, two
/// spaces of indent per depth. A leaf is `leaf` (empty tag) or `leaf
/// tag="<escaped>"`, with a trailing ` env` when the tile was the
/// environment's; a container is `<mode> n=<count> active=<idx>` followed by
/// its children; a non-default weight ends its row as ` w=<weight>`.
pub fn serialize(root: &LayoutNode) -> String {
    let mut s = String::new();
    s.push_str(if has_weights(root) {
        FMT_HEADER_V2
    } else {
        FMT_HEADER
    });
    s.push('\n');
    ser_node(root, 0, &mut s);
    s
}

fn ser_node(node: &LayoutNode, depth: usize, out: &mut String) {
    for _ in 0..depth {
        out.push_str("  ");
    }
    match node {
        LayoutNode::Leaf { tag, env, weight } => {
            out.push_str("leaf");
            if !tag.is_empty() {
                out.push_str(" tag=\"");
                escape_into(tag, out);
                out.push('"');
            }
            if *env {
                out.push_str(" env");
            }
            if *weight != DEFAULT_WEIGHT {
                let _ = write!(out, " w={}", weight);
            }
            out.push('\n');
        }
        LayoutNode::Container {
            mode,
            active,
            children,
            weight,
        } => {
            let _ = write!(
                out,
                "{} n={} active={}",
                mode.name(),
                children.len(),
                active
            );
            if *weight != DEFAULT_WEIGHT {
                let _ = write!(out, " w={}", weight);
            }
            out.push('\n');
            for c in children {
                ser_node(c, depth + 1, out);
            }
        }
    }
}

/// Backslash-escape a tag for the `tag="..."` field: `\` and `"` are escaped,
/// and a newline (which would break the line-oriented format) becomes `\n`.
fn escape_into(s: &str, out: &mut String) {
    for ch in s.chars() {
        match ch {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            _ => out.push(ch),
        }
    }
}

/// The SESSION's part of a saved tree: drop every `env` leaf (a tile the
/// session cannot provide -- the console, another principal's), dissolve the
/// containers that end up with one child (the compositor would dissolve them
/// too), and re-aim each surviving container's `active` at the same child
/// (or the first, when the active child was pruned). `None` when nothing is
/// the session's (a console-only save) -- there is nothing to restore.
pub fn prune_env(node: &LayoutNode) -> Option<LayoutNode> {
    match node {
        LayoutNode::Leaf { env: true, .. } => None,
        LayoutNode::Leaf { .. } => Some(node.clone()),
        LayoutNode::Container {
            mode,
            active,
            children,
            weight,
        } => {
            let mut kept: Vec<LayoutNode> = Vec::new();
            let mut new_active: Option<u32> = None;
            for (i, c) in children.iter().enumerate() {
                if let Some(k) = prune_env(c) {
                    if i as u32 == *active {
                        new_active = Some(kept.len() as u32);
                    }
                    kept.push(k);
                }
            }
            match kept.len() {
                0 => None,
                // The survivor takes the container's place in ITS parent, and
                // its weight there (the compositor's dissolve rule).
                1 => kept.pop().map(|k| k.with_weight(*weight)),
                _ => Some(LayoutNode::Container {
                    mode: *mode,
                    active: new_active.unwrap_or(0),
                    children: kept,
                    weight: *weight,
                }),
            }
        }
    }
}

impl LayoutNode {
    fn with_weight(mut self, w: u16) -> LayoutNode {
        match &mut self {
            LayoutNode::Leaf { weight, .. } | LayoutNode::Container { weight, .. } => *weight = w,
        }
        self
    }
}

struct Frame {
    depth: usize,
    mode: LayoutMode,
    active: u32,
    n: u32,
    weight: u16,
    children: Vec<LayoutNode>,
}

enum Row {
    Leaf(String, bool, u16),
    Cont(LayoutMode, u32, u32, u16),
}

/// How many `env` leaves the tree holds.
fn env_count(node: &LayoutNode) -> usize {
    match node {
        LayoutNode::Leaf { env, .. } => usize::from(*env),
        LayoutNode::Container { children, .. } => children.iter().map(env_count).sum(),
    }
}

/// H-4d: does the saved tree place the ENVIRONMENT's tile -- the one tile
/// already there when the restore runs (the anchor the skeleton grows
/// beside) -- LAST among the root's children, and nowhere else? A restore
/// then moves the anchor past the part it built, so `splith [tour, env]`
/// comes up as tour LEFT, the existing shell RIGHT (the welcome's shape).
/// Any other placement of the env leaf keeps the default: the built part
/// lands after the anchor.
pub fn anchor_last(root: &LayoutNode) -> bool {
    match root {
        LayoutNode::Container { children, .. } => {
            matches!(children.last(), Some(LayoutNode::Leaf { env: true, .. }))
                && env_count(root) == 1
        }
        LayoutNode::Leaf { .. } => false,
    }
}

/// H-4d: is the root container's ACTIVE child an env leaf -- the saved
/// focus on the environment's own tile (the welcome leaves the user at the
/// shell prompt, not on the tour)?
pub fn active_is_env(root: &LayoutNode) -> bool {
    match root {
        LayoutNode::Container {
            active, children, ..
        } => matches!(
            children.get(*active as usize),
            Some(LayoutNode::Leaf { env: true, .. })
        ),
        LayoutNode::Leaf { .. } => false,
    }
}

/// Parse the `halcyon-layout v1` / `v2` format. Bounded + fail-closed on
/// every path; a v1 file loads with every weight default.
pub fn parse(input: &str) -> Result<LayoutNode, ParseError> {
    let mut lines = input.split('\n');
    let header = lines.next().unwrap_or("");
    let weighted = match header.trim_end_matches('\r') {
        h if h == FMT_HEADER => false,
        h if h == FMT_HEADER_V2 => true,
        _ => return Err(ParseError::BadHeader),
    };
    let mut rows: Vec<(usize, Row)> = Vec::new();
    for raw in lines {
        let line = raw.trim_end_matches('\r');
        if line.is_empty() {
            continue; // blank lines (incl. a trailing newline's tail) ignored
        }
        rows.push(parse_row(line, weighted)?);
        if rows.len() > MAX_NODES {
            return Err(ParseError::TooMany);
        }
    }
    assemble(rows)
}

/// Build a `LayoutNode` tree from the DUMP `pane::render_text` produces
/// (HALCYON.md 13.7, the D-decision read side): the same depth-indented
/// pre-order, but each row leads with the pane `<id>` (+ an optional `*` focus
/// marker), a leaf reads `leaf surface=<n>|empty` (its tag is NOT in the dump),
/// and a trailing ` [x,y,w,h]` rect we discard -- geometry is never saved, a
/// restored leaf gets a fresh surface and rect. `leaf_of` resolves each leaf
/// by id to its command line + its `env` marker (the save tool reads
/// `pane/<id>/tag` and compares `pane/<id>/owner` to its own principal); a tag
/// longer than MAX_TAG_LEN is dropped to empty so the result always
/// round-trips through serialize/parse. A row's ` w=<weight>` (emitted after
/// the rect when the child's weight is non-default) is read wherever it
/// falls among the trailing tokens. Bounded + fail-closed exactly like
/// `parse`, so a garbled dump degrades (no save) rather than panicking (a
/// silent exit in a no_std tool). This is the WRITE side's inverse of `parse`:
/// `render_text` in -> `serialize` out.
pub fn from_render_text(
    render: &str,
    leaf_of: impl Fn(u32) -> (String, bool),
) -> Result<LayoutNode, ParseError> {
    let mut lines = render.split('\n');
    let header = lines.next().unwrap_or("");
    // render_text's first line is `epoch <n> focused <m> [zoomed <z>]`.
    if !header.trim_end_matches('\r').starts_with("epoch ") {
        return Err(ParseError::BadHeader);
    }
    let mut rows: Vec<(usize, Row)> = Vec::new();
    for raw in lines {
        let line = raw.trim_end_matches('\r');
        if line.is_empty() {
            continue;
        }
        rows.push(parse_render_row(line, &leaf_of)?);
        if rows.len() > MAX_NODES {
            return Err(ParseError::TooMany);
        }
    }
    assemble(rows)
}

/// The stack machine shared by `parse` and `from_render_text`: fold a pre-order
/// (depth, row) stream into the tree, closing every open container the moment a
/// row at its depth-or-shallower arrives, and validating each container's child
/// count as it closes. Depth/node/tag bounds are enforced upstream by the row
/// tokenizers; this stage owns only the tree shape.
fn assemble(rows: Vec<(usize, Row)>) -> Result<LayoutNode, ParseError> {
    let mut stack: Vec<Frame> = Vec::new();
    let mut root: Option<LayoutNode> = None;
    for (depth, row) in rows {
        // Close every open container at this depth or deeper (complete).
        while stack.last().is_some_and(|f| f.depth >= depth) {
            let f = stack.pop().unwrap();
            let fd = f.depth;
            let node = finalize(f)?;
            attach(&mut stack, &mut root, node, fd)?;
        }
        // The parent must now sit exactly one level up (or none at depth 0).
        if depth == 0 {
            if root.is_some() || !stack.is_empty() {
                return Err(ParseError::Trailing);
            }
        } else if stack.last().is_none_or(|f| f.depth != depth - 1) {
            return Err(ParseError::BadIndent);
        }
        match row {
            Row::Leaf(tag, env, weight) => attach(
                &mut stack,
                &mut root,
                LayoutNode::Leaf { tag, env, weight },
                depth,
            )?,
            Row::Cont(mode, n, active, weight) => stack.push(Frame {
                depth,
                mode,
                active,
                n,
                weight,
                children: Vec::new(),
            }),
        }
    }
    // Drain the open containers (deepest first).
    while let Some(f) = stack.pop() {
        let fd = f.depth;
        let node = finalize(f)?;
        attach(&mut stack, &mut root, node, fd)?;
    }
    root.ok_or(ParseError::Empty)
}

/// Attach a finished node to its parent (the current stack top, at
/// `depth - 1`) or make it the root (depth 0).
fn attach(
    stack: &mut [Frame],
    root: &mut Option<LayoutNode>,
    node: LayoutNode,
    depth: usize,
) -> Result<(), ParseError> {
    if depth == 0 {
        if root.is_some() {
            return Err(ParseError::Trailing);
        }
        *root = Some(node);
    } else {
        match stack.last_mut() {
            Some(f) if f.depth == depth - 1 => f.children.push(node),
            _ => return Err(ParseError::BadIndent),
        }
    }
    Ok(())
}

/// A container's `n=` must equal its actual child count, it must have children,
/// and its active index is clamped into range (a slightly-off active must not
/// fail the whole restore).
fn finalize(f: Frame) -> Result<LayoutNode, ParseError> {
    if f.children.is_empty() || f.children.len() != f.n as usize {
        return Err(ParseError::BadChildCount);
    }
    let active = f.active.min(f.n - 1);
    Ok(LayoutNode::Container {
        mode: f.mode,
        active,
        children: f.children,
        weight: f.weight,
    })
}

/// A `w=<weight>` token's value: `1..=65535`, digits only.
fn parse_weight(tok: &str) -> Result<u16, ParseError> {
    let v = tok.strip_prefix("w=").ok_or(ParseError::BadRow)?;
    if v.is_empty() || !v.bytes().all(|b| b.is_ascii_digit()) {
        return Err(ParseError::BadWeight);
    }
    match v.parse::<u16>() {
        Ok(w) if w >= 1 => Ok(w),
        _ => Err(ParseError::BadWeight),
    }
}

/// The optional ` w=<weight>` a row may end with: absent = the default;
/// present under a v1 header = refused.
fn parse_row_weight(tok: Option<&str>, weighted: bool) -> Result<u16, ParseError> {
    match tok {
        None => Ok(DEFAULT_WEIGHT),
        Some(t) if t.starts_with("w=") => {
            if !weighted {
                return Err(ParseError::BadWeight);
            }
            parse_weight(t)
        }
        Some(_) => Err(ParseError::BadRow),
    }
}

/// Tokenize one row into its depth (leading-space pairs) and its content.
fn parse_row(line: &str, weighted: bool) -> Result<(usize, Row), ParseError> {
    let spaces = line.len() - line.trim_start_matches(' ').len();
    if !spaces.is_multiple_of(2) {
        return Err(ParseError::BadIndent);
    }
    let depth = spaces / 2;
    if depth > MAX_DEPTH {
        return Err(ParseError::TooDeep);
    }
    let rest = &line[spaces..];
    if let Some(tail) = rest.strip_prefix("leaf tag=\"") {
        let (tag, after) = parse_tag(tail)?;
        let (env, weight) = parse_leaf_tail(after, weighted)?;
        return Ok((depth, Row::Leaf(tag, env, weight)));
    }
    if let Some(after) = rest.strip_prefix("leaf") {
        let (env, weight) = parse_leaf_tail(after, weighted)?;
        return Ok((depth, Row::Leaf(String::new(), env, weight)));
    }
    // A container: `<mode> n=<num> active=<num>[ w=<weight>]`.
    let mut it = rest.split(' ');
    let mode = it
        .next()
        .and_then(LayoutMode::parse)
        .ok_or(ParseError::BadRow)?;
    let n = it
        .next()
        .and_then(|t| t.strip_prefix("n="))
        .and_then(|v| v.parse::<u32>().ok())
        .ok_or(ParseError::BadRow)?;
    let active = it
        .next()
        .and_then(|t| t.strip_prefix("active="))
        .and_then(|v| v.parse::<u32>().ok())
        .ok_or(ParseError::BadRow)?;
    let weight = parse_row_weight(it.next(), weighted)?;
    if it.next().is_some() {
        return Err(ParseError::BadRow); // trailing tokens
    }
    if n as usize > MAX_NODES {
        return Err(ParseError::TooMany);
    }
    Ok((depth, Row::Cont(mode, n, active, weight)))
}

/// What may follow `leaf` or a leaf's closing quote: nothing, ` env`,
/// ` w=<weight>`, or ` env w=<weight>` -- exactly one space before each.
fn parse_leaf_tail(after: &str, weighted: bool) -> Result<(bool, u16), ParseError> {
    if after.is_empty() {
        return Ok((false, DEFAULT_WEIGHT));
    }
    let toks = after.strip_prefix(' ').ok_or(ParseError::BadRow)?;
    let mut it = toks.split(' ');
    let first = it.next().ok_or(ParseError::BadRow)?;
    let (env, wtok) = if first == "env" {
        (true, it.next())
    } else {
        (false, Some(first))
    };
    let weight = parse_row_weight(wtok, weighted)?;
    if it.next().is_some() {
        return Err(ParseError::BadRow);
    }
    Ok((env, weight))
}

/// Tokenize one `render_text` row: leading-space pairs -> depth, then
/// `<id>[*] leaf ...` or `<id>[*] <mode> n=<num> active=<num> ...`. The leaf's
/// tag + env marker come from `leaf_of(id)` (render_text carries neither); a
/// ` w=<weight>` token is read wherever it falls; surface/geometry tokens are
/// read past and discarded.
fn parse_render_row(
    line: &str,
    leaf_of: &impl Fn(u32) -> (String, bool),
) -> Result<(usize, Row), ParseError> {
    let spaces = line.len() - line.trim_start_matches(' ').len();
    if !spaces.is_multiple_of(2) {
        return Err(ParseError::BadIndent);
    }
    let depth = spaces / 2;
    if depth > MAX_DEPTH {
        return Err(ParseError::TooDeep);
    }
    let mut it = line[spaces..].split(' ');
    // The pane id, with an optional trailing `*` focus marker stripped.
    let id = it
        .next()
        .map(|t| t.strip_suffix('*').unwrap_or(t))
        .and_then(|t| t.parse::<u32>().ok())
        .ok_or(ParseError::BadRow)?;
    // The weight rides after the rect (`... [x,y,w,h] w=515 hidden`), so the
    // token is looked for among the rest, not at a position.
    let render_weight = |it: core::str::Split<'_, char>| -> Result<u16, ParseError> {
        let mut w = DEFAULT_WEIGHT;
        for t in it {
            if t.starts_with("w=") {
                w = parse_weight(t)?;
            }
        }
        Ok(w)
    };
    match it.next() {
        Some("leaf") => {
            let (tag, env) = leaf_of(id);
            // A tag past the format's cap is dropped, never truncated: a
            // half-command would respawn wrong, whereas an empty leaf is a
            // clean placeholder -- and the output must round-trip through parse.
            let tag = if tag.len() > MAX_TAG_LEN {
                String::new()
            } else {
                tag
            };
            let weight = render_weight(it)?;
            Ok((depth, Row::Leaf(tag, env, weight)))
        }
        Some(tok) => {
            let mode = LayoutMode::parse(tok).ok_or(ParseError::BadRow)?;
            let n = it
                .next()
                .and_then(|t| t.strip_prefix("n="))
                .and_then(|v| v.parse::<u32>().ok())
                .ok_or(ParseError::BadRow)?;
            let active = it
                .next()
                .and_then(|t| t.strip_prefix("active="))
                .and_then(|v| v.parse::<u32>().ok())
                .ok_or(ParseError::BadRow)?;
            if n as usize > MAX_NODES {
                return Err(ParseError::TooMany);
            }
            let weight = render_weight(it)?;
            Ok((depth, Row::Cont(mode, n, active, weight)))
        }
        None => Err(ParseError::BadRow),
    }
}

/// Parse the body of `leaf tag="..."` (everything after the opening quote):
/// unescape `\\`/`\"`/`\n` up to the closing unescaped quote; returns the
/// tag and what follows the quote (the row's tail: nothing, ` env`, ` w=`).
fn parse_tag(tail: &str) -> Result<(String, &str), ParseError> {
    let mut out = String::new();
    let mut chars = tail.char_indices();
    while let Some((i, ch)) = chars.next() {
        match ch {
            '"' => {
                if out.len() > MAX_TAG_LEN {
                    return Err(ParseError::TagTooLong);
                }
                return Ok((out, &tail[i + 1..]));
            }
            '\\' => match chars.next() {
                Some((_, '\\')) => out.push('\\'),
                Some((_, '"')) => out.push('"'),
                Some((_, 'n')) => out.push('\n'),
                _ => return Err(ParseError::BadRow), // dangling / unknown escape
            },
            _ => out.push(ch),
        }
        if out.len() > MAX_TAG_LEN {
            return Err(ParseError::TagTooLong);
        }
    }
    Err(ParseError::BadRow) // unterminated tag
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    fn leaf(t: &str) -> LayoutNode {
        LayoutNode::Leaf {
            tag: t.to_string(),
            env: false,
            weight: DEFAULT_WEIGHT,
        }
    }
    fn env_leaf(t: &str) -> LayoutNode {
        LayoutNode::Leaf {
            tag: t.to_string(),
            env: true,
            weight: DEFAULT_WEIGHT,
        }
    }
    fn cont(m: LayoutMode, a: u32, c: Vec<LayoutNode>) -> LayoutNode {
        LayoutNode::Container {
            mode: m,
            active: a,
            children: c,
            weight: DEFAULT_WEIGHT,
        }
    }
    fn w(n: LayoutNode, weight: u16) -> LayoutNode {
        n.with_weight(weight)
    }

    fn roundtrip(n: &LayoutNode) {
        let s = serialize(n);
        assert!(s.starts_with("halcyon-layout v1\n"), "header: {:?}", s);
        let back = parse(&s).expect("parse own output");
        assert_eq!(&back, n, "round-trip\n---\n{}\n---", s);
    }

    #[test]
    fn a_single_leaf_round_trips() {
        roundtrip(&leaf(""));
        roundtrip(&leaf("ut"));
        roundtrip(&leaf("hx /lib/aurora/config"));
    }

    #[test]
    fn the_two_pane_welcome_round_trips() {
        // The shipped default: SplitH { welcome | ut }, welcome focused.
        let t = cont(
            LayoutMode::SplitH,
            0,
            vec![leaf("halcyon welcome"), leaf("ut")],
        );
        roundtrip(&t);
        // Its exact serialization, pinned.
        assert_eq!(
            serialize(&t),
            "halcyon-layout v1\nsplith n=2 active=0\n  leaf tag=\"halcyon welcome\"\n  leaf tag=\"ut\"\n"
        );
    }

    #[test]
    fn a_deep_nested_tree_round_trips() {
        let t = cont(
            LayoutMode::SplitH,
            1,
            vec![
                leaf("ut"),
                cont(
                    LayoutMode::Tabbed,
                    2,
                    vec![
                        leaf("hx a"),
                        leaf("hx b"),
                        cont(LayoutMode::SplitV, 0, vec![leaf("top"), leaf("")]),
                    ],
                ),
            ],
        );
        roundtrip(&t);
    }

    #[test]
    fn a_tag_with_quotes_backslashes_and_a_newline_round_trips() {
        roundtrip(&leaf(r#"echo "hi" \ there"#));
        roundtrip(&leaf("line1\nline2"));
        // The escape is exactly the three sequences, nothing else.
        assert_eq!(
            serialize(&leaf("a\"b\\c\nd")),
            "halcyon-layout v1\nleaf tag=\"a\\\"b\\\\c\\nd\"\n"
        );
    }

    #[test]
    fn active_is_clamped_not_fatal() {
        // A file naming active=9 on a 2-child container restores with active
        // clamped to the last child, never an out-of-range index or an Err.
        let s = "halcyon-layout v1\nsplith n=2 active=9\n  leaf\n  leaf\n";
        match parse(s).expect("clamp, not fail") {
            LayoutNode::Container { active, .. } => assert_eq!(active, 1),
            _ => panic!("expected a container"),
        }
    }

    #[test]
    fn malformed_inputs_error_and_never_panic() {
        // These are the untrusted-file cases: each must be an Err, no panic.
        assert_eq!(parse(""), Err(ParseError::BadHeader));
        assert_eq!(
            parse("halcyon-layout v3\nleaf\n"),
            Err(ParseError::BadHeader)
        );
        assert_eq!(
            parse("halcyon-layout v2 \nleaf\n"),
            Err(ParseError::BadHeader)
        );
        assert_eq!(parse("halcyon-layout v1\n"), Err(ParseError::Empty));
        // Odd indent.
        assert_eq!(
            parse("halcyon-layout v1\n leaf\n"),
            Err(ParseError::BadIndent)
        );
        // A child with no parent one level up (jumps two levels).
        assert_eq!(
            parse("halcyon-layout v1\nsplith n=1 active=0\n    leaf\n"),
            Err(ParseError::BadIndent)
        );
        // n= mismatch (says 2, has 1).
        assert_eq!(
            parse("halcyon-layout v1\nsplith n=2 active=0\n  leaf\n"),
            Err(ParseError::BadChildCount)
        );
        // A container with zero children.
        assert_eq!(
            parse("halcyon-layout v1\nsplith n=0 active=0\n"),
            Err(ParseError::BadChildCount)
        );
        // Two roots.
        assert_eq!(
            parse("halcyon-layout v1\nleaf\nleaf\n"),
            Err(ParseError::Trailing)
        );
        // Unterminated tag.
        assert_eq!(
            parse("halcyon-layout v1\nleaf tag=\"oops\n"),
            Err(ParseError::BadRow)
        );
        // Junk after the closing quote.
        assert_eq!(
            parse("halcyon-layout v1\nleaf tag=\"x\" junk\n"),
            Err(ParseError::BadRow)
        );
        // Unknown mode.
        assert_eq!(
            parse("halcyon-layout v1\nfloaty n=1 active=0\n  leaf\n"),
            Err(ParseError::BadRow)
        );
        // Dangling escape.
        assert_eq!(
            parse("halcyon-layout v1\nleaf tag=\"a\\\"\n"),
            Err(ParseError::BadRow)
        );
    }

    #[test]
    fn bounds_are_enforced() {
        // Too deep: MAX_DEPTH+1 nested containers.
        let mut s = String::from("halcyon-layout v1\n");
        for d in 0..=MAX_DEPTH + 1 {
            for _ in 0..d {
                s.push_str("  ");
            }
            let _ = writeln!(s, "splith n=1 active=0");
        }
        assert_eq!(parse(&s), Err(ParseError::TooDeep));
        // Tag too long.
        let long = "x".repeat(MAX_TAG_LEN + 1);
        let s = alloc::format!("halcyon-layout v1\nleaf tag=\"{}\"\n", long);
        assert_eq!(parse(&s), Err(ParseError::TagTooLong));
    }

    #[test]
    fn trailing_blank_lines_are_ignored() {
        let s = "halcyon-layout v1\nleaf tag=\"ut\"\n\n\n";
        assert_eq!(parse(s), Ok(leaf("ut")));
    }

    // A leaf resolver for the from_render_text tests: id -> (command line,
    // env), "" + not-env for an id the map doesn't name (an empty pane).
    fn tags(pairs: &'static [(u32, &'static str)]) -> impl Fn(u32) -> (String, bool) {
        move |id| {
            (
                pairs
                    .iter()
                    .find(|(i, _)| *i == id)
                    .map_or_else(String::new, |(_, t)| t.to_string()),
                false,
            )
        }
    }

    #[test]
    fn from_render_text_builds_the_welcome_shape() {
        // Exactly what pane::render_text prints for the shipped welcome: a
        // two-pane SplitH, the left leaf focused (the `*`), the right empty.
        let render = "epoch 4 focused 3\n\
0 splith n=2 active=0 [0,0,1920,1080]\n  \
3* leaf surface=1 [0,0,960,1080]\n  \
4 leaf empty [960,0,960,1080]\n";
        let t = from_render_text(render, tags(&[(3, "halcyon welcome"), (4, "ut")]))
            .expect("parse a render_text dump");
        assert_eq!(
            t,
            cont(
                LayoutMode::SplitH,
                0,
                vec![leaf("halcyon welcome"), leaf("ut")]
            )
        );
        // And it serializes to exactly the save-file bytes (the write side).
        assert_eq!(
            serialize(&t),
            "halcyon-layout v1\nsplith n=2 active=0\n  leaf tag=\"halcyon welcome\"\n  leaf tag=\"ut\"\n"
        );
    }

    #[test]
    fn from_render_text_single_leaf_root() {
        // A fresh session: one full-screen leaf, no container.
        let render = "epoch 1 focused 0\n0* leaf surface=2 [0,0,800,600]\n";
        let t = from_render_text(render, tags(&[(0, "ut")])).expect("single leaf");
        assert_eq!(t, leaf("ut"));
    }

    #[test]
    fn from_render_text_round_trips_a_deep_tree() {
        // A nested dump (with the zoomed header field and focus markers) folds
        // to a tree that serializes and re-parses back to itself.
        let render = "epoch 9 focused 5 zoomed 5\n\
0 splith n=2 active=1 [0,0,1000,800]\n  \
2 leaf surface=0 [0,0,500,800]\n  \
3 tabbed n=2 active=0 [500,0,500,800]\n    \
5* leaf surface=1 [500,0,500,760]\n    \
6 leaf empty [500,0,500,760]\n";
        let t = from_render_text(render, tags(&[(2, "ut"), (5, "hx a.rs"), (6, "")]))
            .expect("deep dump");
        let back = parse(&serialize(&t)).expect("re-parse own serialization");
        assert_eq!(back, t);
        // The empty pane (id 6, no tag) is a bare leaf; the tabbed active is kept.
        assert_eq!(
            t,
            cont(
                LayoutMode::SplitH,
                1,
                vec![
                    leaf("ut"),
                    cont(LayoutMode::Tabbed, 0, vec![leaf("hx a.rs"), leaf("")]),
                ]
            )
        );
    }

    #[test]
    fn from_render_text_drops_an_oversize_tag() {
        // A pane whose tag exceeds the format cap saves as an empty leaf (a
        // clean placeholder), never a truncated half-command.
        let render = "epoch 1 focused 0\n0 leaf surface=0 [0,0,10,10]\n";
        let huge = "x".repeat(MAX_TAG_LEN + 1);
        let pairs: alloc::vec::Vec<(u32, String)> = vec![(0u32, huge)];
        let t = from_render_text(render, |id| {
            (
                pairs
                    .iter()
                    .find(|(i, _)| *i == id)
                    .map_or_else(String::new, |(_, t)| t.clone()),
                false,
            )
        })
        .expect("oversize tag drops, not fails");
        assert_eq!(t, leaf(""));
    }

    #[test]
    fn from_render_text_ignores_geometry_and_hidden() {
        // A zoomed session: the header carries `zoomed`, and the un-zoomed
        // sibling is marked ` hidden` after its rect. Both are transient
        // display state -- the saved tree + tags are exactly as if neither
        // marker existed (a restore is never zoomed, never hides).
        let render = "epoch 2 focused 1 zoomed 1\n\
0 splitv n=2 active=0 [0,0,800,600]\n  \
1* leaf surface=0 [0,0,800,600]\n  \
2 leaf empty [0,0,800,600] hidden\n";
        let t = from_render_text(render, tags(&[(1, "ut"), (2, "")])).expect("hidden/zoom ignored");
        assert_eq!(t, cont(LayoutMode::SplitV, 0, vec![leaf("ut"), leaf("")]));
    }

    #[test]
    fn from_render_text_rejects_malformed_dumps() {
        let m = tags(&[]);
        // Not a render_text header.
        assert_eq!(
            from_render_text("halcyon-layout v1\nleaf\n", &m),
            Err(ParseError::BadHeader)
        );
        // A non-numeric pane id.
        assert_eq!(
            from_render_text("epoch 1 focused 0\nx leaf surface=0 [0,0,1,1]\n", &m),
            Err(ParseError::BadRow)
        );
        // An unknown container mode.
        assert_eq!(
            from_render_text(
                "epoch 1 focused 0\n0 floaty n=1 active=0 [0,0,1,1]\n  1 leaf empty [0,0,1,1]\n",
                &m
            ),
            Err(ParseError::BadRow)
        );
        // A leaf row with no kind token after the id.
        assert_eq!(
            from_render_text("epoch 1 focused 0\n0\n", &m),
            Err(ParseError::BadRow)
        );
        // Odd indent (render_text is always even).
        assert_eq!(
            from_render_text("epoch 1 focused 0\n 0 leaf empty [0,0,1,1]\n", &m),
            Err(ParseError::BadIndent)
        );
        // Header-only (no pane rows) -- an empty compositor is not a layout.
        assert_eq!(
            from_render_text("epoch 0 focused 0\n", &m),
            Err(ParseError::Empty)
        );
    }
    #[test]
    fn env_leaves_round_trip_in_every_form() {
        // The four leaf rows: bare, env, tagged, tagged+env.
        roundtrip(&leaf(""));
        roundtrip(&env_leaf(""));
        roundtrip(&leaf("hx a"));
        roundtrip(&env_leaf("halcyon"));
        assert_eq!(
            serialize(&cont(
                LayoutMode::SplitH,
                1,
                vec![env_leaf("halcyon"), env_leaf(""), leaf("tapestry-demo"), leaf("")]
            )),
            "halcyon-layout v1\nsplith n=4 active=1\n  leaf tag=\"halcyon\" env\n  leaf env\n  leaf tag=\"tapestry-demo\"\n  leaf\n"
        );
        // The marker is exactly ` env` after the row's tag (or after `leaf`).
        assert_eq!(parse("halcyon-layout v1\nleaf env\n"), Ok(env_leaf("")));
        assert_eq!(
            parse("halcyon-layout v1\nleaf tag=\"x\" env\n"),
            Ok(env_leaf("x"))
        );
        assert_eq!(
            parse("halcyon-layout v1\nleaf tag=\"x\"env\n"),
            Err(ParseError::BadRow)
        );
        assert_eq!(
            parse("halcyon-layout v1\nleaf tag=\"x\" envy\n"),
            Err(ParseError::BadRow)
        );
        assert_eq!(
            parse("halcyon-layout v1\nleaf env extra\n"),
            Err(ParseError::BadRow)
        );
        assert_eq!(
            parse("halcyon-layout v1\nleaf  env\n"),
            Err(ParseError::BadRow)
        );
    }

    #[test]
    fn from_render_text_marks_env_leaves() {
        // The save tool's classifier says which tiles are not the session's.
        let render = "epoch 4 focused 3\n\
0 splith n=2 active=0 [0,0,1920,1080]\n  \
3* leaf surface=1 [0,0,960,1080]\n  \
4 leaf surface=2 [960,0,960,1080]\n";
        let t = from_render_text(render, |id| match id {
            3 => (String::from("halcyon"), true),
            4 => (String::from("tapestry-demo"), false),
            _ => (String::new(), false),
        })
        .expect("dump with an env leaf");
        assert_eq!(
            t,
            cont(
                LayoutMode::SplitH,
                0,
                vec![env_leaf("halcyon"), leaf("tapestry-demo")]
            )
        );
        assert_eq!(
            serialize(&t),
            "halcyon-layout v1\nsplith n=2 active=0\n  leaf tag=\"halcyon\" env\n  leaf tag=\"tapestry-demo\"\n"
        );
    }

    #[test]
    fn prune_env_keeps_the_sessions_subtree() {
        // A console-only save prunes to nothing: there is nothing to restore.
        assert_eq!(prune_env(&env_leaf("")), None);
        assert_eq!(
            prune_env(&cont(
                LayoutMode::SplitH,
                0,
                vec![env_leaf("halcyon"), env_leaf("")]
            )),
            None
        );
        // The console beside one program: the container dissolves to the leaf.
        assert_eq!(
            prune_env(&cont(
                LayoutMode::SplitH,
                1,
                vec![env_leaf("halcyon"), leaf("tapestry-demo")]
            )),
            Some(leaf("tapestry-demo"))
        );
        // A session subtree beside the console survives whole; the active
        // index follows its child across the renumbering.
        let saved = cont(
            LayoutMode::SplitH,
            1,
            vec![
                env_leaf("halcyon"),
                cont(LayoutMode::SplitV, 1, vec![leaf("a"), leaf("b")]),
            ],
        );
        assert_eq!(
            prune_env(&saved),
            Some(cont(LayoutMode::SplitV, 1, vec![leaf("a"), leaf("b")]))
        );
        // Pruning INSIDE a container renumbers: active pointed at the third
        // child (index 2); the first was env, so it is now index 1.
        let saved = cont(
            LayoutMode::Tabbed,
            2,
            vec![env_leaf(""), leaf("a"), leaf("b"), leaf("")],
        );
        assert_eq!(
            prune_env(&saved),
            Some(cont(
                LayoutMode::Tabbed,
                1,
                vec![leaf("a"), leaf("b"), leaf("")]
            ))
        );
        // The active child itself pruned -> the first surviving child.
        let saved = cont(
            LayoutMode::SplitH,
            0,
            vec![env_leaf(""), leaf("a"), leaf("b")],
        );
        assert_eq!(
            prune_env(&saved),
            Some(cont(LayoutMode::SplitH, 0, vec![leaf("a"), leaf("b")]))
        );
        // A tree with no env leaf is returned unchanged.
        let t = cont(LayoutMode::SplitV, 1, vec![leaf("a"), leaf("")]);
        assert_eq!(prune_env(&t), Some(t.clone()));
    }
    #[test]
    fn anchor_last_means_one_env_leaf_last_under_the_root() {
        // The welcome: the tour first, the environment's shell last, active.
        let w = parse(
            "halcyon-layout v1\nsplith n=2 active=1\n  leaf tag=\"halcyon welcome\"\n  leaf env\n",
        )
        .unwrap();
        assert!(anchor_last(&w));
        assert!(active_is_env(&w));
        // The env leaf first: the default placement (built part after it).
        let f = parse("halcyon-layout v1\nsplith n=2 active=0\n  leaf env\n  leaf tag=\"a\"\n")
            .unwrap();
        assert!(!anchor_last(&f));
        assert!(active_is_env(&f));
        // Two env leaves: no single anchor to place.
        let two = parse(
            "halcyon-layout v1\nsplith n=3 active=1\n  leaf env\n  leaf tag=\"a\"\n  leaf env\n",
        )
        .unwrap();
        assert!(!anchor_last(&two));
        // A nested env leaf is not the root's last child.
        let nested = parse(
            "halcyon-layout v1\nsplith n=2 active=0\n  leaf tag=\"a\"\n  splitv n=2 active=0\n    leaf tag=\"b\"\n    leaf env\n",
        )
        .unwrap();
        assert!(!anchor_last(&nested));
        assert!(!active_is_env(&nested));
        // A lone leaf anchors nothing.
        assert!(!anchor_last(
            &parse("halcyon-layout v1\nleaf env\n").unwrap()
        ));
        assert!(!active_is_env(
            &parse("halcyon-layout v1\nleaf tag=\"a\"\n").unwrap()
        ));
    }

    // HALCYON-INSTRUMENT 5.3: v2 = v1 plus ` w=<weight>`, emitted only when
    // some weight is non-default -- so the equal-weight tree keeps its v1
    // bytes and an old reader keeps reading it.
    #[test]
    fn a_weighted_tree_round_trips_as_v2_and_a_default_one_stays_v1() {
        let t = cont(
            LayoutMode::SplitH,
            0,
            vec![
                w(leaf("a"), 515),
                w(
                    cont(LayoutMode::SplitV, 1, vec![w(leaf("b"), 49), w(env_leaf("c"), 51)]),
                    485,
                ),
            ],
        );
        let s = serialize(&t);
        assert_eq!(
            s,
            "halcyon-layout v2\nsplith n=2 active=0\n  leaf tag=\"a\" w=515\n  splitv n=2 active=1 w=485\n    leaf tag=\"b\" w=49\n    leaf tag=\"c\" env w=51\n"
        );
        assert_eq!(parse(&s), Ok(t));
        let d = cont(LayoutMode::SplitH, 0, vec![leaf("a"), env_leaf("b")]);
        let ds = serialize(&d);
        assert!(ds.starts_with("halcyon-layout v1\n"), "{ds:?}");
        assert!(!ds.contains("w="), "{ds:?}");
        assert_eq!(parse(&ds), Ok(d));
    }

    #[test]
    fn a_v1_file_loads_with_equal_weights_and_refuses_a_weight() {
        let t = parse("halcyon-layout v1\nsplith n=2 active=0\n  leaf\n  leaf env\n").unwrap();
        assert_eq!(t, cont(LayoutMode::SplitH, 0, vec![leaf(""), env_leaf("")]));
        for bad in [
            "halcyon-layout v1\nsplith n=2 active=0 w=3\n  leaf\n  leaf\n",
            "halcyon-layout v1\nleaf w=3\n",
            "halcyon-layout v1\nleaf tag=\"x\" w=3\n",
            "halcyon-layout v1\nleaf env w=3\n",
        ] {
            assert_eq!(parse(bad), Err(ParseError::BadWeight), "{bad:?}");
        }
    }

    #[test]
    fn a_v2_weight_is_one_to_65535_and_the_tail_grammar_is_exact() {
        assert_eq!(parse("halcyon-layout v2\nleaf\n"), Ok(leaf("")));
        assert_eq!(parse("halcyon-layout v2\nleaf w=1\n"), Ok(leaf("")));
        assert_eq!(parse("halcyon-layout v2\nleaf w=65535\n"), Ok(w(leaf(""), 65535)));
        assert_eq!(parse("halcyon-layout v2\nleaf env w=7\n"), Ok(w(env_leaf(""), 7)));
        assert_eq!(parse("halcyon-layout v2\nleaf tag=\"x\" env w=7\n"), Ok(w(env_leaf("x"), 7)));
        assert_eq!(parse("halcyon-layout v2\nleaf tag=\"x\" w=7\n"), Ok(w(leaf("x"), 7)));
        assert_eq!(
            parse("halcyon-layout v2\nsplith n=2 active=1 w=9\n  leaf\n  leaf\n"),
            Ok(w(cont(LayoutMode::SplitH, 1, vec![leaf(""), leaf("")]), 9))
        );
        for bad in [
            "leaf w=0",
            "leaf w=65536",
            "leaf w=",
            "leaf w=-1",
            "leaf w=1x",
            "leaf w=1 env",
            "leaf w=1 w=2",
            "leaf  w=1",
            "leaf tag=\"x\"w=1",
            "leaf tag=\"x\" envw=1",
            "splith n=2 active=0 w=0\n  leaf\n  leaf",
            "splith n=2 active=0 w=1 w=2\n  leaf\n  leaf",
        ] {
            let r = parse(&alloc::format!("halcyon-layout v2\n{bad}\n"));
            assert!(
                matches!(r, Err(ParseError::BadWeight) | Err(ParseError::BadRow)),
                "{bad:?} -> {r:?}"
            );
        }
    }

    #[test]
    fn from_render_text_reads_the_weight_after_the_rect() {
        let render = "epoch 4 focused 3\n\
0 splith n=2 active=0 [3,37,1434,835]\n  \
3* leaf surface=1 [4,70,733,704] w=515\n  \
5 splitv n=2 active=1 [745,37,692,835] w=485\n    \
6 leaf surface=2 [746,70,690,307] w=49\n    \
7 leaf empty [0,0,0,0] w=51 hidden\n";
        let t = from_render_text(render, |id| (alloc::format!("t{id}"), false)).unwrap();
        assert_eq!(
            t,
            cont(
                LayoutMode::SplitH,
                0,
                vec![
                    w(leaf("t3"), 515),
                    w(
                        cont(LayoutMode::SplitV, 1, vec![w(leaf("t6"), 49), w(leaf("t7"), 51)]),
                        485
                    ),
                ]
            )
        );
        assert!(serialize(&t).starts_with("halcyon-layout v2\n"));
        // A dump row without the token is the default; a bad token is
        // refused, never guessed.
        let plain = from_render_text("epoch 1 focused 1\n1 leaf empty [0,0,1,1]\n", |_| (String::new(), false));
        assert_eq!(plain, Ok(leaf("")));
        assert_eq!(
            from_render_text("epoch 1 focused 1\n1 leaf empty [0,0,1,1] w=0\n", |_| (String::new(), false)),
            Err(ParseError::BadWeight)
        );
    }

    #[test]
    fn prune_env_hands_a_dissolved_containers_weight_to_its_survivor() {
        // [a w=3, splitv w=7 [env, b]]: the env leaf goes, the splitv dissolves
        // and b takes the container's place AND its weight (the compositor's
        // dissolve rule, mirrored).
        let t = cont(
            LayoutMode::SplitH,
            0,
            vec![
                w(leaf("a"), 3),
                w(cont(LayoutMode::SplitV, 0, vec![env_leaf("e"), leaf("b")]), 7),
            ],
        );
        assert_eq!(
            prune_env(&t),
            Some(cont(LayoutMode::SplitH, 0, vec![w(leaf("a"), 3), w(leaf("b"), 7)]))
        );
    }
}
