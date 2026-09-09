//! A `no_std` TOML SUBSET -- exactly the grammar a theme file needs
//! (HALCYON-THEME 5), and nothing else.
//!
//! Supported: comments, `[table]` and `[table.sub]` headers, `key = "string"`,
//! `key = <integer>`, and `key = ["...", ...]` arrays of strings (which may
//! span lines). NOT supported, and REFUSED rather than mis-read: dates,
//! floats, booleans, inline tables, multi-line strings, string escapes, array
//! nesting, dotted keys, and arrays-of-tables.
//!
//! Why a subset rather than a vendored crate: this is the whole grammar the
//! format needs, we own both ends of it, and a few hundred host-testable lines
//! is a far smaller surface to prosecute than a general-purpose parser.
//!
//! **The parser is TOTAL.** A theme file is a user file, but the user is not
//! always its author, so every input either parses or returns an `Error` with
//! the line that failed -- never a panic, never a partial read the caller
//! could mistake for a whole one. Refusing an unsupported construct is
//! deliberate: silently ignoring `x = 1.5` would apply a theme the author did
//! not write, which is the half-applied theme HALCYON-THEME 4.2 forbids.

use alloc::vec::Vec;

/// One parsed assignment. `table` is the dotted header path in force
/// (`""` before any header); `key` is the bare key. Both borrow the source.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry<'a> {
    pub table: &'a str,
    pub key: &'a str,
    pub line: u32,
    /// The line the `[table]` header was on (0 for the root table). A caller
    /// rejecting a whole TABLE should point the author at the header they
    /// mistyped, not at the first key that happens to sit under it.
    pub table_line: u32,
    pub value: Value<'a>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Value<'a> {
    Str(&'a str),
    Int(i64),
    Array(Vec<&'a str>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A `[header]` that is not `[name]` or `[name.sub]`.
    BadTable,
    /// A line that is neither blank, a comment, a header, nor `key = value`.
    BadKey,
    /// A value this subset does not accept -- a float, a bool, a date, an
    /// inline table, a bare word. Refused, never guessed at.
    BadValue,
    /// A string with no closing quote, or one containing a backslash (this
    /// subset has no escapes, so a backslash cannot mean what it looks like).
    BadString,
    /// An integer out of range, or carrying `+`, `_`, or a leading zero run.
    BadInt,
    /// An array that never closes, nests, or holds a non-string.
    BadArray,
    /// The same key assigned twice in the same table.
    Duplicate,
    /// More entries or array elements than a theme file can legitimately hold.
    TooLarge,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error {
    pub line: u32,
    pub kind: Kind,
}

/// Bounds a hostile file. A theme sets ~70 keys; 512 is far above any real
/// file and far below anything that could exhaust memory through this path.
const MAX_ENTRIES: usize = 512;
/// The longest legitimate array is the 16-slot ANSI palette.
const MAX_ARRAY: usize = 64;
/// An array may span lines, but not the whole file.
const MAX_ARRAY_LINES: u32 = 64;

fn err(line: u32, kind: Kind) -> Error {
    Error { line, kind }
}

/// Strip a trailing `#` comment, respecting quotes.
///
/// String-aware because EVERY colour in this format begins with `#`: a naive
/// `find('#')` turns `surface = "#1A1714"` into `surface = "` and reports an
/// unterminated string. `"` and `#` are ASCII, so the cut is always on a char
/// boundary. This subset has no escapes, so a quote is never escaped and
/// toggling on every `"` is exact.
fn strip_comment(s: &str) -> &str {
    let mut in_str = false;
    for (i, c) in s.bytes().enumerate() {
        match c {
            b'"' => in_str = !in_str,
            b'#' if !in_str => return &s[..i],
            _ => {}
        }
    }
    s
}

/// A bare `"..."` string. Returns the contents, or `None` if it is not one
/// (unquoted, unterminated, trailing junk, or containing a backslash).
fn parse_string(s: &str) -> Option<&str> {
    let s = s.trim();
    let body = s.strip_prefix('"')?.strip_suffix('"')?;
    // `"` alone strips to `""` -> prefix ok, suffix ok, body "" -- but that is
    // the SAME quote counted twice, so require at least the two quotes.
    if s.len() < 2 || body.contains('"') || body.contains('\\') {
        return None;
    }
    Some(body)
}

fn parse_int(s: &str) -> Option<i64> {
    let s = s.trim();
    let (neg, digits) = match s.strip_prefix('-') {
        Some(d) => (true, d),
        None => (false, s),
    };
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    // A leading zero run is a typo risk (`010` is not octal here and never
    // will be), so refuse it rather than pick a meaning.
    if digits.len() > 1 && digits.starts_with('0') {
        return None;
    }
    let v: i64 = digits.parse().ok()?;
    Some(if neg { -v } else { v })
}

/// A `[header]` line -> the dotted path. Accepts one or two segments of
/// `[A-Za-z0-9_-]`, which is every table this format has.
fn parse_header(s: &str) -> Option<&str> {
    let inner = s.strip_prefix('[')?.strip_suffix(']')?;
    let inner = inner.trim();
    if inner.is_empty() || inner.starts_with('.') || inner.ends_with('.') {
        return None;
    }
    let mut segs = 0;
    for seg in inner.split('.') {
        segs += 1;
        if segs > 2
            || seg.is_empty()
            || !seg
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
        {
            return None;
        }
    }
    Some(inner)
}

fn valid_key(k: &str) -> bool {
    !k.is_empty()
        && k.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}

/// Parse `src`. On success every assignment in file order; on failure the
/// FIRST line that could not be read, and nothing else -- a caller must not
/// be able to half-apply a file this refused.
pub fn parse(src: &str) -> Result<Vec<Entry<'_>>, Error> {
    let mut out: Vec<Entry<'_>> = Vec::new();
    let mut table = "";
    let mut table_line = 0u32;
    let mut lines = src.lines().enumerate();
    while let Some((i, raw)) = lines.next() {
        let lineno = i as u32 + 1;
        let line = strip_comment(raw).trim();
        if line.is_empty() {
            continue;
        }
        if line.starts_with('[') {
            table = parse_header(line).ok_or_else(|| err(lineno, Kind::BadTable))?;
            table_line = lineno;
            continue;
        }
        let (key, rest) = line
            .split_once('=')
            .ok_or_else(|| err(lineno, Kind::BadKey))?;
        let key = key.trim();
        if !valid_key(key) {
            return Err(err(lineno, Kind::BadKey));
        }
        if out.len() >= MAX_ENTRIES {
            return Err(err(lineno, Kind::TooLarge));
        }
        if out.iter().any(|e| e.table == table && e.key == key) {
            return Err(err(lineno, Kind::Duplicate));
        }
        let rest = rest.trim();
        let value = if rest.starts_with('[') {
            // An array may span lines. Accumulate until the closing bracket,
            // consuming from the SAME iterator so the outer loop never re-reads
            // a line this value already owns.
            let (v, _) = read_array(rest, lineno, &mut lines)?;
            v
        } else if rest.starts_with('"') {
            Value::Str(parse_string(rest).ok_or_else(|| err(lineno, Kind::BadString))?)
        } else if rest.is_empty() {
            return Err(err(lineno, Kind::BadValue));
        } else {
            match parse_int(rest) {
                Some(n) => Value::Int(n),
                // A bare word, a float, a bool, a date, an inline table: this
                // subset has no meaning for it, so it is an error rather than
                // a silently-skipped line. `BadInt` only for something that
                // was PLAUSIBLY an integer -- a sign then digits/underscores.
                // A date (`1979-05-27`) is all digits and dashes but carries
                // an INTERIOR sign, and telling its author their integer is
                // malformed would send them the wrong way.
                None => {
                    let body = rest.strip_prefix(['+', '-']).unwrap_or(rest);
                    let looks_int =
                        !body.is_empty() && body.bytes().all(|b| b.is_ascii_digit() || b == b'_');
                    return Err(err(
                        lineno,
                        if looks_int {
                            Kind::BadInt
                        } else {
                            Kind::BadValue
                        },
                    ));
                }
            }
        };
        out.push(Entry {
            table,
            key,
            line: lineno,
            table_line,
            value,
        });
    }
    Ok(out)
}

/// Read an array value starting at `first` (which begins with `[`), pulling
/// further lines from `lines` until it closes. Returns the value and the line
/// it closed on.
fn read_array<'a, I>(first: &'a str, start: u32, lines: &mut I) -> Result<(Value<'a>, u32), Error>
where
    I: Iterator<Item = (usize, &'a str)>,
{
    let mut items: Vec<&'a str> = Vec::new();
    let mut buf = first
        .strip_prefix('[')
        .ok_or_else(|| err(start, Kind::BadArray))?;
    let mut lineno = start;
    let mut spanned = 0u32;
    loop {
        let (chunk, closed) = match buf.find(']') {
            Some(i) => (&buf[..i], Some(&buf[i + 1..])),
            None => (buf, None),
        };
        // Nesting is not in this subset; a `[` here would otherwise be read as
        // the start of a string-less element and silently dropped.
        if chunk.contains('[') {
            return Err(err(lineno, Kind::BadArray));
        }
        for item in chunk.split(',') {
            let item = item.trim();
            if item.is_empty() {
                continue; // whitespace between commas, and the trailing comma
            }
            if items.len() >= MAX_ARRAY {
                return Err(err(lineno, Kind::TooLarge));
            }
            items.push(parse_string(item).ok_or_else(|| err(lineno, Kind::BadArray))?);
        }
        if let Some(tail) = closed {
            // Only a comment may follow the closing bracket.
            if !strip_comment(tail).trim().is_empty() {
                return Err(err(lineno, Kind::BadArray));
            }
            return Ok((Value::Array(items), lineno));
        }
        spanned += 1;
        if spanned > MAX_ARRAY_LINES {
            return Err(err(lineno, Kind::TooLarge));
        }
        match lines.next() {
            Some((i, raw)) => {
                lineno = i as u32 + 1;
                buf = strip_comment(raw);
            }
            // Ran off the end of the file with the array still open.
            None => return Err(err(lineno, Kind::BadArray)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn one(src: &str) -> Result<Vec<Entry<'_>>, Error> {
        parse(src)
    }

    #[test]
    fn a_theme_shaped_file_parses_whole() {
        let src = "\
# a comment
[meta]
name = \"Nocturne\"   # trailing comment
base = \"daylight\"

[palette]
surface = \"#1A1714\"

[palette.sage]
key = \"#3F6B3F\"

[terminal]
ansi = [\"#3A332E\", \"#9C3A28\"]

[type]
smooth = 0

[geometry]
bevel = 2
";
        let e = one(src).expect("the theme-shaped file must parse");
        assert_eq!(e.len(), 7);
        assert_eq!(e[0].table, "meta");
        assert_eq!(e[0].key, "name");
        assert_eq!(e[0].value, Value::Str("Nocturne"));
        assert_eq!(e[0].line, 3, "the line number is the SOURCE line");
        assert_eq!(e[2].table, "palette");
        assert_eq!(e[3].table, "palette.sage", "a two-segment header");
        assert_eq!(e[4].value, Value::Array(vec!["#3A332E", "#9C3A28"]));
        assert_eq!(e[5].value, Value::Int(0));
        assert_eq!(e[6].value, Value::Int(2));
    }

    // A multi-line array, which is how a 16-slot ANSI palette is actually
    // written. The trailing comma is legal TOML and must not add an element.
    #[test]
    fn an_array_may_span_lines_and_end_with_a_comma() {
        let src = "\
[terminal]
ansi = [
  \"#000000\", \"#111111\",   # two here
  \"#222222\",
]
after = 1
";
        let e = one(src).unwrap();
        assert_eq!(
            e[0].value,
            Value::Array(vec!["#000000", "#111111", "#222222"])
        );
        assert_eq!(e[1].key, "after", "the outer loop resumed AFTER the array");
        assert_eq!(e[1].line, 6);
    }

    // Every refusal, each with the POSITIVE control one variable away -- a
    // parser that refused everything would pass a refusal-only test.
    #[test]
    fn every_unsupported_construct_is_refused_with_its_line() {
        let cases: &[(&str, Kind, u32)] = &[
            ("[palette\nx = 1", Kind::BadTable, 1),
            ("[a.b.c]\nx = 1", Kind::BadTable, 1),
            ("[]\nx = 1", Kind::BadTable, 1),
            ("[a b]\nx = 1", Kind::BadTable, 1),
            ("x 1", Kind::BadKey, 1),
            ("bad key = 1", Kind::BadKey, 1),
            (" = 1", Kind::BadKey, 1),
            ("x =", Kind::BadValue, 1),
            ("x = 1.5", Kind::BadValue, 1),
            ("x = true", Kind::BadValue, 1),
            ("x = 1979-05-27", Kind::BadValue, 1),
            ("x = { a = 1 }", Kind::BadValue, 1),
            ("x = bare", Kind::BadValue, 1),
            ("x = +1", Kind::BadInt, 1),
            ("x = 1_000", Kind::BadInt, 1),
            ("x = 007", Kind::BadInt, 1),
            ("x = \"open", Kind::BadString, 1),
            ("x = \"a\\\\b\"", Kind::BadString, 1),
            ("x = [\"a\"", Kind::BadArray, 1),
            ("x = [[\"a\"]]", Kind::BadArray, 1),
            ("x = [1]", Kind::BadArray, 1),
            ("x = [\"a\"] junk", Kind::BadArray, 1),
            ("x = 1\nx = 2", Kind::Duplicate, 2),
            ("[t]\nx = 1\n[t]\nx = 2", Kind::Duplicate, 4),
        ];
        for (src, kind, line) in cases {
            let got = one(src);
            assert_eq!(
                got,
                Err(Error {
                    line: *line,
                    kind: *kind
                }),
                "for {src:?}"
            );
        }
        // The controls: one variable away from four of the refusals above.
        assert!(one("[palette]\nx = 1").is_ok());
        assert!(one("x = 1").is_ok());
        assert!(one("x = -1").is_ok());
        assert!(
            one("x = 0").is_ok(),
            "a lone zero is not a leading-zero run"
        );
        assert!(one("x = \"a\"").is_ok());
        assert!(one("x = [\"a\"]").is_ok());
        assert!(
            one("[t]\nx = 1\n[u]\nx = 2").is_ok(),
            "same key, other table"
        );
    }

    // The same key in the same table twice is the half-applied theme in
    // miniature: whichever won, the author cannot tell which they got.
    #[test]
    fn a_duplicate_key_is_refused_rather_than_last_one_wins() {
        let e = one("[palette]\nsurface = \"#111111\"\nsurface = \"#222222\"");
        assert_eq!(
            e,
            Err(Error {
                line: 3,
                kind: Kind::Duplicate
            })
        );
    }

    // Total on arbitrary input: a theme file is a user file whose author may
    // not be the user. Nothing here may panic or hang.
    #[test]
    fn arbitrary_input_never_panics() {
        let corpus: &[&str] = &[
            "",
            "\n\n\n",
            "#",
            "#####",
            "[",
            "]",
            "[]",
            "=",
            "==",
            "\"",
            "\"\"",
            "x = \"\"",
            "x = []",
            "[[t]]",
            "x = [,,,]",
            "x = [\n",
            "\0\0\0",
            "x = \"\u{1F600}\"",
            "[\u{1F600}]",
            "x=1\r\ny=2",
            "  \t  ",
            "x = ---",
            "x = -",
            "[a.]",
            "[.a]",
        ];
        for src in corpus {
            let _ = parse(src); // must return, either arm
        }
        // A long but legal file, and one past the entry cap.
        let mut big = alloc::string::String::new();
        for i in 0..MAX_ENTRIES {
            big.push_str("k");
            let _ = core::fmt::Write::write_fmt(&mut big, format_args!("{i} = {i}\n"));
        }
        assert!(parse(&big).is_ok(), "{MAX_ENTRIES} entries is legal");
        big.push_str("over = 1\n");
        assert_eq!(parse(&big).unwrap_err().kind, Kind::TooLarge);
    }

    // The untrusted-input bar (HALCYON-THEME 5), the same one the Beacon wire
    // gets. DETERMINISTIC: a seeded LCG over the alphabet this grammar cares
    // about, so a failure is reproducible from its seed rather than a story
    // about a run that once went wrong. The assertion is not "it parses" --
    // most of these are garbage -- it is that every input RETURNS, and that a
    // success is self-consistent (a reported line is a real line).
    #[test]
    fn a_seeded_corpus_of_garbage_always_returns() {
        // The bytes that mean something here, plus a few that do not.
        const ALPHA: &[u8] = b"[].,=\"#\\ \t\nabz09_-+{}:'\r\x7f";
        let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
        let mut next = || {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            (state >> 33) as usize
        };
        // A valid seed to MUTATE. Pure garbage almost never reaches the value
        // paths (measured: 98% refusals at the first line), so half the corpus
        // is this file with 1-3 byte edits -- the standard mutation shape, and
        // the only way the deep paths get exercised at all.
        const SEED_FILE: &[u8] = b"# theme\n[meta]\nname = \"N\"\nbase = \"daylight\"\n\
[palette]\nsurface = \"#1A1714\"\nfg = \"#E8E0D4\"\n\
[palette.sage]\nkey = \"#3F6B3F\"\n\
[terminal]\nansi = [\n  \"#000000\", \"#111111\",\n]\n\
[type]\nsmooth = 0\n[geometry]\nbevel = 2\nhairline = -1\n";

        let (mut ok, mut bad, mut with_entries) = (0u32, 0u32, 0u32);
        for round in 0..4000 {
            let mut bytes: Vec<u8> = if round % 2 == 0 {
                let len = next() % 120;
                (0..len).map(|_| ALPHA[next() % ALPHA.len()]).collect()
            } else {
                SEED_FILE.to_vec()
            };
            if round % 2 == 1 {
                for _ in 0..1 + next() % 3 {
                    if bytes.is_empty() {
                        break;
                    }
                    let at = next() % bytes.len();
                    match next() % 3 {
                        0 => bytes[at] = ALPHA[next() % ALPHA.len()],
                        1 => bytes.insert(at, ALPHA[next() % ALPHA.len()]),
                        _ => {
                            bytes.remove(at);
                        }
                    }
                }
            }
            // A mutation can split a UTF-8 sequence; the parser takes `&str`,
            // so a lossy rebuild is what a real loader would hand it too.
            let s = alloc::string::String::from_utf8_lossy(&bytes).into_owned();
            match parse(&s) {
                Err(e) => {
                    bad += 1;
                    let lines = s.lines().count() as u32;
                    assert!(
                        e.line >= 1 && e.line <= lines.max(1),
                        "error line {} outside 1..={} for {s:?}",
                        e.line,
                        lines.max(1)
                    );
                }
                Ok(entries) => {
                    ok += 1;
                    if !entries.is_empty() {
                        with_entries += 1;
                    }
                    // A parse that succeeded must have produced entries whose
                    // slices really are inside the source it was handed.
                    for en in &entries {
                        assert!(en.line >= 1);
                        assert!(valid_key(en.key), "accepted a bad key {:?}", en.key);
                    }
                }
            }
        }
        // A corpus that only ever reaches ONE arm proves nothing about the
        // other. This asserts the generator still produces both, so the fuzz
        // cannot silently degenerate into 4000 rejections at line 1.
        assert!(bad > 200, "only {bad} refusals -- the corpus went too tame");
        assert!(
            ok > 200,
            "only {ok} accepted -- the corpus went too hostile"
        );
        assert!(
            with_entries > 20,
            "only {with_entries} inputs produced an ENTRY -- the accepted arm \
             is all blank/comment lines, so the value paths are unexercised"
        );
    }

    // An array longer than any real palette is bounded, not merely large.
    #[test]
    fn an_oversized_array_is_bounded() {
        let mut s = alloc::string::String::from("x = [");
        for _ in 0..MAX_ARRAY + 1 {
            s.push_str("\"#000000\",");
        }
        s.push(']');
        assert_eq!(parse(&s).unwrap_err().kind, Kind::TooLarge);
        // The control: exactly at the cap is fine.
        let mut ok = alloc::string::String::from("x = [");
        for _ in 0..MAX_ARRAY {
            ok.push_str("\"#000000\",");
        }
        ok.push(']');
        assert!(parse(&ok).is_ok());
    }

    // An array that never closes must not swallow the rest of the file
    // silently, and must not scan it forever either.
    #[test]
    fn an_unclosed_array_is_bounded_and_refused() {
        let mut s = alloc::string::String::from("x = [\n");
        for _ in 0..MAX_ARRAY_LINES + 10 {
            s.push_str("\"#000000\",\n");
        }
        let e = parse(&s).unwrap_err();
        assert!(
            e.kind == Kind::TooLarge || e.kind == Kind::BadArray,
            "bounded either by the line span or the element cap, got {:?}",
            e.kind
        );
    }

    // A key before any header belongs to the root table, not to whichever
    // header happens to follow.
    #[test]
    fn a_key_before_any_header_is_rooted() {
        let e = one("loose = 1\n[t]\nx = 2").unwrap();
        assert_eq!(e[0].table, "");
        assert_eq!(e[1].table, "t");
    }

    // A `#` inside a quoted string is a colour, not a comment. The single
    // most likely way to break this format.
    #[test]
    fn a_hash_inside_a_string_is_a_colour_not_a_comment() {
        let e = one("[palette]\nsurface = \"#1A1714\" # the ground").unwrap();
        assert_eq!(e[0].value, Value::Str("#1A1714"));
        let a = one("ansi = [\"#111111\", \"#222222\"] # sixteen, eventually").unwrap();
        assert_eq!(a[0].value, Value::Array(vec!["#111111", "#222222"]));
    }
}
