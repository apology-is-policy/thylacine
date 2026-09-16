//! The realization (MANUAL-DESIGN.md section 4): a section as Beacon frames around
//! its payload at the rich tier, and as the same payload alone at the plain
//! tiers, where paragraphs and list items may be word-wrapped. The renderer is a
//! consumer of the parser's events, and writes as they arrive.

use alloc::vec::Vec;

use beacon::wire::{self, Op};
use beacon::Tier;

use crate::format::{self, Align, Events, Open, Problem, Run, TABLE_COLUMNS_MAX};
use crate::is_control;
use crate::wrap::Wrap;

/// The heading of the contents listing (5).
pub const BOOK_TITLE: &str = "Thylacine Operator's Manual";

/// The largest piece of output passed on at once (4.4).
pub const CHUNK: usize = 64 * 1024;

/// Render a section that has passed the check at `tier`, passing the output to
/// `out` in chunks of at most `CHUNK` bytes. `width` wraps paragraphs and list
/// items at a plain tier (4.3) and is ignored at the rich tier, where the
/// renderer wraps. Text the check would reject is still rendered safely (4.4),
/// but not necessarily as its author meant.
pub fn render(src: &str, tier: Tier, width: Option<usize>, out: &mut dyn FnMut(&[u8])) {
    let mut r = Renderer::new(tier, width, out);
    format::read(None, src, &mut r);
    r.finish();
}

/// One section as the contents listing shows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Listed {
    pub name: alloc::string::String,
    pub title: alloc::string::String,
}

/// The contents listing (5): the book's title-page heading, then the sections
/// in book order with a line saying how to open one, or a line saying that no
/// sections are installed.
pub fn render_contents(sections: &[Listed], tier: Tier, out: &mut dyn FnMut(&[u8])) {
    let mut r = Renderer::new(tier, None, out);
    r.open_frame(Op::Hdr, &[("level", "1"), ("class", "title")]);
    r.put_text(BOOK_TITLE, false);
    r.close_frame(Op::Hdr);
    r.put(b"\n\n");
    if sections.is_empty() {
        r.put(b"No sections are installed.\n");
        r.finish();
        return;
    }
    let mut widths = [0usize; 2];
    let rows = core::iter::once(("Name", "Section"))
        .chain(sections.iter().map(|s| (s.name.as_str(), s.title.as_str())));
    for (name, title) in rows.clone() {
        widths[0] = widths[0].max(name.chars().count());
        widths[1] = widths[1].max(title.chars().count());
    }
    r.open(Open::Table {
        align: &[Align::Left, Align::Left],
        widths: &widths,
    });
    for (name, title) in rows {
        r.open(Open::Row);
        for cell in [name, title] {
            r.open(Open::Cell {
                width: cell.chars().count(),
            });
            r.run(Run::Text, cell);
            r.close();
        }
        r.close();
    }
    r.close();
    r.put(b"\n");
    r.run(Run::Text, "Show a section with ");
    r.run(Run::Code, "manual <name>");
    r.run(Run::Text, ".");
    r.put(b"\n");
    r.finish();
}

/// Output gathered into chunks of at most `CHUNK` bytes.
struct Chunks<'o> {
    buf: Vec<u8>,
    out: &'o mut dyn FnMut(&[u8]),
}

impl Chunks<'_> {
    fn put(&mut self, mut bytes: &[u8]) {
        while !bytes.is_empty() {
            if self.buf.capacity() == 0 {
                self.buf.reserve_exact(CHUNK);
            }
            let take = (CHUNK - self.buf.len()).min(bytes.len());
            self.buf.extend_from_slice(&bytes[..take]);
            bytes = &bytes[take..];
            if self.buf.len() == CHUNK {
                (self.out)(&self.buf);
                self.buf.clear();
            }
        }
    }

    fn flush(&mut self) {
        if !self.buf.is_empty() {
            (self.out)(&self.buf);
            self.buf.clear();
        }
    }
}

/// What an open block is to the renderer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Frame {
    Heading,
    /// A paragraph or list item.
    Flow,
    List,
    Code,
    Table,
    Row,
    Cell,
    /// A cell beyond the table's columns, which a checked section never has.
    Skip,
}

struct Renderer<'o> {
    out: Chunks<'o>,
    rich: bool,
    width: Option<usize>,
    blocks: usize,
    /// Open blocks, innermost last. A checked section nests at most three deep:
    /// a table, a row, a cell.
    stack: [Frame; 3],
    depth: usize,
    numbered: bool,
    align: [Align; TABLE_COLUMNS_MAX],
    widths: [usize; TABLE_COLUMNS_MAX],
    cols: usize,
    col: usize,
    /// Padding owed after the open cell.
    after: usize,
    wrap: Wrap,
    frame: Vec<u8>,
}

impl<'o> Renderer<'o> {
    fn new(tier: Tier, width: Option<usize>, out: &'o mut dyn FnMut(&[u8])) -> Renderer<'o> {
        let rich = tier == Tier::Rich;
        Renderer {
            out: Chunks {
                buf: Vec::new(),
                out,
            },
            rich,
            width: if rich { None } else { width },
            blocks: 0,
            stack: [Frame::Heading; 3],
            depth: 0,
            numbered: false,
            align: [Align::Left; TABLE_COLUMNS_MAX],
            widths: [0; TABLE_COLUMNS_MAX],
            cols: 0,
            col: 0,
            after: 0,
            wrap: Wrap::new(),
            frame: Vec::new(),
        }
    }

    fn finish(&mut self) {
        self.out.flush();
    }

    fn put(&mut self, bytes: &[u8]) {
        self.out.put(bytes);
    }

    /// Section text, with every control character replaced (4.4). A code
    /// block keeps its tabs.
    fn put_text(&mut self, s: &str, keep_tab: bool) {
        let mut clean = 0;
        for (i, c) in s.char_indices() {
            if is_control(c) && !(keep_tab && c == '\t') {
                self.out.put(&s.as_bytes()[clean..i]);
                self.out.put("\u{fffd}".as_bytes());
                clean = i + c.len_utf8();
            }
        }
        self.out.put(&s.as_bytes()[clean..]);
    }

    fn spaces(&mut self, n: usize) {
        const SPACES: &[u8] = &[b' '; 64];
        let mut n = n;
        while n > 0 {
            let k = n.min(SPACES.len());
            self.out.put(&SPACES[..k]);
            n -= k;
        }
    }

    fn open_frame(&mut self, op: Op, args: &[(&str, &str)]) {
        if self.rich {
            self.frame.clear();
            wire::open(&mut self.frame, op, args);
            self.out.put(&self.frame);
        }
    }

    fn close_frame(&mut self, op: Op) {
        if self.rich {
            self.frame.clear();
            wire::close(&mut self.frame, op);
            self.out.put(&self.frame);
        }
    }

    fn em(&mut self, class: &str, s: &str) {
        self.open_frame(Op::Em, &[("class", class)]);
        self.put_text(s, false);
        self.close_frame(Op::Em);
    }

    fn top(&self) -> Option<Frame> {
        self.depth
            .checked_sub(1)
            .and_then(|i| self.stack.get(i).copied())
    }

    fn push(&mut self, f: Frame) {
        if self.depth < self.stack.len() {
            self.stack[self.depth] = f;
        }
        self.depth += 1;
    }

    fn pop(&mut self) -> Option<Frame> {
        let f = self.top();
        self.depth = self.depth.saturating_sub(1);
        f
    }

    fn flow_begin(&mut self, marker: &str) {
        match self.width {
            Some(cols) => self.wrap.begin(marker, cols),
            None => self.put_text(marker, false),
        }
    }

    fn cell_begin(&mut self, width: usize) -> Frame {
        if self.col >= self.cols {
            return Frame::Skip;
        }
        let pad = self.widths[self.col].saturating_sub(width);
        let (before, after) = match self.align[self.col] {
            Align::Right => (pad, 0),
            Align::Center => (pad / 2, pad - pad / 2),
            Align::Left => (0, pad),
        };
        if self.col > 0 {
            self.put(b"  ");
        }
        self.spaces(before);
        self.open_frame(Op::Cell, &[]);
        self.after = after;
        Frame::Cell
    }
}

impl Events for Renderer<'_> {
    fn problem(&mut self, _line: usize, _problem: Problem) {}

    fn open(&mut self, block: Open<'_>) {
        if !matches!(block, Open::Item(_) | Open::Row | Open::Cell { .. }) {
            if self.blocks > 0 {
                self.put(b"\n");
            }
            self.blocks += 1;
        }
        let frame = match block {
            Open::Title => {
                self.open_frame(Op::Hdr, &[("level", "1")]);
                Frame::Heading
            }
            Open::Heading(level) => {
                self.open_frame(Op::Hdr, &[("level", if level == 2 { "2" } else { "3" })]);
                Frame::Heading
            }
            Open::Paragraph => {
                self.flow_begin("");
                Frame::Flow
            }
            Open::Bullets | Open::Numbered => {
                self.numbered = block == Open::Numbered;
                Frame::List
            }
            Open::Item(n) => {
                if self.numbered {
                    // "N. ", written without allocating.
                    let mut marker = [0u8; 24];
                    let mut at = marker.len() - 2;
                    marker[at..].copy_from_slice(b". ");
                    let mut v = n;
                    loop {
                        at -= 1;
                        marker[at] = b'0' + (v % 10) as u8;
                        v /= 10;
                        if v == 0 {
                            break;
                        }
                    }
                    self.flow_begin(core::str::from_utf8(&marker[at..]).unwrap_or(""));
                } else {
                    self.flow_begin("- ");
                }
                Frame::Flow
            }
            Open::Code => {
                self.open_frame(Op::Pre, &[]);
                Frame::Code
            }
            Open::Table { align, widths } => {
                self.cols = align.len().min(TABLE_COLUMNS_MAX);
                self.align[..self.cols].copy_from_slice(&align[..self.cols]);
                for c in 0..self.cols {
                    self.widths[c] = widths.get(c).copied().unwrap_or(0);
                }
                let mut spec = [0u8; TABLE_COLUMNS_MAX];
                for (c, a) in align.iter().take(self.cols).enumerate() {
                    spec[c] = match a {
                        Align::Left => b'l',
                        Align::Right => b'r',
                        Align::Center => b'c',
                    };
                }
                let spec = core::str::from_utf8(&spec[..self.cols]).unwrap_or("");
                self.open_frame(Op::Table, &[("cols", spec), ("hdr", "1")]);
                Frame::Table
            }
            Open::Row => {
                self.open_frame(Op::Row, &[]);
                self.col = 0;
                Frame::Row
            }
            Open::Cell { width } => self.cell_begin(width),
        };
        self.push(frame);
    }

    fn close(&mut self) {
        match self.pop() {
            Some(Frame::Heading) => {
                self.close_frame(Op::Hdr);
                self.put(b"\n");
            }
            Some(Frame::Flow) => {
                if self.width.is_some() {
                    let out = &mut self.out;
                    self.wrap.end(&mut |line| {
                        out.put(line.as_bytes());
                        out.put(b"\n");
                    });
                } else {
                    self.put(b"\n");
                }
            }
            Some(Frame::List) | None => {}
            Some(Frame::Code) => self.close_frame(Op::Pre),
            Some(Frame::Table) => self.close_frame(Op::Table),
            Some(Frame::Row) => {
                self.close_frame(Op::Row);
                self.put(b"\n");
            }
            Some(Frame::Cell) => {
                self.close_frame(Op::Cell);
                if self.col + 1 < self.cols {
                    self.spaces(self.after);
                }
                self.col += 1;
            }
            Some(Frame::Skip) => self.col += 1,
        }
    }

    fn run(&mut self, kind: Run, text: &str) {
        match self.top() {
            Some(Frame::Skip) => {}
            Some(Frame::Flow) if self.width.is_some() => {
                let out = &mut self.out;
                self.wrap.feed(kind, text, &mut |line| {
                    out.put(line.as_bytes());
                    out.put(b"\n");
                });
            }
            _ => match kind {
                Run::Text => self.put_text(text, false),
                Run::Code => self.em("code", text),
                Run::Emph => self.em("emph", text),
                Run::Strong => self.em("strong", text),
            },
        }
    }

    fn code_line(&mut self, text: &str) {
        self.put_text(text, true);
        self.put(b"\n");
    }

    fn measures_tables(&self) -> bool {
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::format;
    use alloc::string::String;
    use alloc::vec;
    use beacon::wire::Event;

    const FIXTURE: &str = "# The Reader\n\
\n\
The `manual` command shows *one* section, **if** it passes.\n\
\n\
## In Practice\n\
\n\
- list\n\
- more\n\
\n\
1. first\n\
\n\
```\n\
manual --check\n\
```\n\
\n\
| Option | Effect |\n\
| --- | ---: |\n\
| `-h` | 1 |\n\
| `--beacon` | 22 |\n";

    /// Render into one buffer, checking every chunk's size on the way.
    fn render_all(src: &str, tier: Tier, width: Option<usize>) -> Vec<u8> {
        assert_eq!(
            format::check(None, src, &mut |_, _| {}),
            0,
            "the fixture fails the check"
        );
        let mut all = Vec::new();
        render(src, tier, width, &mut |chunk| {
            assert!(!chunk.is_empty() && chunk.len() <= CHUNK);
            all.extend_from_slice(chunk);
        });
        all
    }

    fn contents(listed: &[Listed], tier: Tier) -> Vec<u8> {
        let mut all = Vec::new();
        render_contents(listed, tier, &mut |chunk| all.extend_from_slice(chunk));
        all
    }

    fn s(bytes: &[u8]) -> &str {
        core::str::from_utf8(bytes).unwrap()
    }

    /// One frame: the op and its arguments as they appear on the wire.
    fn f(body: &str) -> String {
        format!("\x1b]1936;v1;{}\x1b\\", body)
    }

    fn sp(n: usize) -> String {
        " ".repeat(n)
    }

    #[test]
    fn rich_output_is_exact() {
        let out = render_all(FIXTURE, Tier::Rich, None);
        let cell = |t: &str| format!("{}{}{}", f("cell"), t, f("/cell"));
        let code = |t: &str| format!("{}{}{}", f("em;class=code"), t, f("/em"));
        let mut e = String::new();
        e += &format!("{}The Reader{}\n\n", f("hdr;level=1"), f("/hdr"));
        e += &format!("The {} command shows ", code("manual"));
        e += &format!("{}one{} section, ", f("em;class=emph"), f("/em"));
        e += &format!("{}if{} it passes.\n\n", f("em;class=strong"), f("/em"));
        e += &format!("{}In Practice{}\n\n", f("hdr;level=2"), f("/hdr"));
        e += "- list\n- more\n\n1. first\n\n";
        e += &format!("{}manual --check\n{}\n", f("pre"), f("/pre"));
        e += &f("table;cols=lr;hdr=1");
        // Widths 8 and 6: "Option" pads 2 after, then the 2-space gutter.
        e += &format!(
            "{}{}{}{}{}\n",
            f("row"),
            cell("Option"),
            sp(4),
            cell("Effect"),
            f("/row")
        );
        // "-h" pads 6 after, the gutter, then "1" right-aligned in 6: 5 before.
        e += &format!(
            "{}{}{}{}{}\n",
            f("row"),
            cell(&code("-h")),
            sp(13),
            cell("1"),
            f("/row")
        );
        e += &format!(
            "{}{}{}{}{}\n",
            f("row"),
            cell(&code("--beacon")),
            sp(6),
            cell("22"),
            f("/row")
        );
        e += &f("/table");
        assert_eq!(s(&out), e);
    }

    #[test]
    fn plain_output_is_exact() {
        let out = render_all(FIXTURE, Tier::None, None);
        let expected = format!(
            "The Reader\n\nThe manual command shows one section, if it passes.\n\nIn Practice\n\n- list\n- more\n\n1. first\n\nmanual --check\n\nOption{}Effect\n-h{}1\n--beacon{}22\n",
            sp(4),
            sp(13),
            sp(6)
        );
        assert_eq!(s(&out), expected);
    }

    /// The Beacon tier contract (BEACON.md 12.1 rule 1): removing every frame
    /// from the rich output yields the plain output byte for byte.
    #[test]
    fn stripping_rich_output_yields_plain_output() {
        let rich = render_all(FIXTURE, Tier::Rich, None);
        assert_eq!(wire::strip(&rich), render_all(FIXTURE, Tier::None, None));
        assert_eq!(
            render_all(FIXTURE, Tier::Cells, None),
            render_all(FIXTURE, Tier::None, None)
        );
        // Width never applies at the rich tier.
        assert_eq!(render_all(FIXTURE, Tier::Rich, Some(20)), rich);
    }

    #[test]
    fn the_table_matches_the_beacon_sink_table() {
        use beacon::sink::{Cell, Sink, Table};
        let ours = render_all(
            "# T\n\n| Name | Count | Mid |\n| --- | ---: | :---: |\n| alpha | 1 | x |\n| b | 12345 | yy |\n",
            Tier::Rich,
            None,
        );
        let mut theirs: Vec<u8> = Vec::new();
        {
            let mut sink = Sink::new(&mut theirs, Tier::Rich);
            let mut t = Table::new("lrc").hdr();
            for row in [
                ["Name", "Count", "Mid"],
                ["alpha", "1", "x"],
                ["b", "12345", "yy"],
            ] {
                t.push_row(row.iter().map(|c| Cell::plain(c)).collect());
            }
            t.realize(&mut sink);
        }
        let title = format!("{}T{}\n\n", f("hdr;level=1"), f("/hdr"));
        assert_eq!(s(&ours), format!("{}{}", title, s(&theirs)));
    }

    /// Wrapping at width 40 (8.1): a paragraph, a list item's hanging indent,
    /// an over-long word, and a table and code block that are not wrapped.
    #[test]
    fn plain_wrapping_at_forty_columns() {
        let src = "# A title that is longer than forty columns, never wrapped\n\n\
A paragraph that needs to wrap at forty columns, because it is long.\n\n\
- A list item whose continuation lines hang under the marker's width.\n\n\
1. A numbered item hangs under its wider marker, too.\n\n\
See /a/path/that/is/much/longer/than/the/forty/columns/allowed today.\n\n\
```\na code line that is longer than forty columns and is not wrapped\n```\n\n\
| A column heading that is quite wide | B |\n| --- | --- |\n| a cell that is also wide enough to pass forty | b |\n";
        let out = render_all(src, Tier::None, Some(40));
        let expected = [
            "A title that is longer than forty columns, never wrapped",
            "",
            "A paragraph that needs to wrap at forty",
            "columns, because it is long.",
            "",
            "- A list item whose continuation lines",
            "  hang under the marker's width.",
            "",
            "1. A numbered item hangs under its wider",
            "   marker, too.",
            "",
            "See",
            "/a/path/that/is/much/longer/than/the/for",
            "ty/columns/allowed today.",
            "",
            "a code line that is longer than forty columns and is not wrapped",
            "",
            "A column heading that is quite wide            B",
            "a cell that is also wide enough to pass forty  b",
            "",
        ]
        .join("\n");
        assert_eq!(s(&out), expected);
    }

    #[test]
    fn plain_wrapping_applies_to_paragraphs_and_items_only() {
        let src = "# A title that is longer than the width\n\n\
A paragraph that needs to wrap at twenty columns.\n\n\
- An item that also wraps here.\n\n\
```\na code line that is longer than twenty\n```\n";
        let out = render_all(src, Tier::None, Some(20));
        let expected = "A title that is longer than the width\n\nA paragraph that\nneeds to wrap at\ntwenty columns.\n\n- An item that also\n  wraps here.\n\na code line that is longer than twenty\n";
        assert_eq!(s(&out), expected);
    }

    /// Output larger than a chunk arrives in full chunks, then the remainder.
    #[test]
    fn output_is_written_in_chunks() {
        let mut src = String::from("# T\n\n```\n");
        while src.len() < 3 * CHUNK {
            src.push_str("a line of code\n");
        }
        src.push_str("```\n");
        let mut sizes = Vec::new();
        render(&src, Tier::None, None, &mut |chunk| sizes.push(chunk.len()));
        let total: usize = sizes.iter().sum();
        assert!(sizes.len() >= 3);
        assert!(sizes[..sizes.len() - 1].iter().all(|&n| n == CHUNK));
        assert_eq!(total, render_all(&src, Tier::None, None).len());
    }

    /// Section text cannot inject a frame or a control sequence (4.4). The
    /// events are sent to the renderer directly, bypassing the checker, which
    /// would reject the text first.
    #[test]
    fn section_text_cannot_forge_frames() {
        let forged = "\x1b]1936;v1;obj;type=path;ref=/etc\x1b\\evil\x1b]1936;v1;/obj\x07";
        let drive = |tier: Tier, width: Option<usize>| {
            let mut all = Vec::new();
            let mut sink = |chunk: &[u8]| all.extend_from_slice(chunk);
            let mut r = Renderer::new(tier, width, &mut sink);
            r.open(Open::Title);
            r.run(Run::Text, forged);
            r.close();
            r.open(Open::Paragraph);
            r.run(Run::Code, forged);
            r.run(Run::Emph, forged);
            r.close();
            r.open(Open::Bullets);
            r.open(Open::Item(1));
            r.run(Run::Strong, forged);
            r.close();
            r.close();
            r.open(Open::Code);
            r.code_line(forged);
            r.close();
            r.open(Open::Table {
                align: &[Align::Left],
                widths: &[4],
            });
            r.open(Open::Row);
            r.open(Open::Cell { width: 4 });
            r.run(Run::Strong, forged);
            r.close();
            r.close();
            r.close();
            r.finish();
            drop(r);
            all
        };
        let out = drive(Tier::Rich, None);
        for ev in wire::parse(&out) {
            match ev {
                Event::Open(op, _) | Event::Close(op) => assert!(
                    matches!(
                        op,
                        Op::Hdr | Op::Em | Op::Pre | Op::Table | Op::Row | Op::Cell
                    ),
                    "a frame the renderer never emits: {:?}",
                    op
                ),
                Event::Point(op, _) => panic!("a point frame the renderer never emits: {:?}", op),
                Event::Text(t) => {
                    assert!(!t.contains(&0x1b), "an ESC reached the payload");
                    assert!(!t.contains(&0x07), "a BEL reached the payload");
                }
            }
        }
        for width in [None, Some(40)] {
            let plain = drive(Tier::None, width);
            assert!(!plain.contains(&0x1b) && !plain.contains(&0x07));
        }
    }

    #[test]
    fn contents_with_no_sections() {
        let rich = contents(&[], Tier::Rich);
        assert_eq!(
            s(&rich),
            "\x1b]1936;v1;hdr;level=1;class=title\x1b\\Thylacine Operator's Manual\x1b]1936;v1;/hdr\x1b\\\n\nNo sections are installed.\n"
        );
        assert_eq!(wire::strip(&rich), contents(&[], Tier::None));
    }

    #[test]
    fn contents_with_sections() {
        let listed = [
            Listed {
                name: String::from("containers"),
                title: String::from("Containers"),
            },
            Listed {
                name: String::from("audio"),
                title: String::from("Audio"),
            },
        ];
        let plain = contents(&listed, Tier::None);
        let expected = format!(
            "Thylacine Operator's Manual\n\nName{}Section\ncontainers{}Containers\naudio{}Audio\n\nShow a section with manual <name>.\n",
            sp(8),
            sp(2),
            sp(7)
        );
        assert_eq!(s(&plain), expected);
        assert_eq!(wire::strip(&contents(&listed, Tier::Rich)), plain);
        // A title is section text: its controls are replaced.
        let forged = [Listed {
            name: String::from("x"),
            title: String::from("A\x1b]1936;v1;obj\x07"),
        }];
        let out = contents(&forged, Tier::Rich);
        assert!(
            s(&out).contains("A\u{fffd}]1936;v1;obj\u{fffd}"),
            "{:?}",
            s(&out)
        );
    }

    /// Every installed section (docs/manual) checks clean, renders at both
    /// tiers, and keeps the strip identity. The directory must exist: an
    /// absent one and an empty one would otherwise both pass (8.1).
    #[test]
    fn every_installed_section_checks_and_renders() {
        extern crate std;
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../docs/manual");
        let mut checked = 0usize;
        for ent in std::fs::read_dir(&dir).expect("docs/manual must exist") {
            let path = ent.expect("a readable entry").path();
            let file = path.file_name().unwrap().to_string_lossy().into_owned();
            if file == ".gitkeep" {
                continue;
            }
            assert!(
                crate::catalog::parse_file_name(&file).is_some(),
                "docs/manual/{} is not named NN-<name>.md; a draft belongs in docs/manual-drafts",
                file
            );
            let src = std::fs::read_to_string(&path).expect("a readable section");
            let mut problems = Vec::new();
            format::check(Some(&file), &src, &mut |line, p| problems.push((line, p)));
            assert!(
                problems.is_empty(),
                "docs/manual/{} fails the check: {:?}",
                file,
                problems
            );
            let rich = render_all(&src, Tier::Rich, None);
            assert_eq!(
                wire::strip(&rich),
                render_all(&src, Tier::None, None),
                "{}",
                file
            );
            let _ = render_all(&src, Tier::None, Some(80));
            checked += 1;
        }
        std::eprintln!("docs/manual: {} section(s) checked", checked);
    }

    /// A tree built from the same events renders to the same bytes: the
    /// renderer's layout does not depend on how the parser delivers runs.
    #[test]
    fn rendering_matches_the_parsed_tree() {
        use crate::format::tree::{parse, plain_text, Block, Inline};
        let src = "# T *x*\n\nA \\*b\\* `c` *d* e_f.\n\n- one\n  two\n\n| h | `i` |\n| :---: | ---: |\n| **j** | k |\n";
        let doc = parse(src).unwrap();
        let mut expected = String::new();
        for (i, b) in doc.blocks.iter().enumerate() {
            if i > 0 {
                expected.push('\n');
            }
            match b {
                Block::Title(r) | Block::Paragraph(r) => {
                    expected += &plain_text(r);
                    expected.push('\n');
                }
                Block::Bullets(items) => {
                    for it in items {
                        expected += "- ";
                        expected += &plain_text(it);
                        expected.push('\n');
                    }
                }
                Block::Table { header, rows, .. } => {
                    assert_eq!(header[1], vec![Inline::Code(String::from("i"))]);
                    assert_eq!(rows[0][0], vec![Inline::Strong(String::from("j"))]);
                    expected += "h  i\nj  k\n";
                }
                other => panic!("{:?}", other),
            }
        }
        assert_eq!(s(&render_all(src, Tier::None, None)), expected);
    }
}
