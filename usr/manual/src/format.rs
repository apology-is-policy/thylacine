//! The section format (MANUAL-DESIGN.md section 3): a strict Markdown subset in
//! which every accepted form has both a Beacon and a plain realization.
//!
//! The parser is also the checker. It reads a section front to back and tells an
//! [`Events`] consumer what it finds as it goes: each problem, in line order, and
//! the section's blocks and inline runs. It keeps nothing beyond the block it is
//! reading, so its memory is bounded by the largest block rather than by the
//! section, and every search it makes ahead of its position resumes rather than
//! repeats, so its time grows linearly with the section.

use core::fmt;
use core::mem;

use alloc::string::String;
use alloc::vec::Vec;

use crate::{catalog, is_bidi_control, is_control, SECTION_MAX};

/// The most columns a table may have (3.2).
pub const TABLE_COLUMNS_MAX: usize = 16;

/// The most characters a table cell's displayed text may hold (3.2). Every cell
/// pads to its column's widest, so this bounds the padding a row can carry, and
/// with it the output and the time a table takes (4.4).
pub const TABLE_CELL_MAX: usize = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Align {
    Left,
    Right,
    Center,
}

/// The form of an inline run (3.3).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Run {
    Text,
    Code,
    Emph,
    Strong,
}

/// A block, or a part of one, as the parser opens it. Each `open` is matched by
/// one `close`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Open<'a> {
    Title,
    /// Level 2 or 3.
    Heading(u8),
    Paragraph,
    Bullets,
    Numbered,
    /// A list item, numbered from 1 within its list.
    Item(usize),
    Code,
    /// `widths` holds each column's widest cell, in Unicode scalar values, when
    /// the consumer measures tables; it is empty otherwise.
    Table {
        align: &'a [Align],
        widths: &'a [usize],
    },
    /// The first row of a table is its header.
    Row,
    /// `width` is the cell's own width when the consumer measures tables.
    Cell {
        width: usize,
    },
}

/// The block a line should have been separated from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Above {
    Heading,
    CodeBlock,
    Table,
    List,
    Paragraph,
}

/// A construct the format rejects (3.1 to 3.3).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Problem {
    TooLarge,
    ByteOrderMark,
    CarriageReturn,
    Tab,
    Control(char),
    BidiControl(char),
    NoTitle,
    TitleNumber,
    LevelOneHeading,
    DeepHeading,
    HeadingText,
    ClosingHashes,
    HeadingSpace,
    NotSeparated(Above),
    BadFence,
    UnclosedFence,
    EmptyCodeBlock,
    BadBullet,
    BadNumbered,
    /// The number the item should have had.
    NumberGap(usize),
    ItemText,
    ItemSpace,
    /// The indentation continuation lines of this item take.
    ContinuationIndent(usize),
    NestedList,
    TooManyColumns,
    CellTooWide,
    NoDelimiterRow,
    DelimiterCell,
    CellCount {
        cells: usize,
        header: usize,
    },
    RowPipes,
    ParagraphIndent,
    Indented,
    BlockQuote,
    ThematicBreak,
    Setext,
    Html,
    LinkDefinition,
    Footnote,
    HardBreak,
    Angle,
    CharacterReference,
    CodeInEmphasis,
    CodeSpanTicks,
    UnclosedCodeSpan,
    EmptyCodeSpan,
    NestedEmphasis,
    UnmatchedStar,
    Image,
    Link,
    Strikethrough,
    Underscore,
}

impl fmt::Display for Problem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use Problem::*;
        let m = match *self {
            TooLarge => "the section is larger than 1 MiB",
            ByteOrderMark => "the file begins with a byte-order mark",
            CarriageReturn => "a carriage return; a section uses LF line endings",
            Tab => "a tab outside a code block",
            Control(c) => return write!(f, "the control character U+{:04X}", c as u32),
            BidiControl(c) => {
                return write!(
                    f,
                    "the bidirectional control character U+{:04X}, which reorders how text is displayed",
                    c as u32
                )
            }
            NoTitle => "a section begins with its title on line 1, written '# Title'",
            TitleNumber => {
                "the title begins with the section number; the number only orders the book"
            }
            LevelOneHeading => "only the title, on line 1, is a level-1 heading",
            DeepHeading => "headings deeper than ### are not supported",
            HeadingText => "a heading has text after its marker",
            ClosingHashes => "closing hashes are not supported; end the heading at its text",
            HeadingSpace => "one space separates a heading's marker from its text",
            NotSeparated(above) => {
                let what = match above {
                    Above::Heading => "heading",
                    Above::CodeBlock => "code block",
                    Above::Table => "table",
                    Above::List => "list",
                    Above::Paragraph => "paragraph",
                };
                return write!(
                    f,
                    "a blank line must separate this line from the {} above",
                    what
                );
            }
            BadFence => "a code block fence is a line of exactly three backticks, optionally followed by one word",
            UnclosedFence => {
                "the code block opened here is not closed by a line of three backticks"
            }
            EmptyCodeBlock => "a code block has at least one line of content",
            BadBullet => "a bulleted item begins with '- '",
            BadNumbered => "a numbered item begins with 'N. '",
            NumberGap(expected) => {
                return write!(
                    f,
                    "numbered items run from 1 without gaps; expected {}",
                    expected
                )
            }
            ItemText => "a list item has text after its marker",
            ItemSpace => "one space separates a list marker from its text",
            ContinuationIndent(col) => {
                return write!(
                    f,
                    "a continuation line is indented by exactly {} spaces",
                    col
                )
            }
            NestedList => "nested lists are not supported",
            TooManyColumns => {
                return write!(f, "a table has at most {} columns", TABLE_COLUMNS_MAX)
            }
            CellTooWide => {
                return write!(
                    f,
                    "a table cell holds at most {} characters",
                    TABLE_CELL_MAX
                )
            }
            NoDelimiterRow => {
                "a table's header row is followed by a delimiter row, such as | --- | --- |"
            }
            DelimiterCell => "a delimiter cell is ---, :---, ---:, or :---:",
            CellCount { cells, header } => {
                return write!(
                    f,
                    "this row has {} cells; the header has {}",
                    cells, header
                )
            }
            RowPipes => "a table row begins and ends with |",
            ParagraphIndent => "a paragraph's lines start at the left margin",
            Indented => {
                "unexpected indentation; a block starts at the left margin (for code, use a ``` fence)"
            }
            BlockQuote => "block quotes are not supported",
            ThematicBreak => {
                "thematic breaks are not supported; structure a section with headings"
            }
            Setext => "setext headings are not supported; write '## Heading'",
            Html => "raw HTML is not supported",
            LinkDefinition => "link reference definitions are not supported",
            Footnote => "footnotes are not supported",
            HardBreak => "hard line breaks are not supported; end the line without two trailing spaces or a backslash",
            Angle => "write '<' and '>' inside a code span (a placeholder is written `<name>`) or escape them",
            CharacterReference => {
                "character references such as &amp; are not supported; write the character itself, or escape the ampersand as \\&"
            }
            CodeInEmphasis => "emphasis cannot contain a code span",
            CodeSpanTicks => "a code span is delimited by one or two backticks",
            UnclosedCodeSpan => "this code span is not closed",
            EmptyCodeSpan => "an empty code span",
            NestedEmphasis => "emphasis does not nest",
            UnmatchedStar => "an unmatched '*'; write a literal asterisk as \\*",
            Image => "images are not supported",
            Link => "links are not supported; name the section or resource in prose",
            Strikethrough => "strikethrough is not supported",
            Underscore => "underscore emphasis is not supported; write *emphasis*",
        };
        f.write_str(m)
    }
}

/// What the parser reports as it reads. Every method has an empty default.
pub trait Events {
    /// A problem on a 1-based line. Problems arrive in line order, and one
    /// problem is reported at most once per line.
    fn problem(&mut self, _line: usize, _problem: Problem) {}
    fn open(&mut self, _block: Open<'_>) {}
    fn close(&mut self) {}
    /// Inline content of the innermost open title, heading, paragraph, item or
    /// cell. Consecutive text runs continue one another.
    fn run(&mut self, _kind: Run, _text: &str) {}
    /// One content line of the open code block, verbatim.
    fn code_line(&mut self, _text: &str) {}
    /// Whether `Open::Table` and `Open::Cell` carry widths.
    fn measures_tables(&self) -> bool {
        false
    }
}

/// Read a section, reporting to `events`. `file_name` is the section file's last
/// path component; when it is `NN-<name>.md`, the title must not begin with the
/// section number (3.2).
pub fn read(file_name: Option<&str>, src: &str, events: &mut dyn Events) {
    let measure = events.measures_tables();
    Parser::new(src, events, measure).run(file_name);
}

/// Check a section: `report` receives each problem, in line order. Returns the
/// number of problems; a section passes when it has none.
pub fn check(file_name: Option<&str>, src: &str, report: &mut dyn FnMut(usize, Problem)) -> usize {
    struct Checker<'r> {
        report: &'r mut dyn FnMut(usize, Problem),
        count: usize,
    }
    impl Events for Checker<'_> {
        fn problem(&mut self, line: usize, problem: Problem) {
            self.count += 1;
            (self.report)(line, problem);
        }
    }
    let mut checker = Checker { report, count: 0 };
    read(file_name, src, &mut checker);
    checker.count
}

/// The title's plain text from line 1, or `None` when line 1 is not a title.
/// The contents listing uses it; a title with inline errors still yields text.
pub fn title_text(src: &str) -> Option<String> {
    let body = src.strip_prefix('\u{feff}').unwrap_or(src);
    let first = body.split('\n').next()?;
    let text = first.strip_prefix("# ")?.trim();
    if text.is_empty() {
        return None;
    }
    struct Quiet;
    impl Events for Quiet {}
    let mut quiet = Quiet;
    let mut out = String::new();
    Parser::new(text, &mut quiet, false).inline_one(1, text, false, &mut Sink::Plain(&mut out));
    Some(out)
}

fn repeats_number(title: &str, n: u8) -> bool {
    let t = title.trim_start();
    [alloc::format!("{:02}", n), alloc::format!("{}", n)]
        .iter()
        .any(|p| match t.strip_prefix(p.as_str()) {
            Some(rest) => !rest.starts_with(|c: char| c.is_ascii_digit()),
            None => false,
        })
}

fn is_blank(line: &str) -> bool {
    line.trim().is_empty()
}

fn leading_spaces(line: &str) -> usize {
    line.len() - line.trim_start_matches(' ').len()
}

// ---------------------------------------------------------------------------
// Line classification (3.2)
// ---------------------------------------------------------------------------

/// What a line begins, judged from the line alone.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind<'a> {
    Heading(usize, &'a str),
    Fence,
    BadFence,
    Bullet(&'a str),
    BadBullet,
    /// The number, the marker's width, the text after the marker.
    Numbered(u64, usize, &'a str),
    BadNumbered,
    TableRow,
    Quote,
    Break(char),
    Setext,
    Html,
    LinkDef,
    Footnote,
    Indented,
    Text,
}

fn classify(line: &str) -> Kind<'_> {
    let b = line.as_bytes();
    if b.is_empty() {
        return Kind::Text;
    }
    if b[0] == b' ' || b[0] == b'\t' {
        return Kind::Indented;
    }
    if let Some(c) = thematic_break(line) {
        return Kind::Break(c);
    }
    if b[0] == b'#' {
        let level = b.iter().take_while(|&&c| c == b'#').count();
        let rest = &line[level..];
        if rest.is_empty() {
            return Kind::Heading(level, "");
        }
        return match rest.strip_prefix(' ') {
            Some(text) => Kind::Heading(level, text),
            None => Kind::Text,
        };
    }
    if let Some(info) = line.strip_prefix("```") {
        if info.contains('`') || info.contains(char::is_whitespace) {
            return Kind::BadFence;
        }
        return Kind::Fence;
    }
    if line.starts_with("~~~") {
        return Kind::BadFence;
    }
    if let Some(text) = line.strip_prefix("- ") {
        return Kind::Bullet(text);
    }
    if line == "-" {
        return Kind::Bullet("");
    }
    if line.starts_with("* ") || line.starts_with("+ ") || line == "*" || line == "+" {
        return Kind::BadBullet;
    }
    let digits = b.iter().take_while(|c| c.is_ascii_digit()).count();
    if (1..=9).contains(&digits) {
        let num: u64 = line[..digits].parse().unwrap_or(0);
        let rest = &line[digits..];
        if let Some(text) = rest.strip_prefix(". ") {
            return Kind::Numbered(num, digits + 2, text);
        }
        if rest == "." {
            return Kind::Numbered(num, digits + 2, "");
        }
        if rest.starts_with(") ") || rest == ")" {
            return Kind::BadNumbered;
        }
    }
    match b[0] {
        b'|' => return Kind::TableRow,
        b'>' => return Kind::Quote,
        _ => {}
    }
    if line.trim_end().bytes().all(|c| c == b'=') {
        return Kind::Setext;
    }
    if b[0] == b'<'
        && b.len() > 1
        && (b[1].is_ascii_alphabetic() || matches!(b[1], b'/' | b'!' | b'?'))
    {
        return Kind::Html;
    }
    if b[0] == b'[' {
        if let Some(close) = line.find("]:") {
            if !line[1..close].contains(']') {
                return if line[1..].starts_with('^') {
                    Kind::Footnote
                } else {
                    Kind::LinkDef
                };
            }
        }
    }
    Kind::Text
}

/// Three or more of one of `-`, `*`, `_`, optionally separated by spaces.
fn thematic_break(line: &str) -> Option<char> {
    let mut kind: Option<char> = None;
    let mut count = 0;
    for c in line.chars() {
        match c {
            ' ' => {}
            '-' | '*' | '_' => {
                if kind.is_some_and(|k| k != c) {
                    return None;
                }
                kind = Some(c);
                count += 1;
            }
            _ => return None,
        }
    }
    if count >= 3 {
        kind
    } else {
        None
    }
}

fn is_list_marker(text: &str) -> bool {
    matches!(
        classify(text),
        Kind::Bullet(_) | Kind::Numbered(..) | Kind::BadBullet | Kind::BadNumbered
    )
}

/// How a line after a paragraph's first line takes part in the paragraph.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Cont {
    Text,
    Indented,
    /// A setext underline: rejected, and joins no text.
    Underline,
    /// Any other block, which ends the paragraph.
    Other,
}

fn continuation(line: &str) -> Cont {
    let t = line.trim_end_matches(' ');
    if !t.is_empty() && (t.bytes().all(|c| c == b'-') || t.bytes().all(|c| c == b'=')) {
        return Cont::Underline;
    }
    match classify(line) {
        Kind::Text => Cont::Text,
        Kind::Indented => Cont::Indented,
        Kind::Setext | Kind::Break('-') => Cont::Underline,
        _ => Cont::Other,
    }
}

fn delimiter_align(cell: &str) -> Option<Align> {
    if cell.starts_with("::") || cell.ends_with("::") {
        return None;
    }
    let dashes = cell.trim_start_matches(':').trim_end_matches(':');
    if dashes.len() < 3 || !dashes.bytes().all(|b| b == b'-') {
        return None;
    }
    let left = cell.starts_with(':');
    let right = cell.ends_with(':');
    Some(match (left, right) {
        (true, true) => Align::Center,
        (false, true) => Align::Right,
        _ => Align::Left,
    })
}

/// The cells of a table row, each trimmed and with `\|` still escaped. Once
/// exhausted, `closed` says whether the row ended with an unescaped `|`.
struct Cells<'a> {
    row: &'a str,
    k: usize,
    start: usize,
    closed: bool,
    done: bool,
}

fn cells(line: &str) -> Cells<'_> {
    // The leading `|` made the line a row; cells start after it.
    Cells {
        row: line.trim_end_matches(' '),
        k: 1,
        start: 1,
        closed: false,
        done: false,
    }
}

impl<'a> Iterator for Cells<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<&'a str> {
        if self.done {
            return None;
        }
        let b = self.row.as_bytes();
        while self.k < b.len() {
            let k = self.k;
            if b[k] == b'\\' && k + 1 < b.len() {
                // A backslash pairs with the character after it, so `\|` is not a
                // separator and neither is the pipe of `\\|`'s second pair.
                self.k += 2;
                self.closed = false;
                continue;
            }
            self.k += 1;
            if b[k] == b'|' {
                self.closed = true;
                let cell = self.row[self.start..k].trim();
                self.start = self.k;
                return Some(cell);
            }
            self.closed = false;
        }
        self.done = true;
        if !self.closed {
            let tail = self.row.get(self.start..).unwrap_or("").trim();
            if !tail.is_empty() {
                return Some(tail);
            }
        }
        None
    }
}

/// A row's cell count, and whether it ended with an unescaped `|`.
fn row_shape(line: &str) -> (usize, bool) {
    let mut c = cells(line);
    let n = c.by_ref().count();
    (n, c.closed)
}

/// A cell's inline text: `\|` becomes `|`; any other backslash pair is left for
/// the inline scanner.
fn cell_text<'c>(cell: &'c str, buf: &'c mut String) -> &'c str {
    if !cell.contains('|') {
        return cell;
    }
    buf.clear();
    buf.reserve(cell.len());
    let b = cell.as_bytes();
    let mut from = 0;
    let mut k = 0;
    while k < b.len() {
        if b[k] == b'\\' && k + 1 < b.len() {
            if b[k + 1] == b'|' {
                buf.push_str(&cell[from..k]);
                from = k + 1;
            }
            k += 2;
        } else {
            k += 1;
        }
    }
    buf.push_str(&cell[from..]);
    buf
}

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------

/// A line of the section: its text without the LF, its 1-based number, and the
/// byte offset it starts at.
#[derive(Clone, Copy, Debug)]
struct Line<'s> {
    text: &'s str,
    no: usize,
    pos: usize,
}

/// How the lines of a block map onto its joined inline text, and what each line
/// checks as the scan enters and leaves it.
#[derive(Clone, Copy)]
enum Lines<'s> {
    /// One line whose own structure is already checked: a heading, a table
    /// cell, a title's text.
    One,
    /// A paragraph from line `first`; `last_text` is its last line that joins
    /// text (a setext underline joins none).
    Paragraph { first: usize, last_text: usize },
    /// A list item: its marker line and the text after the marker, the
    /// indentation its continuation lines take, and its last line.
    Item {
        first: usize,
        text: &'s str,
        col: usize,
        last: usize,
    },
}

/// A scan over a block's joined inline text, where each line break of the
/// source is a `\n`. `line` is the source line holding the scan's position.
struct Scan<'j, 's> {
    text: &'j str,
    lines: Lines<'s>,
    line: Line<'s>,
    report: bool,
}

/// Where a scan's runs go.
enum Sink<'b> {
    /// To the consumer, as `run` events.
    Events,
    /// Appended as plain text: an emphasis body, a title's text.
    Plain(&'b mut String),
    /// Counted in Unicode scalar values: a table cell's width.
    Count(&'b mut usize),
    /// Nowhere: inline content checked outside any block.
    Drop,
}

/// Where the searches of one scan resume. Each records a position from which a
/// search is known to find nothing, or what the last search found.
struct Look {
    code_none_from: [usize; 3],
    emph_none_from: [usize; 3],
    bracket: Option<(usize, Option<usize>)>,
    underscore_closer: Option<Option<usize>>,
}

impl Look {
    fn new() -> Look {
        Look {
            code_none_from: [usize::MAX; 3],
            emph_none_from: [usize::MAX; 3],
            bracket: None,
            underscore_closer: None,
        }
    }
}

struct Parser<'s, 'e> {
    src: &'s str,
    events: &'e mut dyn Events,
    measure: bool,
    /// The line of the last problem reported, and the problems reported on it.
    problem_line: usize,
    seen: Vec<Problem>,
    /// A CRLF file is reported once, not once per line.
    cr_reported: bool,
    title_seen: bool,
    blocks: usize,
    /// Scratch space kept from block to block.
    joined: String,
    body: String,
    cell: String,
}

impl<'s, 'e> Parser<'s, 'e> {
    fn new(src: &'s str, events: &'e mut dyn Events, measure: bool) -> Parser<'s, 'e> {
        Parser {
            src,
            events,
            measure,
            problem_line: 0,
            seen: Vec::new(),
            cr_reported: false,
            title_seen: false,
            blocks: 0,
            joined: String::new(),
            body: String::new(),
            cell: String::new(),
        }
    }

    fn line(&self, pos: usize, no: usize) -> Option<Line<'s>> {
        let len = self.src.len();
        if pos > len || (pos == len && len > 0) {
            return None;
        }
        let rest = &self.src[pos..];
        let text = match rest.find('\n') {
            Some(i) => &rest[..i],
            None => rest,
        };
        Some(Line { text, no, pos })
    }

    fn after(&self, l: Line<'s>) -> Option<Line<'s>> {
        self.line(l.pos + l.text.len() + 1, l.no + 1)
    }

    fn problem(&mut self, line: usize, problem: Problem) {
        debug_assert!(
            line >= self.problem_line,
            "{:?} on line {} reported after a problem on line {}",
            problem,
            line,
            self.problem_line
        );
        if line != self.problem_line {
            self.problem_line = line;
            self.seen.clear();
        }
        if self.seen.contains(&problem) {
            return;
        }
        self.seen.push(problem);
        self.events.problem(line, problem);
    }

    fn open_block(&mut self, block: Open<'_>) {
        self.blocks += 1;
        self.events.open(block);
    }

    fn run(&mut self, file_name: Option<&str>) {
        // The title check reads line 1 on its own, so it is reported first.
        if let Some((n, _)) = file_name.and_then(catalog::parse_file_name) {
            if title_text(self.src).is_some_and(|t| repeats_number(&t, n)) {
                self.problem(1, Problem::TitleNumber);
            }
        }
        if self.src.len() > SECTION_MAX {
            self.problem(1, Problem::TooLarge);
            return;
        }
        if let Some(rest) = self.src.strip_prefix('\u{feff}') {
            self.problem(1, Problem::ByteOrderMark);
            self.src = rest;
        }
        let mut next = self.line(0, 1);
        if !matches!(next.map(|l| classify(l.text)), Some(Kind::Heading(1, _))) {
            self.problem(1, Problem::NoTitle);
        }
        while let Some(l) = next {
            if is_blank(l.text) {
                self.check_chars(l, false);
                next = self.after(l);
                continue;
            }
            let kind = classify(l.text);
            let rejected = match kind {
                Kind::Heading(level, text) => {
                    next = self.heading(l, level, text);
                    continue;
                }
                Kind::Fence => {
                    next = self.fence(l);
                    continue;
                }
                Kind::BadFence => {
                    next = self.bad_fence(l);
                    continue;
                }
                Kind::Bullet(_) | Kind::Numbered(..) => {
                    next = self.list(l, kind);
                    continue;
                }
                Kind::TableRow => {
                    next = self.table(l);
                    continue;
                }
                Kind::Text => {
                    next = self.paragraph(l);
                    continue;
                }
                Kind::BadBullet => Problem::BadBullet,
                Kind::BadNumbered => Problem::BadNumbered,
                Kind::Quote => Problem::BlockQuote,
                Kind::Break(_) => Problem::ThematicBreak,
                Kind::Setext => Problem::Setext,
                Kind::Html => Problem::Html,
                Kind::LinkDef => Problem::LinkDefinition,
                Kind::Footnote => Problem::Footnote,
                Kind::Indented => Problem::Indented,
            };
            self.check_chars(l, false);
            self.problem(l.no, rejected);
            next = self.after(l);
        }
    }

    /// A carriage return (reported once per file) and the first other control
    /// character on the line (TAB is allowed only inside a code block), or its
    /// first bidirectional control, which no block allows.
    fn check_chars(&mut self, l: Line<'s>, in_code: bool) {
        if l.text.contains('\r') && !self.cr_reported {
            self.cr_reported = true;
            self.problem(l.no, Problem::CarriageReturn);
        }
        for c in l.text.chars() {
            if c == '\r' || (c == '\t' && in_code) {
                continue;
            }
            if c == '\t' {
                self.problem(l.no, Problem::Tab);
                return;
            }
            if is_control(c) {
                self.problem(l.no, Problem::Control(c));
                return;
            }
            if is_bidi_control(c) {
                self.problem(l.no, Problem::BidiControl(c));
                return;
            }
        }
    }

    fn expect_blank(&mut self, next: Option<Line<'s>>, above: Above) {
        if let Some(n) = next {
            if !is_blank(n.text) {
                self.problem(n.no, Problem::NotSeparated(above));
            }
        }
    }

    fn heading(&mut self, l: Line<'s>, level: usize, text: &'s str) -> Option<Line<'s>> {
        self.check_chars(l, false);
        let body = text.trim();
        if body.is_empty() {
            self.problem(l.no, Problem::HeadingText);
        }
        let unhashed = body.trim_end_matches('#');
        if unhashed.len() < body.len() && (unhashed.is_empty() || unhashed.ends_with(' ')) {
            self.problem(l.no, Problem::ClosingHashes);
        }
        if text.starts_with(' ') {
            self.problem(l.no, Problem::HeadingSpace);
        }
        let block = match level {
            // A title below line 1 has already been reported by `run`.
            1 if !self.title_seen && self.blocks == 0 => {
                self.title_seen = true;
                Some(Open::Title)
            }
            2 | 3 => Some(Open::Heading(level as u8)),
            _ => None,
        };
        match block {
            Some(b) => {
                self.open_block(b);
                self.inline_one(l.no, body, true, &mut Sink::Events);
                self.events.close();
            }
            None => self.inline_one(l.no, body, true, &mut Sink::Drop),
        }
        match level {
            1 if block.is_none() => self.problem(l.no, Problem::LevelOneHeading),
            1..=3 => {}
            _ => self.problem(l.no, Problem::DeepHeading),
        }
        let next = self.after(l);
        self.expect_blank(next, Above::Heading);
        next
    }

    fn fence(&mut self, open: Line<'s>) -> Option<Line<'s>> {
        self.check_chars(open, false);
        // Find the closing fence first, so the opening line's problems precede
        // those of the content.
        let mut close = self.after(open);
        while let Some(c) = close {
            if c.text == "```" {
                break;
            }
            close = self.after(c);
        }
        let Some(close) = close else {
            self.problem(open.no, Problem::UnclosedFence);
            let mut l = self.after(open);
            while let Some(c) = l {
                self.check_chars(c, true);
                l = self.after(c);
            }
            return None;
        };
        if close.no == open.no + 1 {
            self.problem(open.no, Problem::EmptyCodeBlock);
        }
        self.open_block(Open::Code);
        let mut l = self.after(open);
        while let Some(c) = l {
            if c.no == close.no {
                break;
            }
            self.check_chars(c, true);
            self.events.code_line(c.text);
            l = self.after(c);
        }
        self.events.close();
        let next = self.after(close);
        self.expect_blank(next, Above::CodeBlock);
        next
    }

    /// A fence the format rejects (`~~~`, four or more backticks, or an info
    /// string that is not one word). Its body is skipped to the matching fence,
    /// so the content is not reported as Markdown it was never meant to be.
    fn bad_fence(&mut self, open: Line<'s>) -> Option<Line<'s>> {
        self.check_chars(open, false);
        self.problem(open.no, Problem::BadFence);
        let closer = if open.text.starts_with("~~~") {
            "~~~"
        } else {
            &open.text[..open.text.bytes().take_while(|&b| b == b'`').count()]
        };
        let mut l = self.after(open);
        while let Some(c) = l {
            if c.text.trim_end() == closer {
                return self.after(c);
            }
            self.check_chars(c, true);
            l = self.after(c);
        }
        None
    }

    fn list(&mut self, first: Line<'s>, first_kind: Kind<'s>) -> Option<Line<'s>> {
        let bullets = matches!(first_kind, Kind::Bullet(_));
        self.open_block(if bullets {
            Open::Bullets
        } else {
            Open::Numbered
        });
        let mut items = 0usize;
        let mut l = first;
        let mut kind = first_kind;
        let next = loop {
            self.check_chars(l, false);
            let (text, col) = match kind {
                Kind::Bullet(t) => (t, 2),
                Kind::Numbered(num, width, t) => {
                    if num != (items + 1) as u64 {
                        self.problem(l.no, Problem::NumberGap(items + 1));
                    }
                    (t, width)
                }
                // Only item lines reach here: `run` and the check below.
                _ => (l.text, 0),
            };
            if text.trim().is_empty() {
                self.problem(l.no, Problem::ItemText);
            }
            if text.starts_with(' ') {
                self.problem(l.no, Problem::ItemSpace);
            }
            let mut last = l;
            let mut after = self.after(l);
            while let Some(c) = after {
                if is_blank(c.text) || !c.text.starts_with(' ') {
                    break;
                }
                last = c;
                after = self.after(c);
            }
            let mut joined = mem::take(&mut self.joined);
            joined.clear();
            joined.reserve(last.pos + last.text.len() - l.pos);
            joined.push_str(text.trim_start().trim_end_matches(' '));
            let mut c = l;
            while c.no < last.no {
                let Some(n) = self.after(c) else { break };
                c = n;
                joined.push('\n');
                joined.push_str(c.text[leading_spaces(c.text)..].trim_end_matches(' '));
            }
            items += 1;
            self.events.open(Open::Item(items));
            let mut sc = Scan {
                text: &joined,
                lines: Lines::Item {
                    first: l.no,
                    text,
                    col,
                    last: last.no,
                },
                line: l,
                report: true,
            };
            self.scan(&mut sc, 0, joined.len(), false, &mut Sink::Events);
            self.events.close();
            self.joined = joined;
            match after {
                None => break None,
                Some(c) if is_blank(c.text) => break Some(c),
                Some(c) => {
                    let k = classify(c.text);
                    let same = match k {
                        Kind::Bullet(_) => bullets,
                        Kind::Numbered(..) => !bullets,
                        _ => false,
                    };
                    if !same {
                        self.problem(c.no, Problem::NotSeparated(Above::List));
                        break Some(c);
                    }
                    l = c;
                    kind = k;
                }
            }
        };
        self.events.close();
        next
    }

    fn table(&mut self, head: Line<'s>) -> Option<Line<'s>> {
        self.check_chars(head, false);
        let (ncols, closed) = row_shape(head.text);
        if !closed {
            self.problem(head.no, Problem::RowPipes);
        }
        if ncols > TABLE_COLUMNS_MAX {
            self.problem(head.no, Problem::TooManyColumns);
        }
        let Some(delim) = self
            .after(head)
            .filter(|d| classify(d.text) == Kind::TableRow)
        else {
            for cell in cells(head.text) {
                self.cell_inline(head.no, cell, true, &mut Sink::Drop);
            }
            self.problem(head.no, Problem::NoDelimiterRow);
            let next = self.after(head);
            self.expect_blank(next, Above::Table);
            return next;
        };
        let cols = ncols.min(TABLE_COLUMNS_MAX);
        let mut align = [Align::Left; TABLE_COLUMNS_MAX];
        for (c, cell) in cells(delim.text).take(cols).enumerate() {
            align[c] = delimiter_align(cell).unwrap_or(Align::Left);
        }
        let mut widths = [0usize; TABLE_COLUMNS_MAX];
        if self.measure {
            let mut row = Some(head);
            while let Some(r) = row {
                if r.no != delim.no {
                    if r.no != head.no && classify(r.text) != Kind::TableRow {
                        break;
                    }
                    for (c, cell) in cells(r.text).take(cols).enumerate() {
                        widths[c] = widths[c].max(self.cell_width(cell));
                    }
                }
                row = self.after(r);
            }
        }
        let widths: &[usize] = if self.measure { &widths[..cols] } else { &[] };
        self.open_block(Open::Table {
            align: &align[..cols],
            widths,
        });
        self.row(head);
        // The delimiter row's problems follow the header's, which are on the
        // line above.
        self.check_chars(delim, false);
        let (dcells, dclosed) = row_shape(delim.text);
        if !dclosed {
            self.problem(delim.no, Problem::RowPipes);
        }
        if cells(delim.text).any(|c| delimiter_align(c).is_none()) {
            self.problem(delim.no, Problem::DelimiterCell);
        }
        if dcells != ncols {
            self.problem(
                delim.no,
                Problem::CellCount {
                    cells: dcells,
                    header: ncols,
                },
            );
        }
        let mut next = self.after(delim);
        while let Some(r) = next {
            if classify(r.text) != Kind::TableRow {
                break;
            }
            self.check_chars(r, false);
            let (n, closed) = row_shape(r.text);
            if !closed {
                self.problem(r.no, Problem::RowPipes);
            }
            if n != ncols {
                self.problem(
                    r.no,
                    Problem::CellCount {
                        cells: n,
                        header: ncols,
                    },
                );
            }
            self.row(r);
            next = self.after(r);
        }
        self.events.close();
        self.expect_blank(next, Above::Table);
        next
    }

    fn row(&mut self, r: Line<'s>) {
        self.events.open(Open::Row);
        for cell in cells(r.text) {
            let width = self.cell_width(cell);
            self.events.open(Open::Cell {
                width: if self.measure { width } else { 0 },
            });
            self.cell_inline(r.no, cell, true, &mut Sink::Events);
            self.events.close();
            if width > TABLE_CELL_MAX {
                self.problem(r.no, Problem::CellTooWide);
            }
        }
        self.events.close();
    }

    fn cell_width(&mut self, cell: &str) -> usize {
        let mut n = 0;
        self.cell_inline(0, cell, false, &mut Sink::Count(&mut n));
        n
    }

    fn cell_inline(&mut self, no: usize, cell: &str, report: bool, sink: &mut Sink<'_>) {
        let mut buf = mem::take(&mut self.cell);
        let text = cell_text(cell, &mut buf);
        self.inline_one(no, text, report, sink);
        self.cell = buf;
    }

    fn paragraph(&mut self, first: Line<'s>) -> Option<Line<'s>> {
        let mut last = first;
        let mut last_text = first.no;
        let mut stop = None;
        let mut next = self.after(first);
        while let Some(l) = next {
            if is_blank(l.text) {
                break;
            }
            match continuation(l.text) {
                Cont::Text | Cont::Indented => last_text = l.no,
                Cont::Underline => {}
                Cont::Other => {
                    stop = Some(l);
                    break;
                }
            }
            last = l;
            next = self.after(l);
        }
        let mut joined = mem::take(&mut self.joined);
        joined.clear();
        joined.reserve(last.pos + last.text.len() - first.pos);
        let mut l = first;
        loop {
            if l.no == first.no || continuation(l.text) != Cont::Underline {
                joined.push_str(l.text.trim_start().trim_end_matches(' '));
            }
            if l.no >= last.no {
                break;
            }
            let Some(n) = self.after(l) else { break };
            joined.push('\n');
            l = n;
        }
        self.open_block(Open::Paragraph);
        let mut sc = Scan {
            text: &joined,
            lines: Lines::Paragraph {
                first: first.no,
                last_text,
            },
            line: first,
            report: true,
        };
        self.enter(&sc, first);
        self.scan(&mut sc, 0, joined.len(), false, &mut Sink::Events);
        self.events.close();
        self.joined = joined;
        if let Some(s) = stop {
            self.problem(s.no, Problem::NotSeparated(Above::Paragraph));
        }
        next
    }

    // -----------------------------------------------------------------------
    // Inline forms (3.3)
    // -----------------------------------------------------------------------

    /// Scan one line's inline content, reported against line `no`.
    fn inline_one(&mut self, no: usize, text: &str, report: bool, sink: &mut Sink<'_>) {
        let mut sc = Scan {
            text,
            lines: Lines::One,
            line: Line {
                text: "",
                no,
                pos: 0,
            },
            report,
        };
        self.scan(&mut sc, 0, text.len(), false, sink);
    }

    /// The problems a line carries that its inline content does not, reported
    /// as the scan reaches the line.
    fn enter(&mut self, sc: &Scan<'_, 's>, l: Line<'s>) {
        match sc.lines {
            Lines::One => {}
            Lines::Paragraph { first, .. } => {
                if l.no != first {
                    match continuation(l.text) {
                        Cont::Indented => self.problem(l.no, Problem::ParagraphIndent),
                        Cont::Underline => {
                            self.check_chars(l, false);
                            self.problem(l.no, Problem::Setext);
                            return;
                        }
                        Cont::Text | Cont::Other => {}
                    }
                }
                self.check_chars(l, false);
            }
            Lines::Item { first, col, .. } => {
                // The marker line was checked before the item opened.
                if l.no != first {
                    self.check_chars(l, false);
                    let indent = leading_spaces(l.text);
                    if indent != col {
                        self.problem(l.no, Problem::ContinuationIndent(col));
                    } else if is_list_marker(&l.text[indent..]) {
                        self.problem(l.no, Problem::NestedList);
                    }
                }
            }
        }
    }

    /// A line that ends in a hard line break outside a code span, reported as
    /// the scan leaves the line. A block's last line of text has none.
    fn leave(&mut self, sc: &Scan<'_, 's>, l: Line<'s>) {
        let seg = match sc.lines {
            Lines::One => return,
            Lines::Paragraph { first, last_text } => {
                if l.no == last_text || (l.no != first && continuation(l.text) == Cont::Underline) {
                    return;
                }
                l.text.trim_start()
            }
            Lines::Item {
                first, text, last, ..
            } => {
                if l.no == last {
                    return;
                }
                if l.no == first {
                    text.trim_start()
                } else {
                    &l.text[leading_spaces(l.text)..]
                }
            }
        };
        let trimmed = seg.trim_end_matches(' ');
        let spaces = seg.len() - trimmed.len();
        let backslashes = trimmed.bytes().rev().take_while(|&b| b == b'\\').count();
        if spaces >= 2 || backslashes % 2 == 1 {
            self.problem(l.no, Problem::HardBreak);
        }
    }

    /// The scan passes a line break.
    fn cross(&mut self, sc: &mut Scan<'_, 's>, in_code: bool) {
        let left = sc.line;
        if sc.report && !in_code {
            self.leave(sc, left);
        }
        if let Lines::One = sc.lines {
            return;
        }
        if let Some(next) = self.after(left) {
            sc.line = next;
            if sc.report {
                self.enter(sc, next);
            }
        }
    }

    fn flag(&mut self, sc: &Scan<'_, 's>, problem: Problem) {
        if sc.report {
            self.problem(sc.line.no, problem);
        }
    }

    fn emit(&mut self, sink: &mut Sink<'_>, kind: Run, text: &str) {
        match sink {
            Sink::Events => self.events.run(kind, text),
            Sink::Plain(out) => out.push_str(text),
            Sink::Count(n) => **n += text.chars().count(),
            Sink::Drop => {}
        }
    }

    /// Scan `sc.text[lo..hi]`. Inside an emphasis body (`in_emphasis`), a code
    /// span or another asterisk is nesting, which 3.3 rejects.
    fn scan(
        &mut self,
        sc: &mut Scan<'_, 's>,
        lo: usize,
        hi: usize,
        in_emphasis: bool,
        sink: &mut Sink<'_>,
    ) {
        let t = sc.text;
        let b = t.as_bytes();
        let mut look = Look::new();
        let mut k = lo;
        while k < hi {
            match b[k] {
                b'\\' => {
                    if k + 1 < hi && b[k + 1].is_ascii_punctuation() {
                        self.emit(sink, Run::Text, &t[k + 1..k + 2]);
                        k += 2;
                    } else {
                        self.emit(sink, Run::Text, "\\");
                        k += 1;
                    }
                }
                b'\n' => {
                    self.cross(sc, false);
                    self.emit(sink, Run::Text, " ");
                    k += 1;
                }
                b'`' if in_emphasis => {
                    self.flag(sc, Problem::CodeInEmphasis);
                    self.emit(sink, Run::Text, "`");
                    k += 1;
                }
                b'`' => k = self.code_span(sc, k, hi, &mut look, sink),
                b'*' if in_emphasis => {
                    self.flag(sc, Problem::NestedEmphasis);
                    self.emit(sink, Run::Text, "*");
                    k += 1;
                }
                b'*' => k = self.emphasis(sc, lo, k, hi, &mut look, sink),
                b'<' | b'>' => {
                    self.flag(sc, Problem::Angle);
                    self.emit(sink, Run::Text, &t[k..k + 1]);
                    k += 1;
                }
                b'&' => {
                    if is_character_reference(&b[k + 1..hi]) {
                        self.flag(sc, Problem::CharacterReference);
                    }
                    self.emit(sink, Run::Text, "&");
                    k += 1;
                }
                b'[' => {
                    if sc.report {
                        self.bracket(sc, lo, k, hi, &mut look);
                    }
                    self.emit(sink, Run::Text, "[");
                    k += 1;
                }
                b'~' if k + 1 < hi && b[k + 1] == b'~' => {
                    self.flag(sc, Problem::Strikethrough);
                    self.emit(sink, Run::Text, "~~");
                    k += 2;
                }
                b'_' => {
                    if sc.report && underscore_opens(t, lo, k, hi, &mut look) {
                        self.problem(sc.line.no, Problem::Underscore);
                    }
                    self.emit(sink, Run::Text, "_");
                    k += 1;
                }
                _ => {
                    // Up to the next byte that can begin a form (every one is
                    // ASCII, so this ends on a character boundary). A `~` that
                    // does not begin `~~` is consumed on its own.
                    let end = b[k + 1..hi]
                        .iter()
                        .position(|&c| is_special(c))
                        .map_or(hi, |p| k + 1 + p);
                    self.emit(sink, Run::Text, &t[k..end]);
                    k = end;
                }
            }
        }
    }

    fn code_span(
        &mut self,
        sc: &mut Scan<'_, 's>,
        k: usize,
        hi: usize,
        look: &mut Look,
        sink: &mut Sink<'_>,
    ) -> usize {
        let t = sc.text;
        let b = t.as_bytes();
        let run = run_len(b, k, hi, b'`');
        if run > 2 {
            self.flag(sc, Problem::CodeSpanTicks);
            return k + run;
        }
        let Some(close) = find_code_close(b, k + run, hi, run, look) else {
            self.flag(sc, Problem::UnclosedCodeSpan);
            return k + run;
        };
        let raw = &t[k + run..close];
        let breaks = raw.bytes().filter(|&c| c == b'\n').count();
        let mut buf = mem::take(&mut self.body);
        let body = if breaks == 0 {
            raw
        } else {
            // A code span's line breaks join with a space, as everywhere else.
            buf.clear();
            buf.reserve(raw.len());
            buf.extend(raw.chars().map(|c| if c == '\n' { ' ' } else { c }));
            buf.as_str()
        };
        let body = if body.len() >= 2
            && body.starts_with(' ')
            && body.ends_with(' ')
            && !body.trim().is_empty()
        {
            &body[1..body.len() - 1]
        } else {
            body
        };
        if body.trim().is_empty() {
            self.flag(sc, Problem::EmptyCodeSpan);
        }
        self.emit(sink, Run::Code, body);
        self.body = buf;
        for _ in 0..breaks {
            self.cross(sc, true);
        }
        close + run
    }

    /// The asterisk run at `k`; returns the offset after what it consumed.
    fn emphasis(
        &mut self,
        sc: &mut Scan<'_, 's>,
        lo: usize,
        k: usize,
        hi: usize,
        look: &mut Look,
        sink: &mut Sink<'_>,
    ) -> usize {
        let t = sc.text;
        let b = t.as_bytes();
        let run = run_len(b, k, hi, b'*');
        let stars = &t[k..k + run];
        let space_before = k == lo || is_separator(b[k - 1]);
        let space_after = k + run >= hi || is_separator(b[k + run]);
        if space_before && space_after {
            self.emit(sink, Run::Text, stars);
            return k + run;
        }
        if run > 2 {
            self.flag(sc, Problem::NestedEmphasis);
            self.emit(sink, Run::Text, stars);
            return k + run;
        }
        let close = if space_after {
            None
        } else {
            find_emphasis_close(b, k + run, hi, run, look)
        };
        let Some(close) = close else {
            self.flag(sc, Problem::UnmatchedStar);
            self.emit(sink, Run::Text, stars);
            return k + run;
        };
        let kind = if run == 2 { Run::Strong } else { Run::Emph };
        match sink {
            Sink::Events => {
                let mut body = mem::take(&mut self.body);
                body.clear();
                body.reserve(close - (k + run));
                self.scan(sc, k + run, close, true, &mut Sink::Plain(&mut body));
                self.events.run(kind, &body);
                self.body = body;
            }
            // Plain text, a count, or nothing: the body's text is all that is kept.
            _ => self.scan(sc, k + run, close, true, sink),
        }
        close + run
    }

    /// A `[` that begins a link, an image, or a footnote reference.
    fn bracket(&mut self, sc: &Scan<'_, 's>, lo: usize, k: usize, hi: usize, look: &mut Look) {
        let b = sc.text.as_bytes();
        let Some(close) = next_bracket(b, k + 1, hi, look) else {
            return;
        };
        if close + 1 < hi && b[close + 1] == b'(' {
            let image = k > lo && b[k - 1] == b'!';
            self.problem(
                sc.line.no,
                if image { Problem::Image } else { Problem::Link },
            );
        } else if b[k + 1] == b'^' && close > k + 2 {
            self.problem(sc.line.no, Problem::Footnote);
        }
    }
}

/// A byte that can begin an inline form, or a line break.
fn is_special(c: u8) -> bool {
    matches!(
        c,
        b'\\' | b'\n' | b'`' | b'*' | b'<' | b'>' | b'[' | b'~' | b'_' | b'&'
    )
}

/// Whether the bytes after an `&` complete a character reference (3.3): a letter
/// followed by letters and digits, `#` followed by decimal digits, or `#x` or
/// `#X` followed by hexadecimal digits, then `;`. Any name counts, not only the
/// names a code host decodes, so no decoded form is missed. The walk stops at the
/// first byte outside the form, so the walks of successive `&`s never overlap.
fn is_character_reference(rest: &[u8]) -> bool {
    let (body, allowed): (&[u8], fn(&u8) -> bool) = match rest {
        [b'#', b'x' | b'X', tail @ ..] => (tail, u8::is_ascii_hexdigit),
        [b'#', tail @ ..] => (tail, u8::is_ascii_digit),
        [first, ..] if first.is_ascii_alphabetic() => (rest, u8::is_ascii_alphanumeric),
        _ => return false,
    };
    let n = body.iter().take_while(|&c| allowed(c)).count();
    n > 0 && body.get(n) == Some(&b';')
}

/// A space, or a line break, which joins lines as a space.
fn is_separator(c: u8) -> bool {
    c == b' ' || c == b'\n'
}

fn run_len(b: &[u8], k: usize, hi: usize, ch: u8) -> usize {
    b[k..hi].iter().take_while(|&&c| c == ch).count()
}

/// The first run of exactly `run` backticks at or after `from`. A search that
/// finds none records where it started: a later search from there on would
/// examine a subset of the same runs.
fn find_code_close(b: &[u8], from: usize, hi: usize, run: usize, look: &mut Look) -> Option<usize> {
    if from >= look.code_none_from[run] {
        return None;
    }
    let mut m = from;
    while m < hi {
        if b[m] == b'`' {
            let r = run_len(b, m, hi, b'`');
            if r == run {
                return Some(m);
            }
            m += r;
        } else {
            m += 1;
        }
    }
    look.code_none_from[run] = from;
    None
}

/// The first run of exactly `run` asterisks at or after `from` that is not
/// preceded by a space, skipping escaped characters. `from` always follows an
/// asterisk run, so every search reads the escapes after it the same way, and a
/// search that finds nothing settles every later one.
fn find_emphasis_close(
    b: &[u8],
    from: usize,
    hi: usize,
    run: usize,
    look: &mut Look,
) -> Option<usize> {
    if from >= look.emph_none_from[run] {
        return None;
    }
    let mut m = from;
    while m < hi {
        match b[m] {
            b'\\' => {
                m += 1;
                if m < hi {
                    m += utf8_len(b[m]);
                }
            }
            b'*' => {
                let r = run_len(b, m, hi, b'*');
                if r == run && !is_separator(b[m - 1]) {
                    return Some(m);
                }
                m += r;
            }
            _ => m += 1,
        }
    }
    look.emph_none_from[run] = from;
    None
}

/// The first `]` at or after `from`, resuming the previous search when it
/// already covers `from`.
fn next_bracket(b: &[u8], from: usize, hi: usize, look: &mut Look) -> Option<usize> {
    if let Some((start, found)) = look.bracket {
        if from >= start {
            match found {
                Some(q) if from <= q => return Some(q),
                None => return None,
                _ => {}
            }
        }
    }
    let found = b[from.min(hi)..hi]
        .iter()
        .position(|&c| c == b']')
        .map(|p| from + p);
    look.bracket = Some((from, found));
    found
}

fn utf8_len(lead: u8) -> usize {
    match lead {
        0x00..=0x7f => 1,
        0xc0..=0xdf => 2,
        0xe0..=0xef => 3,
        _ => 4,
    }
}

fn char_before(t: &str, k: usize) -> Option<char> {
    t[..k].chars().next_back()
}

fn char_at(t: &str, k: usize, hi: usize) -> Option<char> {
    if k < hi {
        t[k..hi].chars().next()
    } else {
        None
    }
}

/// An underscore that opens what Markdown elsewhere would render as emphasis:
/// at a word start, followed by a non-space, with a closing underscore at a
/// word end.
fn underscore_opens(t: &str, lo: usize, k: usize, hi: usize, look: &mut Look) -> bool {
    let after_word = k > lo && char_before(t, k).is_some_and(|c| c.is_alphanumeric());
    let opens =
        !after_word && char_at(t, k + 1, hi).is_some_and(|c| !c.is_whitespace() && c != '_');
    if !opens {
        return false;
    }
    let last = *look.underscore_closer.get_or_insert_with(|| {
        let b = t.as_bytes();
        (lo + 1..hi).rev().find(|&m| {
            b[m] == b'_'
                && !char_before(t, m).is_some_and(|c| c.is_whitespace())
                && !char_at(t, m + 1, hi).is_some_and(|c| c.is_alphanumeric())
        })
    });
    last.is_some_and(|m| m >= k + 2)
}

#[cfg(test)]
pub(crate) mod tree {
    //! A document tree built from the parser's events, and the diagnostics in
    //! the order they arrived: the shape the format tests assert. Building it
    //! also checks that the events are well formed.

    use super::*;
    use alloc::string::ToString;

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub enum Inline {
        Text(String),
        Code(String),
        Emph(String),
        Strong(String),
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub enum Block {
        Title(Vec<Inline>),
        Heading(u8, Vec<Inline>),
        Paragraph(Vec<Inline>),
        Bullets(Vec<Vec<Inline>>),
        Numbered(Vec<Vec<Inline>>),
        Code(Vec<String>),
        Table {
            align: Vec<Align>,
            header: Vec<Vec<Inline>>,
            rows: Vec<Vec<Vec<Inline>>>,
        },
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct Document {
        pub blocks: Vec<Block>,
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    pub struct Diagnostic {
        pub line: usize,
        pub message: String,
    }

    pub fn plain_text(runs: &[Inline]) -> String {
        let mut s = String::new();
        for r in runs {
            match r {
                Inline::Text(t) | Inline::Code(t) | Inline::Emph(t) | Inline::Strong(t) => {
                    s.push_str(t)
                }
            }
        }
        s
    }

    enum Frame {
        Runs(Open<'static>, Vec<Inline>),
        List(bool, Vec<Vec<Inline>>),
        Code(Vec<String>),
        Table(Vec<Align>, Vec<usize>, Vec<Vec<Vec<Inline>>>),
        Row(Vec<Vec<Inline>>),
    }

    struct Builder {
        measure: bool,
        blocks: Vec<Block>,
        stack: Vec<Frame>,
        diags: Vec<(usize, Problem)>,
        cell_width: Vec<usize>,
    }

    impl Events for Builder {
        fn problem(&mut self, line: usize, problem: Problem) {
            if let Some(&(last, _)) = self.diags.last() {
                assert!(
                    line >= last,
                    "{:?} on line {} after line {}",
                    problem,
                    line,
                    last
                );
            }
            assert!(
                !self.diags.contains(&(line, problem)),
                "{:?} reported twice on line {}",
                problem,
                line
            );
            self.diags.push((line, problem));
        }

        fn open(&mut self, block: Open<'_>) {
            let top_level = self.stack.is_empty();
            let frame = match block {
                Open::Title => {
                    assert!(top_level);
                    Frame::Runs(Open::Title, Vec::new())
                }
                Open::Heading(level) => {
                    assert!(top_level && (level == 2 || level == 3));
                    Frame::Runs(Open::Heading(level), Vec::new())
                }
                Open::Paragraph => {
                    assert!(top_level);
                    Frame::Runs(Open::Paragraph, Vec::new())
                }
                Open::Bullets | Open::Numbered => {
                    assert!(top_level);
                    Frame::List(block == Open::Numbered, Vec::new())
                }
                Open::Item(n) => {
                    match self.stack.last() {
                        Some(Frame::List(_, items)) => assert_eq!(n, items.len() + 1),
                        _ => panic!("an item outside a list"),
                    }
                    Frame::Runs(Open::Item(n), Vec::new())
                }
                Open::Code => {
                    assert!(top_level);
                    Frame::Code(Vec::new())
                }
                Open::Table { align, widths } => {
                    assert!(top_level);
                    assert_eq!(widths.len(), if self.measure { align.len() } else { 0 });
                    Frame::Table(align.to_vec(), widths.to_vec(), Vec::new())
                }
                Open::Row => {
                    assert!(matches!(self.stack.last(), Some(Frame::Table(..))));
                    Frame::Row(Vec::new())
                }
                Open::Cell { width } => {
                    assert!(matches!(self.stack.last(), Some(Frame::Row(_))));
                    self.cell_width.push(width);
                    Frame::Runs(Open::Cell { width }, Vec::new())
                }
            };
            self.stack.push(frame);
        }

        fn close(&mut self) {
            let frame = self.stack.pop().expect("a close without an open");
            let block = match frame {
                Frame::Runs(Open::Title, runs) => Block::Title(runs),
                Frame::Runs(Open::Heading(level), runs) => Block::Heading(level, runs),
                Frame::Runs(Open::Paragraph, runs) => Block::Paragraph(runs),
                Frame::Runs(Open::Item(_), runs) => {
                    match self.stack.last_mut() {
                        Some(Frame::List(_, items)) => items.push(runs),
                        _ => unreachable!(),
                    }
                    return;
                }
                Frame::Runs(Open::Cell { .. }, runs) => {
                    let width = self.cell_width.pop().unwrap();
                    if self.measure {
                        assert_eq!(width, plain_text(&runs).chars().count());
                    }
                    match self.stack.last_mut() {
                        Some(Frame::Row(cells)) => cells.push(runs),
                        _ => unreachable!(),
                    }
                    return;
                }
                Frame::Runs(..) => unreachable!(),
                Frame::List(numbered, items) => {
                    if numbered {
                        Block::Numbered(items)
                    } else {
                        Block::Bullets(items)
                    }
                }
                Frame::Code(lines) => Block::Code(lines),
                Frame::Row(cells) => {
                    match self.stack.last_mut() {
                        Some(Frame::Table(_, _, rows)) => rows.push(cells),
                        _ => unreachable!(),
                    }
                    return;
                }
                Frame::Table(align, widths, mut rows) => {
                    if self.measure {
                        for (c, w) in widths.iter().enumerate() {
                            let widest = rows
                                .iter()
                                .filter_map(|r| r.get(c))
                                .map(|cell| plain_text(cell).chars().count())
                                .max()
                                .unwrap_or(0);
                            assert_eq!(*w, widest, "column {}", c);
                        }
                    }
                    let header = if rows.is_empty() {
                        Vec::new()
                    } else {
                        rows.remove(0)
                    };
                    Block::Table {
                        align,
                        header,
                        rows,
                    }
                }
            };
            self.blocks.push(block);
        }

        fn run(&mut self, kind: Run, text: &str) {
            let Some(Frame::Runs(_, runs)) = self.stack.last_mut() else {
                panic!("a run outside a block that holds runs");
            };
            if kind == Run::Text {
                if let Some(Inline::Text(t)) = runs.last_mut() {
                    t.push_str(text);
                    return;
                }
            }
            let s = String::from(text);
            runs.push(match kind {
                Run::Text => Inline::Text(s),
                Run::Code => Inline::Code(s),
                Run::Emph => Inline::Emph(s),
                Run::Strong => Inline::Strong(s),
            });
        }

        fn code_line(&mut self, text: &str) {
            let Some(Frame::Code(lines)) = self.stack.last_mut() else {
                panic!("a code line outside a code block");
            };
            lines.push(String::from(text));
        }

        fn measures_tables(&self) -> bool {
            self.measure
        }
    }

    /// `check_section` for tests: the document when there are no problems,
    /// otherwise the diagnostics. Both measuring and not measuring are run,
    /// and they must agree.
    pub fn check_section(file_name: Option<&str>, src: &str) -> Result<Document, Vec<Diagnostic>> {
        let mut outcomes = [true, false].map(|measure| {
            let mut b = Builder {
                measure,
                blocks: Vec::new(),
                stack: Vec::new(),
                diags: Vec::new(),
                cell_width: Vec::new(),
            };
            read(file_name, src, &mut b);
            assert!(b.stack.is_empty(), "blocks left open");
            (b.blocks, b.diags)
        });
        let (blocks, diags) = mem::take(&mut outcomes[0]);
        assert_eq!(blocks, outcomes[1].0);
        assert_eq!(diags, outcomes[1].1);
        assert_eq!(check(file_name, src, &mut |_, _| {}), diags.len());
        if diags.is_empty() {
            return Ok(Document { blocks });
        }
        Err(diags
            .into_iter()
            .map(|(line, p)| Diagnostic {
                line,
                message: p.to_string(),
            })
            .collect())
    }

    pub fn parse(src: &str) -> Result<Document, Vec<Diagnostic>> {
        check_section(None, src)
    }
}

#[cfg(test)]
mod tests {
    use super::tree::{check_section, parse, Block, Document, Inline};
    use super::*;
    use alloc::vec;

    fn t(s: &str) -> Inline {
        Inline::Text(String::from(s))
    }

    fn ok(src: &str) -> Document {
        match parse(src) {
            Ok(d) => d,
            Err(ds) => panic!("expected a clean parse, got {:?}", ds),
        }
    }

    /// `src` contains one mistake: exactly one diagnostic, on `line`, containing
    /// `fragment`. A second diagnostic is a cascade the author would have to
    /// read past, so it fails the test.
    fn rejects(src: &str, line: usize, fragment: &str) {
        match parse(src) {
            Ok(_) => panic!("expected a diagnostic containing {:?}", fragment),
            Err(ds) => assert!(
                ds.len() == 1 && ds[0].line == line && ds[0].message.contains(fragment),
                "expected one diagnostic on line {} containing {:?}; got {:?}",
                line,
                fragment,
                ds
            ),
        }
    }

    /// The diagnostics of `src`, as (line, message fragment) pairs in order.
    fn diagnoses(src: &str, expected: &[(usize, &str)]) {
        let ds = parse(src).expect_err("expected diagnostics");
        assert_eq!(ds.len(), expected.len(), "got {:?}", ds);
        for (d, (line, fragment)) in ds.iter().zip(expected) {
            assert!(
                d.line == *line && d.message.contains(fragment),
                "expected line {} containing {:?}; got {:?}",
                line,
                fragment,
                ds
            );
        }
    }

    #[test]
    fn a_title_and_paragraph() {
        let d = ok("# Containers\n\nA container runs\na Linux program.\n");
        assert_eq!(
            d.blocks,
            vec![
                Block::Title(vec![t("Containers")]),
                Block::Paragraph(vec![t("A container runs a Linux program.")]),
            ]
        );
    }

    #[test]
    fn headings_of_level_two_and_three() {
        let d = ok("# T\n\n## In Practice\n\n### Run a program\n");
        assert_eq!(d.blocks[1], Block::Heading(2, vec![t("In Practice")]));
        assert_eq!(d.blocks[2], Block::Heading(3, vec![t("Run a program")]));
    }

    #[test]
    fn inline_forms() {
        let d = ok("# T\n\nRun `manual <name>` for *one* section, **not** all.\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![
                t("Run "),
                Inline::Code(String::from("manual <name>")),
                t(" for "),
                Inline::Emph(String::from("one")),
                t(" section, "),
                Inline::Strong(String::from("not")),
                t(" all."),
            ])
        );
    }

    #[test]
    fn code_span_forms() {
        let d = ok("# T\n\nA ``tick ` inside`` and `` `x` `` span.\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![
                t("A "),
                Inline::Code(String::from("tick ` inside")),
                t(" and "),
                Inline::Code(String::from("`x`")),
                t(" span."),
            ])
        );
    }

    #[test]
    fn escapes_and_literal_characters() {
        let d = ok("# T\n\nA \\*literal\\* star, 2 * 3, a \\<tag\\>, snake_case_name, \\\\ and a [bracket].\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![t(
                "A *literal* star, 2 * 3, a <tag>, snake_case_name, \\ and a [bracket]."
            )])
        );
    }

    #[test]
    fn any_ascii_punctuation_escapes() {
        let d = ok("# T\n\nA \\&, a \\~~, a \\[^1], and a \\_x_.\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![t("A &, a ~~, a [^1], and a _x_.")])
        );
    }

    #[test]
    fn emphasis_spans_lines_and_adjoins_punctuation() {
        let d = ok("# T\n\nThis is *very\nimportant*.\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![
                t("This is "),
                Inline::Emph(String::from("very important")),
                t(".")
            ])
        );
    }

    #[test]
    fn a_code_span_spans_lines() {
        let d = ok("# T\n\nRun `manual\n--check` first.\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![
                t("Run "),
                Inline::Code(String::from("manual --check")),
                t(" first.")
            ])
        );
    }

    /// A line break inside a code span is part of the span's content, not a hard
    /// line break, however the line ends.
    #[test]
    fn a_line_break_inside_a_code_span_is_not_a_hard_break() {
        let d = ok("# T\n\nA `code  \nspan` and `back\\\nslash` here.\n");
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![
                t("A "),
                Inline::Code(String::from("code span")),
                t(" and "),
                Inline::Code(String::from("back\\ slash")),
                t(" here."),
            ])
        );
        let d = ok("# T\n\n- An item with `code  \n  span` in it.\n");
        assert_eq!(
            d.blocks[1],
            Block::Bullets(vec![vec![
                t("An item with "),
                Inline::Code(String::from("code span")),
                t(" in it."),
            ]])
        );
    }

    #[test]
    fn lists() {
        let d = ok("# T\n\n- first\n  continued\n- second\n\n1. one\n2. two\n");
        assert_eq!(
            d.blocks[1],
            Block::Bullets(vec![vec![t("first continued")], vec![t("second")]])
        );
        assert_eq!(
            d.blocks[2],
            Block::Numbered(vec![vec![t("one")], vec![t("two")]])
        );
    }

    #[test]
    fn numbered_continuation_follows_the_marker_width() {
        let src = "# T\n\n1. a\n2. b\n3. c\n4. d\n5. e\n6. f\n7. g\n8. h\n9. i\n10. j\n    more\n";
        let d = ok(src);
        match &d.blocks[1] {
            Block::Numbered(items) => assert_eq!(items[9], vec![t("j more")]),
            other => panic!("{:?}", other),
        }
    }

    #[test]
    fn code_block_is_verbatim() {
        let d = ok("# T\n\n```sh\nmanual  --check *.md\n\tindented <x>\n```\n");
        assert_eq!(
            d.blocks[1],
            Block::Code(vec![
                String::from("manual  --check *.md"),
                String::from("\tindented <x>")
            ])
        );
    }

    #[test]
    fn table_with_alignment_and_escaped_pipe() {
        let d = ok("# T\n\n| Option | Default | Effect |\n| :--- | ---: | :---: |\n| `--beacon` | `auto` | a \\| b |\n");
        match &d.blocks[1] {
            Block::Table {
                align,
                header,
                rows,
            } => {
                assert_eq!(align, &vec![Align::Left, Align::Right, Align::Center]);
                assert_eq!(header[2], vec![t("Effect")]);
                assert_eq!(rows[0][0], vec![Inline::Code(String::from("--beacon"))]);
                assert_eq!(rows[0][2], vec![t("a | b")]);
            }
            other => panic!("{:?}", other),
        }
    }

    #[test]
    fn empty_table_cells_are_allowed() {
        let d = ok("# T\n\n| A | B |\n| --- | --- |\n| x |  |\n");
        match &d.blocks[1] {
            Block::Table { rows, .. } => assert_eq!(rows[0][1], Vec::<Inline>::new()),
            other => panic!("{:?}", other),
        }
    }

    #[test]
    fn a_file_without_a_final_newline_parses() {
        ok("# T\n\nLast line");
    }

    // --- rejected constructs (3.2, 3.3) -----------------------------------

    #[test]
    fn rejects_a_missing_title() {
        rejects("Intro.\n", 1, "begins with its title");
        rejects("\n# T\n", 1, "begins with its title");
        rejects("## T\n", 1, "begins with its title");
        rejects("", 1, "begins with its title");
    }

    #[test]
    fn rejects_heading_errors() {
        rejects("# T\n\n# Again\n", 3, "only the title");
        rejects("# T\n\n#### Deep\n", 3, "deeper than ###");
        rejects("# T\n\n## Closed ##\n", 3, "closing hashes");
        rejects("# T\n\n##\n", 3, "has text");
        rejects("#  T\n", 1, "one space");
        rejects("# T\n## Next\n", 2, "blank line must separate");
        rejects("# T\n\n## H\nText.\n", 4, "blank line must separate");
    }

    #[test]
    fn rejects_block_constructs() {
        rejects("# T\n\n> quoted\n", 3, "block quotes");
        rejects("# T\n\n---\n", 3, "thematic breaks");
        rejects("# T\n\n* item\n", 3, "begins with '- '");
        rejects("# T\n\n+ item\n", 3, "begins with '- '");
        rejects("# T\n\n1) item\n", 3, "begins with 'N. '");
        rejects("# T\n\n<div>x</div>\n", 3, "raw HTML");
        rejects(
            "# T\n\n[ref]: https://example.org\n",
            3,
            "link reference definitions",
        );
        rejects("# T\n\n[^1]: A note.\n", 3, "footnotes");
        rejects("# T\n\n    indented\n", 3, "unexpected indentation");
        rejects("# T\n\n~~~\nx\n~~~\n", 3, "three backticks");
        rejects("# T\n\n````\nx\n````\n", 3, "three backticks");
        rejects("# T\n\n``` sh\nx\n```\n", 3, "three backticks");
        rejects("# T\n\n```s`h\nx\n```\n", 3, "three backticks");
    }

    #[test]
    fn rejects_setext_headings() {
        rejects("# T\n\nHeading\n=======\n", 4, "setext");
        rejects("# T\n\nHeading\n---\n", 4, "setext");
        rejects("# T\n\nHeading\n--\n", 4, "setext");
        rejects("# T\n\nHeading\n-\n", 4, "setext");
        rejects("# T\n\nHeading\n=\n", 4, "setext");
        // A line of two hyphens that is not under a paragraph is text.
        ok("# T\n\n--\n");
    }

    #[test]
    fn rejects_blocks_that_are_not_separated() {
        rejects("# T\n\nText.\n- item\n", 4, "blank line must separate");
        rejects(
            "# T\n\nText.\n| a |\n| --- |\n",
            4,
            "blank line must separate",
        );
        rejects("# T\n\nText.\n```\nx\n```\n", 4, "blank line must separate");
        rejects("# T\n\n- item\nText.\n", 4, "blank line must separate");
        rejects("# T\n\n```\nx\n```\nText.\n", 6, "blank line must separate");
        rejects(
            "# T\n\n| a |\n| --- |\n| b |\nText.\n",
            6,
            "blank line must separate",
        );
        rejects("# T\n\n- item\n1. item\n", 4, "blank line must separate");
    }

    #[test]
    fn rejects_list_errors() {
        rejects("# T\n\n- a\n  - b\n", 4, "nested lists");
        rejects("# T\n\n- a\n   c\n", 4, "exactly 2 spaces");
        rejects("# T\n\n2. a\n", 3, "expected 1");
        rejects("# T\n\n1. a\n3. b\n", 4, "expected 2");
        rejects("# T\n\n-\n", 3, "has text");
        rejects("# T\n\n-  a\n", 3, "one space");
        rejects("# T\n\nText\n  indented\n", 4, "left margin");
    }

    #[test]
    fn rejects_code_block_errors() {
        rejects("# T\n\n```\nnever closed\n", 3, "not closed");
        rejects("# T\n\n```\n```\n", 3, "at least one line");
    }

    #[test]
    fn rejects_table_errors() {
        rejects("# T\n\n| a | b |\n", 3, "delimiter row");
        rejects("# T\n\n| a | b |\n| -- | --- |\n", 4, "delimiter cell");
        rejects(
            "# T\n\n| a | b |\n| --- | --- |\n| x |\n",
            5,
            "has 1 cells; the header has 2",
        );
        rejects(
            "# T\n\n| a | b |\n| --- | --- |\n| x | y\n",
            5,
            "begins and ends with |",
        );
        let wide = "# T\n\n|a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        rejects(wide, 3, "at most 16 columns");
    }

    #[test]
    fn rejects_inline_errors() {
        rejects(
            "# T\n\nSee [Namespaces](namespaces.md).\n",
            3,
            "links are not supported",
        );
        rejects("# T\n\n![alt](x.png)\n", 3, "images are not supported");
        rejects("# T\n\nA <placeholder> here.\n", 3, "inside a code span");
        rejects("# T\n\nA -> arrow.\n", 3, "inside a code span");
        rejects("# T\n\nAn `open span.\n", 3, "not closed");
        rejects("# T\n\nAn ```a``` span.\n", 3, "one or two backticks");
        rejects("# T\n\nAn `` `` span.\n", 3, "empty code span");
        rejects("# T\n\nA *dangling star.\n", 3, "unmatched");
        rejects("# T\n\nA dangling* star.\n", 3, "unmatched");
        rejects("# T\n\nA ***both*** run.\n", 3, "does not nest");
        rejects("# T\n\nA *nested **strong** run*.\n", 3, "does not nest");
        rejects("# T\n\nA *`code`* run.\n", 3, "cannot contain a code span");
        rejects("# T\n\nAn _underscore_ run.\n", 3, "underscore emphasis");
        rejects("# T\n\nA ~~struck~~ run.\n", 3, "strikethrough");
        rejects("# T\n\nA note[^1] here.\n", 3, "footnotes");
        rejects("# T\n\n| a[^n] |\n| --- |\n", 3, "footnotes");
        // A caret that is not a footnote reference.
        ok("# T\n\nThe [^] pair and 2^10 [x] stay.\n");
    }

    #[test]
    fn diagnostics_name_the_line_the_character_came_from() {
        rejects(
            "# T\n\nFirst line,\nsecond <line>,\nthird.\n",
            4,
            "inside a code span",
        );
        rejects(
            "# T\n\n- An item,\n  a <second> line.\n",
            4,
            "inside a code span",
        );
        rejects(
            "# T\n\nA *long\nopen star\nrun, <x>*.\n",
            5,
            "inside a code span",
        );
    }

    #[test]
    fn rejects_hard_line_breaks() {
        rejects("# T\n\nTrailing spaces  \nnext.\n", 3, "hard line breaks");
        rejects(
            "# T\n\nTrailing backslash\\\nnext.\n",
            3,
            "hard line breaks",
        );
        rejects("# T\n\n- item  \n  more\n", 3, "hard line breaks");
        // An escaped backslash at a line end is not a break, and neither are
        // trailing spaces on a block's last line.
        ok("# T\n\nA literal \\\\\nnext.\n");
        ok("# T\n\nThe last line  \n");
    }

    #[test]
    fn rejects_character_errors() {
        rejects("# T\n\nA\ttab.\n", 3, "tab outside a code block");
        rejects("# T\n\nAn \x1b]1936;v1;obj escape.\n", 3, "U+001B");
        rejects("# T\r\n\r\nText.\r\n", 1, "carriage return");
        rejects("\u{feff}# T\n", 1, "byte-order mark");
        rejects("# T\n\nA C1 \u{9b} control.\n", 3, "U+009B");
    }

    /// No block allows a bidirectional control, a code block and a code span
    /// included: reordered code is the case these controls are used to forge.
    #[test]
    fn rejects_bidirectional_controls_in_every_block() {
        for c in [
            '\u{202a}', '\u{202b}', '\u{202c}', '\u{202d}', '\u{202e}', '\u{2066}', '\u{2067}',
            '\u{2068}', '\u{2069}',
        ] {
            let m = alloc::format!("bidirectional control character U+{:04X}", c as u32);
            rejects(&alloc::format!("# T{}\n", c), 1, &m);
            rejects(&alloc::format!("# T\n\n## H{}\n", c), 3, &m);
            rejects(&alloc::format!("# T\n\nA {} B.\n", c), 3, &m);
            rejects(&alloc::format!("# T\n\nA\nB {}.\n", c), 4, &m);
            rejects(&alloc::format!("# T\n\n- A {}\n", c), 3, &m);
            rejects(&alloc::format!("# T\n\nA `x{}y` span.\n", c), 3, &m);
            rejects(
                &alloc::format!("# T\n\n```\nlet s = \"{}\";\n```\n", c),
                4,
                &m,
            );
            rejects(&alloc::format!("# T\n\n| a{} |\n| --- |\n", c), 3, &m);
            rejects(
                &alloc::format!("# T\n\n| a |\n| --- |\n| b{} |\n", c),
                5,
                &m,
            );
        }
        // The implicit direction marks reorder no letters, and are text.
        ok("# T\n\nA \u{200e}mark, \u{200f}another, and \u{61c}a third.\n");
    }

    /// Every form a code host decodes is rejected, whether or not its name is
    /// one the host knows, in every block that holds inline content.
    #[test]
    fn rejects_character_references() {
        for r in [
            "&amp;",
            "&nbsp;",
            "&copy;",
            "&unknown;",
            "&a1;",
            "&#38;",
            "&#0;",
            "&#0000038;",
            "&#x26;",
            "&#X1F;",
            "&#x202E;",
            "&#27;",
            "AT&T;",
        ] {
            rejects(
                &alloc::format!("# T\n\nAn {} here.\n", r),
                3,
                "character references",
            );
        }
        rejects("# T &amp; U\n", 1, "character references");
        rejects("# T\n\n## A &amp; B\n", 3, "character references");
        rejects("# T\n\n- A &amp; B\n", 3, "character references");
        rejects("# T\n\n| A &amp; B |\n| --- |\n", 3, "character references");
        rejects(
            "# T\n\nA *run &amp; more* here.\n",
            3,
            "character references",
        );
        rejects(
            "# T\n\nA **run &#38; more** here.\n",
            3,
            "character references",
        );
        rejects(
            "# T\n\nFirst line,\nthen &amp;.\n",
            4,
            "character references",
        );
        // Code, an escaped ampersand, and every ampersand that completes no
        // reference are literal.
        let d = ok(concat!(
            "# T\n\n",
            "R&D, a & b, &, &;, &#;, &#x;, &#xg;, &#1a, & amp;, &amp and, &\namp; ",
            "`&amp;` and \\&amp;.\n\n",
            "```\n&amp;\n```\n"
        ));
        assert_eq!(
            d.blocks[1],
            Block::Paragraph(vec![
                t("R&D, a & b, &, &;, &#;, &#x;, &#xg;, &#1a, & amp;, &amp and, & amp; "),
                Inline::Code(String::from("&amp;")),
                t(" and &amp;."),
            ])
        );
        assert_eq!(d.blocks[2], Block::Code(vec![String::from("&amp;")]));
    }

    /// A cell's width is its displayed text in characters, the unit its padding
    /// is counted in (render.rs), so the limit bounds the padding exactly.
    #[test]
    fn a_table_cell_holds_at_most_256_characters() {
        fn n(count: usize, s: &str) -> String {
            s.repeat(count)
        }
        let table = |cell: &str| alloc::format!("# T\n\n| h |\n| --- |\n| {} |\n", cell);
        ok(&table(&n(TABLE_CELL_MAX, "a")));
        rejects(
            &table(&n(TABLE_CELL_MAX + 1, "a")),
            5,
            "a table cell holds at most 256 characters",
        );
        // Characters, not bytes; escapes and code-span delimiters are not
        // displayed; emphasis markers are not either.
        ok(&table(&n(TABLE_CELL_MAX, "\u{e9}")));
        ok(&table(&n(TABLE_CELL_MAX, "\\|")));
        ok(&table(&alloc::format!("`{}`", n(TABLE_CELL_MAX, "x"))));
        ok(&table(&alloc::format!("*{}*", n(TABLE_CELL_MAX, "x"))));
        rejects(
            &table(&alloc::format!("{}\\|", n(TABLE_CELL_MAX, "\u{e9}"))),
            5,
            "at most 256 characters",
        );
        // The header row counts, and a row with several wide cells reports once.
        rejects(
            &alloc::format!("# T\n\n| {} |\n| --- |\n", n(TABLE_CELL_MAX + 1, "a")),
            3,
            "at most 256 characters",
        );
        rejects(
            &alloc::format!(
                "# T\n\n| h | h |\n| --- | --- |\n| {} | {} |\n",
                n(TABLE_CELL_MAX + 1, "a"),
                n(300, "b")
            ),
            5,
            "at most 256 characters",
        );
    }

    #[test]
    fn rejects_an_oversized_section() {
        let mut big = String::from("# T\n\n");
        while big.len() <= SECTION_MAX {
            big.push_str("A line of filler text for the size bound.\n");
        }
        rejects(&big, 1, "larger than 1 MiB");
    }

    /// Problems are reported in line order even where a block learns of them
    /// out of order: a table's header cells before its delimiter row, a
    /// paragraph's line structure between its inline content, an unclosed code
    /// block before its content, a list item's continuation lines between its
    /// inline content. The tree builder rejects any other order.
    #[test]
    fn diagnostics_are_in_line_order() {
        diagnoses(
            "# T\n\nA <b>.\n\n> q\n\n## H ##\n",
            &[
                (3, "inside a code span"),
                (5, "block quotes"),
                (7, "closing hashes"),
            ],
        );
        diagnoses(
            "# T\n\n| <a> | b |\n| -- | --- |\n",
            &[(3, "inside a code span"), (4, "delimiter cell")],
        );
        diagnoses(
            "# T\n\nOne <x>\n  two <y>\n===\nthree [a](b)\n",
            &[
                (3, "inside a code span"),
                (4, "left margin"),
                (4, "inside a code span"),
                (5, "setext"),
                (6, "links are not supported"),
            ],
        );
        diagnoses(
            "# T\n\n```\nA \x01 control.\n",
            &[(3, "not closed"), (4, "U+0001")],
        );
        diagnoses(
            "# T\n\n- item <a>  \n   odd <b>\n  - nested\n",
            &[
                (3, "inside a code span"),
                (3, "hard line breaks"),
                (4, "exactly 2 spaces"),
                (4, "inside a code span"),
                (5, "nested lists"),
            ],
        );
    }

    #[test]
    fn a_header_row_without_a_delimiter_still_has_its_inline_content_checked() {
        diagnoses(
            "# T\n\n| <a> |\n",
            &[(3, "inside a code span"), (3, "delimiter row")],
        );
    }

    #[test]
    fn check_section_rejects_a_numbered_title() {
        let ds = check_section(Some("40-dosbox.md"), "# 40 - DOSBox\n").unwrap_err();
        assert!(ds[0].message.contains("section number"), "{:?}", ds);
        assert!(check_section(Some("04-things.md"), "# 4 things\n").is_err());
        // A number that only begins a longer figure is not the section number.
        assert!(check_section(Some("40-errors.md"), "# 400 errors\n").is_ok());
        // Without a section file name the check does not apply.
        assert!(check_section(Some("draft.md"), "# 40 - DOSBox\n").is_ok());
        assert!(check_section(None, "# 40 - DOSBox\n").is_ok());
    }

    #[test]
    fn title_text_flattens_inline_forms() {
        assert_eq!(
            title_text("# The `manual` *reader*\n\nx\n").as_deref(),
            Some("The manual reader")
        );
        assert_eq!(title_text("## Not a title\n"), None);
        assert_eq!(title_text("# \n"), None);
    }

    #[test]
    fn row_cells_pair_backslashes() {
        fn split(row: &str) -> (Vec<&str>, bool) {
            let mut c = cells(row);
            let v: Vec<&str> = c.by_ref().collect();
            (v, c.closed)
        }
        assert_eq!(split("| a | b |"), (vec!["a", "b"], true));
        assert_eq!(split("| a \\| b |"), (vec!["a \\| b"], true));
        assert_eq!(split("| a \\\\| b |"), (vec!["a \\\\", "b"], true));
        assert_eq!(split("| a \\\\\\| b |"), (vec!["a \\\\\\| b"], true));
        assert_eq!(split("| a | b"), (vec!["a", "b"], false));
        assert_eq!(split("| a |\u{a0}"), (vec!["a"], false));
        assert_eq!(split("|"), (vec![], false));
        assert_eq!(split("||"), (vec![""], true));
        assert_eq!(split("| x \\"), (vec!["x \\"], false));
        let mut buf = String::new();
        assert_eq!(cell_text("a \\| b \\\\ c", &mut buf), "a | b \\\\ c");
    }
}
