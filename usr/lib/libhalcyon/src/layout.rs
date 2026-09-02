// layout -- the `halcyon-layout v1` save format (HALCYON.md 13.7, H-4).
//
// The pure half of layout save/restore: a bounded, no-panic serializer +
// parser for the pane tree's SHAPE (container modes + active child; per-leaf
// tag = the command line). Surface ids are runtime, never saved -- a restored
// leaf gets a fresh surface from the respawned program. Shared by halcyond
// (the device-tier restore + the gesture) and the user-authority session tool
// (the session-tier save/restore, the D decision), so it lives here in
// libhalcyon rather than in either.
//
// The parser reads UNTRUSTED input (a layout file in the user's $home): every
// path is bounded and fail-closed -- a malformed or oversize file returns an
// Err the caller degrades on (geometry-only, or no restore), NEVER a panic
// (a panic in a no_std tool is a silent exit(1)).

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;

/// The format's first line (exact match required).
pub const FMT_HEADER: &str = "halcyon-layout v1";
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
/// index, and children.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum LayoutNode {
    Leaf {
        tag: String,
    },
    Container {
        mode: LayoutMode,
        active: u32,
        children: Vec<LayoutNode>,
    },
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
}

/// Serialize a tree to the `halcyon-layout v1` format: the header, then one
/// pre-order row per node, two spaces of indent per depth. A leaf is
/// `leaf` (empty tag) or `leaf tag="<escaped>"`; a container is
/// `<mode> n=<count> active=<idx>` followed by its children.
pub fn serialize(root: &LayoutNode) -> String {
    let mut s = String::new();
    s.push_str(FMT_HEADER);
    s.push('\n');
    ser_node(root, 0, &mut s);
    s
}

fn ser_node(node: &LayoutNode, depth: usize, out: &mut String) {
    for _ in 0..depth {
        out.push_str("  ");
    }
    match node {
        LayoutNode::Leaf { tag } => {
            out.push_str("leaf");
            if !tag.is_empty() {
                out.push_str(" tag=\"");
                escape_into(tag, out);
                out.push('"');
            }
            out.push('\n');
        }
        LayoutNode::Container {
            mode,
            active,
            children,
        } => {
            let _ = write!(
                out,
                "{} n={} active={}",
                mode.name(),
                children.len(),
                active
            );
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

struct Frame {
    depth: usize,
    mode: LayoutMode,
    active: u32,
    n: u32,
    children: Vec<LayoutNode>,
}

enum Row {
    Leaf(String),
    Cont(LayoutMode, u32, u32),
}

/// Parse the `halcyon-layout v1` format. Bounded + fail-closed on every path.
pub fn parse(input: &str) -> Result<LayoutNode, ParseError> {
    let mut lines = input.split('\n');
    let header = lines.next().unwrap_or("");
    if header.trim_end_matches('\r') != FMT_HEADER {
        return Err(ParseError::BadHeader);
    }
    let mut rows: Vec<(usize, Row)> = Vec::new();
    for raw in lines {
        let line = raw.trim_end_matches('\r');
        if line.is_empty() {
            continue; // blank lines (incl. a trailing newline's tail) ignored
        }
        rows.push(parse_row(line)?);
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
/// restored leaf gets a fresh surface and rect. `tag_of` resolves each leaf's
/// command line by id (the save tool reads `pane/<id>/tag`); a tag longer than
/// MAX_TAG_LEN is dropped to empty so the result always round-trips through
/// serialize/parse. Bounded + fail-closed exactly like `parse`, so a garbled
/// dump degrades (no save) rather than panicking (a silent exit in a no_std
/// tool). This is the WRITE side's inverse of `parse`: `render_text` in ->
/// `serialize` out.
pub fn from_render_text(
    render: &str,
    tag_of: impl Fn(u32) -> String,
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
        rows.push(parse_render_row(line, &tag_of)?);
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
            Row::Leaf(tag) => attach(&mut stack, &mut root, LayoutNode::Leaf { tag }, depth)?,
            Row::Cont(mode, n, active) => stack.push(Frame {
                depth,
                mode,
                active,
                n,
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
    })
}

/// Tokenize one row into its depth (leading-space pairs) and its content.
fn parse_row(line: &str) -> Result<(usize, Row), ParseError> {
    let spaces = line.len() - line.trim_start_matches(' ').len();
    if !spaces.is_multiple_of(2) {
        return Err(ParseError::BadIndent);
    }
    let depth = spaces / 2;
    if depth > MAX_DEPTH {
        return Err(ParseError::TooDeep);
    }
    let rest = &line[spaces..];
    if rest == "leaf" {
        return Ok((depth, Row::Leaf(String::new())));
    }
    if let Some(tail) = rest.strip_prefix("leaf tag=\"") {
        let tag = parse_tag(tail)?;
        return Ok((depth, Row::Leaf(tag)));
    }
    // A container: `<mode> n=<num> active=<num>`.
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
    if it.next().is_some() {
        return Err(ParseError::BadRow); // trailing tokens
    }
    if n as usize > MAX_NODES {
        return Err(ParseError::TooMany);
    }
    Ok((depth, Row::Cont(mode, n, active)))
}

/// Tokenize one `render_text` row: leading-space pairs -> depth, then
/// `<id>[*] leaf ...` or `<id>[*] <mode> n=<num> active=<num> ...`. The leaf's
/// tag comes from `tag_of(id)` (render_text omits it); surface/geometry tokens
/// are read past and discarded.
fn parse_render_row(
    line: &str,
    tag_of: &impl Fn(u32) -> String,
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
    match it.next() {
        Some("leaf") => {
            let tag = tag_of(id);
            // A tag past the format's cap is dropped, never truncated: a
            // half-command would respawn wrong, whereas an empty leaf is a
            // clean placeholder -- and the output must round-trip through parse.
            let tag = if tag.len() > MAX_TAG_LEN {
                String::new()
            } else {
                tag
            };
            Ok((depth, Row::Leaf(tag)))
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
            Ok((depth, Row::Cont(mode, n, active)))
        }
        None => Err(ParseError::BadRow),
    }
}

/// Parse the body of `leaf tag="..."` (everything after the opening quote):
/// unescape `\\`/`\"`/`\n` up to the closing unescaped quote, which must be
/// the last character of the row.
fn parse_tag(tail: &str) -> Result<String, ParseError> {
    let mut out = String::new();
    let mut chars = tail.char_indices();
    while let Some((i, ch)) = chars.next() {
        match ch {
            '"' => {
                // The closing quote must end the row.
                if i + 1 != tail.len() {
                    return Err(ParseError::BadRow);
                }
                if out.len() > MAX_TAG_LEN {
                    return Err(ParseError::TagTooLong);
                }
                return Ok(out);
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
        LayoutNode::Leaf { tag: t.to_string() }
    }
    fn cont(m: LayoutMode, a: u32, c: Vec<LayoutNode>) -> LayoutNode {
        LayoutNode::Container {
            mode: m,
            active: a,
            children: c,
        }
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
            parse("halcyon-layout v2\nleaf\n"),
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

    // A tag resolver for the from_render_text tests: id -> command line, "" for
    // an id the map doesn't name (an empty pane).
    fn tags(pairs: &'static [(u32, &'static str)]) -> impl Fn(u32) -> String {
        move |id| {
            pairs
                .iter()
                .find(|(i, _)| *i == id)
                .map_or_else(String::new, |(_, t)| t.to_string())
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
            pairs
                .iter()
                .find(|(i, _)| *i == id)
                .map_or_else(String::new, |(_, t)| t.clone())
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
}
