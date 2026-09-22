//! The deck manifest: `slides.toml` in the deck directory.
//!
//! ```toml
//! title = "AI and the shape of the work"
//! slides = [
//!     "01-title.md",
//!     "02-why.md",
//! ]
//! ```
//!
//! Parsed with `libhalcyon::toml`, the tree's existing no_std TOML subset --
//! the theme loader's parser, reused rather than a second one written here.
//! Its own bounds (512 entries, 64 array elements, 64 array lines) are the
//! first gate a hostile file meets.
//!
//! Strict on purpose, in the manual format's spirit: the parser IS the
//! checker. An unknown key, a table header, a duplicate, a slide name that
//! could be a path or an option -- each is refused with the line to look at,
//! never guessed at or ignored. A deck the operator will stand in front of
//! should fail at `lantern --check`, not mid-talk.

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt;

use libhalcyon::toml;

/// The manifest's file name inside a deck directory.
pub const MANIFEST: &str = "slides.toml";

/// The most slides a deck may name.
///
/// Equal to `libhalcyon::toml`'s own array cap, so in practice the TOML
/// subset refuses a longer list before this is consulted. Stated here anyway:
/// lantern's bound must not be an unstated inheritance from another crate's
/// constant, which is free to move for a reason that has nothing to do with
/// decks.
pub const SLIDES_MAX: usize = 64;

/// The largest manifest lantern reads, in bytes. A manifest is a title and up
/// to `SLIDES_MAX` file names; 64 KiB is far above any real one.
pub const MANIFEST_MAX: usize = 64 * 1024;

/// A parsed manifest.
pub struct Deck {
    /// The deck's title, when the manifest gives one.
    pub title: Option<String>,
    /// The slide file names, in presentation order. Never empty.
    pub slides: Vec<String>,
}

/// What a manifest can be wrong about.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Problem {
    /// The TOML subset refused the file.
    Toml(toml::Kind),
    /// A `[header]`: a manifest is a flat set of keys.
    TableHeader,
    /// A key this manifest has no meaning for.
    UnknownKey,
    /// No `slides` key at all.
    NoSlides,
    /// `slides` is not an array of strings.
    SlidesNotArray,
    /// `slides = []`.
    NoSlidesListed,
    /// More than `SLIDES_MAX`.
    TooManySlides(usize),
    /// `title` is not a string.
    TitleNotString,
    /// An empty slide name.
    SlideEmpty,
    /// A slide name carrying a path separator: a slide lives IN the deck
    /// directory, so a name is a file name and never a path.
    SlidePath,
    /// A slide name starting with `.` -- `.`, `..`, and hidden files alike.
    SlideDotted,
    /// A slide name starting with `-`, which a command line would read as an
    /// option.
    SlideOption,
    /// A slide name that is not a `.md` file.
    SlideNotMarkdown,
    /// The same slide named twice. Showing one file twice in a deck is far
    /// more often a copy-paste slip than an intention.
    SlideDuplicate,
}

impl fmt::Display for Problem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use Problem::*;
        let m = match *self {
            Toml(k) => {
                return write!(f, "{}", toml_kind(k));
            }
            TableHeader => "a manifest is a flat set of keys; it has no [tables]",
            UnknownKey => "a manifest sets only 'slides' and, optionally, 'title'",
            NoSlides => "a manifest lists its slides, in order, as slides = [\"first.md\", ...]",
            SlidesNotArray => "'slides' is an array of file names in quotes",
            NoSlidesListed => "'slides' names at least one slide",
            TooManySlides(n) => {
                return write!(
                    f,
                    "a deck holds at most {} slides; this names {}",
                    SLIDES_MAX, n
                )
            }
            TitleNotString => "'title' is a string in quotes",
            SlideEmpty => "an empty slide name",
            SlidePath => "a slide is a file IN the deck directory; its name carries no '/'",
            SlideDotted => "a slide name does not begin with '.'",
            SlideOption => "a slide name does not begin with '-'",
            SlideNotMarkdown => "a slide is a Markdown file, named '<something>.md'",
            SlideDuplicate => "this slide is named twice",
        };
        f.write_str(m)
    }
}

/// The TOML subset's own refusals, in this manifest's vocabulary.
fn toml_kind(k: toml::Kind) -> &'static str {
    use toml::Kind::*;
    match k {
        BadTable => "a [header] this manifest does not accept",
        BadKey => "neither a comment nor 'key = value'",
        BadValue => "a value this manifest does not accept; a name goes in quotes",
        BadString => "a string with no closing quote, or carrying a backslash",
        BadInt => "a number this manifest does not accept",
        BadArray => "an array that does not close, or does not hold quoted names",
        Duplicate => "this key is set twice",
        TooLarge => "the manifest is larger than this format holds",
    }
}

/// A problem and the 1-based line to look at.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub line: u32,
    pub problem: Problem,
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.line, self.problem)
    }
}

fn bad(line: u32, problem: Problem) -> Diagnostic {
    Diagnostic { line, problem }
}

/// Whether `name` is usable as a slide file name, or why not.
fn name_problem(name: &str) -> Option<Problem> {
    if name.is_empty() {
        return Some(Problem::SlideEmpty);
    }
    if name.contains('/') || name.contains('\\') {
        return Some(Problem::SlidePath);
    }
    if name.starts_with('.') {
        return Some(Problem::SlideDotted);
    }
    if name.starts_with('-') {
        return Some(Problem::SlideOption);
    }
    // ".md" alone is a dotted name, already refused above.
    if !name.ends_with(".md") {
        return Some(Problem::SlideNotMarkdown);
    }
    None
}

/// Parse a manifest. Every problem is reported against the line it is on; the
/// first one stops the parse, because a manifest is short and a reader fixes
/// them one at a time.
pub fn parse(src: &str) -> Result<Deck, Diagnostic> {
    if src.len() > MANIFEST_MAX {
        return Err(bad(1, Problem::Toml(toml::Kind::TooLarge)));
    }
    let entries = toml::parse(src).map_err(|e| bad(e.line, Problem::Toml(e.kind)))?;

    let mut title: Option<String> = None;
    let mut slides: Option<(u32, Vec<String>)> = None;

    for e in &entries {
        if !e.table.is_empty() {
            return Err(bad(e.table_line, Problem::TableHeader));
        }
        match e.key {
            "title" => match e.value {
                toml::Value::Str(s) => title = Some(String::from(s)),
                _ => return Err(bad(e.line, Problem::TitleNotString)),
            },
            "slides" => match &e.value {
                toml::Value::Array(names) => {
                    if names.is_empty() {
                        return Err(bad(e.line, Problem::NoSlidesListed));
                    }
                    if names.len() > SLIDES_MAX {
                        return Err(bad(e.line, Problem::TooManySlides(names.len())));
                    }
                    let mut out: Vec<String> = Vec::with_capacity(names.len());
                    for name in names {
                        if let Some(p) = name_problem(name) {
                            return Err(bad(e.line, p));
                        }
                        if out.iter().any(|s| s == name) {
                            return Err(bad(e.line, Problem::SlideDuplicate));
                        }
                        out.push(String::from(*name));
                    }
                    slides = Some((e.line, out));
                }
                _ => return Err(bad(e.line, Problem::SlidesNotArray)),
            },
            _ => return Err(bad(e.line, Problem::UnknownKey)),
        }
    }

    match slides {
        Some((_, slides)) => Ok(Deck { title, slides }),
        None => Err(bad(1, Problem::NoSlides)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_minimal_manifest_parses() {
        let d = parse("slides = [\"a.md\"]").expect("valid");
        assert_eq!(d.slides, ["a.md"]);
        assert!(d.title.is_none());
    }

    #[test]
    fn order_is_the_manifests_order_not_sorted() {
        let d = parse("slides = [\"z.md\", \"a.md\", \"m.md\"]").expect("valid");
        assert_eq!(d.slides, ["z.md", "a.md", "m.md"]);
    }

    #[test]
    fn a_title_and_comments_and_a_multiline_array() {
        let src = "\
# the deck
title = \"A talk\"
slides = [
    \"01-open.md\",   # the title card
    \"02-body.md\",
]
";
        let d = parse(src).expect("valid");
        assert_eq!(d.title.as_deref(), Some("A talk"));
        assert_eq!(d.slides, ["01-open.md", "02-body.md"]);
    }

    fn problem_of(src: &str) -> Problem {
        parse(src).err().expect("must be refused").problem
    }

    #[test]
    fn the_slides_key_is_required_and_non_empty() {
        assert_eq!(problem_of("title = \"t\""), Problem::NoSlides);
        assert_eq!(problem_of(""), Problem::NoSlides);
        assert_eq!(problem_of("slides = []"), Problem::NoSlidesListed);
    }

    #[test]
    fn a_slide_name_is_a_markdown_file_name_and_nothing_else() {
        assert_eq!(problem_of("slides = [\"\"]"), Problem::SlideEmpty);
        assert_eq!(problem_of("slides = [\"sub/a.md\"]"), Problem::SlidePath);
        // Traversal is refused twice over, and which rule catches it depends
        // only on whether a separator is present: the separator check runs
        // first and is the more specific complaint.
        assert_eq!(problem_of("slides = [\"../a.md\"]"), Problem::SlidePath);
        assert_eq!(problem_of("slides = [\"..\"]"), Problem::SlideDotted);
        assert_eq!(
            problem_of("slides = [\".hidden.md\"]"),
            Problem::SlideDotted
        );
        assert_eq!(problem_of("slides = [\"-rf.md\"]"), Problem::SlideOption);
        assert_eq!(
            problem_of("slides = [\"notes.txt\"]"),
            Problem::SlideNotMarkdown
        );
        assert_eq!(
            problem_of("slides = [\"a.md.bak\"]"),
            Problem::SlideNotMarkdown
        );
        // An absolute path trips the separator rule, not the dot rule.
        assert_eq!(problem_of("slides = [\"/etc/passwd\"]"), Problem::SlidePath);
    }

    #[test]
    fn a_slide_named_twice_is_refused() {
        assert_eq!(
            problem_of("slides = [\"a.md\", \"b.md\", \"a.md\"]"),
            Problem::SlideDuplicate
        );
    }

    #[test]
    fn unknown_keys_and_tables_are_refused_not_ignored() {
        assert_eq!(
            problem_of("slides = [\"a.md\"]\nscale = 200"),
            Problem::UnknownKey
        );
        // A deck file carries content and order; it has no display authority,
        // so a key reaching for one is refused like any other unknown key.
        assert_eq!(
            problem_of("theme = \"big\"\nslides = [\"a.md\"]"),
            Problem::UnknownKey
        );
        assert_eq!(
            problem_of("[deck]\nslides = [\"a.md\"]"),
            Problem::TableHeader
        );
    }

    #[test]
    fn wrong_value_shapes_are_refused() {
        assert_eq!(problem_of("slides = \"a.md\""), Problem::SlidesNotArray);
        assert_eq!(problem_of("slides = 3"), Problem::SlidesNotArray);
        assert_eq!(
            problem_of("slides = [\"a.md\"]\ntitle = 3"),
            Problem::TitleNotString
        );
    }

    #[test]
    fn the_slide_cap_holds_at_its_edge() {
        let mut src = String::from("slides = [");
        for i in 0..SLIDES_MAX {
            if i > 0 {
                src.push_str(", ");
            }
            src.push('"');
            // Distinct names, so the duplicate rule is not what is measured.
            for _ in 0..=i {
                src.push('a');
            }
            src.push_str(".md\"");
        }
        src.push(']');
        let d = parse(&src).expect("SLIDES_MAX slides fit");
        assert_eq!(d.slides.len(), SLIDES_MAX);

        // One more is refused. The TOML subset's array cap equals SLIDES_MAX,
        // so the refusal arrives from there -- which is the point: the first
        // gate a hostile manifest meets is the parser's own bound.
        let mut over = String::from("slides = [");
        for i in 0..=SLIDES_MAX {
            if i > 0 {
                over.push_str(", ");
            }
            over.push('"');
            for _ in 0..=i {
                over.push('b');
            }
            over.push_str(".md\"");
        }
        over.push(']');
        assert!(parse(&over).is_err(), "SLIDES_MAX + 1 slides are refused");
    }

    #[test]
    fn a_diagnostic_names_the_line() {
        let src = "slides = [\"a.md\"]\n\nnonsense\n";
        let d = parse(src).err().expect("refused");
        assert_eq!(d.line, 3);
    }
}
