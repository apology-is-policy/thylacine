// libutopia::eval -- the rc-shape evaluator for the `ut` shell.
//
// === Position in the U-6 arc ===
//
// Per UTOPIA-SHELL-DESIGN.md section 19 + the U-6 implementation
// sketch in memory/project_next_session.md, U-6 is the BIGGEST
// single chunk in the U-* arc. It glues the line editor (U-4) and
// parser (U-5) into a working interactive shell. The plan splits
// U-6 into sub-chunks:
//
//   U-6a (this commit)  evaluator scaffold + value model +
//                       expression eval for the no-side-effect
//                       subset
//   U-6b                statement eval (Pipeline / Let / Assign /
//                       Return / If / While / For / Case / Try /
//                       Trace / OnNote / MaskNote -- still
//                       without external spawn)
//   U-6c                external command spawn (SimpleCommand ->
//                       libthyla-rs t::process::Command::spawn)
//   U-6d                pipes + redirection
//   U-6e                built-ins (cd / exit / set / source / fn /
//                       alias / eval / type)
//   U-6f                substitution ($(cmd) / `{cmd} / proc-sub /
//                       heredoc) -- requires U-6c
//   U-6g                poll() main loop + line-editor integration
//                       (the U-3 ut binary grows a REPL)
//   U-6-test            cumulative integration smoke
//
// === U-6a's surface ===
//
//   pub struct Value(pub Vec<String>)
//     -- the unified-list value model (scripture 4.1 + 4.2 + 6.3).
//        A "scalar" is a one-element list.
//
//   pub struct Env
//     -- variable scope stack + function table + $status + $errstr
//        + $cwd. push_scope/pop_scope for U-6b function calls.
//
//   pub struct EvalError + pub enum EvalErrorKind + EvalResult
//     -- error taxonomy at U-6a (NonNumeric / DivByZero / Overflow /
//        InvalidShift / NoCaseMatch / NotImplemented / Internal).
//
//   pub fn eval_expr(env: &Env, expr: &Expr) -> EvalResult<Value>
//     -- the recursive expression walker. Covers every ExprKind
//        variant EXCEPT Subst/Backtick/ProcSub/Match::Regex
//        (NotImplemented at v1.0).
//
//   pub mod glob (re-exported as eval::glob)
//     -- the rc/POSIX-shape pattern matcher. Used by case-as-
//        expression and the `matches` operator. NO filesystem
//        walks (that lands at U-6c/e for argv expansion).
//
// === What lands LATER ===
//
//   - Statement evaluation (let/assign/control-flow): U-6b.
//   - External command spawn: U-6c.
//   - $(cmd) substitution: U-6f.
//   - Job control hooks (background, jobs/fg/bg): U-7.
//   - PTY raw-mode + line-editor I/O: U-PTY.
//
// === Reference doc ===
//
// docs/reference/94-utopia-eval.md (created at U-6a) is the binding
// reference for this module. Per-sub-chunk extensions append rows
// to its Status section.

pub mod env;
pub mod error;
pub mod expr;
pub mod glob;
pub mod stmt;
pub mod value;

pub use env::Env;
pub use error::{EvalError, EvalErrorKind, EvalResult};
pub use expr::eval_expr;
pub use stmt::{
    aggregate_pipefail, eval_block, eval_script, eval_source, eval_statement, StatementFlow,
};
pub use value::Value;
