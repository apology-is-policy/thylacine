// libutopia::eval::stmt -- statement evaluation.
//
// === Scope at U-6b ===
//
// Walks a `Statement` (or a `Vec<Statement>` body) and executes it
// against an `Env`. Statement evaluation is mutating (assignment +
// fn registration + $status updates), unlike expression evaluation
// which is pure with respect to `Env`.
//
// Implemented statement kinds:
//   - Pipeline (single-element; supports CommandKind::Simple with
//     argv[0] resolving to a defined fn, CommandKind::BraceBlock,
//     CommandKind::Arith; Subshell + external Simple + multi-element
//     pipeline -> NotImplemented)
//   - Let / Assign / FnDecl
//   - Return / Break / Continue
//   - If / While / For
//   - Case (statement form)
//   - Try / Catch
//   - Trace
//   - OnNote / MaskNote
//
// Deferred (return NotImplemented at v1.0):
//   - External command spawn (SimpleCommand whose argv[0] is not a
//     defined fn or built-in) -- U-6c
//   - Multi-element Pipeline (pipes) -- U-6d
//   - Redirects -- U-6d
//   - Background (&) -- U-7
//   - Subshell ( cmds ) (fork) -- U-6c
//
// === StatementFlow ===
//
// Statement evaluation produces a `StatementFlow` value that the
// caller (`eval_block`) interprets:
//
//   Normal       -- continue to the next statement
//   Return       -- exit the enclosing function; $status carries the
//                   exit code (set by `return expr` or by the last
//                   command)
//   Break        -- exit the enclosing loop
//   Continue     -- skip to the next iteration of the enclosing loop
//
// `Return` is also raised by `eval_block` when implicit-fail fires
// (a non-zero $status in script mode without try-suppression).
//
// === Implicit-fail (scripture 8.1 + 8.3 + 8.9) ===
//
// In script mode (`env.interactive == false`) and outside a `try`
// block (`env.implicit_fail_suppressed == 0`), a non-zero $status
// after a statement causes the enclosing function (or script) to
// return with that $status. This is `set -e`'s intent done correctly
// per scripture. Interactive mode (`env.interactive == true`)
// suppresses this -- typing a command that fails at the REPL just
// sets $status and prints a fresh prompt.
//
// The `?` postfix on a command (`build?` per scripture 8.2) forces
// fail-propagate regardless of mode -- even at the interactive
// prompt, a `?`-marked command's non-zero exit triggers Return.
//
// === Function-call semantics ===
//
// `invoke_function` pushes a new scope frame, binds $0 = name +
// $1..$N positional + $* full-args-list + any FnDecl::args names,
// runs the body via `eval_block`, then pops. Per rc convention:
//
//   - A `Return` inside the function body normalizes to `Normal` at
//     the call site (the function returned).
//   - An unconsumed `Break` or `Continue` across the function
//     boundary likewise normalizes to `Normal` (no enclosing loop
//     to consume them).

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use crate::parser::ast::{
    AssignStmt, CaseStmt, Command, CommandKind, Expr, FnDecl, ForStmt, IfStmt, LetStmt,
    MaskNoteStmt, OnNoteStmt, Pipeline, Script, Statement, StatementKind, TraceStmt, TryStmt,
    WhileStmt, Word,
};
use crate::parser::token::{DqPart, Token, TokenKind};

use super::env::Env;
use super::error::{EvalError, EvalErrorKind, EvalResult};
use super::expr::eval_expr;
use super::glob;
use super::value::Value;

/// The result of evaluating one statement (or one block).
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum StatementFlow {
    /// Execution continues normally.
    Normal,
    /// Exit the enclosing function. $status carries the exit code.
    Return,
    /// Exit the enclosing loop.
    Break,
    /// Skip to the next iteration of the enclosing loop.
    Continue,
}

/// Walk a parsed Script. Equivalent to `eval_block(env,
/// &script.statements)`.
pub fn eval_script(env: &mut Env, script: &Script) -> EvalResult<StatementFlow> {
    eval_block(env, &script.statements)
}

/// Walk a sequence of statements (a function body, a brace-block,
/// the top-level of a script). Short-circuits on Return/Break/
/// Continue. After each statement that returned `Normal`, checks
/// for fail-propagate conditions (the `?` postfix on a Pipeline OR
/// implicit-fail in script mode without try-suppression) and
/// converts to `Return` if triggered.
pub fn eval_block(env: &mut Env, stmts: &[Statement]) -> EvalResult<StatementFlow> {
    for stmt in stmts {
        let flow = eval_statement(env, stmt)?;
        match flow {
            StatementFlow::Normal => {
                if env.status() != 0 && should_propagate_failure(env, stmt) {
                    return Ok(StatementFlow::Return);
                }
            }
            other => return Ok(other),
        }
    }
    Ok(StatementFlow::Normal)
}

/// Evaluate a single statement. Per-StatementKind dispatch.
pub fn eval_statement(env: &mut Env, stmt: &Statement) -> EvalResult<StatementFlow> {
    match &stmt.kind {
        StatementKind::Pipeline(p) => eval_pipeline(env, p),
        StatementKind::Let(let_stmt) => eval_let(env, let_stmt),
        StatementKind::Assign(asn_stmt) => eval_assign(env, asn_stmt),
        StatementKind::FnDecl(decl) => eval_fn_decl(env, decl),
        StatementKind::Return(opt_expr) => eval_return(env, opt_expr.as_ref()),
        StatementKind::Break => Ok(StatementFlow::Break),
        StatementKind::Continue => Ok(StatementFlow::Continue),
        StatementKind::If(if_stmt) => eval_if(env, if_stmt),
        StatementKind::While(while_stmt) => eval_while(env, while_stmt),
        StatementKind::For(for_stmt) => eval_for(env, for_stmt),
        StatementKind::Case(case_stmt) => eval_case_stmt(env, case_stmt),
        StatementKind::Try(try_stmt) => eval_try(env, try_stmt),
        StatementKind::Trace(trace_stmt) => eval_trace(env, trace_stmt),
        StatementKind::OnNote(on_note) => eval_on_note(env, on_note),
        StatementKind::MaskNote(mask_note) => eval_mask_note(env, mask_note),
    }
}

// ---------------------------------------------------------------------
// Implicit-fail bookkeeping
// ---------------------------------------------------------------------

/// Whether a non-zero $status after `stmt` should propagate as
/// Return. True if:
///   - the statement is a Pipeline whose first element has the `?`
///     postfix set (scripture 8.2 -- visible fail-propagate), OR
///   - the env is in script mode (`!interactive`) AND not inside a
///     try-block (`implicit_fail_suppressed == 0`) (scripture 8.3 +
///     8.9 -- implicit-fail).
fn should_propagate_failure(env: &Env, stmt: &Statement) -> bool {
    let explicit = matches!(
        &stmt.kind,
        StatementKind::Pipeline(p)
            if !p.elements.is_empty() && p.elements[0].command.fail_propagate
    );
    let implicit = !env.interactive && env.implicit_fail_suppressed == 0;
    explicit || implicit
}

// ---------------------------------------------------------------------
// Pipeline + commands
// ---------------------------------------------------------------------

fn eval_pipeline(env: &mut Env, p: &Pipeline) -> EvalResult<StatementFlow> {
    if p.background {
        return Err(EvalError::new(
            EvalErrorKind::NotImplemented("background pipeline (&)"),
            p.span,
        ));
    }
    if p.elements.is_empty() {
        // Empty pipeline shouldn't reach here -- parser rejects.
        return Err(EvalError::new(
            EvalErrorKind::Internal("empty pipeline"),
            p.span,
        ));
    }
    if p.elements.len() > 1 {
        return Err(EvalError::new(
            EvalErrorKind::NotImplemented("multi-element pipeline (|)"),
            p.span,
        ));
    }
    let elem = &p.elements[0];
    let cmd = &elem.command;
    if !cmd.redirects.is_empty() {
        return Err(EvalError::new(
            EvalErrorKind::NotImplemented("redirects"),
            elem.span,
        ));
    }
    eval_command(env, cmd)
}

fn eval_command(env: &mut Env, cmd: &Command) -> EvalResult<StatementFlow> {
    match &cmd.kind {
        CommandKind::Simple(simple) => {
            let argv = evaluate_argv(env, &simple.words)?;
            if argv.is_empty() {
                // All words evaluated to empty (e.g., $undef-only
                // line). rc convention: status 0.
                env.status_set(0);
                return Ok(StatementFlow::Normal);
            }
            // Look up argv[0] in fns; clone the FnDecl so we don't
            // hold an Env borrow across mutation.
            if let Some(decl) = env.fn_get(&argv[0]).cloned() {
                return invoke_function(env, &decl, &argv);
            }
            // Not a fn -- external spawn (U-6c) or built-in (U-6e).
            Err(EvalError::new(
                EvalErrorKind::NotImplemented(
                    "external command spawn or built-in (U-6c / U-6e)",
                ),
                cmd.span,
            ))
        }
        CommandKind::BraceBlock(stmts) => {
            // Scripture 5.6: brace blocks run in the current shell;
            // no fork, no scope push.
            eval_block(env, stmts)
        }
        CommandKind::Subshell(_) => Err(EvalError::new(
            EvalErrorKind::NotImplemented("subshell ( cmds )"),
            cmd.span,
        )),
        CommandKind::Arith(expr) => {
            // `(( body ))` -- evaluate as integer; status = 0 if
            // non-zero (truthy), 1 if zero (falsy). Matches rc /
            // bash convention.
            let v = eval_expr(env, expr)?;
            let i = v.as_int().unwrap_or(0);
            env.status_set(if i != 0 { 0 } else { 1 });
            Ok(StatementFlow::Normal)
        }
    }
}

// ---------------------------------------------------------------------
// Function calls
// ---------------------------------------------------------------------

fn invoke_function(
    env: &mut Env,
    decl: &FnDecl,
    argv: &[String],
) -> EvalResult<StatementFlow> {
    env.push_scope();
    // $0 = function name (argv[0]).
    env.let_set("0", Value::scalar(argv[0].clone()));
    // $1..$N = positional args (argv[1..]).
    for (i, arg) in argv.iter().skip(1).enumerate() {
        let mut name = String::new();
        let _ = write!(&mut name, "{}", i + 1);
        env.let_set(name, Value::scalar(arg.clone()));
    }
    // $* = full args list (excluding name).
    let star_args: Vec<String> = argv.iter().skip(1).cloned().collect();
    env.let_set("*", Value::list(star_args));
    // Named positional args (the `fn name a b c { ... }` form per
    // scripture 5.5): bind $a = argv[1], $b = argv[2], etc. Missing
    // args bind to empty.
    for (i, arg_name) in decl.args.iter().enumerate() {
        let val = argv
            .get(i + 1)
            .map(|s| Value::scalar(s.clone()))
            .unwrap_or_else(Value::empty);
        env.let_set(arg_name.clone(), val);
    }

    let result = eval_block(env, &decl.body);
    env.pop_scope();

    // Normalize control-flow that escaped the function body:
    //   Return -> Normal at the call site (function returned;
    //     $status already reflects the result).
    //   Break / Continue -> Normal at the call site (no enclosing
    //     loop to consume them; rc convention is to swallow at the
    //     function boundary).
    match result {
        Ok(StatementFlow::Return) => Ok(StatementFlow::Normal),
        Ok(StatementFlow::Break) | Ok(StatementFlow::Continue) => {
            Ok(StatementFlow::Normal)
        }
        other => other,
    }
}

// ---------------------------------------------------------------------
// Argv expansion (Word -> Vec<String>)
// ---------------------------------------------------------------------

/// Expand a sequence of Words into argv elements. Each Word's
/// `Value` contributes each of its elements as one argv item (rc /
/// scripture 6.3: a list interpolates as N separate words). The
/// `$"var` form is the only way to collapse a multi-element value
/// into one argv element.
fn evaluate_argv(env: &Env, words: &[Word]) -> EvalResult<Vec<String>> {
    let mut argv = Vec::new();
    for w in words {
        let v = eval_word(env, w)?;
        argv.extend(v.0);
    }
    Ok(argv)
}

/// Evaluate a single Word. `Word::Single(tok)` evaluates the token;
/// `Word::Concat(toks)` evaluates each token then applies rc-style
/// cross-product concatenation (matches `eval_expr`'s Concat
/// semantics).
fn eval_word(env: &Env, word: &Word) -> EvalResult<Value> {
    match word {
        Word::Single(tok) => eval_value_token(env, tok),
        Word::Concat(toks) => {
            if toks.is_empty() {
                return Err(EvalError::new(
                    EvalErrorKind::Internal("empty Word::Concat"),
                    word.span(),
                ));
            }
            let mut acc = eval_value_token(env, &toks[0])?;
            for tok in &toks[1..] {
                let next = eval_value_token(env, tok)?;
                let mut new_acc = Vec::with_capacity(acc.len() * next.len());
                for a in &acc.0 {
                    for b in &next.0 {
                        let mut s = String::with_capacity(a.len() + b.len());
                        s.push_str(a);
                        s.push_str(b);
                        new_acc.push(s);
                    }
                }
                acc = Value::list(new_acc);
            }
            Ok(acc)
        }
    }
}

/// Resolve a single value-producing Token to a Value. Mirrors the
/// atom handling in `expr::eval_expr` for the token-shape AST nodes
/// the Word layer carries.
fn eval_value_token(env: &Env, tok: &Token) -> EvalResult<Value> {
    match &tok.kind {
        TokenKind::Word(s) => Ok(Value::scalar(s.clone())),
        TokenKind::SingleQuoted(s) => Ok(Value::scalar(s.clone())),
        TokenKind::DoubleQuoted(parts) => eval_dq_in_word(env, parts, tok),
        TokenKind::Var(name) => Ok(env.get(name)),
        TokenKind::VarLen(name) => {
            let mut s = String::new();
            let _ = write!(&mut s, "{}", env.get(name).len());
            Ok(Value::scalar(s))
        }
        TokenKind::VarNoSplit(name) => {
            let v = env.get(name);
            Ok(Value::scalar(v.joined(" ")))
        }
        TokenKind::Subst(_) => Err(EvalError::new(
            EvalErrorKind::NotImplemented("$(cmd) substitution"),
            tok.span,
        )),
        TokenKind::Backtick(_) => Err(EvalError::new(
            EvalErrorKind::NotImplemented("`{cmd} substitution"),
            tok.span,
        )),
        TokenKind::ProcSubIn(_) => Err(EvalError::new(
            EvalErrorKind::NotImplemented("<(cmd) process substitution"),
            tok.span,
        )),
        TokenKind::ProcSubOut(_) => Err(EvalError::new(
            EvalErrorKind::NotImplemented(">(cmd) process substitution"),
            tok.span,
        )),
        _ => Err(EvalError::new(
            EvalErrorKind::Internal("non-value token in Word"),
            tok.span,
        )),
    }
}

/// DQ interpolation duplicating expr.rs::eval_dq's logic but
/// scoped to a Token's span for error reporting.
fn eval_dq_in_word(env: &Env, parts: &[DqPart], tok: &Token) -> EvalResult<Value> {
    let mut out = String::new();
    for part in parts {
        match part {
            DqPart::Literal(s) => out.push_str(s),
            DqPart::Var(name) => out.push_str(&env.get(name).as_scalar()),
            DqPart::VarLen(name) => {
                let _ = write!(out, "{}", env.get(name).len());
            }
            DqPart::Subst(_) => {
                return Err(EvalError::new(
                    EvalErrorKind::NotImplemented("$(cmd) substitution"),
                    tok.span,
                ));
            }
        }
    }
    Ok(Value::scalar(out))
}

// ---------------------------------------------------------------------
// Assignment + declarations
// ---------------------------------------------------------------------

fn eval_let(env: &mut Env, stmt: &LetStmt) -> EvalResult<StatementFlow> {
    let v = eval_expr(env, &stmt.value)?;
    env.let_set(stmt.name.clone(), v);
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_assign(env: &mut Env, stmt: &AssignStmt) -> EvalResult<StatementFlow> {
    let v = eval_expr(env, &stmt.value)?;
    env.assign(stmt.name.clone(), v);
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_fn_decl(env: &mut Env, decl: &FnDecl) -> EvalResult<StatementFlow> {
    env.fn_set(decl.clone());
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_return(env: &mut Env, expr: Option<&Expr>) -> EvalResult<StatementFlow> {
    if let Some(e) = expr {
        let v = eval_expr(env, e)?;
        // Map the returned value to $status: try as integer, fall
        // back to 0 on non-numeric. Scripture is silent on the
        // exact mapping; matches rc convention.
        let code = v.as_int().unwrap_or(0);
        env.status_set(code as i32);
    }
    Ok(StatementFlow::Return)
}

// ---------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------

fn eval_if(env: &mut Env, stmt: &IfStmt) -> EvalResult<StatementFlow> {
    let cond_v = eval_expr(env, &stmt.cond)?;
    if cond_v.is_truthy() {
        return eval_block(env, &stmt.then_branch);
    }
    for (elif_cond, elif_body) in &stmt.elif_branches {
        let v = eval_expr(env, elif_cond)?;
        if v.is_truthy() {
            return eval_block(env, elif_body);
        }
    }
    if let Some(else_body) = &stmt.else_branch {
        return eval_block(env, else_body);
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_while(env: &mut Env, stmt: &WhileStmt) -> EvalResult<StatementFlow> {
    loop {
        let cond_v = eval_expr(env, &stmt.cond)?;
        if !cond_v.is_truthy() {
            break;
        }
        match eval_block(env, &stmt.body)? {
            StatementFlow::Normal | StatementFlow::Continue => continue,
            StatementFlow::Break => break,
            StatementFlow::Return => return Ok(StatementFlow::Return),
        }
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_for(env: &mut Env, stmt: &ForStmt) -> EvalResult<StatementFlow> {
    let list_v = eval_expr(env, &stmt.list_expr)?;
    // Clone elements out so the loop can mutate env (env.let_set
    // needs &mut env, but we'd otherwise hold an immutable borrow
    // via list_v.0).
    let elements: Vec<String> = list_v.0.clone();
    for el in elements {
        env.let_set(stmt.var_name.clone(), Value::scalar(el));
        match eval_block(env, &stmt.body)? {
            StatementFlow::Normal | StatementFlow::Continue => continue,
            StatementFlow::Break => break,
            StatementFlow::Return => return Ok(StatementFlow::Return),
        }
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_case_stmt(env: &mut Env, stmt: &CaseStmt) -> EvalResult<StatementFlow> {
    let scrutinee = eval_expr(env, &stmt.scrutinee)?;
    let scrutinee_str = scrutinee.as_scalar();
    for arm in &stmt.arms {
        for pat in &arm.patterns {
            let pv = eval_expr(env, pat)?;
            let pat_str = pv.as_scalar();
            if glob::matches(&pat_str, &scrutinee_str) {
                return eval_statement(env, &arm.body);
            }
        }
    }
    // Statement-form case is permissive on no-match: status 0, no
    // error. (Expression-form case errors via NoCaseMatch; that's
    // in expr.rs.)
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

// ---------------------------------------------------------------------
// Try / Catch
// ---------------------------------------------------------------------

fn eval_try(env: &mut Env, stmt: &TryStmt) -> EvalResult<StatementFlow> {
    env.implicit_fail_suppressed = env.implicit_fail_suppressed.saturating_add(1);
    let body_result = eval_block(env, &stmt.body);
    env.implicit_fail_suppressed = env.implicit_fail_suppressed.saturating_sub(1);

    let triggered = match &body_result {
        Ok(_) => env.status() != 0,
        Err(_) => true,
    };

    if let Err(e) = &body_result {
        // The body raised an eval error -- expose it via $errstr
        // and set $status to 1 (rc convention for catch-handled
        // failure). The error itself is NOT propagated; the catch
        // block handles it.
        let mut s = String::new();
        let _ = write!(&mut s, "{}", e.kind);
        env.errstr_set(s);
        env.status_set(1);
    }

    if triggered {
        return eval_block(env, &stmt.catch);
    }
    // Body succeeded -- propagate its flow as-is (including any
    // Return/Break/Continue that escaped the body).
    body_result
}

// ---------------------------------------------------------------------
// Trace
// ---------------------------------------------------------------------

fn eval_trace(env: &mut Env, stmt: &TraceStmt) -> EvalResult<StatementFlow> {
    env.trace_depth = env.trace_depth.saturating_add(1);
    let result = eval_block(env, &stmt.body);
    env.trace_depth = env.trace_depth.saturating_sub(1);
    result
}

// ---------------------------------------------------------------------
// Note handlers
// ---------------------------------------------------------------------

fn eval_on_note(env: &mut Env, stmt: &OnNoteStmt) -> EvalResult<StatementFlow> {
    let name = eval_expr(env, &stmt.note_name)?.as_scalar();
    env.note_handler_set(name, stmt.body.clone());
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

fn eval_mask_note(env: &mut Env, stmt: &MaskNoteStmt) -> EvalResult<StatementFlow> {
    // U-6b: mask is a pass-through that records intent via the body
    // executing. Actual note-delivery deferral wires at U-7 alongside
    // the note-fd polling main loop. The note_name expression is
    // still evaluated so any side-effects (var lookups, etc.) fire.
    let _name = eval_expr(env, &stmt.note_name)?;
    eval_block(env, &stmt.body)
}

// ---------------------------------------------------------------------
// Convenience: parse + eval a source string in one call.
// ---------------------------------------------------------------------

/// Parse + evaluate a source string. Convenience wrapper for tests
/// and for the U-6g REPL's read-parse-eval cycle. Parse errors are
/// surfaced as an `EvalError` with `EvalErrorKind::Internal` and a
/// `Span` derived from the parse failure -- callers that need finer
/// granularity should call `parse` + `eval_script` directly.
pub fn eval_source(env: &mut Env, src: &str) -> EvalResult<StatementFlow> {
    use crate::parser::parse;
    let script = parse(src).map_err(|e| {
        // ParseErrorKind has no Display impl; use Debug to keep
        // the diagnostic readable in $errstr. EvalError::Internal
        // takes a &'static str, so the user-facing detail goes via
        // errstr; the static tag identifies the source.
        let mut msg = String::new();
        let _ = write!(&mut msg, "parse error: {:?}", e.kind);
        env.errstr_set(msg);
        EvalError::new(EvalErrorKind::Internal("parse failed"), e.span)
    })?;
    eval_script(env, &script)
}
