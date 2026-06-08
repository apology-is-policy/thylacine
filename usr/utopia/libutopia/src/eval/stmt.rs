// libutopia::eval::stmt -- statement evaluation.
//
// === Scope at U-6d-a ===
//
// Walks a `Statement` (or a `Vec<Statement>` body) and executes it
// against an `Env`. Statement evaluation is mutating (assignment +
// fn registration + $status updates), unlike expression evaluation
// which is pure with respect to `Env`.
//
// Implemented statement kinds:
//   - Pipeline:
//       * single-element -- CommandKind::Simple with argv[0]
//         resolving to a defined fn OR an external binary (U-6c),
//         CommandKind::BraceBlock, CommandKind::Arith.
//       * multi-element (`a | b | c`) (U-6d-a) -- every element must
//         be a redirect-free external Simple command; pipe-connected;
//         pipefail-aggregated. See `exec_pipeline`.
//   - Let / Assign / FnDecl
//   - Return / Break / Continue
//   - If / While / For
//   - Case (statement form)
//   - Try / Catch
//   - Trace (echoes argv to the trace sink when trace_depth > 0)
//   - OnNote / MaskNote
//
// Deferred (return NotImplemented at v1.0):
//   - Redirects (`>` `<` `>>` `<<`) -- U-6d-b
//   - Background (&) -- U-7
//   - Subshell ( cmds ) (fork) -- U-6f or later (needs a fork
//     surface that re-runs the parsed body in the child; the
//     spawn-then-exec model in libthyla-rs::process doesn't fit)
//   - In-process pipeline elements (fn / BraceBlock / Arith /
//     Subshell as a pipeline stage) -- they have no fd to wire to a
//     pipe without fork; U-6f or later
//   - Filesystem glob expansion on argv (e.g., `cat *.c`) -- U-6e
//     (the glob module in this crate already does pattern match;
//     fs-walk needs a ReadDir surface in libthyla-rs that v1.0
//     doesn't yet expose)
//
// === External command spawn (U-6c) ===
//
// When a SimpleCommand's argv[0] does not resolve to a defined fn,
// `exec_external` spawns the binary via libthyla_rs::process. The
// kernel does name lookup (single component, no `/`) against
// devramfs OR the pivoted root. Stdio inherits (pipes wire at
// U-6d). The child is reaped synchronously via Child::wait; the
// resulting raw exit status becomes $status.
//
// Spawn failure semantics: any error (name too long, name with
// `/`, NotFound, kernel rejection) sets $status = 127 (bash
// "command not found" convention) + populates $errstr with a
// description. The eval call itself returns Ok(Normal) so
// implicit-fail discipline (see below) can take it from there --
// a spawn failure is a runtime non-zero exit, not a parse-/eval-
// time error.
//
// === Trace echo (U-6c closes U-6b deferral) ===
//
// When `env.trace_depth > 0`, exec_external echoes `+ argv[0]
// argv[1] ...\n` to the trace sink BEFORE the spawn. The sink at
// v1.0 is the kernel UART (via t_putstr), matching the rest of the
// shell's diagnostic output at this stage. When fd 2 (stderr) is
// wired through line discipline (U-PTY + U-6g), the sink should
// switch to fd 2. Quoting of args with embedded whitespace is a
// v1.x refinement; bash's set -x style quoting is the target.
//
// === Stdio model at U-6c ===
//
// The shell at v1.0 has no terminal-backed fd 0/1/2 (those will
// land with U-PTY + U-6g; until then ut/u-test/joey all write via
// SYS_PUTS direct to the kernel UART). Stdio::Inherit would tell
// the kernel to install the parent's fd-0/1/2 into the child's
// handle table, but the parent has none, and SYS_SPAWN_FULL_ARGV
// would reject the spawn. Until PTY lands, exec_external uses the
// established v1.0 convention (matches alloc-smoke + u-test's
// flow_process_pipe): Stdio::Piped on all three slots followed by
// an immediate drop of every parent-side pipe end. The child has
// pipes installed at 0/1/2 (so the kernel's fd_count=3 path
// succeeds), but native binaries write via SYS_PUTS and don't
// read fd 0, so the pipes are functionally inert. When PTY lands,
// this whole block flips to Stdio::Inherit cleanly.
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
    MaskNoteStmt, OnNoteStmt, Pipeline, Redirect, RedirectKind, Script, Statement,
    StatementKind, TraceStmt, TryStmt, WhileStmt, Word,
};
use crate::parser::token::{DqPart, Token, TokenKind};
use crate::parser::Span;

use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::Write as IoWrite;
use libthyla_rs::process::{pipe, Stdio};
use libthyla_rs::{t_putstr, t_wait_pid};

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
        // Multi-element pipeline (U-6d-a).
        return exec_pipeline(env, p);
    }
    let elem = &p.elements[0];
    let cmd = &elem.command;
    if cmd.redirects.is_empty() {
        return eval_command(env, cmd);
    }
    // Redirects present (U-6d-b). Only an external Simple command has
    // fds to redirect at v1.0; a fn / brace-block / arith / subshell
    // runs IN-PROCESS in this shell with no stdin/stdout fd to retarget
    // without fork (same constraint as in-process pipeline stages).
    match &cmd.kind {
        CommandKind::Simple(simple) => {
            let argv = evaluate_argv(env, &simple.words)?;
            if argv.is_empty() {
                // Bare redirect (`> file`) -- no command: the redirect
                // targets are still opened/created/truncated (the side
                // effect), then nothing runs. status 0 (rc convention).
                return exec_bare_redirect(env, &cmd.redirects);
            }
            if env.fn_get(&argv[0]).is_some() {
                return Err(EvalError::new(
                    EvalErrorKind::NotImplemented("redirect on function (U-6f)"),
                    cmd.span,
                ));
            }
            exec_external_redirected(env, &argv, &cmd.redirects, cmd.span)
        }
        _ => Err(EvalError::new(
            EvalErrorKind::NotImplemented("redirect on in-process command (U-6f)"),
            cmd.span,
        )),
    }
}

// ---------------------------------------------------------------------
// Redirects (U-6d-b)
// ---------------------------------------------------------------------
//
// A command's redirects retarget its stdin (fd 0) and/or stdout (fd 1).
// The parser models four kinds (no fd-numbered `2>` at v1.0):
//   - `< file`   (RedirectKind::Stdin)   -- fd 0 from an existing file.
//   - `> file`   (RedirectKind::Stdout)  -- fd 1 to a file (create-or-
//                                           open + truncate).
//   - `>> file`  (RedirectKind::Append)  -- fd 1 to a file (create-or-
//                                           open + append).
//   - `<< TAG`   (RedirectKind::Heredoc) -- fd 0 from an inline body fed
//                                           through a kernel pipe.
//
// Source order matters only as last-wins-per-fd: two stdin redirects
// (or two stdout redirects) keep the last; the earlier file is opened
// (its side effect, e.g. `> a > b` truncates a) then closed when its
// `Stdio::File` is dropped on reassignment.
//
// The shell OPENS the target and HANDS the fd to the child (Stdio::File);
// it does no bulk I/O itself (so Loom -- the async 9P data path -- is not
// the right layer here; that is the child's concern). The one exception
// is the heredoc body, which the shell writes into a pipe whose read end
// the child gets as fd 0; a pipe is a kernel-local object, not a 9P fid,
// so that write is a plain SYS_WRITE.

/// The resolved stdio overrides a command's redirects impose. `None`
/// means "use the caller's default for this slot" (Piped for a lone
/// command; the pipe end for a pipeline element).
struct ResolvedStdio {
    stdin: Option<Stdio>,
    stdout: Option<Stdio>,
    /// For a heredoc: the pipe WRITE end + the rendered body. The body
    /// is fed AFTER the child spawns (so it is draining fd 0), then the
    /// write end is dropped to deliver EOF.
    heredoc: Option<(File, String)>,
}

/// A redirect-resolution failure. `Eval` propagates as an eval error
/// (NotImplemented / Internal); `Runtime` is a per-command runtime
/// failure (target open failed, ambiguous target) reflected via
/// `$status` + `$errstr`, not an eval error.
enum RedirError {
    Eval(EvalError),
    Runtime(String),
}

/// Open every redirect target in source order, producing the stdio
/// overrides. Files are opened UP FRONT so a failure aborts before any
/// spawn (consistent with the pipeline's up-front validation).
fn resolve_redirects(env: &Env, redirects: &[Redirect]) -> Result<ResolvedStdio, RedirError> {
    let mut out = ResolvedStdio {
        stdin: None,
        stdout: None,
        heredoc: None,
    };
    for r in redirects {
        match &r.kind {
            RedirectKind::Stdin => {
                let path = redir_target_path(env, r)?;
                let f = File::open(&path)
                    .map_err(|e| RedirError::Runtime(redir_open_err("<", &path, e)))?;
                // A `<` after a `<<` (or vice versa) wins; drop the
                // earlier heredoc pipe so it doesn't leak.
                out.heredoc = None;
                out.stdin = Some(Stdio::File(f));
            }
            RedirectKind::Stdout => {
                let path = redir_target_path(env, r)?;
                let f = OpenOptions::new()
                    .write(true)
                    .create(true)
                    .truncate(true)
                    .open(&path)
                    .map_err(|e| RedirError::Runtime(redir_open_err(">", &path, e)))?;
                out.stdout = Some(Stdio::File(f));
            }
            RedirectKind::Append => {
                let path = redir_target_path(env, r)?;
                let f = OpenOptions::new()
                    .write(true)
                    .create(true)
                    .append(true)
                    .open(&path)
                    .map_err(|e| RedirError::Runtime(redir_open_err(">>", &path, e)))?;
                out.stdout = Some(Stdio::File(f));
            }
            RedirectKind::Heredoc { body, .. } => {
                // `strip_tabs` was applied by the lexer when it captured
                // the body, and `interp` is reflected in the DqPart
                // structure (non-interp bodies are a single Literal), so
                // rendering just walks the parts.
                let rendered =
                    render_heredoc_body(env, body, r.span).map_err(RedirError::Eval)?;
                let (rd, wr) = pipe().map_err(|e| {
                    let mut s = String::new();
                    let _ = write!(&mut s, "heredoc pipe() failed: {:?}", e);
                    RedirError::Runtime(s)
                })?;
                out.heredoc = Some((wr, rendered));
                out.stdin = Some(Stdio::File(rd));
            }
        }
    }
    Ok(out)
}

/// Evaluate a redirect's target Word to a single path. A target that
/// expands to zero or many words is an "ambiguous redirect" (rc / bash
/// behaviour).
fn redir_target_path(env: &Env, r: &Redirect) -> Result<String, RedirError> {
    let word = r.target.as_ref().ok_or_else(|| {
        RedirError::Eval(EvalError::new(
            EvalErrorKind::Internal("redirect missing target"),
            r.span,
        ))
    })?;
    let v = eval_word(env, word).map_err(RedirError::Eval)?;
    if v.0.len() != 1 {
        return Err(RedirError::Runtime(String::from(
            "ambiguous redirect (target must expand to exactly one word)",
        )));
    }
    Ok(v.0[0].clone())
}

/// `<op> <path>: open failed: <Error>` -- the `$errstr` for a failed
/// redirect-target open.
fn redir_open_err(op: &str, path: &str, e: impl core::fmt::Debug) -> String {
    let mut s = String::new();
    let _ = write!(&mut s, "{} {}: open failed: {:?}", op, path, e);
    s
}

/// Render a heredoc body (`Vec<DqPart>`) to bytes. Mirrors
/// `eval_dq_in_word`: literals copy through, `$var` interpolates (only
/// present when the heredoc was interpolating), `$#var` -> length,
/// `$(cmd)` is NotImplemented (U-6f).
fn render_heredoc_body(env: &Env, parts: &[DqPart], span: Span) -> EvalResult<String> {
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
                    EvalErrorKind::NotImplemented("$(cmd) in heredoc (U-6f)"),
                    span,
                ));
            }
        }
    }
    Ok(out)
}

/// Best-effort write of `buf` to `f` (a pipe write end). Partial writes
/// loop; an error or zero-progress write stops (the child closed its
/// read end -- a normal early exit, not our problem).
fn feed_pipe(f: &mut File, mut buf: &[u8]) {
    while !buf.is_empty() {
        match f.write(buf) {
            Ok(0) => break,
            Ok(n) => buf = &buf[n..],
            Err(_) => break,
        }
    }
}

/// Spawn an external command with redirects applied. Non-redirected
/// slots take the U-6c v1.0 convention (`Stdio::Piped` + immediate
/// parent-side drop -- the shell has no terminal-backed fd 0/1/2 yet).
fn exec_external_redirected(
    env: &mut Env,
    argv: &[String],
    redirects: &[Redirect],
    _span: Span,
) -> EvalResult<StatementFlow> {
    let ResolvedStdio {
        stdin,
        stdout,
        heredoc,
    } = match resolve_redirects(env, redirects) {
        Ok(r) => r,
        Err(RedirError::Eval(e)) => return Err(e),
        Err(RedirError::Runtime(msg)) => {
            env.errstr_set(msg);
            env.status_set(1);
            return Ok(StatementFlow::Normal);
        }
    };

    if env.trace_depth > 0 {
        trace_echo(argv);
    }

    let mut spawn_cmd = libthyla_rs::process::Command::new(argv[0].clone());
    if argv.len() > 1 {
        spawn_cmd.args(argv[1..].iter().cloned());
    }
    match stdin {
        Some(s) => {
            spawn_cmd.stdin(s);
        }
        None => {
            spawn_cmd.stdin(Stdio::Piped);
        }
    }
    match stdout {
        Some(s) => {
            spawn_cmd.stdout(s);
        }
        None => {
            spawn_cmd.stdout(Stdio::Piped);
        }
    }
    spawn_cmd.stderr(Stdio::Piped);

    match spawn_cmd.spawn() {
        Ok(mut child) => {
            // Drop any Piped parent ends immediately (File ends were
            // consumed inside spawn). For a heredoc-driven stdin the
            // child holds the pipe read end; feed the body now that it
            // is draining, then drop the write end to deliver EOF.
            drop(child.stdin.take());
            drop(child.stdout.take());
            drop(child.stderr.take());
            if let Some((mut wr, body)) = heredoc {
                feed_pipe(&mut wr, body.as_bytes());
                drop(wr);
            }
            match child.wait() {
                Ok(status) => {
                    env.status_set(status.raw());
                    Ok(StatementFlow::Normal)
                }
                Err(e) => {
                    let mut s = String::new();
                    let _ = write!(&mut s, "wait failed: {:?}", e);
                    env.errstr_set(s);
                    env.status_set(1);
                    Ok(StatementFlow::Normal)
                }
            }
        }
        Err(e) => {
            let mut s = String::new();
            let _ = write!(&mut s, "spawn failed: {:?}", e);
            env.errstr_set(s);
            env.status_set(127);
            Ok(StatementFlow::Normal)
        }
    }
}

/// A bare redirect with no command (`> file`): resolve the redirects
/// (the open/create/truncate side effect happens), then drop everything
/// and report success. A heredoc with no command discards its body.
fn exec_bare_redirect(env: &mut Env, redirects: &[Redirect]) -> EvalResult<StatementFlow> {
    match resolve_redirects(env, redirects) {
        Ok(_resolved) => {
            // _resolved drops here: opened files close, heredoc pipe ends
            // close (body never written -- discarded, matching bash).
            env.status_set(0);
            Ok(StatementFlow::Normal)
        }
        Err(RedirError::Eval(e)) => Err(e),
        Err(RedirError::Runtime(msg)) => {
            env.errstr_set(msg);
            env.status_set(1);
            Ok(StatementFlow::Normal)
        }
    }
}

// ---------------------------------------------------------------------
// Multi-element pipeline (U-6d-a)
// ---------------------------------------------------------------------
//
// `cmd1 | cmd2 | ... | cmdN` -- spawn all N elements concurrently,
// connect element i's stdout to element i+1's stdin via a kernel pipe,
// then reap all and aggregate the exit statuses per scripture 8.4
// (pipefail).
//
// === Element restriction at U-6d-a ===
//
// Every pipeline element must be a redirect-free EXTERNAL Simple
// command (argv[0] not a defined fn). The reasons:
//   - A fn / BraceBlock / Arith runs IN-PROCESS in this shell; it has
//     no stdin/stdout fd to wire to a pipe. rc/bash run these in a
//     forked subshell; libthyla-rs has spawn-then-exec, not fork, so
//     in-process pipeline elements are NotImplemented at v1.0.
//   - Subshell `( cmds )` likewise needs fork.
//   - Per-element redirects (`cmd > f | cmd2`) land at U-6d-b.
// Any of these -> NotImplemented with a specific tag; the whole
// pipeline aborts BEFORE any spawn (no half-spawned state).
//
// === Stdio wiring ===
//
// For N elements there are N-1 pipes. Pipe j connects element j
// (write end -> its stdout) to element j+1 (read end -> its stdin).
// The OUTER ends -- element 0's stdin, element N-1's stdout, and every
// element's stderr -- get the U-6c v1.0 convention (Stdio::Piped +
// immediate drop), because the shell has no terminal-backed fd 0/1/2
// to inherit yet (see exec_external's module note). When U-PTY lands,
// the outer stdin/stdout become Inherit.
//
// `Stdio::File(end)` hands a specific pipe end to a child slot: the
// kernel installs that end into the child's handle table at the slot
// index, then libthyla-rs drops the parent's copy after the spawn
// returns. So after element j spawns, the parent no longer holds
// element j's write end -- only the child does -- which is exactly
// what lets the downstream reader see EOF when the upstream exits.
//
// === Spawn-all-before-wait ===
//
// All N children are spawned before the first wait. If we waited after
// each spawn, a pipeline like `producer | consumer` would deadlock:
// producer fills the pipe buffer and blocks on write while we block
// waiting for it, never having spawned consumer to drain the pipe.
//
// === Reaping (wait-any + pid match) ===
//
// SYS_WAIT_PID reaps ANY of the caller's zombie children, not a
// specific pid. We spawn N, collect their pids, then wait N times and
// match each reaped pid back to its element index to record the right
// status. At v1.0 the shell has no background children (U-7), so the
// only outstanding children are this pipeline's -- exactly N waits
// reap them all. A reaped pid not in our set (impossible at v1.0) is
// ignored.
//
// === Pipefail (scripture 8.4) ===
//
// The pipeline's $status is the rightmost non-zero exit among elements
// NOT marked `?|` (tolerate_failure), or 0 if all (non-tolerated)
// elements succeeded. See `aggregate_pipefail`.
fn exec_pipeline(env: &mut Env, p: &Pipeline) -> EvalResult<StatementFlow> {
    let n = p.elements.len();

    // Validate + evaluate every element's argv AND resolve its redirects
    // up front. Any NotImplemented / empty-argv / redirect-open failure
    // aborts before a single spawn (no half-spawned state).
    let mut argvs: Vec<Vec<String>> = Vec::with_capacity(n);
    let mut redirs: Vec<ResolvedStdio> = Vec::with_capacity(n);
    for elem in &p.elements {
        let cmd = &elem.command;
        let simple = match &cmd.kind {
            CommandKind::Simple(s) => s,
            _ => {
                return Err(EvalError::new(
                    EvalErrorKind::NotImplemented("non-simple command in pipeline"),
                    cmd.span,
                ));
            }
        };
        let argv = evaluate_argv(env, &simple.words)?;
        if argv.is_empty() {
            return Err(EvalError::new(
                EvalErrorKind::Internal("empty argv in pipeline element"),
                cmd.span,
            ));
        }
        if env.fn_get(&argv[0]).is_some() {
            return Err(EvalError::new(
                EvalErrorKind::NotImplemented("function as pipeline element"),
                cmd.span,
            ));
        }
        // Per-element redirects (U-6d-b): they OVERRIDE the pipe wiring
        // for the redirected fd (e.g. `cmd > f | next` sends cmd's stdout
        // to f, not the pipe, so next reads EOF). A redirect-target open
        // failure aborts the whole pipeline before any spawn.
        let rr = match resolve_redirects(env, &cmd.redirects) {
            Ok(r) => r,
            Err(RedirError::Eval(e)) => return Err(e),
            Err(RedirError::Runtime(msg)) => {
                env.errstr_set(msg);
                env.status_set(1);
                return Ok(StatementFlow::Normal);
            }
        };
        argvs.push(argv);
        redirs.push(rr);
    }

    if env.trace_depth > 0 {
        for argv in &argvs {
            trace_echo(argv);
        }
    }

    // Allocate N-1 pipes. stdin_files[i] feeds element i's stdin (Some
    // for i in 1..n); stdout_files[i] is element i's stdout (Some for
    // i in 0..n-1). On a pipe() failure, the Files allocated so far
    // drop when this fn returns (the Vecs go out of scope).
    let mut stdin_files: Vec<Option<File>> = (0..n).map(|_| None).collect();
    let mut stdout_files: Vec<Option<File>> = (0..n).map(|_| None).collect();
    for j in 0..n - 1 {
        match pipe() {
            Ok((rd, wr)) => {
                stdout_files[j] = Some(wr);
                stdin_files[j + 1] = Some(rd);
            }
            Err(e) => {
                let mut s = String::new();
                let _ = write!(&mut s, "pipe() failed: {:?}", e);
                env.errstr_set(s);
                env.status_set(1);
                return Ok(StatementFlow::Normal);
            }
        }
    }

    // Spawn each element in order. Each spawn consumes its Stdio::File
    // ends (the kernel refcount-bumps them into the child; libthyla-rs
    // drops the parent copy on spawn return). On a mid-pipeline spawn
    // failure we stop spawning and still reap the children already
    // launched; their pipe ends + the un-spawned elements' ends drop
    // via the Vecs at fn return.
    let mut pids: Vec<i32> = Vec::with_capacity(n);
    let mut spawn_err: Option<&'static str> = None;
    // Heredoc write ends + bodies, fed after every child is spawned (so
    // each is draining its fd 0), then dropped to deliver EOF.
    let mut heredoc_writes: Vec<(File, String)> = Vec::new();
    for i in 0..n {
        let argv = &argvs[i];
        let mut spawn_cmd = libthyla_rs::process::Command::new(argv[0].clone());
        if argv.len() > 1 {
            spawn_cmd.args(argv[1..].iter().cloned());
        }
        // stdin: a per-element redirect (`< f` / `<<`) wins over the pipe
        // read end (which is dropped so the upstream sees the right EOF
        // behaviour). Otherwise the pipe read end (Some for i in 1..n) or
        // the outer Piped default (element 0).
        match redirs[i].stdin.take() {
            Some(s) => {
                drop(stdin_files[i].take());
                spawn_cmd.stdin(s);
            }
            None => match stdin_files[i].take() {
                Some(f) => {
                    spawn_cmd.stdin(Stdio::File(f));
                }
                None => {
                    spawn_cmd.stdin(Stdio::Piped);
                }
            },
        }
        // stdout: a per-element redirect (`> f` / `>> f`) wins over the
        // pipe write end (dropped so the downstream reader sees EOF).
        match redirs[i].stdout.take() {
            Some(s) => {
                drop(stdout_files[i].take());
                spawn_cmd.stdout(s);
            }
            None => match stdout_files[i].take() {
                Some(f) => {
                    spawn_cmd.stdout(Stdio::File(f));
                }
                None => {
                    spawn_cmd.stdout(Stdio::Piped);
                }
            },
        }
        spawn_cmd.stderr(Stdio::Piped);
        if let Some(h) = redirs[i].heredoc.take() {
            heredoc_writes.push(h);
        }

        match spawn_cmd.spawn() {
            Ok(mut child) => {
                // Drop the Piped parent ends immediately (outer stdin /
                // stdout / stderr). The Stdio::File ends were already
                // consumed + dropped inside spawn().
                drop(child.stdin.take());
                drop(child.stdout.take());
                drop(child.stderr.take());
                pids.push(child.pid());
            }
            Err(_) => {
                spawn_err = Some("pipeline element spawn failed");
                break;
            }
        }
    }

    // Feed heredoc bodies (best-effort; small bodies at v1.0 -- a body
    // exceeding the pipe buffer fed to a non-reading element would block,
    // a documented v1.0 limitation), then drop the write ends for EOF.
    for (mut wr, body) in heredoc_writes {
        feed_pipe(&mut wr, body.as_bytes());
        drop(wr);
    }

    // Reap every spawned child (wait-any + pid match). statuses[i]
    // holds element i's exit status once reaped. NB: at v1.0 the kernel
    // normalizes any non-zero child exit to 1 (sys_exits_handler:
    // x0==0 -> "ok"/0; x0!=0 -> "fail"/1; richer u64 status is a Phase
    // 5+ deferral), so each status here is 0 or 1 -- the literal exit
    // code is not observable. aggregate_pipefail still computes the
    // rightmost-non-zero correctly over whatever statuses it gets; the
    // value-distinction is moot while the kernel collapses to 0/1.
    let mut statuses: Vec<i32> = (0..pids.len()).map(|_| 0i32).collect();
    let mut reaped: Vec<bool> = (0..pids.len()).map(|_| false).collect();
    for _ in 0..pids.len() {
        let mut st: i32 = 0;
        // SAFETY: t_wait_pid is the SYS_WAIT_PID SVC wrapper; &mut st
        // is a valid writable i32.
        let rc = unsafe { t_wait_pid(&mut st as *mut i32) };
        if rc < 0 {
            break; // no more children (shouldn't happen with live pids)
        }
        let reaped_pid = rc as i32;
        if let Some(idx) = pids.iter().position(|&pp| pp == reaped_pid) {
            if !reaped[idx] {
                statuses[idx] = st;
                reaped[idx] = true;
            }
        }
        // else: a reaped pid not in our set -- impossible at v1.0
        // (no background children); ignore.
    }

    if let Some(tag) = spawn_err {
        env.errstr_set(tag);
        env.status_set(127);
        return Ok(StatementFlow::Normal);
    }

    // Pipefail aggregation. Pair each element's status with its
    // tolerate_failure flag (`?|` sets it on the LEFT element).
    let pairs: Vec<(i32, bool)> = statuses
        .iter()
        .enumerate()
        .map(|(i, &st)| (st, p.elements[i].tolerate_failure))
        .collect();
    env.status_set(aggregate_pipefail(&pairs));
    Ok(StatementFlow::Normal)
}

/// Aggregate pipeline element exit statuses per scripture 8.4
/// (pipefail). Each entry is `(raw_status, tolerate_failure)`. Returns
/// the RIGHTMOST non-zero status among elements NOT marked `?|`
/// (tolerate_failure == false), or 0 if every non-tolerated element
/// exited 0. A tolerated element's status never contributes.
///
/// Exposed (pub) for direct boot-probe validation in u-test: native
/// binaries can't read argv at v1.0, so exit-status fixtures with
/// controllable codes don't exist; testing the rightmost-non-zero rule
/// and the `?|` tolerate rule is done by calling this pure function
/// with synthetic inputs.
pub fn aggregate_pipefail(elements: &[(i32, bool)]) -> i32 {
    let mut result = 0;
    for &(status, tolerate) in elements {
        if tolerate {
            continue;
        }
        if status != 0 {
            result = status;
        }
    }
    result
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
            // Not a fn -- external spawn. Built-ins land at U-6e
            // (they'll be tried ahead of spawn).
            exec_external(env, &argv, cmd.span)
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
// External command spawn (U-6c)
// ---------------------------------------------------------------------

/// Spawn an external binary, wait for it, and reflect the result in
/// `$status` + `$errstr`. The caller has already evaluated the Word
/// list into argv; argv[0] is the binary name (a single component,
/// no `/` -- the kernel name-lookup resolves it against devramfs OR
/// the pivoted root).
///
/// Stdio at U-6c: all three slots inherit from the shell. Pipes
/// land at U-6d.
///
/// Trace: when `env.trace_depth > 0`, echo argv to the trace sink
/// via `trace_echo` BEFORE the spawn. This closes the U-6b
/// deferral.
///
/// Failure modes -- all reflected as $status + $errstr, returning
/// Ok(StatementFlow::Normal) so the caller's implicit-fail
/// discipline can act:
///   - spawn failure (name too long, name with '/', NotFound,
///     kernel rejection): $status = 127, $errstr = "spawn failed:
///     <Error variant>" (bash command-not-found convention).
///   - wait failure (rare; kernel reaped a different child or
///     other reap error): $status = 1, $errstr = "wait failed:
///     <Error variant>".
///   - clean reap: $status = ExitStatus::raw(), $errstr untouched.
///
/// SYS_WAIT_PID at v1.0 reaps any zombie child (not the specific
/// pid). Multi-child wait-by-pid lands with job control at U-7;
/// for the foreground-only case here, this is benign because the
/// only outstanding child at this point is the one we just
/// spawned.
fn exec_external(
    env: &mut Env,
    argv: &[String],
    _span: Span,
) -> EvalResult<StatementFlow> {
    if env.trace_depth > 0 {
        trace_echo(argv);
    }

    let mut spawn_cmd = libthyla_rs::process::Command::new(argv[0].clone());
    if argv.len() > 1 {
        spawn_cmd.args(argv[1..].iter().cloned());
    }
    // v1.0 stdio convention: Piped + immediate drop. The shell has
    // no terminal-backed 0/1/2 yet (see module-level note). When
    // U-PTY + U-6g wire fd 0/1/2 to PTY-slave, drop these three
    // setters and let the Stdio::Inherit defaults take over.
    spawn_cmd.stdin(Stdio::Piped);
    spawn_cmd.stdout(Stdio::Piped);
    spawn_cmd.stderr(Stdio::Piped);

    match spawn_cmd.spawn() {
        Ok(mut child) => {
            // Drop the parent-side pipe ends immediately. The child
            // sees the kernel-installed pipes on 0/1/2; we discard
            // our copies. Native children (SYS_PUTS-writers) don't
            // touch the pipes; ported children get EOF on read /
            // EPIPE on write when they try -- the correct v1.0
            // behavior for a shell without a terminal. Per
            // libthyla-rs::process: parent_keeps is Option<File>;
            // dropping the take()n value runs File::Drop, which
            // closes the kernel handle.
            drop(child.stdin.take());
            drop(child.stdout.take());
            drop(child.stderr.take());
            match child.wait() {
                Ok(status) => {
                    env.status_set(status.raw());
                    Ok(StatementFlow::Normal)
                }
                Err(e) => {
                    let mut s = String::new();
                    let _ = write!(&mut s, "wait failed: {:?}", e);
                    env.errstr_set(s);
                    env.status_set(1);
                    Ok(StatementFlow::Normal)
                }
            }
        }
        Err(e) => {
            let mut s = String::new();
            let _ = write!(&mut s, "spawn failed: {:?}", e);
            env.errstr_set(s);
            env.status_set(127);
            Ok(StatementFlow::Normal)
        }
    }
}

/// Emit a bash-style `+ argv[0] argv[1] ...\n` trace line. The sink
/// at v1.0 is the kernel UART via `t_putstr`, matching the rest of
/// the shell's diagnostic output. When fd 2 (stderr) is wired
/// through line discipline (U-PTY + U-6g), this should switch to
/// fd 2. Args with embedded whitespace are NOT quoted at v1.0
/// (bash's set -x quoting is a v1.x refinement).
fn trace_echo(argv: &[String]) {
    let mut line = String::new();
    line.push_str("+ ");
    for (i, a) in argv.iter().enumerate() {
        if i > 0 {
            line.push(' ');
        }
        line.push_str(a);
    }
    line.push('\n');
    t_putstr(&line);
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
