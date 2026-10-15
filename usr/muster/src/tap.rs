// The verdict meson gives a TAP test (mesonbuild/mtest.py at 1.12.1: TAPParser
// and TestRunTAP.parse), over the test's stdout; the exit status is the step's.
// A test fails on a bad result (`not ok`, or `ok` under TODO), a `Bail out!`, or
// any parse error, as meson's does. Two refusals are muster's own, because a run
// that reports nothing proved nothing: no plan, or no result that is not a SKIP
// (meson reports the latter SKIP, not failed).

use alloc::format;
use alloc::string::String;

#[derive(Clone, Copy, PartialEq)]
enum State {
    Main,
    AfterTest,
    Yaml,
}

#[derive(Clone, Copy)]
struct Plan {
    num_tests: u64,
    late: bool,
}

struct Parser {
    state: State,
    version: u64,
    lineno: u64,
    plan: Option<Plan>,
    num_tests: u64,
    last_test: u64,
    highest_test: u64,
    found_late_test: bool,
    bailed_out: bool,
    yaml_lineno: u64,
    yaml_indent: String,
    passed: u64,
    counted: u64,
    failure: Option<String>,
}

fn is_space(c: char) -> bool {
    c.is_whitespace()
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

// `\s*` then a run of ASCII digits; the digits and the rest.
fn digits(s: &str) -> (&str, &str) {
    let end = s.find(|c: char| !c.is_ascii_digit()).unwrap_or(s.len());
    (&s[..end], &s[end..])
}

// The directive tail: `\s*#\s*(SKIP\S*|TODO)\b`, matched at the start of `s`.
// Python's `re` tries SKIP\S* longest first and backs off to a word boundary.
fn directive(s: &str) -> Option<&'static str> {
    let s = s.trim_start_matches(is_space);
    let s = s.strip_prefix('#')?.trim_start_matches(is_space);
    let head: String = s.chars().take(4).collect();
    let up = head.to_ascii_uppercase();
    // \S* backs off to a word boundary, and one always follows some prefix of
    // the run (P is a word character), so SKIP matches whatever comes after it.
    if up == "SKIP" {
        return Some("SKIP");
    }
    if up == "TODO" {
        let next = s[head.len()..].chars().next();
        if next.is_none_or(|c| !is_word(c)) {
            return Some("TODO");
        }
    }
    None
}

impl Parser {
    fn new() -> Parser {
        Parser {
            state: State::Main,
            version: 12,
            lineno: 0,
            plan: None,
            num_tests: 0,
            last_test: 0,
            highest_test: 0,
            found_late_test: false,
            bailed_out: false,
            yaml_lineno: 0,
            yaml_indent: String::new(),
            passed: 0,
            counted: 0,
            failure: None,
        }
    }

    fn fail(&mut self, why: String) {
        if self.failure.is_none() {
            self.failure = Some(why);
        }
    }

    fn line(&mut self, raw: &str) {
        self.lineno += 1;
        match self.state {
            State::AfterTest => {
                if self.version >= 13 {
                    let indent = raw.len() - raw.trim_start_matches(is_space).len();
                    if indent > 0 && raw[indent..].starts_with("---") {
                        self.state = State::Yaml;
                        self.yaml_lineno = self.lineno;
                        self.yaml_indent = String::from(&raw[..indent]);
                        return;
                    }
                }
                self.state = State::Main;
            }
            State::Yaml => {
                let indent = raw.len() - raw.trim_start_matches(is_space).len();
                if indent > 0 && raw[indent..].starts_with("...") {
                    self.state = State::Main;
                    return;
                }
                if raw.starts_with(self.yaml_indent.as_str()) {
                    return;
                }
                let l = self.yaml_lineno;
                self.fail(format!("TAP: YAML block not terminated (started on line {})", l));
                self.state = State::Main;
            }
            State::Main => {}
        }
        let line = raw.trim_end_matches(is_space);
        if line.is_empty() || line.starts_with('#') {
            return;
        }

        let (ok, rest) = if let Some(r) = line.strip_prefix("not ok") {
            (false, Some(r))
        } else if let Some(r) = line.strip_prefix("ok") {
            (true, Some(r))
        } else {
            (false, None)
        };
        if let Some(rest) = rest {
            if let Some(p) = self.plan {
                if p.late && !self.found_late_test {
                    self.fail(String::from("TAP: unexpected test after late plan"));
                    self.found_late_test = true;
                }
            }
            self.num_tests += 1;
            let after = rest.trim_start_matches(is_space);
            let (num, after) = digits(after);
            let named = !num.is_empty();
            self.last_test = if named {
                match num.parse::<u64>() {
                    Ok(n) => n,
                    Err(_) => {
                        self.fail(format!("TAP: test number {} is out of range", num));
                        self.last_test + 1
                    }
                }
            } else {
                self.last_test + 1
            };
            self.highest_test = self.highest_test.max(self.last_test);
            if let Some(p) = self.plan {
                if self.last_test > p.num_tests {
                    self.fail(String::from("TAP: test number exceeds maximum specified in test plan"));
                }
            }
            let after = if named { after.trim_start_matches(is_space) } else { after };
            let hash = after.find('#').unwrap_or(after.len());
            let name = after[..hash].trim();
            let label = format!("{} {}", self.last_test, name);
            let label = label.trim();
            match directive(&after[hash..]) {
                Some("SKIP") if ok => {}
                Some("TODO") if ok => self.fail(format!("TAP: {} passed under TODO", label)),
                Some("TODO") => self.counted += 1,
                _ if ok => {
                    self.passed += 1;
                    self.counted += 1;
                }
                _ => self.fail(format!("TAP: not ok {}", label)),
            }
            self.state = State::AfterTest;
            return;
        }

        if let Some(r) = line.strip_prefix("1..") {
            let (num, after) = digits(r);
            if !num.is_empty() {
                if self.plan.is_some() {
                    self.fail(String::from("TAP: more than one plan found"));
                    return;
                }
                let n = match num.parse::<u64>() {
                    Ok(n) => n,
                    Err(_) => {
                        self.fail(format!("TAP: plan 1..{} is out of range", num));
                        return;
                    }
                };
                match directive(after) {
                    Some("SKIP") if n > 0 => self.fail(String::from("TAP: invalid SKIP directive for plan")),
                    Some("SKIP") | None => {}
                    Some(_) => self.fail(String::from("TAP: invalid directive for plan")),
                }
                self.plan = Some(Plan { num_tests: n, late: self.num_tests > 0 });
                return;
            }
        }

        if let Some(m) = line.strip_prefix("Bail out!") {
            self.fail(format!("TAP: Bail out! {}", m.trim_start_matches(is_space)));
            self.bailed_out = true;
            return;
        }

        if let Some(r) = line.strip_prefix("TAP version ") {
            let (num, _) = digits(r);
            if !num.is_empty() {
                if self.lineno != 1 {
                    self.fail(String::from("TAP: version number must be on the first line"));
                    return;
                }
                self.version = num.parse::<u64>().unwrap_or(u64::MAX);
                if self.version < 13 {
                    self.fail(String::from("TAP: version number should be at least 13"));
                }
            }
        }
        // Anything else is an unknown line: a warning to meson, not a failure.
    }

    fn end(mut self) -> Result<u64, String> {
        if self.state == State::Yaml {
            let l = self.yaml_lineno;
            self.fail(format!("TAP: YAML block not terminated (started on line {})", l));
        }
        if !self.bailed_out {
            match self.plan {
                Some(p) if self.num_tests != p.num_tests => {
                    let (which, n) = if self.num_tests < p.num_tests { ("few", p.num_tests) } else { ("many", p.num_tests) };
                    let got = self.num_tests;
                    self.fail(format!("TAP: Too {} tests run (expected {}, got {})", which, n, got));
                }
                _ if self.highest_test != self.num_tests => {
                    let (which, n, h) = if self.highest_test < self.num_tests {
                        ("Duplicate", self.num_tests, self.highest_test)
                    } else {
                        ("Missing", self.num_tests, self.highest_test)
                    };
                    self.fail(format!("TAP: {} test numbers (expected {}, got test numbered {})", which, n, h));
                }
                _ => {}
            }
        }
        if let Some(why) = self.failure {
            return Err(why);
        }
        if self.plan.is_none() {
            return Err(String::from("TAP: no plan (the program reported no test count)"));
        }
        if self.counted == 0 {
            return Err(String::from("TAP: every result is a SKIP, or there is none (meson's SKIP)"));
        }
        Ok(self.passed)
    }
}

/// The TAP verdict over a test's stdout: the number of passing results, or why
/// it failed.
pub fn verdict(text: &str) -> Result<u64, String> {
    let mut p = Parser::new();
    for raw in text.split_inclusive('\n') {
        p.line(raw);
    }
    p.end()
}
