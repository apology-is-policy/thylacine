//! The presentation verb table (BEACON.md 7): the ONE rules engine the
//! transcript's context menu, the tag bar and acme-style selection
//! execution share -- "text + (inferred or annotated) type -> verb". A
//! plumber-style rules file, one rule per line:
//!
//! ```text
//! # comment
//! <type> <label> <command template...>
//! path   ls      ls -l {}
//! ```
//!
//! `{}` in the template is replaced by the RESOLVED ref, single-quoted the
//! way ut's lexer reads it (rc's rule: `''` is the one escape), so the
//! command the user chose acts on exactly the ref the menu displayed (the
//! anti-clickjack corollary of the security clause). A template that starts
//! with `#` is an INTERNAL action a renderer interprets itself (a test lever,
//! never a shell command); the parser admits it only when asked to
//! (`allow_internal`), so a production build drops such rules on sight --
//! the #880 strip class.
//!
//! Bounded parse (the security clause's third corollary): the file may come
//! from a session tier one day, so every field is length-capped, an
//! over-long or malformed line is dropped rather than guessed, and the rule
//! count is capped.

use alloc::string::String;
use alloc::vec::Vec;

/// One rule: verbs are offered per `ty`; `label` is what the menu shows;
/// `template` is what runs (with `{}` expanded) when it is chosen.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Rule {
    pub ty: String,
    pub label: String,
    pub template: String,
}

/// Field bounds (chars) + the rule-count cap.
pub const MAX_TYPE: usize = 16;
pub const MAX_LABEL: usize = 32;
pub const MAX_TEMPLATE: usize = 256;
pub const MAX_RULES: usize = 256;

fn take_token(s: &str) -> (&str, &str) {
    let s = s.trim_start();
    let end = s.find(char::is_whitespace).unwrap_or(s.len());
    (&s[..end], &s[end..])
}

/// Parse a rules file. Comments (`#` first) and blank lines are skipped; a
/// line short of three fields, or over a bound, is dropped. Internal
/// actions (template `#...`) are kept only under `allow_internal`.
pub fn parse(text: &str, allow_internal: bool) -> Vec<Rule> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (ty, rest) = take_token(line);
        let (label, rest) = take_token(rest);
        let template = rest.trim();
        if ty.is_empty() || label.is_empty() || template.is_empty() {
            continue;
        }
        if ty.chars().count() > MAX_TYPE
            || label.chars().count() > MAX_LABEL
            || template.chars().count() > MAX_TEMPLATE
        {
            continue;
        }
        if is_internal(template) && !allow_internal {
            continue;
        }
        out.push(Rule { ty: String::from(ty), label: String::from(label), template: String::from(template) });
        if out.len() >= MAX_RULES {
            break;
        }
    }
    out
}

/// Is this template a renderer-internal action rather than a command?
pub fn is_internal(template: &str) -> bool {
    template.starts_with('#')
}

/// The rules offered for one obj type, in file order.
pub fn rules_for<'a>(rules: &'a [Rule], ty: &str) -> Vec<&'a Rule> {
    rules.iter().filter(|r| r.ty == ty).collect()
}

/// Single-quote a ref for ut (rc's rule: `'` inside becomes `''`). None when
/// the ref carries a control character -- no quoting makes a newline safe
/// on a command line, so such a ref gets no runnable verb at all.
pub fn quote(refv: &str) -> Option<String> {
    if refv.chars().any(|c| (c as u32) < 0x20 || c == '\u{7f}') {
        return None;
    }
    let mut s = String::with_capacity(refv.len() + 2);
    s.push('\'');
    for c in refv.chars() {
        if c == '\'' {
            s.push_str("''");
        } else {
            s.push(c);
        }
    }
    s.push('\'');
    Some(s)
}

/// Expand a template against the resolved ref: every `{}` becomes the quoted
/// ref. None when the ref cannot be quoted safely (see `quote`).
pub fn expand(template: &str, refv: &str) -> Option<String> {
    let q = quote(refv)?;
    let mut out = String::with_capacity(template.len() + q.len());
    let mut rest = template;
    while let Some(i) = rest.find("{}") {
        out.push_str(&rest[..i]);
        out.push_str(&q);
        rest = &rest[i + 2..];
    }
    out.push_str(rest);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    const FILE: &str = "# the system tier\n\
\n\
path  ls     ls -l {}\n\
path\tcat\tcat {}\n\
pid   kill   kill {}\n\
path  wedge-test  #wedge 6000\n\
broken line\n\
url   fetch  wget {} {}\n";

    #[test]
    fn parse_skips_comments_blanks_and_short_lines_and_keeps_order() {
        let r = parse(FILE, false);
        let labels: Vec<&str> = r.iter().map(|r| r.label.as_str()).collect();
        assert_eq!(labels, ["ls", "cat", "kill", "fetch"]);
        assert_eq!(r[0].template, "ls -l {}", "double spaces are separators, the template keeps its own");
        assert_eq!(r[1].template, "cat {}", "tabs separate too");
    }

    #[test]
    fn internal_actions_are_gated() {
        assert!(parse(FILE, false).iter().all(|r| !is_internal(&r.template)));
        let r = parse(FILE, true);
        let w = r.iter().find(|r| r.label == "wedge-test").expect("admitted under allow_internal");
        assert!(is_internal(&w.template));
        assert_eq!(w.template, "#wedge 6000");
    }

    #[test]
    fn bounds_drop_the_line_never_truncate() {
        let long_label = alloc::format!("path {} ls {{}}\n", "x".repeat(MAX_LABEL + 1));
        assert!(parse(&long_label, false).is_empty());
        let long_tpl = alloc::format!("path ls {}\n", "y".repeat(MAX_TEMPLATE + 1));
        assert!(parse(&long_tpl, false).is_empty());
        let ok = alloc::format!("path {} ls {{}}\n", "x".repeat(MAX_LABEL));
        assert_eq!(parse(&ok, false).len(), 1);
        let many: String = (0..MAX_RULES + 10).map(|i| alloc::format!("path l{} ls {{}}\n", i)).collect();
        assert_eq!(parse(&many, false).len(), MAX_RULES);
    }

    #[test]
    fn rules_for_filters_by_type() {
        let r = parse(FILE, false);
        let p: Vec<&str> = rules_for(&r, "path").iter().map(|r| r.label.as_str()).collect();
        assert_eq!(p, ["ls", "cat"]);
        assert!(rules_for(&r, "commit").is_empty());
    }

    // The anti-clickjack corollary in code: the ref is quoted exactly as
    // ut's lexer reads a single-quoted word, so the command acts on the ref
    // shown and nothing else -- an embedded quote cannot end the word.
    #[test]
    fn quote_is_rc_single_quoting() {
        assert_eq!(quote("/lib/aurora/config").as_deref(), Some("'/lib/aurora/config'"));
        assert_eq!(quote("/home/o'brien").as_deref(), Some("'/home/o''brien'"));
        assert_eq!(quote("a b; rm -rf /").as_deref(), Some("'a b; rm -rf /'"));
        assert_eq!(quote("x\ny"), None, "a newline cannot be quoted onto one line");
        assert_eq!(quote("x\u{7f}"), None);
    }

    #[test]
    fn expand_replaces_every_placeholder() {
        assert_eq!(expand("ls -l {}", "/x").as_deref(), Some("ls -l '/x'"));
        assert_eq!(expand("cp {} {}.bak", "/x y").as_deref(), Some("cp '/x y' '/x y'.bak"));
        assert_eq!(expand("ps", "/x").as_deref(), Some("ps"), "no placeholder: the template as is");
        assert_eq!(expand("cat {}", "a\nb"), None);
    }
}
