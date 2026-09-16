//! The section format (MANUAL-DESIGN.md section 3): a strict Markdown subset in
//! which every accepted form has both a Beacon and a plain realization. The
//! parser is also the checker: it reports every rejected construct with its
//! line, and yields a document only when it reports nothing.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;

use crate::{is_control, SECTION_MAX};

/// The most columns a table may have (3.2).
pub const TABLE_COLUMNS_MAX: usize = 16;

/// An inline run (3.3). Emphasis never nests, so runs are flat.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Inline {
    Text(String),
    Code(String),
    Emph(String),
    Strong(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Align {
    Left,
    Right,
    Center,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Block {
    Title(Vec<Inline>),
    /// Level 2 or 3.
    Heading(u8, Vec<Inline>),
    Paragraph(Vec<Inline>),
    Bullets(Vec<Vec<Inline>>),
    Numbered(Vec<Vec<Inline>>),
    /// Content lines, verbatim.
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

/// A rejected construct: its 1-based line and what is wrong with it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub line: usize,
    pub message: String,
}

/// Parse a section. `Err` carries every diagnostic, in line order.
pub fn parse(src: &str) -> Result<Document, Vec<Diagnostic>> {
    let mut p = Parser::new(src);
    p.run();
    if p.diags.is_empty() {
        return Ok(Document { blocks: p.blocks });
    }
    // One report per problem per line: `<b>` is one mistake, not two.
    let mut unique: Vec<Diagnostic> = Vec::with_capacity(p.diags.len());
    for d in p.diags {
        if !unique.contains(&d) {
            unique.push(d);
        }
    }
    unique.sort_by_key(|d| d.line);
    Err(unique)
}

/// `parse`, plus the check that needs the file's name: a section's title does
/// not begin with its number (3.2). `file_name` is the last path component; a
/// name other than `NN-<name>.md` skips that check.
pub fn check_section(file_name: Option<&str>, src: &str) -> Result<Document, Vec<Diagnostic>> {
    let result = parse(src);
    let number = file_name
        .and_then(crate::catalog::parse_file_name)
        .map(|(n, _)| n);
    let repeats = match (number, title_text(src)) {
        (Some(n), Some(title)) => repeats_number(&title, n),
        _ => false,
    };
    if !repeats {
        return result;
    }
    let d = Diagnostic {
        line: 1,
        message: String::from(
            "the title begins with the section number; the number only orders the book",
        ),
    };
    match result {
        Ok(_) => Err(vec![d]),
        Err(mut ds) => {
            ds.insert(0, d);
            Err(ds)
        }
    }
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
    let mut scratch = Vec::new();
    Some(plain_text(&inline(&[(text, 1)], &mut scratch)))
}

/// The text of a run sequence with its inline structure removed.
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

fn repeats_number(title: &str, n: u8) -> bool {
    let t = title.trim_start();
    [format!("{:02}", n), format!("{}", n)]
        .iter()
        .any(|p| match t.strip_prefix(p.as_str()) {
            Some(rest) => !rest.starts_with(|c: char| c.is_ascii_digit()),
            None => false,
        })
}

fn push_diag(diags: &mut Vec<Diagnostic>, line: usize, message: &str) {
    diags.push(Diagnostic {
        line,
        message: String::from(message),
    });
}

fn is_blank(line: &str) -> bool {
    line.trim().is_empty()
}

// ---------------------------------------------------------------------------
// Blocks (3.2)
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
        if info.starts_with('`') || info.contains(char::is_whitespace) {
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
                return Kind::LinkDef;
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

fn rejected_message(kind: Kind<'_>) -> &'static str {
    match kind {
        Kind::BadFence => "a code block fence is a line of exactly three backticks, optionally followed by one word",
        Kind::BadBullet => "a bulleted item begins with '- '",
        Kind::BadNumbered => "a numbered item begins with 'N. '",
        Kind::Quote => "block quotes are not supported",
        Kind::Break(_) => "thematic breaks are not supported; structure a section with headings",
        Kind::Setext => "setext headings are not supported; write '## Heading'",
        Kind::Html => "raw HTML is not supported",
        Kind::LinkDef => "link reference definitions are not supported",
        Kind::Indented => {
            "unexpected indentation; a block starts at the left margin (for code, use a ``` fence)"
        }
        _ => "unsupported construct",
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

struct Parser<'a> {
    lines: Vec<&'a str>,
    blocks: Vec<Block>,
    diags: Vec<Diagnostic>,
    too_large: bool,
    /// A CRLF file is reported once, not once per line.
    cr_reported: bool,
    title_seen: bool,
}

impl<'a> Parser<'a> {
    fn new(src: &'a str) -> Parser<'a> {
        let mut p = Parser {
            lines: Vec::new(),
            blocks: Vec::new(),
            diags: Vec::new(),
            too_large: false,
            cr_reported: false,
            title_seen: false,
        };
        if src.len() > SECTION_MAX {
            p.too_large = true;
            p.diag(1, "the section is larger than 1 MiB");
            return p;
        }
        let body = match src.strip_prefix('\u{feff}') {
            Some(rest) => {
                p.diag(1, "the file begins with a byte-order mark");
                rest
            }
            None => src,
        };
        let mut lines: Vec<&'a str> = body.split('\n').collect();
        if body.ends_with('\n') {
            lines.pop();
        }
        p.lines = lines;
        p
    }

    fn diag(&mut self, line: usize, message: &str) {
        push_diag(&mut self.diags, line, message);
    }

    fn run(&mut self) {
        if self.too_large {
            return;
        }
        let n = self.lines.len();
        if n == 0 || !matches!(classify(self.lines[0]), Kind::Heading(1, _)) {
            self.diag(
                1,
                "a section begins with its title on line 1, written '# Title'",
            );
        }
        let mut i = 0;
        while i < n {
            let line = self.lines[i];
            if is_blank(line) {
                self.check_chars(i + 1, line, false);
                i += 1;
                continue;
            }
            i = match classify(line) {
                Kind::Heading(level, text) => self.heading(i, level, text),
                Kind::Fence => self.fence(i),
                Kind::BadFence => self.bad_fence(i),
                Kind::Bullet(_) | Kind::Numbered(..) => self.list(i),
                Kind::TableRow => self.table(i),
                Kind::Text => self.paragraph(i),
                other => {
                    self.check_chars(i + 1, line, false);
                    self.diag(i + 1, rejected_message(other));
                    i + 1
                }
            };
        }
    }

    /// A carriage return (reported once per file) and the first other control
    /// character on the line (TAB is allowed only inside a code block).
    fn check_chars(&mut self, ln: usize, line: &str, in_code: bool) {
        if line.contains('\r') && !self.cr_reported {
            self.cr_reported = true;
            self.diag(ln, "a carriage return; a section uses LF line endings");
        }
        for c in line.chars() {
            if c == '\r' || (c == '\t' && in_code) {
                continue;
            }
            if c == '\t' {
                self.diag(ln, "a tab outside a code block");
                return;
            }
            if is_control(c) {
                let m = format!("the control character U+{:04X}", c as u32);
                self.diag(ln, &m);
                return;
            }
        }
    }

    fn expect_blank_at(&mut self, j: usize, what: &str) {
        if j < self.lines.len() && !is_blank(self.lines[j]) {
            let m = format!(
                "a blank line must separate this line from the {} above",
                what
            );
            self.diag(j + 1, &m);
        }
    }

    fn heading(&mut self, i: usize, level: usize, text: &'a str) -> usize {
        let ln = i + 1;
        self.check_chars(ln, self.lines[i], false);
        let body = text.trim();
        if body.is_empty() {
            self.diag(ln, "a heading has text after its marker");
        }
        let unhashed = body.trim_end_matches('#');
        if unhashed.len() < body.len() && (unhashed.is_empty() || unhashed.ends_with(' ')) {
            self.diag(
                ln,
                "closing hashes are not supported; end the heading at its text",
            );
        }
        if text.starts_with(' ') {
            self.diag(ln, "one space separates a heading's marker from its text");
        }
        let runs = inline(&[(body, ln)], &mut self.diags);
        match level {
            // A title below line 1 has already been reported by `run`.
            1 if !self.title_seen && self.blocks.is_empty() => {
                self.title_seen = true;
                self.blocks.push(Block::Title(runs));
            }
            1 => self.diag(ln, "only the title, on line 1, is a level-1 heading"),
            2 | 3 => self.blocks.push(Block::Heading(level as u8, runs)),
            _ => self.diag(ln, "headings deeper than ### are not supported"),
        }
        self.expect_blank_at(i + 1, "heading");
        i + 1
    }

    fn fence(&mut self, i: usize) -> usize {
        let ln = i + 1;
        self.check_chars(ln, self.lines[i], false);
        let n = self.lines.len();
        let mut content = Vec::new();
        let mut j = i + 1;
        while j < n && self.lines[j] != "```" {
            let l = self.lines[j];
            self.check_chars(j + 1, l, true);
            content.push(String::from(l));
            j += 1;
        }
        if j == n {
            self.diag(
                ln,
                "the code block opened here is not closed by a line of three backticks",
            );
            return n;
        }
        if content.is_empty() {
            self.diag(ln, "a code block has at least one line of content");
        }
        self.blocks.push(Block::Code(content));
        self.expect_blank_at(j + 1, "code block");
        j + 1
    }

    /// A fence the format rejects (`~~~`, four or more backticks, or a spaced
    /// info string). Its body is skipped to the matching fence, so the
    /// content is not reported as Markdown it was never meant to be.
    fn bad_fence(&mut self, i: usize) -> usize {
        let line = self.lines[i];
        self.check_chars(i + 1, line, false);
        self.diag(i + 1, rejected_message(Kind::BadFence));
        let closer: String = if line.starts_with("~~~") {
            String::from("~~~")
        } else {
            "`".repeat(line.chars().take_while(|&c| c == '`').count())
        };
        let n = self.lines.len();
        let mut j = i + 1;
        while j < n {
            let l = self.lines[j];
            if l.trim_end() == closer {
                return j + 1;
            }
            self.check_chars(j + 1, l, true);
            j += 1;
        }
        n
    }

    fn list(&mut self, i: usize) -> usize {
        let n = self.lines.len();
        let bullets = matches!(classify(self.lines[i]), Kind::Bullet(_));
        let mut items: Vec<Vec<Inline>> = Vec::new();
        let mut j = i;
        loop {
            let ln = j + 1;
            let line = self.lines[j];
            self.check_chars(ln, line, false);
            let (text, col) = match classify(line) {
                Kind::Bullet(t) => (t, 2),
                Kind::Numbered(num, width, t) => {
                    let expected = items.len() as u64 + 1;
                    if num != expected {
                        let m = format!(
                            "numbered items run from 1 without gaps; expected {}",
                            expected
                        );
                        self.diag(ln, &m);
                    }
                    (t, width)
                }
                // The caller and the continuation test below admit only items.
                _ => break,
            };
            if text.trim().is_empty() {
                self.diag(ln, "a list item has text after its marker");
            }
            if text.starts_with(' ') {
                self.diag(ln, "one space separates a list marker from its text");
            }
            let mut segs: Vec<(&'a str, usize)> = vec![(text.trim_start(), ln)];
            let mut k = j + 1;
            while k < n {
                let l = self.lines[k];
                if is_blank(l) {
                    break;
                }
                let indent = l.len() - l.trim_start_matches(' ').len();
                if indent == 0 {
                    break;
                }
                self.check_chars(k + 1, l, false);
                let inner = &l[indent..];
                if indent != col {
                    let m = format!("a continuation line is indented by exactly {} spaces", col);
                    self.diag(k + 1, &m);
                } else if is_list_marker(inner) {
                    self.diag(k + 1, "nested lists are not supported");
                }
                segs.push((inner, k + 1));
                k += 1;
            }
            items.push(inline(&segs, &mut self.diags));
            j = k;
            if j >= n || is_blank(self.lines[j]) {
                break;
            }
            let same = match classify(self.lines[j]) {
                Kind::Bullet(_) => bullets,
                Kind::Numbered(..) => !bullets,
                _ => false,
            };
            if !same {
                self.diag(
                    j + 1,
                    "a blank line must separate this line from the list above",
                );
                break;
            }
        }
        self.blocks.push(if bullets {
            Block::Bullets(items)
        } else {
            Block::Numbered(items)
        });
        j
    }

    fn table(&mut self, i: usize) -> usize {
        let n = self.lines.len();
        let ln = i + 1;
        self.check_chars(ln, self.lines[i], false);
        let header_cells = self.row_cells(i);
        let ncols = header_cells.len();
        if ncols > TABLE_COLUMNS_MAX {
            let m = format!("a table has at most {} columns", TABLE_COLUMNS_MAX);
            self.diag(ln, &m);
        }
        if i + 1 >= n || classify(self.lines[i + 1]) != Kind::TableRow {
            self.diag(
                ln,
                "a table's header row is followed by a delimiter row, such as | --- | --- |",
            );
            self.expect_blank_at(i + 1, "table");
            return i + 1;
        }
        self.check_chars(i + 2, self.lines[i + 1], false);
        let delim = self.row_cells(i + 1);
        let mut align = Vec::new();
        for c in &delim {
            match delimiter_align(c) {
                Some(a) => align.push(a),
                None => {
                    self.diag(i + 2, "a delimiter cell is ---, :---, ---:, or :---:");
                    align.push(Align::Left);
                }
            }
        }
        if delim.len() != ncols {
            let m = format!(
                "this row has {} cells; the header has {}",
                delim.len(),
                ncols
            );
            self.diag(i + 2, &m);
        }
        align.resize(ncols, Align::Left);
        let header: Vec<Vec<Inline>> = header_cells
            .iter()
            .map(|c| inline(&[(c.as_str(), ln)], &mut self.diags))
            .collect();
        let mut rows = Vec::new();
        let mut j = i + 2;
        while j < n && classify(self.lines[j]) == Kind::TableRow {
            self.check_chars(j + 1, self.lines[j], false);
            let cells = self.row_cells(j);
            if cells.len() != ncols {
                let m = format!(
                    "this row has {} cells; the header has {}",
                    cells.len(),
                    ncols
                );
                self.diag(j + 1, &m);
            }
            let row: Vec<Vec<Inline>> = cells
                .iter()
                .map(|c| inline(&[(c.as_str(), j + 1)], &mut self.diags))
                .collect();
            rows.push(row);
            j += 1;
        }
        self.blocks.push(Block::Table {
            align,
            header,
            rows,
        });
        self.expect_blank_at(j, "table");
        j
    }

    /// Split a table row into trimmed cells. `\|` is a literal pipe; any
    /// other backslash pair is kept for the inline parser.
    fn row_cells(&mut self, idx: usize) -> Vec<String> {
        let line = self.lines[idx].trim_end_matches(' ');
        let chars: Vec<char> = line.chars().collect();
        let mut cells = Vec::new();
        let mut cur = String::new();
        let mut closed = false;
        let mut k = 1;
        while k < chars.len() {
            let c = chars[k];
            if c == '\\' && k + 1 < chars.len() {
                if chars[k + 1] == '|' {
                    cur.push('|');
                } else {
                    cur.push('\\');
                    cur.push(chars[k + 1]);
                }
                k += 2;
                closed = false;
                continue;
            }
            if c == '|' {
                cells.push(core::mem::take(&mut cur).trim().to_string());
                closed = true;
            } else {
                cur.push(c);
                closed = false;
            }
            k += 1;
        }
        if !closed {
            self.diag(idx + 1, "a table row begins and ends with |");
            if !cur.trim().is_empty() {
                cells.push(cur.trim().to_string());
            }
        }
        cells
    }

    fn paragraph(&mut self, i: usize) -> usize {
        let n = self.lines.len();
        let mut segs: Vec<(&'a str, usize)> = Vec::new();
        let mut j = i;
        while j < n {
            let l = self.lines[j];
            if is_blank(l) {
                break;
            }
            if j > i {
                match classify(l) {
                    Kind::Text => {}
                    Kind::Indented => {
                        self.diag(j + 1, "a paragraph's lines start at the left margin");
                    }
                    Kind::Setext | Kind::Break('-') => {
                        self.check_chars(j + 1, l, false);
                        self.diag(
                            j + 1,
                            "setext headings are not supported; write '## Heading'",
                        );
                        j += 1;
                        continue;
                    }
                    _ => {
                        self.diag(
                            j + 1,
                            "a blank line must separate this line from the paragraph above",
                        );
                        break;
                    }
                }
            }
            self.check_chars(j + 1, l, false);
            segs.push((l.trim_start(), j + 1));
            j += 1;
        }
        let runs = inline(&segs, &mut self.diags);
        self.blocks.push(Block::Paragraph(runs));
        j
    }
}

// ---------------------------------------------------------------------------
// Inline forms (3.3)
// ---------------------------------------------------------------------------

/// Parse the inline content of one block, given its source lines. Lines are
/// joined with a single space; each diagnostic names the line its character
/// came from.
fn inline(segs: &[(&str, usize)], diags: &mut Vec<Diagnostic>) -> Vec<Inline> {
    let mut chars: Vec<char> = Vec::new();
    let mut starts: Vec<(usize, usize)> = Vec::new();
    for (idx, (seg, ln)) in segs.iter().enumerate() {
        let trimmed = seg.trim_end_matches(' ');
        if idx + 1 < segs.len() {
            let spaces = seg.len() - trimmed.len();
            let backslashes = trimmed.chars().rev().take_while(|&c| c == '\\').count();
            if spaces >= 2 || backslashes % 2 == 1 {
                push_diag(
                    diags,
                    *ln,
                    "hard line breaks are not supported; end the line without two trailing spaces or a backslash",
                );
            }
        }
        if idx > 0 {
            chars.push(' ');
        }
        starts.push((chars.len(), *ln));
        chars.extend(trimmed.chars());
    }
    scan(&chars, 0, &starts, diags, false)
}

fn line_at(starts: &[(usize, usize)], idx: usize) -> usize {
    let mut line = starts.first().map_or(1, |s| s.1);
    for &(at, ln) in starts {
        if at > idx {
            break;
        }
        line = ln;
    }
    line
}

fn run_len(chars: &[char], k: usize, ch: char) -> usize {
    chars[k..].iter().take_while(|&&c| c == ch).count()
}

fn flush(out: &mut Vec<Inline>, text: &mut String) {
    if !text.is_empty() {
        out.push(Inline::Text(core::mem::take(text)));
    }
}

/// The inline scanner. `base` is the offset of `chars[0]` in the block's
/// joined text (for line numbers). Inside an emphasis body (`in_emphasis`), a
/// code span or another asterisk is nesting, which 3.3 rejects.
fn scan(
    chars: &[char],
    base: usize,
    starts: &[(usize, usize)],
    diags: &mut Vec<Diagnostic>,
    in_emphasis: bool,
) -> Vec<Inline> {
    const ANGLE: &str =
        "write '<' and '>' inside a code span (a placeholder is written `<name>`) or escape them";
    let n = chars.len();
    let mut out: Vec<Inline> = Vec::new();
    let mut text = String::new();
    let mut k = 0;
    while k < n {
        let c = chars[k];
        let ln = line_at(starts, base + k);
        match c {
            '\\' => {
                if k + 1 < n && chars[k + 1].is_ascii_punctuation() {
                    text.push(chars[k + 1]);
                    k += 2;
                } else {
                    text.push('\\');
                    k += 1;
                }
            }
            '`' if in_emphasis => {
                push_diag(diags, ln, "emphasis cannot contain a code span");
                text.push('`');
                k += 1;
            }
            '`' => {
                let run = run_len(chars, k, '`');
                if run > 2 {
                    push_diag(
                        diags,
                        ln,
                        "a code span is delimited by one or two backticks",
                    );
                    k += run;
                    continue;
                }
                match find_run(chars, k + run, '`', run) {
                    None => {
                        push_diag(diags, ln, "this code span is not closed");
                        k += run;
                    }
                    Some(close) => {
                        let mut body: String = chars[k + run..close].iter().collect();
                        if body.len() >= 2
                            && body.starts_with(' ')
                            && body.ends_with(' ')
                            && !body.trim().is_empty()
                        {
                            body = String::from(&body[1..body.len() - 1]);
                        }
                        if body.trim().is_empty() {
                            push_diag(diags, ln, "an empty code span");
                        }
                        flush(&mut out, &mut text);
                        out.push(Inline::Code(body));
                        k = close + run;
                    }
                }
            }
            '*' if in_emphasis => {
                push_diag(diags, ln, "emphasis does not nest");
                text.push('*');
                k += 1;
            }
            '*' => {
                k = emphasis(chars, base, k, starts, &mut out, &mut text, diags);
            }
            '<' | '>' => {
                push_diag(diags, ln, ANGLE);
                text.push(c);
                k += 1;
            }
            '[' => {
                if let Some(p) = chars[k + 1..].iter().position(|&x| x == ']') {
                    let close = k + 1 + p;
                    if close + 1 < n && chars[close + 1] == '(' {
                        let image = k > 0 && chars[k - 1] == '!';
                        let m = if image {
                            "images are not supported"
                        } else {
                            "links are not supported; name the section or resource in prose"
                        };
                        push_diag(diags, ln, m);
                    }
                }
                text.push('[');
                k += 1;
            }
            '~' if k + 1 < n && chars[k + 1] == '~' => {
                push_diag(diags, ln, "strikethrough is not supported");
                text.push_str("~~");
                k += 2;
            }
            '_' => {
                if underscore_emphasis(chars, k) {
                    push_diag(
                        diags,
                        ln,
                        "underscore emphasis is not supported; write *emphasis*",
                    );
                }
                text.push('_');
                k += 1;
            }
            _ => {
                text.push(c);
                k += 1;
            }
        }
    }
    flush(&mut out, &mut text);
    out
}

/// The first run of exactly `len` copies of `ch` at or after `from`.
fn find_run(chars: &[char], from: usize, ch: char, len: usize) -> Option<usize> {
    let mut m = from;
    while m < chars.len() {
        if chars[m] == ch {
            let r = run_len(chars, m, ch);
            if r == len {
                return Some(m);
            }
            m += r;
        } else {
            m += 1;
        }
    }
    None
}

/// Handle the asterisk run at `k`; returns the index after what it consumed.
fn emphasis(
    chars: &[char],
    base: usize,
    k: usize,
    starts: &[(usize, usize)],
    out: &mut Vec<Inline>,
    text: &mut String,
    diags: &mut Vec<Diagnostic>,
) -> usize {
    const UNMATCHED: &str = "an unmatched '*'; write a literal asterisk as \\*";
    let n = chars.len();
    let ln = line_at(starts, base + k);
    let run = run_len(chars, k, '*');
    let space_before = k == 0 || chars[k - 1] == ' ';
    let space_after = k + run >= n || chars[k + run] == ' ';
    let literal = |text: &mut String| {
        for _ in 0..run {
            text.push('*');
        }
    };
    if space_before && space_after {
        literal(text);
        return k + run;
    }
    if run > 2 {
        push_diag(diags, ln, "emphasis does not nest");
        literal(text);
        return k + run;
    }
    if space_after {
        push_diag(diags, ln, UNMATCHED);
        literal(text);
        return k + run;
    }
    let mut m = k + run;
    let mut close = None;
    while m < n {
        match chars[m] {
            '\\' => m += 2,
            '*' => {
                let r = run_len(chars, m, '*');
                if r == run && chars[m - 1] != ' ' {
                    close = Some(m);
                    break;
                }
                m += r;
            }
            _ => m += 1,
        }
    }
    let Some(close) = close else {
        push_diag(diags, ln, UNMATCHED);
        literal(text);
        return k + run;
    };
    let body_runs = scan(&chars[k + run..close], base + k + run, starts, diags, true);
    flush(out, text);
    let body = plain_text(&body_runs);
    out.push(if run == 2 {
        Inline::Strong(body)
    } else {
        Inline::Emph(body)
    });
    close + run
}

/// An underscore that opens what Markdown elsewhere would render as emphasis:
/// at a word start, followed by a non-space, with a closing underscore at a
/// word end.
fn underscore_emphasis(chars: &[char], k: usize) -> bool {
    let opens = (k == 0 || !chars[k - 1].is_alphanumeric())
        && k + 1 < chars.len()
        && !chars[k + 1].is_whitespace()
        && chars[k + 1] != '_';
    if !opens {
        return false;
    }
    (k + 2..chars.len()).any(|m| {
        chars[m] == '_'
            && !chars[m - 1].is_whitespace()
            && (m + 1 == chars.len() || !chars[m + 1].is_alphanumeric())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

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
        rejects("# T\n\n    indented\n", 3, "unexpected indentation");
        rejects("# T\n\n~~~\nx\n~~~\n", 3, "three backticks");
        rejects("# T\n\n````\nx\n````\n", 3, "three backticks");
        rejects("# T\n\n``` sh\nx\n```\n", 3, "three backticks");
    }

    #[test]
    fn rejects_setext_headings() {
        rejects("# T\n\nHeading\n=======\n", 4, "setext");
        rejects("# T\n\nHeading\n---\n", 4, "setext");
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
    }

    #[test]
    fn diagnostics_name_the_line_the_character_came_from() {
        rejects(
            "# T\n\nFirst line,\nsecond <line>,\nthird.\n",
            4,
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
        // An escaped backslash at a line end is not a break.
        ok("# T\n\nA literal \\\\\nnext.\n");
    }

    #[test]
    fn rejects_character_errors() {
        rejects("# T\n\nA\ttab.\n", 3, "tab outside a code block");
        rejects("# T\n\nAn \x1b]1936;v1;obj escape.\n", 3, "U+001B");
        rejects("# T\r\n\r\nText.\r\n", 1, "carriage return");
        rejects("\u{feff}# T\n", 1, "byte-order mark");
        rejects("# T\n\nA C1 \u{9b} control.\n", 3, "U+009B");
    }

    #[test]
    fn rejects_an_oversized_section() {
        let mut big = String::from("# T\n\n");
        while big.len() <= SECTION_MAX {
            big.push_str("A line of filler text for the size bound.\n");
        }
        rejects(&big, 1, "larger than 1 MiB");
    }

    #[test]
    fn diagnostics_are_in_line_order() {
        let ds = parse("# T\n\nA <b>.\n\n> q\n\n## H ##\n").unwrap_err();
        let lines: Vec<usize> = ds.iter().map(|d| d.line).collect();
        let mut sorted = lines.clone();
        sorted.sort();
        assert_eq!(lines, sorted);
        assert_eq!(lines.len(), 3, "{:?}", ds);
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
}
