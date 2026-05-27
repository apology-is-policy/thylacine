// libutopia::eval::value -- the Utopia value model.
//
// === The unified-list representation ===
//
// Per scripture (UTOPIA-SHELL-DESIGN.md section 4.1): "Variables are
// strings. Lists of strings are first-class but list-elements are
// strings." And section 6.3: "A list is a list; a scalar is a scalar
// (a one-element list)."
//
// Section 4.2 pins the encoding: "UTF-8 throughout. Every Utopia
// program emits UTF-8. Filesystem paths are bytes (as is Plan 9 /
// Unix tradition) but conventionally UTF-8."
//
// Combined: every value is a flat list of UTF-8 strings. A scalar is
// the special case of a one-element list. This is the rc semantic
// model -- bash/zsh's scalar-vs-array distinction collapses to a
// single representation, and the user-visible distinction (scripture
// 6.3's $#var = 1 for scalar vs N for list) emerges naturally from
// Vec::len.
//
// Why unified instead of `enum Value { String(String), List(Vec<String>) }`:
//   - Every consumer would have to match the enum to do anything
//     useful with it; the match arms are nearly identical.
//   - Scripture explicitly says "a scalar is a scalar (a one-element
//     list)" -- the conceptual distinction is preserved, but
//     implementations of rc agree the underlying representation is
//     uniform.
//   - argv expansion is a Vec<String> by definition (each element ->
//     one argv item); having Value already shaped that way means
//     argv expansion is just `value.0.iter().cloned()`.
//   - $"var (no-split per scripture 6.8) collapses a multi-element
//     value into a one-element value by joining with space -- a
//     simple `Value(vec![self.joined(" ")])`.
//
// === Empty values ===
//
// An undefined variable evaluates to `Value(vec![])` (the empty
// list). This is rc's behaviour: $undefined expands to zero argv
// elements. Scripture is silent on this (section 6 doesn't mention
// undefined-var semantics) but rc's default is well-established
// and matches the principle of least surprise for shell users.

use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::fmt;

/// A Utopia runtime value. Always a list of UTF-8 strings; a "scalar"
/// is a one-element list. See module-level docs for the rationale.
#[derive(Clone, Debug, Eq, PartialEq, Default)]
pub struct Value(pub Vec<String>);

impl Value {
    /// Construct an empty value (zero elements). The result of
    /// looking up an undefined variable.
    pub fn empty() -> Self {
        Value(Vec::new())
    }

    /// Construct a scalar (one-element) value. Used for atom
    /// evaluation (Word, SingleQuoted, Integer, etc.) and for the
    /// result of arithmetic operators.
    pub fn scalar<S: Into<String>>(s: S) -> Self {
        let mut v = Vec::with_capacity(1);
        v.push(s.into());
        Value(v)
    }

    /// Construct a value from a Vec of element strings. Used for
    /// list literals (scripture section 6.3) and for evaluation of
    /// the List ExprKind variant.
    pub fn list(parts: Vec<String>) -> Self {
        Value(parts)
    }

    /// Number of elements (scripture section 6.9: `$#var`).
    pub fn len(&self) -> usize {
        self.0.len()
    }

    /// True if zero elements -- the result of looking up an undefined
    /// variable, or of an empty list literal `()`.
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// Join all elements with a separator. Used for $"var (scripture
    /// 6.8) and for collapsing a multi-element value into a single
    /// scalar (e.g., for the scrutinee of `case`, where scripture
    /// requires a single value to match against).
    pub fn joined(&self, sep: &str) -> String {
        self.0.join(sep)
    }

    /// Collapse to a single scalar string by joining elements with a
    /// space. The conventional flattening when a context expects ONE
    /// string from a value (e.g., the scrutinee of `case`, the
    /// argument of `cd`, the right-hand side of `=~`). Empty values
    /// collapse to "".
    pub fn as_scalar(&self) -> String {
        self.joined(" ")
    }

    /// Borrow the underlying elements for argv expansion. Each
    /// element produces one argv item. Used at SimpleCommand
    /// evaluation (U-6c).
    pub fn as_elements(&self) -> &[String] {
        &self.0
    }

    /// Attempt to interpret a value as a single integer for
    /// arithmetic. Scripture section 6.13: "Integers only at v1;
    /// floats are deferred." A multi-element value is collapsed via
    /// `as_scalar` first; an empty value parses as 0; non-numeric
    /// text returns None.
    ///
    /// Returns None on parse failure; the caller decides whether to
    /// surface `EvalErrorKind::NonNumeric` (typically yes in arith
    /// context) or fall back (e.g., string comparison).
    pub fn as_int(&self) -> Option<i64> {
        if self.0.is_empty() {
            return Some(0);
        }
        let s = self.as_scalar();
        s.trim().parse::<i64>().ok()
    }

    /// Whether the value is "truthy" for control-flow purposes
    /// (scripture section 7.5 `if`, section 7.6 `while`). The
    /// scripture doesn't pin truthiness explicitly; rc's tradition
    /// is "$status == 0 is truthy" for commands but for expression
    /// values the convention is:
    ///   - empty value -> false
    ///   - one-element value "0" -> false
    ///   - one-element value "" -> false
    ///   - everything else -> true
    /// This matches the principle that "false-y" values in shells
    /// are empty / zero / false strings.
    ///
    /// At U-6a this is used by `Match` and `Case` arms. Control-flow
    /// dispatch lands at U-6b.
    pub fn is_truthy(&self) -> bool {
        if self.0.is_empty() {
            return false;
        }
        if self.0.len() == 1 {
            let s = &self.0[0];
            if s.is_empty() || s == "0" || s == "false" {
                return false;
            }
        }
        true
    }

    /// Push an element onto the value's list. Used by the Concat
    /// evaluator when flattening sub-expressions.
    pub fn push(&mut self, s: String) {
        self.0.push(s);
    }

    /// Extend with another value's elements. Used by list-literal
    /// evaluation when a sub-expression is itself a multi-element
    /// value.
    pub fn extend_from(&mut self, other: Value) {
        self.0.extend(other.0);
    }
}

impl From<&str> for Value {
    fn from(s: &str) -> Self {
        Value::scalar(s.to_string())
    }
}

impl From<String> for Value {
    fn from(s: String) -> Self {
        Value::scalar(s)
    }
}

impl From<i64> for Value {
    fn from(n: i64) -> Self {
        let mut s = String::new();
        // alloc-only no_std: use the fmt::Write trait via format_into.
        use core::fmt::Write;
        let _ = write!(s, "{}", n);
        Value::scalar(s)
    }
}

impl fmt::Display for Value {
    /// Default Display joins elements with a single space, mirroring
    /// rc's interpolation behaviour (`echo $list` prints elements
    /// space-separated).
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.as_scalar())
    }
}
