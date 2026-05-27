// libutopia::eval::error -- runtime errors from the evaluator.
//
// === The error model at U-6a ===
//
// At U-6a only expression evaluation lands. The errors below cover
// only the subset of failures reachable from `eval_expr`:
//
//   - Arithmetic: NonNumeric (the value can't parse as i64),
//     DivByZero, Overflow.
//   - Patterns: NoCaseMatch (case-as-expression exhausted without
//     matching; rc errors here).
//   - Deferred: NotImplemented (a feature not in v1 -- Subst /
//     Backtick / ProcSub / Regex). The variant carries a short
//     static string naming the feature so callers (and tests) can
//     match.
//
// Variable lookup of an undefined name is NOT an error -- it
// evaluates to the empty Value per rc convention. See
// `value.rs::Value::empty()`.
//
// === Span discipline ===
//
// Every EvalError carries the Span of the offending Expr. The U-6
// driver (the REPL or a future error formatter) uses the span to
// attach the error message to the source location.

use alloc::string::String;
use core::fmt;

use crate::parser::span::Span;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct EvalError {
    pub kind: EvalErrorKind,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum EvalErrorKind {
    /// Arithmetic on a value that does not parse as an integer.
    /// Scripture 6.13: "Integers only at v1". A non-integer in arith
    /// context is a runtime error.
    NonNumeric(String),
    /// Division or modulo by zero.
    DivByZero,
    /// Integer overflow in arithmetic. Scripture is silent on
    /// overflow semantics; we pick error (rather than wrap or
    /// saturate) so latent bugs surface at evaluation rather than
    /// silently producing garbage results.
    Overflow,
    /// Shift count out of range (negative or >= 64). Scripture is
    /// silent; we pick error. Distinct variant from Overflow so the
    /// error message can be specific.
    InvalidShift(i64),
    /// case-as-expression exhausted without matching any arm
    /// (scripture 7.2: "the chosen branch's value becomes the
    /// case-expression's value" -- if no branch is chosen, there is
    /// no value to produce, so we error).
    NoCaseMatch,
    /// Negative or zero index in $var(N) -- scripture 6.9 says
    /// 1-indexed, so 0 and negatives are out of range. We pick:
    /// out-of-range index produces an empty value (rc convention),
    /// not an error -- this variant is unused at v1.0 and reserved
    /// for stricter modes.
    ///
    /// Slice with M > N is similarly handled by returning empty.
    /// This variant remains as a placeholder for v1.x strict mode.
    #[allow(dead_code)]
    InvalidIndex(i64),
    /// A feature not yet implemented at v1.0. The static string
    /// names the feature ("substitution", "regex match", "process
    /// substitution") for error-message clarity. Callers (and tests)
    /// can match on the string to gate behaviour.
    NotImplemented(&'static str),
    /// Internal evaluator inconsistency: an AST shape the evaluator
    /// believed could not occur. Indicates a parser/evaluator
    /// invariant violation, not user error. Reaching this is a bug.
    Internal(&'static str),
}

pub type EvalResult<T> = Result<T, EvalError>;

impl EvalError {
    pub fn new(kind: EvalErrorKind, span: Span) -> Self {
        EvalError { kind, span }
    }
}

impl fmt::Display for EvalError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} at {}:{}", self.kind, self.span.start, self.span.end)
    }
}

impl fmt::Display for EvalErrorKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            EvalErrorKind::NonNumeric(s) => write!(f, "expected integer, got {:?}", s),
            EvalErrorKind::DivByZero => f.write_str("division by zero"),
            EvalErrorKind::Overflow => f.write_str("integer overflow"),
            EvalErrorKind::InvalidShift(n) => write!(f, "invalid shift count {}", n),
            EvalErrorKind::NoCaseMatch => f.write_str("case expression matched no arm"),
            EvalErrorKind::InvalidIndex(n) => write!(f, "invalid index {}", n),
            EvalErrorKind::NotImplemented(what) => {
                write!(f, "{} not yet implemented at v1.0", what)
            }
            EvalErrorKind::Internal(msg) => write!(f, "internal evaluator error: {}", msg),
        }
    }
}
