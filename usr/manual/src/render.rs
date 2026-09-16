//! The realization (MANUAL-DESIGN.md section 4): a document as Beacon frames
//! around its payload at the rich tier, and as the same payload alone at the
//! plain tiers, where paragraphs and list items may be word-wrapped.

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use beacon::wire::{self, Op};
use beacon::Tier;

use crate::format::{plain_text, Align, Block, Document, Inline};
use crate::sanitize;
use crate::wrap::wrap;

/// The heading of the contents listing (5).
pub const BOOK_TITLE: &str = "Thylacine Operator's Manual";

/// Render a document at `tier`. `width` wraps paragraphs and list items at a
/// plain tier (4.3) and is ignored at the rich tier, where the renderer wraps.
pub fn render(doc: &Document, tier: Tier, width: Option<usize>) -> Vec<u8> {
    let mut w = Writer::new(tier);
    let width = if w.rich { None } else { width };
    for (i, block) in doc.blocks.iter().enumerate() {
        if i > 0 {
            w.text("\n");
        }
        match block {
            Block::Title(runs) => w.heading(1, false, runs),
            Block::Heading(level, runs) => w.heading(*level, false, runs),
            Block::Paragraph(runs) => w.flow("", runs, width),
            Block::Bullets(items) => {
                for item in items {
                    w.flow("- ", item, width);
                }
            }
            Block::Numbered(items) => {
                for (n, item) in items.iter().enumerate() {
                    w.flow(&format!("{}. ", n + 1), item, width);
                }
            }
            Block::Code(lines) => w.code(lines),
            Block::Table {
                align,
                header,
                rows,
            } => w.table(align, header, rows),
        }
    }
    w.out
}

/// One section as the contents listing shows it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Listed {
    pub name: String,
    pub title: String,
}

/// The contents listing (5): the book's title-page heading, then the sections
/// in book order with a line saying how to open one, or a line saying that no
/// sections are installed.
pub fn render_contents(sections: &[Listed], tier: Tier) -> Vec<u8> {
    let mut w = Writer::new(tier);
    w.heading(1, true, &[Inline::Text(String::from(BOOK_TITLE))]);
    w.text("\n");
    if sections.is_empty() {
        w.text("No sections are installed.\n");
        return w.out;
    }
    let text = |s: &str| vec![Inline::Text(String::from(s))];
    let header = vec![text("Name"), text("Section")];
    let rows: Vec<Vec<Vec<Inline>>> = sections
        .iter()
        .map(|s| vec![text(&s.name), text(&s.title)])
        .collect();
    w.table(&[Align::Left, Align::Left], &header, &rows);
    w.text("\n");
    w.runs(&[
        Inline::Text(String::from("Show a section with ")),
        Inline::Code(String::from("manual <name>")),
        Inline::Text(String::from(".")),
    ]);
    w.text("\n");
    w.out
}

struct Writer {
    out: Vec<u8>,
    rich: bool,
}

impl Writer {
    fn new(tier: Tier) -> Writer {
        Writer {
            out: Vec::new(),
            rich: tier == Tier::Rich,
        }
    }

    /// Payload, every tier. Callers pass only renderer text or sanitized
    /// section text.
    fn text(&mut self, s: &str) {
        self.out.extend_from_slice(s.as_bytes());
    }

    fn open(&mut self, op: Op, args: &[(&str, &str)]) {
        if self.rich {
            wire::open(&mut self.out, op, args);
        }
    }

    fn close(&mut self, op: Op) {
        if self.rich {
            wire::close(&mut self.out, op);
        }
    }

    fn runs(&mut self, runs: &[Inline]) {
        for r in runs {
            match r {
                Inline::Text(s) => self.text(&sanitize(s, false)),
                Inline::Code(s) => self.em("code", s),
                Inline::Emph(s) => self.em("emph", s),
                Inline::Strong(s) => self.em("strong", s),
            }
        }
    }

    fn em(&mut self, class: &str, s: &str) {
        self.open(Op::Em, &[("class", class)]);
        self.text(&sanitize(s, false));
        self.close(Op::Em);
    }

    fn heading(&mut self, level: u8, title_page: bool, runs: &[Inline]) {
        let lvl = match level {
            1 => "1",
            2 => "2",
            _ => "3",
        };
        if title_page {
            self.open(Op::Hdr, &[("level", lvl), ("class", "title")]);
        } else {
            self.open(Op::Hdr, &[("level", lvl)]);
        }
        self.runs(runs);
        self.close(Op::Hdr);
        self.text("\n");
    }

    fn flow(&mut self, marker: &str, runs: &[Inline], width: Option<usize>) {
        match width {
            Some(cols) => {
                for line in wrap(marker, runs, cols) {
                    self.text(&line);
                    self.text("\n");
                }
            }
            None => {
                self.text(marker);
                self.runs(runs);
                self.text("\n");
            }
        }
    }

    fn code(&mut self, lines: &[String]) {
        self.open(Op::Pre, &[]);
        for l in lines {
            self.text(&sanitize(l, true));
            self.text("\n");
        }
        self.close(Op::Pre);
    }

    /// A table laid out as `beacon::sink::Table` lays one out, so a manual
    /// table aligns like every other tool's: two spaces between columns, the
    /// last column unpadded, padding outside the cell frames.
    fn table(&mut self, align: &[Align], header: &[Vec<Inline>], rows: &[Vec<Vec<Inline>>]) {
        let ncols = align.len();
        let mut widths = vec![0usize; ncols];
        for row in core::iter::once(header).chain(rows.iter().map(|r| r.as_slice())) {
            for (i, cell) in row.iter().enumerate().take(ncols) {
                widths[i] = widths[i].max(plain_text(cell).chars().count());
            }
        }
        let spec: String = align
            .iter()
            .map(|a| match a {
                Align::Left => 'l',
                Align::Right => 'r',
                Align::Center => 'c',
            })
            .collect();
        self.open(Op::Table, &[("cols", spec.as_str()), ("hdr", "1")]);
        self.row(align, &widths, header);
        for r in rows {
            self.row(align, &widths, r);
        }
        self.close(Op::Table);
    }

    fn row(&mut self, align: &[Align], widths: &[usize], cells: &[Vec<Inline>]) {
        let ncols = align.len();
        let count = cells.len().min(ncols);
        self.open(Op::Row, &[]);
        for (i, cell) in cells.iter().enumerate().take(ncols) {
            let pad = widths[i].saturating_sub(plain_text(cell).chars().count());
            let (before, after) = match align[i] {
                Align::Right => (pad, 0),
                Align::Center => (pad / 2, pad - pad / 2),
                Align::Left => (0, pad),
            };
            if i > 0 {
                self.text("  ");
            }
            self.text(&" ".repeat(before));
            self.open(Op::Cell, &[]);
            self.runs(cell);
            self.close(Op::Cell);
            if i + 1 < count {
                self.text(&" ".repeat(after));
            }
        }
        self.close(Op::Row);
        self.text("\n");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::format::{check_section, parse};
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

    fn doc(src: &str) -> Document {
        parse(src).unwrap_or_else(|ds| panic!("fixture does not parse: {:?}", ds))
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
        let out = render(&doc(FIXTURE), Tier::Rich, None);
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
        let out = render(&doc(FIXTURE), Tier::None, None);
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
        let d = doc(FIXTURE);
        let rich = render(&d, Tier::Rich, None);
        assert_eq!(wire::strip(&rich), render(&d, Tier::None, None));
        assert_eq!(render(&d, Tier::Cells, None), render(&d, Tier::None, None));
        // Width never applies at the rich tier.
        assert_eq!(render(&d, Tier::Rich, Some(20)), rich);
    }

    #[test]
    fn the_table_matches_the_beacon_sink_table() {
        use beacon::sink::{Cell, Sink, Table};
        let d = doc("# T\n\n| Name | Count | Mid |\n| --- | ---: | :---: |\n| alpha | 1 | x |\n| b | 12345 | yy |\n");
        let ours = render(
            &Document {
                blocks: vec![d.blocks[1].clone()],
            },
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
        assert_eq!(s(&ours), s(&theirs));
    }

    #[test]
    fn plain_wrapping_applies_to_paragraphs_and_items_only() {
        let src = "# A title that is longer than the width\n\n\
A paragraph that needs to wrap at twenty columns.\n\n\
- An item that also wraps here.\n\n\
```\na code line that is longer than twenty\n```\n";
        let out = render(&doc(src), Tier::None, Some(20));
        let expected = "A title that is longer than the width\n\nA paragraph that\nneeds to wrap at\ntwenty columns.\n\n- An item that also\n  wraps here.\n\na code line that is longer than twenty\n";
        assert_eq!(s(&out), expected);
    }

    /// Section text cannot inject a frame or a control sequence (4.4). The
    /// document is built directly, bypassing the checker, which would reject
    /// the text first.
    #[test]
    fn section_text_cannot_forge_frames() {
        let forged = "\x1b]1936;v1;obj;type=path;ref=/etc\x1b\\evil\x1b]1936;v1;/obj\x07";
        let d = Document {
            blocks: vec![
                Block::Title(vec![Inline::Text(String::from(forged))]),
                Block::Paragraph(vec![
                    Inline::Code(String::from(forged)),
                    Inline::Emph(String::from(forged)),
                ]),
                Block::Code(vec![String::from(forged)]),
                Block::Table {
                    align: vec![Align::Left],
                    header: vec![vec![Inline::Strong(String::from(forged))]],
                    rows: vec![],
                },
            ],
        };
        let out = render(&d, Tier::Rich, None);
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
        let plain = render(&d, Tier::None, Some(40));
        assert!(!plain.contains(&0x1b) && !plain.contains(&0x07));
    }

    #[test]
    fn contents_with_no_sections() {
        let rich = render_contents(&[], Tier::Rich);
        assert_eq!(
            s(&rich),
            "\x1b]1936;v1;hdr;level=1;class=title\x1b\\Thylacine Operator's Manual\x1b]1936;v1;/hdr\x1b\\\n\nNo sections are installed.\n"
        );
        assert_eq!(wire::strip(&rich), render_contents(&[], Tier::None));
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
        let plain = render_contents(&listed, Tier::None);
        let expected = format!(
            "Thylacine Operator's Manual\n\nName{}Section\ncontainers{}Containers\naudio{}Audio\n\nShow a section with manual <name>.\n",
            sp(8),
            sp(2),
            sp(7)
        );
        assert_eq!(s(&plain), expected);
        assert_eq!(wire::strip(&render_contents(&listed, Tier::Rich)), plain);
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
            let d = check_section(Some(&file), &src)
                .unwrap_or_else(|ds| panic!("docs/manual/{} fails the check: {:?}", file, ds));
            let rich = render(&d, Tier::Rich, None);
            assert_eq!(wire::strip(&rich), render(&d, Tier::None, None), "{}", file);
            let _ = render(&d, Tier::None, Some(80));
            checked += 1;
        }
        std::eprintln!("docs/manual: {} section(s) checked", checked);
    }
}
