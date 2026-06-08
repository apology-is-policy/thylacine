// libutopia::eval::expr -- the recursive expression evaluator.
//
// === Scope at U-6a ===
//
// Walks an `Expr` produced by `libutopia::parser::parse_expr_tokens`
// and produces a `Value`. Covers everything except external-process
// or filesystem-bearing forms:
//
//   IMPLEMENTED:
//     ExprKind::Word         literal scalar
//     ExprKind::SingleQuoted literal scalar
//     ExprKind::DoubleQuoted scalar with $var / $#var interp
//                            (DqPart::Subst -> NotImplemented)
//     ExprKind::Integer      scalar (formatted via core::fmt)
//     ExprKind::Var          env lookup
//     ExprKind::VarLen       env lookup -> length-as-string
//     ExprKind::VarNoSplit   env lookup -> joined-with-space scalar
//     ExprKind::VarIndex     env lookup -> 1-indexed element (rc;
//                            out-of-range -> empty)
//     ExprKind::VarSlice     env lookup -> sub-list (rc-style)
//     ExprKind::List         flatten sub-expressions into a list
//     ExprKind::Concat       flatten sub-expressions; cross-product
//                            join (rc-style)
//     ExprKind::BinOp        integer arith (Add/Sub/Mul/Div/Mod/Pow/
//                            Shl/Shr/BitAnd/BitOr/BitXor) +
//                            comparison + logical (treat as 0/1)
//     ExprKind::UnOp         Neg / BitNot / Not
//     ExprKind::Match        MatchOp::Glob -> 1/0;
//                            MatchOp::In   -> 1/0;
//                            MatchOp::Regex -> NotImplemented
//     ExprKind::Case         case-as-expression (scripture 7.2)
//
//   DEFERRED (NotImplemented):
//     ExprKind::Subst        $(cmd) -- U-6e/g
//     ExprKind::Backtick     `{cmd} -- U-6e/g
//     ExprKind::ProcSubIn    <(cmd) -- U-6e/g
//     ExprKind::ProcSubOut   >(cmd) -- U-6e/g
//     ExprKind::Regex        only reachable via Match::Regex's RHS;
//                            return the pattern as a scalar so
//                            other code paths that hold a regex
//                            literal as a value (rare; not idiomatic)
//                            see a sensible default.
//
// === Comparison + truthiness convention ===
//
// Per scripture (UTOPIA-SHELL-DESIGN.md):
//   - Section 6.13: arithmetic operators are integer-typed. The
//     comparison operators (`< <= > >= == !=`) appearing in arith
//     context produce 0/1 (1 == true). This matches C/Python.
//   - Section 6.14: top-level `==` (outside `(( ))`) is "shell-
//     syntactic; both sides treated as strings". At U-6a the parser
//     emits `BinOp(Eq, ...)` for `==` in cond context too; we
//     compare strings then.
//
// We resolve this at the evaluator: BinOp::Eq / Ne dispatch on
// "are both operands successfully parseable as i64?" -- if yes,
// numeric compare; if no, lexicographic string compare on
// `as_scalar()`. This collapses scripture 6.13 + 6.14 into one
// rule and matches rc's behaviour.
//
// === Glob + In match operators ===
//
// `a matches b`: evaluates `a` and `b` to scalars; runs
// `glob::matches(b_scalar, a_scalar)`. Returns Value::scalar("1") or
// "0".
//
// `a in list`: evaluates `a` to scalar; evaluates `list` to a
// multi-element value; returns "1" if any element string-equals the
// scalar, else "0".
//
// === Case-as-expression ===
//
// Per scripture 7.2: each arm produces a single VALUE expression.
// First match wins. No fallthrough. Arm patterns are evaluated to a
// scalar string and matched as a GLOB against the scrutinee's
// scalar form. If no arm matches, EvalErrorKind::NoCaseMatch is
// raised at the case-expression's span (rc behaviour).

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use crate::parser::ast::{BinOp, Expr, ExprKind, MatchOp, UnOp};
use crate::parser::token::DqPart;

use super::env::Env;
use super::error::{EvalError, EvalErrorKind, EvalResult};
use super::glob;
use super::stmt::{run_command_substitution, run_command_substitution_script, split_fields};
use super::value::Value;

/// Evaluate an `Expr` against an `Env`. Pure function with respect
/// to the AST; side effects are limited to errors raised through
/// `EvalResult`.
pub fn eval_expr(env: &Env, expr: &Expr) -> EvalResult<Value> {
    match &expr.kind {
        // === Atoms ===
        ExprKind::Word(s) => Ok(Value::scalar(s.clone())),
        ExprKind::SingleQuoted(s) => Ok(Value::scalar(s.clone())),
        ExprKind::Integer(n) => Ok(Value::from(*n)),
        ExprKind::DoubleQuoted(parts) => eval_dq(env, parts, expr),

        ExprKind::Var(name) => Ok(env.get(name)),
        ExprKind::VarLen(name) => {
            let mut s = String::new();
            let _ = write!(s, "{}", env.get(name).len());
            Ok(Value::scalar(s))
        }
        ExprKind::VarNoSplit(name) => {
            let v = env.get(name);
            Ok(Value::scalar(v.joined(" ")))
        }
        ExprKind::VarIndex(name, idx_expr) => {
            let idx = eval_int(env, idx_expr)?;
            let v = env.get(name);
            Ok(index_one(&v, idx))
        }
        ExprKind::VarSlice(name, lo_expr, hi_expr) => {
            let lo = eval_int(env, lo_expr)?;
            let hi = eval_int(env, hi_expr)?;
            let v = env.get(name);
            Ok(slice_inclusive(&v, lo, hi))
        }

        // === Substitutions (U-6f) ===
        // The expr layer holds a PRE-PARSED body (`Box<Script>`); run it
        // directly. Value context whitespace-splits the captured output
        // into a list (rc $ifs), like the bare-word path. See
        // stmt::run_command_substitution_script for the v1.0 body-shape
        // restriction + the $status semantics (scripture 6.6 + 8.7).
        ExprKind::Subst(script) | ExprKind::Backtick(script) => {
            let captured = run_command_substitution_script(env, script, expr.span)?;
            Ok(Value::list(split_fields(&captured)))
        }
        ExprKind::ProcSubIn(_) | ExprKind::ProcSubOut(_) => Err(EvalError::new(
            // Process substitution needs a `/proc/self/fd/N` namespace
            // surface (scripture 6.12) the kernel does not expose yet --
            // a separable v1.x feature, not a dependency of `$(cmd)`.
            EvalErrorKind::NotImplemented("process substitution <(cmd) / >(cmd)"),
            expr.span,
        )),
        ExprKind::Regex(pat) => Ok(Value::scalar(pat.clone())),

        // === Composites ===
        ExprKind::List(parts) => {
            let mut out = Value::empty();
            for p in parts {
                out.extend_from(eval_expr(env, p)?);
            }
            Ok(out)
        }
        ExprKind::Concat(parts) => eval_concat(env, parts),

        // === Operators ===
        ExprKind::BinOp(op, l, r) => eval_binop(env, *op, l, r),
        ExprKind::UnOp(op, inner) => eval_unop(env, *op, inner),
        ExprKind::Match(op, l, r) => eval_match(env, *op, l, r, expr),

        // === Case-as-expression ===
        ExprKind::Case(case_expr) => eval_case(env, case_expr, expr),
    }
}

// ---------------------------------------------------------------------
// Double-quoted string interpolation
// ---------------------------------------------------------------------

fn eval_dq(env: &Env, parts: &[DqPart], outer: &Expr) -> EvalResult<Value> {
    let mut out = String::new();
    for part in parts {
        match part {
            DqPart::Literal(s) => out.push_str(s),
            DqPart::Var(name) => {
                let v = env.get(name);
                out.push_str(&v.as_scalar());
            }
            DqPart::VarLen(name) => {
                let _ = write!(out, "{}", env.get(name).len());
            }
            // Inside `"..."` a substitution is inserted verbatim (no field
            // split). The DqPart body is raw source, so re-parse via the
            // string entry point.
            DqPart::Subst(body) => {
                out.push_str(&run_command_substitution(env, body, outer.span)?);
            }
        }
    }
    Ok(Value::scalar(out))
}

// ---------------------------------------------------------------------
// VarIndex and VarSlice
// ---------------------------------------------------------------------

/// 1-indexed element access (scripture 6.9). Index 0, negatives, or
/// out-of-range return an empty value (rc convention; matches the
/// principle that an undefined index is "empty list").
fn index_one(v: &Value, idx: i64) -> Value {
    if idx < 1 {
        return Value::empty();
    }
    let zero_based = (idx - 1) as usize;
    match v.0.get(zero_based) {
        Some(s) => Value::scalar(s.clone()),
        None => Value::empty(),
    }
}

/// 1-indexed inclusive slice (scripture 6.9 -- `$var(M N)`). Out-of-
/// range clamps; if lo > hi or lo > len, returns empty.
fn slice_inclusive(v: &Value, lo: i64, hi: i64) -> Value {
    if lo < 1 || hi < lo {
        return Value::empty();
    }
    let len = v.0.len() as i64;
    if lo > len {
        return Value::empty();
    }
    let lo_z = (lo - 1) as usize;
    let hi_z = ((hi.min(len)) - 1) as usize;
    Value::list(v.0[lo_z..=hi_z].to_vec())
}

// ---------------------------------------------------------------------
// Concat
// ---------------------------------------------------------------------

/// `a^b^c` -- string concatenation across span-adjacent sub-
/// expressions (scripture 6.7). The semantics with lists is
/// CROSS-PRODUCT (rc-style): if any operand is multi-element, the
/// result has |a| * |b| * |c| elements, each formed by concatenating
/// one element from each operand.
///
/// Examples:
///   ('a')      ^ ('b')      -> ('ab')           [1 * 1 = 1]
///   ('a' 'b')  ^ ('1')      -> ('a1' 'b1')      [2 * 1 = 2]
///   ('a' 'b')  ^ ('1' '2')  -> ('a1' 'a2' 'b1' 'b2') [2 * 2 = 4]
///   ()         ^ ('x')      -> ()               [0 * 1 = 0]
///
/// rc's docs spell this out; the cross-product is the natural lift
/// of string concatenation to lists. (Note: bash/zsh do NOT do this;
/// they pick one of "concat to first element" or "error" depending
/// on the option set. rc's cross-product is cleaner.)
fn eval_concat(env: &Env, parts: &[Expr]) -> EvalResult<Value> {
    if parts.is_empty() {
        return Ok(Value::empty());
    }
    let mut acc: Vec<String> = eval_expr(env, &parts[0])?.0;
    for p in &parts[1..] {
        let next = eval_expr(env, p)?.0;
        // Cross-product
        let mut new_acc = Vec::with_capacity(acc.len() * next.len());
        for a in &acc {
            for b in &next {
                let mut s = String::with_capacity(a.len() + b.len());
                s.push_str(a);
                s.push_str(b);
                new_acc.push(s);
            }
        }
        acc = new_acc;
    }
    Ok(Value::list(acc))
}

// ---------------------------------------------------------------------
// Binary operators
// ---------------------------------------------------------------------

fn eval_binop(env: &Env, op: BinOp, l: &Expr, r: &Expr) -> EvalResult<Value> {
    // Short-circuit logical operators.
    if matches!(op, BinOp::And | BinOp::Or) {
        let l_val = eval_expr(env, l)?;
        let l_t = l_val.is_truthy();
        if op == BinOp::And && !l_t {
            return Ok(Value::scalar("0"));
        }
        if op == BinOp::Or && l_t {
            return Ok(Value::scalar("1"));
        }
        let r_val = eval_expr(env, r)?;
        return Ok(Value::scalar(if r_val.is_truthy() { "1" } else { "0" }));
    }

    // Comparison: numeric if both parse as int; otherwise string.
    if matches!(
        op,
        BinOp::Lt | BinOp::Le | BinOp::Gt | BinOp::Ge | BinOp::Eq | BinOp::Ne
    ) {
        let lv = eval_expr(env, l)?;
        let rv = eval_expr(env, r)?;
        return Ok(eval_compare(op, &lv, &rv));
    }

    // Arithmetic + bitwise + shift: both sides must parse as int.
    let lv = eval_expr(env, l)?;
    let rv = eval_expr(env, r)?;
    let li = lv.as_int().ok_or_else(|| {
        EvalError::new(EvalErrorKind::NonNumeric(lv.as_scalar()), l.span)
    })?;
    let ri = rv.as_int().ok_or_else(|| {
        EvalError::new(EvalErrorKind::NonNumeric(rv.as_scalar()), r.span)
    })?;

    let result: i64 = match op {
        BinOp::Add => li
            .checked_add(ri)
            .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?,
        BinOp::Sub => li
            .checked_sub(ri)
            .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?,
        BinOp::Mul => li
            .checked_mul(ri)
            .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?,
        BinOp::Div => {
            if ri == 0 {
                return Err(EvalError::new(EvalErrorKind::DivByZero, r.span));
            }
            li.checked_div(ri)
                .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?
        }
        BinOp::Mod => {
            if ri == 0 {
                return Err(EvalError::new(EvalErrorKind::DivByZero, r.span));
            }
            li.checked_rem(ri)
                .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?
        }
        BinOp::Pow => {
            if ri < 0 {
                return Err(EvalError::new(EvalErrorKind::Overflow, r.span));
            }
            if ri > (u32::MAX as i64) {
                return Err(EvalError::new(EvalErrorKind::Overflow, r.span));
            }
            li.checked_pow(ri as u32)
                .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?
        }
        BinOp::BitAnd => li & ri,
        BinOp::BitOr => li | ri,
        BinOp::BitXor => li ^ ri,
        BinOp::Shl => {
            if !(0..64).contains(&ri) {
                return Err(EvalError::new(EvalErrorKind::InvalidShift(ri), r.span));
            }
            li.checked_shl(ri as u32)
                .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?
        }
        BinOp::Shr => {
            if !(0..64).contains(&ri) {
                return Err(EvalError::new(EvalErrorKind::InvalidShift(ri), r.span));
            }
            li.checked_shr(ri as u32)
                .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, r.span))?
        }
        // Comparison + logical handled above.
        BinOp::Lt
        | BinOp::Le
        | BinOp::Gt
        | BinOp::Ge
        | BinOp::Eq
        | BinOp::Ne
        | BinOp::And
        | BinOp::Or => {
            return Err(EvalError::new(
                EvalErrorKind::Internal("comparison/logical reached arithmetic branch"),
                r.span,
            ));
        }
    };

    Ok(Value::from(result))
}

fn eval_compare(op: BinOp, lv: &Value, rv: &Value) -> Value {
    // Numeric if both parse; else string.
    let result = match (lv.as_int(), rv.as_int()) {
        (Some(li), Some(ri)) => match op {
            BinOp::Lt => li < ri,
            BinOp::Le => li <= ri,
            BinOp::Gt => li > ri,
            BinOp::Ge => li >= ri,
            BinOp::Eq => li == ri,
            BinOp::Ne => li != ri,
            _ => unreachable!("eval_compare: non-comparison op"),
        },
        _ => {
            let ls = lv.as_scalar();
            let rs = rv.as_scalar();
            match op {
                BinOp::Lt => ls < rs,
                BinOp::Le => ls <= rs,
                BinOp::Gt => ls > rs,
                BinOp::Ge => ls >= rs,
                BinOp::Eq => ls == rs,
                BinOp::Ne => ls != rs,
                _ => unreachable!("eval_compare: non-comparison op"),
            }
        }
    };
    Value::scalar(if result { "1" } else { "0" })
}

fn eval_unop(env: &Env, op: UnOp, inner: &Expr) -> EvalResult<Value> {
    let v = eval_expr(env, inner)?;
    match op {
        UnOp::Neg => {
            let i = v
                .as_int()
                .ok_or_else(|| EvalError::new(EvalErrorKind::NonNumeric(v.as_scalar()), inner.span))?;
            let n = i
                .checked_neg()
                .ok_or_else(|| EvalError::new(EvalErrorKind::Overflow, inner.span))?;
            Ok(Value::from(n))
        }
        UnOp::BitNot => {
            let i = v
                .as_int()
                .ok_or_else(|| EvalError::new(EvalErrorKind::NonNumeric(v.as_scalar()), inner.span))?;
            Ok(Value::from(!i))
        }
        UnOp::Not => Ok(Value::scalar(if v.is_truthy() { "0" } else { "1" })),
    }
}

// ---------------------------------------------------------------------
// Helper: evaluate a sub-expression and require an integer.
// ---------------------------------------------------------------------

fn eval_int(env: &Env, expr: &Expr) -> EvalResult<i64> {
    let v = eval_expr(env, expr)?;
    v.as_int()
        .ok_or_else(|| EvalError::new(EvalErrorKind::NonNumeric(v.as_scalar()), expr.span))
}

// ---------------------------------------------------------------------
// Match operators
// ---------------------------------------------------------------------

fn eval_match(
    env: &Env,
    op: MatchOp,
    l: &Expr,
    r: &Expr,
    outer: &Expr,
) -> EvalResult<Value> {
    let lv = eval_expr(env, l)?;
    match op {
        MatchOp::Glob => {
            let rv = eval_expr(env, r)?;
            let pat = rv.as_scalar();
            let input = lv.as_scalar();
            Ok(Value::scalar(if glob::matches(&pat, &input) { "1" } else { "0" }))
        }
        MatchOp::In => {
            let rv = eval_expr(env, r)?;
            let needle = lv.as_scalar();
            let hit = rv.0.iter().any(|el| el == &needle);
            Ok(Value::scalar(if hit { "1" } else { "0" }))
        }
        MatchOp::Regex => Err(EvalError::new(
            EvalErrorKind::NotImplemented("regex match (=~)"),
            outer.span,
        )),
    }
}

// ---------------------------------------------------------------------
// Case-as-expression
// ---------------------------------------------------------------------

fn eval_case(
    env: &Env,
    case_expr: &crate::parser::ast::CaseExpr,
    outer: &Expr,
) -> EvalResult<Value> {
    let scrutinee = eval_expr(env, &case_expr.scrutinee)?;
    let scrutinee_str = scrutinee.as_scalar();
    for arm in &case_expr.arms {
        for pat in &arm.patterns {
            let pv = eval_expr(env, pat)?;
            let pat_str = pv.as_scalar();
            if glob::matches(&pat_str, &scrutinee_str) {
                return eval_expr(env, &arm.value);
            }
        }
    }
    Err(EvalError::new(EvalErrorKind::NoCaseMatch, outer.span))
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parser::parse_expr_tokens;
    use crate::parser::ast::ExprContext;
    use crate::parser::lexer::tokenize;
    use alloc::string::ToString;

    /// Parse-then-eval helper. Takes a fragment string + a context;
    /// re-lexes the fragment to a token stream (stripping the
    /// terminal Eof, which parse_expr_tokens does not accept), parses
    /// to an Expr, then evaluates against the given Env.
    fn eval_str(env: &Env, s: &str, ctx: ExprContext) -> EvalResult<Value> {
        let toks = tokenize(s).expect("test fragment lexes");
        let body: Vec<crate::parser::token::Token> = toks
            .into_iter()
            .filter(|t| !matches!(t.kind, crate::parser::token::TokenKind::Eof))
            .collect();
        let expr = parse_expr_tokens(body, s.len(), ctx).expect("test fragment parses");
        eval_expr(env, &expr)
    }

    #[test]
    fn integer_atom_arith() {
        let env = Env::new();
        let v = eval_str(&env, "42", ExprContext::Arith).unwrap();
        assert_eq!(v.as_scalar(), "42");
    }

    #[test]
    fn arith_add() {
        let env = Env::new();
        let v = eval_str(&env, "1 + 2 * 3", ExprContext::Arith).unwrap();
        assert_eq!(v.as_scalar(), "7");
    }

    #[test]
    fn arith_div_by_zero() {
        let env = Env::new();
        let err = eval_str(&env, "1 / 0", ExprContext::Arith).unwrap_err();
        assert_eq!(err.kind, EvalErrorKind::DivByZero);
    }

    #[test]
    fn arith_overflow() {
        let env = Env::new();
        // 2^32 * 2^32 overflows i64.
        let err = eval_str(&env, "9223372036854775807 + 1", ExprContext::Arith)
            .unwrap_err();
        assert_eq!(err.kind, EvalErrorKind::Overflow);
    }

    #[test]
    fn var_lookup() {
        let mut env = Env::new();
        env.let_set("x", Value::scalar("hello"));
        let v = eval_str(&env, "$x", ExprContext::Value).unwrap();
        assert_eq!(v.as_scalar(), "hello");
    }

    #[test]
    fn var_undefined_is_empty() {
        let env = Env::new();
        let v = eval_str(&env, "$nope", ExprContext::Value).unwrap();
        assert!(v.is_empty());
    }

    #[test]
    fn varlen_works() {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec![
            "a".to_string(),
            "b".to_string(),
            "c".to_string(),
        ]));
        let v = eval_str(&env, "$#xs", ExprContext::Value).unwrap();
        assert_eq!(v.as_scalar(), "3");
    }

    #[test]
    fn varindex_one_indexed() {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec![
            "a".to_string(),
            "b".to_string(),
            "c".to_string(),
        ]));
        let v = eval_str(&env, "$xs(2)", ExprContext::Value).unwrap();
        assert_eq!(v.as_scalar(), "b");
    }

    #[test]
    fn varindex_out_of_range_is_empty() {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec!["a".to_string()]));
        let v = eval_str(&env, "$xs(99)", ExprContext::Value).unwrap();
        assert!(v.is_empty());
    }

    #[test]
    fn varslice_works() {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec![
            "a".to_string(),
            "b".to_string(),
            "c".to_string(),
            "d".to_string(),
        ]));
        let v = eval_str(&env, "$xs(2 3)", ExprContext::Value).unwrap();
        assert_eq!(v.0, alloc::vec!["b".to_string(), "c".to_string()]);
    }

    #[test]
    fn list_literal() {
        let env = Env::new();
        let v = eval_str(&env, "(a b c)", ExprContext::Value).unwrap();
        assert_eq!(v.0, alloc::vec!["a".to_string(), "b".to_string(), "c".to_string()]);
    }

    #[test]
    fn concat_simple() {
        let mut env = Env::new();
        env.let_set("x", Value::scalar("42"));
        let v = eval_str(&env, "pre-^$x^-post", ExprContext::Value).unwrap();
        assert_eq!(v.as_scalar(), "pre-42-post");
    }

    #[test]
    fn concat_cross_product() {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec!["a".to_string(), "b".to_string()]));
        env.let_set("ys", Value::list(alloc::vec!["1".to_string(), "2".to_string()]));
        let v = eval_str(&env, "$xs^$ys", ExprContext::Value).unwrap();
        assert_eq!(v.0, alloc::vec![
            "a1".to_string(),
            "a2".to_string(),
            "b1".to_string(),
            "b2".to_string(),
        ]);
    }

    #[test]
    fn doublequoted_interp() {
        let mut env = Env::new();
        env.let_set("user", Value::scalar("alice"));
        let v = eval_str(&env, r#""Hello, $user""#, ExprContext::Value).unwrap();
        assert_eq!(v.as_scalar(), "Hello, alice");
    }

    #[test]
    fn doublequoted_varlen_interp() {
        let mut env = Env::new();
        env.let_set("xs", Value::list(alloc::vec![
            "a".to_string(),
            "b".to_string(),
        ]));
        let v = eval_str(&env, r#""count=$#xs""#, ExprContext::Value).unwrap();
        assert_eq!(v.as_scalar(), "count=2");
    }

    // Command substitution `$(cmd)` is implemented (U-6f) -- it spawns the
    // inner command, so it cannot be exercised by a host `cargo test` (no
    // syscall surface). The boot probe /u-subst-test covers it in-VM.
    // Process substitution `<(cmd)` / `>(cmd)` remains NotImplemented
    // (needs a /proc/self/fd/N surface); assert that here.
    #[test]
    fn procsub_not_implemented() {
        let env = Env::new();
        let err = eval_str(&env, "<(echo hi)", ExprContext::Value).unwrap_err();
        assert!(matches!(err.kind, EvalErrorKind::NotImplemented(_)));
    }

    #[test]
    fn match_glob_yes() {
        let mut env = Env::new();
        env.let_set("f", Value::scalar("foo.c"));
        let v = eval_str(&env, "($f matches *.c)", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "1");
    }

    #[test]
    fn match_glob_no() {
        let mut env = Env::new();
        env.let_set("f", Value::scalar("foo.rs"));
        let v = eval_str(&env, "($f matches *.c)", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "0");
    }

    #[test]
    fn match_in_yes() {
        let mut env = Env::new();
        env.let_set("v", Value::scalar("yes"));
        let v = eval_str(&env, "($v in (yes y true on))", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "1");
    }

    #[test]
    fn match_in_no() {
        let mut env = Env::new();
        env.let_set("v", Value::scalar("nope"));
        let v = eval_str(&env, "($v in (yes y true on))", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "0");
    }

    #[test]
    fn match_regex_not_implemented() {
        let mut env = Env::new();
        env.let_set("v", Value::scalar("foo"));
        let err = eval_str(&env, "($v =~ /^foo/)", ExprContext::Cond).unwrap_err();
        assert!(matches!(err.kind, EvalErrorKind::NotImplemented(_)));
    }

    #[test]
    fn compare_numeric() {
        let env = Env::new();
        let v = eval_str(&env, "(3 < 10)", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "1");
        let v = eval_str(&env, "(3 > 10)", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "0");
    }

    #[test]
    fn compare_string_lexicographic() {
        let mut env = Env::new();
        env.let_set("a", Value::scalar("alpha"));
        env.let_set("b", Value::scalar("beta"));
        let v = eval_str(&env, "($a < $b)", ExprContext::Cond).unwrap();
        assert_eq!(v.as_scalar(), "1");
    }

    #[test]
    fn unary_neg_not_bitnot() {
        let env = Env::new();
        let v = eval_str(&env, "-5", ExprContext::Arith).unwrap();
        assert_eq!(v.as_scalar(), "-5");
        let v = eval_str(&env, "~0", ExprContext::Arith).unwrap();
        assert_eq!(v.as_scalar(), "-1");
        let v = eval_str(&env, "!0", ExprContext::Arith).unwrap();
        assert_eq!(v.as_scalar(), "1");
    }

    #[test]
    fn shift_in_range() {
        let env = Env::new();
        let v = eval_str(&env, "1 << 4", ExprContext::Arith).unwrap();
        assert_eq!(v.as_scalar(), "16");
    }

    // case-as-expression is parsed in Value context (per U-5d).
    #[test]
    fn case_expr_basic() {
        let mut env = Env::new();
        env.let_set("f", Value::scalar("foo.c"));
        let v = eval_str(
            &env,
            "case $f { *.c => 'C source' ; *.rs => 'Rust source' ; * => 'unknown' }",
            ExprContext::Value,
        )
        .unwrap();
        assert_eq!(v.as_scalar(), "C source");
    }

    #[test]
    fn case_expr_catchall() {
        let mut env = Env::new();
        env.let_set("f", Value::scalar("README.md"));
        let v = eval_str(
            &env,
            "case $f { *.c => 'C source' ; *.rs => 'Rust source' ; * => 'unknown' }",
            ExprContext::Value,
        )
        .unwrap();
        assert_eq!(v.as_scalar(), "unknown");
    }

    #[test]
    fn case_expr_no_match() {
        let mut env = Env::new();
        env.let_set("f", Value::scalar("nope"));
        let err = eval_str(
            &env,
            "case $f { *.c => 'C' ; *.rs => 'Rust' }",
            ExprContext::Value,
        )
        .unwrap_err();
        assert_eq!(err.kind, EvalErrorKind::NoCaseMatch);
    }

    #[test]
    fn varnosplit_joins() {
        let mut env = Env::new();
        env.let_set("args", Value::list(alloc::vec![
            "a".to_string(),
            "b c".to_string(),
            "d".to_string(),
        ]));
        let v = eval_str(&env, "$\"args", ExprContext::Value).unwrap();
        assert_eq!(v.0.len(), 1);
        assert_eq!(v.0[0], "a b c d");
    }
}
