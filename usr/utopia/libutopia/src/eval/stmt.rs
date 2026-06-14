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
// kernel resolves the spawn name through the caller's namespace
// (#58: stalk + per-component X-search); the shell does the `$path`
// search via `resolve_command` (a bare command -> `/bin/<name>`, a
// `/`-bearing name as-is). Stdio inherits (pipes wire at
// U-6d). The child is reaped synchronously via Child::wait; the
// resulting raw exit status becomes $status.
//
// Spawn failure semantics: any error (name too long, NotFound on
// `$path`, kernel rejection) sets $status = 127 (bash
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
// === Stdio model (U-6c convention; LS-2 console inherit) ===
//
// A session `ut` DOES hold a terminal-backed fd 0/1/2 (login spawns it
// with Stdio::Inherit over the SYS_CONSOLE_OPEN handle). LS-2 keys off
// `env.stdio_inherit` (set by `ut::main` via an fd-1 liveness probe):
//   - stdio_inherit == true: external stdout/stderr use Stdio::Inherit,
//     so the kernel installs the shell's fd 1/2 into the child and its
//     output reaches the terminal. See `out_stdio`.
//   - stdio_inherit == false: the U-6c v1.0 convention (matches
//     alloc-smoke + u-test's flow_process_pipe + every fd-less boot-check
//     / test harness, which has no 0/1/2 to inherit -- Inherit would make
//     SYS_SPAWN_FULL_ARGV reject the spawn): Stdio::Piped on the slot
//     followed by an immediate drop of the parent-side pipe end. The
//     child gets a (functionally inert) pipe; native binaries write via
//     their fd 1 and don't read fd 0.
// stdin stays Piped-drop for an ordinary foreground external -- it does not
// read the console; the shell's wait loop owns fd 0. The ONE exception
// (LS-7 / Kaua T-4) is a full-screen TUI child (`console::is_raw_command`,
// e.g. nora): `exec_external_raw` hands it fd 0/1/2 via Inherit + flips the
// console to RAW for its lifetime + restores on exit -- see `exec_external_raw`.
// Command substitution ($(cmd)) keeps its captured stdout Piped regardless
// (the shell reads it).
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
use libthyla_rs::io::{Read as IoRead, Write as IoWrite};
use libthyla_rs::process::{pipe, Stdio};
use libthyla_rs::notes::{send, Note, NoteClass, NoteTarget};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::{t_putstr, t_wait_pid_for, T_WAIT_WNOHANG};

use super::builtin;
use super::console;
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
    // Bound eval-stack recursion (scripture 8.1). Function calls, `source`, and
    // `eval` all re-enter through eval_block; without a cap, `fn f { f }`, a
    // self-`source`, or an `eval`-bomb overflows the 256 KiB EL0 stack into a
    // guard-page snare:segv -> shell death. Command substitution adds its own
    // bound at run_command_substitution_script (it routes through eval_pipeline,
    // not eval_block). An empty block cannot recurse -> skip the accounting
    // (and avoid needing a fallback error span).
    if stmts.is_empty() {
        return Ok(StatementFlow::Normal);
    }
    if env.eval_recursion_enter() {
        return Err(EvalError::new(EvalErrorKind::RecursionLimit, stmts[0].span));
    }
    let result = eval_block_inner(env, stmts);
    env.eval_recursion_leave();
    result
}

fn eval_block_inner(env: &mut Env, stmts: &[Statement]) -> EvalResult<StatementFlow> {
    for stmt in stmts {
        let flow = eval_statement(env, stmt)?;
        // `exit` requested anywhere in this statement (directly, or via
        // a called function / sourced / eval'd sub-script) unwinds the
        // whole statement stack: short-circuit to Return so every
        // enclosing block / loop / function returns, and the top-level
        // driver reads `env.exit_requested()` to terminate (U-6e-a). A
        // terminate-intent interrupt (Ctrl-C in a runaway loop, R2-F3)
        // unwinds the same way -- it propagates across function-call
        // boundaries here, where a loop-local Break could not -- and the
        // REPL clears it via `take_interrupt()` after the command unwinds.
        if env.exit_requested().is_some() || env.interrupt_pending() {
            return Ok(StatementFlow::Return);
        }
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
    if p.elements.is_empty() {
        // Empty pipeline shouldn't reach here -- parser rejects.
        return Err(EvalError::new(
            EvalErrorKind::Internal("empty pipeline"),
            p.span,
        ));
    }
    if p.background {
        // `cmd &` / `a | b &` (U-7a). Spawn detached, register a background
        // job, announce `[N] PID`, status 0; the REPL prompt-cycle reaper
        // reclaims + reports it. Same external-only element restriction as a
        // foreground pipeline (a fn / builtin / in-process form needs fork
        // -> NotImplemented, via the shared spawn helper).
        return eval_background_pipeline(env, p);
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
            if builtin::is_builtin(&argv[0]) {
                // A built-in runs in-process with no spawned fd to
                // retarget; redirecting its stdio needs the same fork /
                // fd-dup surface as a redirected function (U-6f).
                return Err(EvalError::new(
                    EvalErrorKind::NotImplemented("redirect on builtin (U-6f)"),
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
/// `$(cmd)` substitutes the command's stdout verbatim (no field split --
/// a heredoc body is text, like a double-quoted string).
fn render_heredoc_body(env: &Env, parts: &[DqPart], span: Span) -> EvalResult<String> {
    let mut out = String::new();
    for part in parts {
        match part {
            DqPart::Literal(s) => out.push_str(s),
            DqPart::Var(name) => out.push_str(&env.get(name).as_scalar()),
            DqPart::VarLen(name) => {
                let _ = write!(out, "{}", env.get(name).len());
            }
            DqPart::Subst(body) => {
                out.push_str(&run_command_substitution(env, body, span)?);
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

/// The stdout/stderr `Stdio` for a non-redirected slot (LS-2). When the
/// shell holds a terminal-backed fd 1/2 (`env.stdio_inherit` -- a session
/// `ut`, which login spawns with the console inherited as fd 0/1/2), the
/// child inherits it so its output reaches the terminal. Otherwise the
/// U-6c convention: `Stdio::Piped` then an immediate parent-side drop (a
/// fd-less boot-check / test harness has no 0/1/2 to inherit, so Inherit
/// would fail the spawn). The `drop(child.std*.take())` epilogues stay
/// correct under Inherit -- `resolve_stdio(Inherit)` keeps no parent end,
/// so the take() yields None and the drop is a no-op. stdin keeps
/// Piped-drop either way at LS-2 (interactive child stdin is LS-5/LS-8).
fn out_stdio(inherit: bool) -> Stdio {
    if inherit {
        Stdio::Inherit
    } else {
        Stdio::Piped
    }
}

/// Resolve an external command name to a path for spawning (#58). The kernel
/// resolves the spawn name through the caller's namespace (stalk + X-search),
/// so the shell does the `$path` search. A name containing `/` is used as-is
/// (absolute or relative). A bare command is searched on `$path = ["/bin", "/"]`:
/// `/bin` is the post-pivot installed location (joey binds the initrd binary
/// tree there); `/` is the pre-pivot initrd root (where the boot-test shell
/// runs). The first existing candidate wins; a miss defaults to `/bin/<name>`
/// for a clean spawn error. (The Plan 9 / Unix split -- the kernel resolves a
/// path, the shell does `$path`; a multi-entry user-settable `$path` is v1.x.)
fn resolve_command(name: &str) -> String {
    if name.contains('/') {
        return String::from(name);
    }
    for dir in ["/bin/", "/"] {
        let mut cand = String::with_capacity(dir.len() + name.len());
        cand.push_str(dir);
        cand.push_str(name);
        // O-read existence probe (binaries are 0755 = readable): resolves the
        // candidate through the namespace without spawning; File drops/closes.
        if libthyla_rs::fs::File::open(&cand).is_ok() {
            return cand;
        }
    }
    let mut p = String::with_capacity(5 + name.len());
    p.push_str("/bin/");
    p.push_str(name);
    p
}

/// Build a `Command` for `argv`: resolve `argv[0]` through `$path` (`/bin`)
/// and expand a `#!` interpreter line if the resolved program is a text
/// script. The single chokepoint for every external-spawn site (foreground,
/// raw-mode, redirected, pipeline element, background), so a script runs
/// identically in each. The caller configures stdio + spawns.
fn build_command(argv: &[String]) -> libthyla_rs::process::Command {
    let mut spawn_argv = prepare_argv(argv).into_iter();
    // prepare_argv always yields >= 1 element; fall back defensively rather
    // than panic (no_std panic=abort) if it ever did not.
    let prog = spawn_argv
        .next()
        .unwrap_or_else(|| resolve_command(&argv[0]));
    let mut cmd = libthyla_rs::process::Command::new(prog);
    cmd.args(spawn_argv);
    cmd
}

/// Resolve `argv[0]` to a concrete program and expand a `#!` interpreter
/// line, returning the effective argv to spawn (`[program, args...]`).
///
/// The kernel exec loads ELF only (ARCH 9.6.8). A text file beginning with
/// `#!interp [arg]` is a script; the shell -- Plan 9 lineage keeps `#!` out
/// of the kernel -- reads that line and rewrites the command to run the
/// interpreter with the script path as its argument. A file that is not a
/// shebang script (an ELF, or unreadable) passes through unchanged; the
/// kernel then loads it (or fails) as before.
fn prepare_argv(argv: &[String]) -> Vec<String> {
    let prog = resolve_command(&argv[0]);
    match peek_shebang(&prog) {
        Some((interp, maybe_arg)) => {
            let mut out: Vec<String> = Vec::with_capacity(argv.len() + 2);
            // $path-resolve the interpreter too, so `#!ut` works as well as
            // `#!/bin/ut`.
            out.push(resolve_command(&interp));
            if let Some(a) = maybe_arg {
                out.push(a);
            }
            out.push(prog); // the script path, as the interpreter's argument
            out.extend(argv[1..].iter().cloned()); // then the user's args
            out
        }
        None => {
            let mut out: Vec<String> = Vec::with_capacity(argv.len());
            out.push(prog);
            out.extend(argv[1..].iter().cloned());
            out
        }
    }
}

/// Peek `path`'s first line for a `#!interp [arg]` shebang. Reads only the
/// first 128 bytes (the conventional shebang bound) and delegates to the pure
/// `parse_shebang_line`. An unreadable file (or one not starting with `#!`) is
/// `None` -- it then passes through to the kernel ELF loader unchanged.
fn peek_shebang(path: &str) -> Option<(String, Option<String>)> {
    let mut f = File::open(path).ok()?;
    let mut buf = [0u8; 128];
    let n = f.read(&mut buf).ok()?;
    parse_shebang_line(&buf[..n])
}

/// Parse a file's leading bytes as a `#!interp [arg]` shebang. Returns
/// `(interpreter, optional single argument)`. `None` if `head` does not start
/// with `#!`, the interpreter is empty, or either field is not valid UTF-8. At
/// most ONE argument after the interpreter is recognized (the Linux/BSD
/// convention) -- the trimmed remainder of the first line verbatim, no further
/// word-splitting. Pure (no I/O) so it is host-testable.
fn parse_shebang_line(head: &[u8]) -> Option<(String, Option<String>)> {
    let rest = head.strip_prefix(b"#!")?;
    let line_end = rest
        .iter()
        .position(|&b| b == b'\n' || b == b'\r')
        .unwrap_or(rest.len());
    let line = trim_ascii_ws(&rest[..line_end]);
    if line.is_empty() {
        return None;
    }
    let (interp_b, arg_b) = match line.iter().position(|&b| b == b' ' || b == b'\t') {
        Some(i) => (&line[..i], trim_ascii_ws(&line[i + 1..])),
        None => (line, &b""[..]),
    };
    let interp = core::str::from_utf8(interp_b).ok()?;
    if interp.is_empty() {
        return None;
    }
    let arg = if arg_b.is_empty() {
        None
    } else {
        Some(String::from(core::str::from_utf8(arg_b).ok()?))
    };
    Some((String::from(interp), arg))
}

/// Trim leading + trailing ASCII spaces/tabs from a byte slice (no_std; the
/// stable `[u8]::trim_ascii` is too new for this toolchain).
fn trim_ascii_ws(mut b: &[u8]) -> &[u8] {
    while let [first, rest @ ..] = b {
        if *first == b' ' || *first == b'\t' {
            b = rest;
        } else {
            break;
        }
    }
    while let [rest @ .., last] = b {
        if *last == b' ' || *last == b'\t' {
            b = rest;
        } else {
            break;
        }
    }
    b
}

/// Spawn an external command with redirects applied. Non-redirected
/// stdout/stderr inherit the console when the shell holds one
/// (`env.stdio_inherit`, LS-2); otherwise the U-6c `Stdio::Piped` +
/// immediate parent-side drop. Non-redirected stdin is always Piped-drop.
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

    let mut spawn_cmd = build_command(argv);
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
            spawn_cmd.stdout(out_stdio(env.stdio_inherit));
        }
    }
    spawn_cmd.stderr(out_stdio(env.stdio_inherit));

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
            // Interruptible foreground wait (U-7c-b): reap by pid while
            // forwarding a Ctrl-C `interrupt` to the running child (see
            // exec_external for the by-pid / vanished-pid rationale).
            let pid = child.pid();
            let statuses = wait_pids_interruptible(env, &[pid]);
            env.status_set(statuses[0]);
            Ok(StatementFlow::Normal)
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
// === Reaping (by pid -- U-7a) ===
//
// We spawn N, collect their pids, then `wait_pid_for(pid, 0)` each one
// (the U-7-pre by-pid primitive). A by-pid wait reaps exactly that element
// and NEVER a coexisting background zombie, so the foreground demux is
// pid-precise once `&` jobs exist: `cmd & ; producer | consumer` cannot
// lose the bg child's exit to this pipeline's reap. (Through U-6d-a this was
// a wait-any loop, sound only because there were no background children.)
//
// === Pipefail (scripture 8.4) ===
//
// The pipeline's $status is the rightmost non-zero exit among elements
// NOT marked `?|` (tolerate_failure), or 0 if all (non-tolerated)
// elements succeeded. See `aggregate_pipefail`.
/// The outcome of spawning a pipeline's elements -- the phase shared by the
/// foreground reap-and-aggregate path (`exec_pipeline`) and the `&`
/// background-register path (`eval_background_pipeline`).
enum PipelineSpawn {
    /// Elements launched. `pids` are the spawned element pids in element
    /// order (LEN may be < n on a mid-pipeline spawn failure, in which case
    /// `spawn_err` is Some). `argvs` is every element's evaluated argv (n
    /// entries) for command rendering + pipefail tolerate-flag pairing by
    /// index.
    Spawned {
        pids: Vec<i32>,
        argvs: Vec<Vec<String>>,
        spawn_err: Option<&'static str>,
    },
    /// A pre-spawn failure (redirect-target open / `pipe()` alloc) already
    /// set `$status` + `$errstr`; the caller returns `Ok(Normal)`.
    Aborted,
}

/// Spawn a pipeline's elements: validate (external-simple-only), resolve
/// per-element redirects, allocate the N-1 connecting pipes, spawn all N
/// concurrently (spawn-all-before-wait), and feed any heredoc bodies. Does
/// NOT wait -- the caller decides foreground (reap-by-pid + aggregate) vs
/// background (register a job). Shared by `exec_pipeline` (foreground) and
/// `eval_background_pipeline` (`&`); works for n == 1 (a single command:
/// zero pipes, one element).
fn spawn_pipeline_elements(env: &mut Env, p: &Pipeline) -> EvalResult<PipelineSpawn> {
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
        if builtin::is_builtin(&argv[0]) {
            // A built-in has no spawned fd to wire to a pipe; like a
            // function pipeline element, this needs fork (U-6f+).
            return Err(EvalError::new(
                EvalErrorKind::NotImplemented("builtin as pipeline element"),
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
                return Ok(PipelineSpawn::Aborted);
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
                return Ok(PipelineSpawn::Aborted);
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
        let mut spawn_cmd = build_command(argv);
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
                    // The last element's outer stdout: inherit the console
                    // when the shell holds one (LS-2) so the pipeline's
                    // result is visible; else the U-6c Piped-drop.
                    spawn_cmd.stdout(out_stdio(env.stdio_inherit));
                }
            },
        }
        // Every element's stderr goes to the terminal (not the pipe), so it
        // inherits the console under LS-2 -- a mid-pipeline error is visible.
        spawn_cmd.stderr(out_stdio(env.stdio_inherit));
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

    Ok(PipelineSpawn::Spawned {
        pids,
        argvs,
        spawn_err,
    })
}

/// Foreground multi-element pipeline (U-6d-a): spawn all elements, reap each
/// BY PID, aggregate pipefail (scripture 8.4).
fn exec_pipeline(env: &mut Env, p: &Pipeline) -> EvalResult<StatementFlow> {
    let (pids, _argvs, spawn_err) = match spawn_pipeline_elements(env, p)? {
        PipelineSpawn::Aborted => return Ok(StatementFlow::Normal),
        PipelineSpawn::Spawned {
            pids,
            argvs,
            spawn_err,
        } => (pids, argvs, spawn_err),
    };

    // Reap each spawned element BY PID (U-7-pre `wait_pid_for`). A by-pid
    // wait never reaps a coexisting BACKGROUND zombie, so the foreground
    // demux is pid-precise: `cmd & ; producer | consumer` cannot lose the
    // bg child's exit to this pipeline's reap (closes the U-6d-a wait-any
    // limitation). statuses[i] holds element i's exit. NB: at v1.0 the
    // kernel collapses any non-zero child exit to 1, so each status is 0
    // or 1; aggregate_pipefail computes rightmost-non-zero regardless.
    // The reap is the interruptible foreground wait (U-7c-b): a Ctrl-C while
    // the pipeline runs forwards `interrupt` to its still-live elements. A
    // vanished pid yields status 0 (never spins; see the helper).
    let statuses = wait_pids_interruptible(env, &pids);

    if let Some(tag) = spawn_err {
        env.errstr_set(tag);
        env.status_set(127);
        return Ok(StatementFlow::Normal);
    }

    // Pipefail aggregation. Pair each element's status with its
    // tolerate_failure flag (`?|` sets it on the LEFT element). No spawn_err
    // here means pids.len() == n, so the index is in bounds.
    let pairs: Vec<(i32, bool)> = statuses
        .iter()
        .enumerate()
        .map(|(i, &st)| (st, p.elements[i].tolerate_failure))
        .collect();
    env.status_set(aggregate_pipefail(&pairs));
    Ok(StatementFlow::Normal)
}

/// Background pipeline (`cmd &` / `a | b &`, U-7a): spawn all elements
/// detached, register ONE job tracking every element pid, announce
/// `[N] PID` (the last element's pid -- bash's `$!`), set `$status` 0. The
/// REPL prompt-cycle reaper (`Repl::reap_jobs`) reclaims the zombies + emits
/// the `[N]+ Done` line. The spawned children are NOT waited here.
fn eval_background_pipeline(env: &mut Env, p: &Pipeline) -> EvalResult<StatementFlow> {
    let (pids, argvs, spawn_err) = match spawn_pipeline_elements(env, p)? {
        PipelineSpawn::Aborted => return Ok(StatementFlow::Normal),
        PipelineSpawn::Spawned {
            pids,
            argvs,
            spawn_err,
        } => (pids, argvs, spawn_err),
    };

    if pids.is_empty() {
        // The first element failed to spawn -- nothing to background.
        env.errstr_set(spawn_err.unwrap_or("background spawn failed"));
        env.status_set(127);
        return Ok(StatementFlow::Normal);
    }

    // Register ONE job for the whole pipeline (bash's model). On a partial
    // spawn failure we still register the launched pids so the reaper
    // reclaims their zombies -- the dropped pipe ends EOF them into exit.
    let last_pid = pids[pids.len() - 1];
    let cmd = render_pipeline_cmd(&argvs);
    let spec = env.jobs_mut().add(pids, cmd);

    // `[N] PID` -- the background-launch announcement (bash). Via t_putstr
    // (the shell's v1.0 diagnostic sink; same convention as trace_echo + the
    // banner -- there is no terminal-backed fd 1 yet).
    let mut line = String::new();
    let _ = write!(&mut line, "[{}] {}\n", spec, last_pid);
    t_putstr(&line);

    // status 0 on a clean launch; 127 if a mid-pipeline element failed (the
    // launched ones still background + reap normally).
    if spawn_err.is_some() {
        env.status_set(127);
    } else {
        env.status_set(0);
    }
    Ok(StatementFlow::Normal)
}

/// Render a pipeline's evaluated argvs to a display command line: each
/// element's argv space-joined, elements joined by ` | `. Used for the
/// `[N] PID` launch line + the `[N]+ Done` notification.
fn render_pipeline_cmd(argvs: &[Vec<String>]) -> String {
    let mut s = String::new();
    for (i, argv) in argvs.iter().enumerate() {
        if i > 0 {
            s.push_str(" | ");
        }
        for (j, a) in argv.iter().enumerate() {
            if j > 0 {
                s.push(' ');
            }
            s.push_str(a);
        }
    }
    s
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
            // Default this command to success BEFORE expanding argv. A
            // command substitution in the words sets $status to the
            // inner exit (scripture 8.7); a bare `$(cmd)` line that
            // expands to nothing must then PRESERVE that exit, while a
            // substitution-free empty expansion ($undef-only line) still
            // reports 0. (Setting 0 up front + not resetting on the
            // empty-argv path achieves both.)
            env.status_set(0);
            let argv = evaluate_argv(env, &simple.words)?;
            // SA-1 (RW-9 round-2 self-audit): a Ctrl-C that interrupted a
            // `$(...)` argument latched interrupt_pending while expanding argv.
            // Do NOT spawn the command on the partial capture -- bail so the
            // flag unwinds to the prompt (eval_block re-observes it after this
            // statement). Mirrors the exit_requested checks on this path.
            if env.interrupt_pending() {
                return Ok(StatementFlow::Normal);
            }
            if argv.is_empty() {
                return Ok(StatementFlow::Normal);
            }
            // argv[0] is already alias-expanded (evaluate_argv folds it in for
            // every command position). Resolution order (scripture 9.1): fn ->
            // builtin -> external. Look up argv[0] in fns first; clone the
            // FnDecl so we don't hold an Env borrow across mutation.
            if let Some(decl) = env.fn_get(&argv[0]).cloned() {
                return invoke_function(env, &decl, &argv);
            }
            // Then built-ins (cd / pwd / exit / unset / eval / source /
            // type / true / false -- U-6e-a; jobs / fg / bg / wait / kill --
            // U-7b) -- they run in-process and mutate the shell's own state.
            if let Some(result) = builtin::try_builtin(env, &argv) {
                return result;
            }
            // Otherwise an external binary spawn.
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

    // A pending `exit` must NOT be normalized away at the function
    // boundary -- it unwinds the entire stack (U-6e-a). Propagate the
    // flow (Return) straight up so the next eval_block re-observes the
    // exit request and keeps unwinding.
    if env.exit_requested().is_some() {
        return result;
    }

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
    // LS-7 / Kaua T-4: a full-screen TUI child (nora) needs the console in RAW
    // mode + its OWN fd 0/1/2 (Inherit), and the shell must NOT forward Ctrl-C to
    // it (Ctrl-C is the child's keystroke, not a terminate). This requires a
    // console to inherit (stdio_inherit); the consctl fd is optional and threaded
    // into the dance:
    //   Some(fd): the full dance -- flip the discipline to RAW and restore it.
    //   None:     a non-login ut whose console was never consctl-forwarded. Still
    //             hand the child the inherited console and bracket it with the
    //             screen crash backstop, just without the raw-mode flips (the
    //             console keeps its current discipline). #106-F1: this path used
    //             to fall through to the normal spawn with NO restore_screen, so a
    //             TUI crash there wedged the alt-screen.
    // The fd-less harness (stdio_inherit == false) never reaches here and keeps
    // its Piped-drop-EOF degrade.
    if env.stdio_inherit && console::is_raw_command(&argv[0]) {
        return exec_external_raw(env, argv, env.consctl_fd);
    }

    if env.trace_depth > 0 {
        trace_echo(argv);
    }

    let mut spawn_cmd = build_command(argv);
    // stdout/stderr inherit the console when the shell holds one
    // (env.stdio_inherit, LS-2) so output is visible; else the U-6c
    // Piped + immediate-drop convention (a fd-less harness has nothing
    // to inherit). stdin stays Piped-drop at LS-2 (interactive child
    // stdin is LS-5/LS-8). See out_stdio + the module-level note.
    spawn_cmd.stdin(Stdio::Piped);
    spawn_cmd.stdout(out_stdio(env.stdio_inherit));
    spawn_cmd.stderr(out_stdio(env.stdio_inherit));

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
            // Interruptible foreground wait (U-7c-b): reap by pid while
            // forwarding a Ctrl-C `interrupt` to the running child. Child::Drop
            // does not reap, so this by-pid wait is the sole reap. A vanished
            // pid yields 0 (the prior `wait failed -> status 1` arm was
            // unreachable for a just-spawned child -- nothing else reaps it).
            let pid = child.pid();
            let statuses = wait_pids_interruptible(env, &[pid]);
            env.status_set(statuses[0]);
            Ok(StatementFlow::Normal)
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

/// Run a full-screen TUI child (nora) under the LS-7 raw-mode dance (Kaua T-4).
/// `consctl_fd` is the caller's already-validated `/dev/consctl` fd, or `None`
/// for the consctl-less degrade (a non-login ut). Steps: when a consctl fd is
/// held, RAW the console (ISIG off -> Ctrl-C is the child's keystroke); hand the
/// child fd 0/1/2 (Inherit); wait by pid; then -- on EVERY exit path -- re-emit
/// the screen-restore escapes (the crash backstop, ALWAYS) and, when held,
/// restore ut's PROMPT mode. Without a consctl fd the discipline flips are
/// skipped (the console keeps its mode) but the screen backstop still runs, so a
/// crashed TUI never wedges the alt-screen (#106-F1). I-27: ut stays the sole
/// consctl writer; the child never touches the discipline and is never
/// console-attached, so the SAK / elevation gate is untouched.
fn exec_external_raw(
    env: &mut Env,
    argv: &[String],
    consctl_fd: Option<i32>,
) -> EvalResult<StatementFlow> {
    if env.trace_depth > 0 {
        trace_echo(argv);
    }

    // 1. RAW the console BEFORE the spawn so the child's first read sees raw
    //    bytes. A rejected consctl write (bad fd / pre-LS-8b kernel) still lets
    //    the child run -- in whatever mode the console held -- a graceful degrade.
    //    With no consctl fd (the degrade) the console keeps its current mode (ut's
    //    PROMPT discipline: raw, no-echo, but +isig -- so Ctrl-C terminates the
    //    child rather than reaching it as a keystroke; acceptable for a non-login
    //    ut, and strictly better than the old no-backstop fall-through).
    if let Some(fd) = consctl_fd {
        console::set_mode(fd, console::RAW_MODE);
    }

    let mut spawn_cmd = build_command(argv);
    // 2. The child gets the console on all three slots (Inherit): a TUI reads fd
    //    0 + draws fd 1, and stderr shares the screen. This is the Piped->Inherit
    //    switch the normal path does NOT do for stdin.
    spawn_cmd.stdin(Stdio::Inherit);
    spawn_cmd.stdout(Stdio::Inherit);
    spawn_cmd.stderr(Stdio::Inherit);

    match spawn_cmd.spawn() {
        Ok(child) => {
            // 3. A plain by-pid wait -- NOT wait_pids_interruptible. With a consctl
            //    fd the discipline is RAW (ISIG off) so the kernel posts no
            //    `interrupt` for this child; in the consctl-less degrade ISIG is on,
            //    so a Ctrl-C terminates the child, which this same wait then reaps --
            //    either way the child owns the console until it exits and there is
            //    nothing for ut to forward. Under all-Inherit the parent holds no
            //    pipe ends, so `child` carries nothing to close (Child::Drop does not
            //    reap) -- reading its pid then letting it drop is clean.
            let pid = child.pid();
            let mut st: i32 = 0;
            // SAFETY: SVC wrapper; &mut st is a valid writable i32. A vanished /
            // not-our-child pid yields rc < 0 -> status 0 (matches the reap loops).
            let rc = unsafe { t_wait_pid_for(pid, 0, &mut st as *mut i32) };
            // 4. Restore on EVERY exit path (clean, error, OR death): re-emit the
            //    screen escapes (ALWAYS -- the crash backstop) and, when a consctl
            //    fd is held, ut's PROMPT mode. The child's own Drop emitted the same
            //    escapes on a clean exit (idempotent); on a CRASH (panic=abort, no
            //    Drop) this is the sole restore, so a crashed TUI never wedges the
            //    console in the alt-screen with a hidden cursor.
            if let Some(fd) = consctl_fd {
                console::set_mode(fd, console::PROMPT_MODE);
            }
            console::restore_screen();
            env.status_set(if rc < 0 { 0 } else { st });
            Ok(StatementFlow::Normal)
        }
        Err(e) => {
            // The child never spawned -> never entered the alt-screen, so only the
            // line discipline needs restoring (and only if we held a consctl fd to
            // RAW it); no screen escapes. Report the failure like the normal
            // spawn-fail path.
            if let Some(fd) = consctl_fd {
                console::set_mode(fd, console::PROMPT_MODE);
            }
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
///
/// A bare unquoted word carrying a glob meta char is expanded against
/// the filesystem (scripture 6.10, U-6e-b-2): it contributes the SORTED
/// list of matching paths, or -- if nothing matches -- NO argv element
/// at all (rc nullglob: a no-match glob expands to the empty list, never
/// the literal). Quoted strings, `$var`, and `^`-concat words are taken
/// literally and never expand.
fn evaluate_argv(env: &Env, words: &[Word]) -> EvalResult<Vec<String>> {
    let mut argv = Vec::new();
    for w in words {
        if let Some(pat) = glob_candidate(w) {
            let hits = glob::expand(env, pat);
            // rc nullglob (scripture 6.10): an empty match set drops the
            // word entirely rather than passing the literal pattern.
            argv.extend(hits);
            continue;
        }
        let v = eval_word(env, w)?;
        argv.extend(v.0);
    }
    // Aliases expand argv[0] here, the single command-argv builder shared by
    // every command position (bare, redirect, pipeline element, background) --
    // so `la`/`ll` work uniformly wherever a command runs, not just at the bare
    // prompt. One pass (the baked set targets `ls`, not an alias); the result
    // then flows through the caller's fn -> builtin -> external resolution.
    // (Unlike bash, this keys on the resolved argv[0], so a $var/glob that
    // yields an alias name expands too -- benign; the literal-first-word
    // refinement lands with the `alias` builtin.)
    Ok(env.expand_alias(argv))
}

/// If `w` is a bare unquoted word carrying a glob meta char, return its
/// pattern for filesystem expansion; otherwise `None` (the word is taken
/// via normal value evaluation). Only `Word::Single(TokenKind::Word)`
/// qualifies -- a single/double-quoted string, a `$var`, and a `^`-concat
/// word are rc-literal (`echo "*.c"` and `echo a^*` do not glob).
fn glob_candidate(w: &Word) -> Option<&str> {
    match w {
        Word::Single(Token {
            kind: TokenKind::Word(s),
            ..
        }) if glob::has_meta(s) => Some(s.as_str()),
        _ => None,
    }
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
        // Bare-word context: a command substitution's output is split on
        // whitespace into a list (rc's default $ifs -- scripture 6.3 +
        // 6.6). `$(echo a b)` is two words; `for f in $(ls)` iterates.
        TokenKind::Subst(body) | TokenKind::Backtick(body) => {
            let captured = run_command_substitution(env, body, tok.span)?;
            Ok(Value::list(split_fields(&captured)))
        }
        TokenKind::ProcSubIn(_) | TokenKind::ProcSubOut(_) => Err(EvalError::new(
            // Process substitution needs a `/proc/self/fd/N` namespace
            // surface (scripture 6.12) the kernel does not expose yet; a
            // separable v1.x feature, not a dependency of `$(cmd)`.
            EvalErrorKind::NotImplemented("process substitution <(cmd) / >(cmd)"),
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
            // Inside `"..."` a substitution is inserted verbatim (no
            // field split) -- the surrounding quotes make it one word.
            DqPart::Subst(body) => {
                out.push_str(&run_command_substitution(env, body, tok.span)?);
            }
        }
    }
    Ok(Value::scalar(out))
}

// ---------------------------------------------------------------------
// Command substitution (U-6f)
// ---------------------------------------------------------------------
//
// `$(cmd)` and `` `{cmd} `` run `cmd` and substitute its stdout, trimmed
// of trailing newlines (scripture 6.6), at the call site. A bare-word
// substitution whitespace-splits the result into a list (rc's default
// $ifs); one inside `"..."` (or a heredoc body) is inserted verbatim
// with no split.
//
// v1.0 scope: the body must parse to a SINGLE redirect-free pipeline of
// EXTERNAL simple commands (1+ elements). The common forms -- `$(echo
// x)`, `$(date)`, `$(seq 1 9)`, `$(cmd | filter)` -- all fit. Anything
// else (a builtin / function / control-flow / multi-statement body / a
// per-element redirect) is NotImplemented for two reasons: capturing an
// IN-PROCESS command's output would need a fork (or a redirect of the
// shell's own UART sink) that v1.0 lacks; AND running an in-process body
// here would leak its env mutations into the parent shell, which a
// substitution must not do (scripture 6.6: "run cmd in a subshell").
// Restricting to a pure external pipeline keeps the capture deadlock-free
// (drain-before-reap) AND keeps the parent env untouched. Process
// substitution `<(cmd)`/`>(cmd)` stays NotImplemented (scripture 6.12
// wants a `/proc/self/fd/N` namespace surface the kernel does not expose
// yet -- a separable v1.x feature).
//
// Status (scripture 8.7: "inside $(cmd) a non-zero exit DOES propagate
// up"): the inner pipeline's pipefail-aggregated exit becomes `$status`.
// This is why `$status` is a `Cell` (env.rs) -- the substitution runs
// inside the `&Env` expression evaluator. A spawn failure reports 127
// (command-not-found convention); `$errstr` is left unchanged (a v1.0
// simplification -- substitution failure is observable via `$status`).
// `$(cmd?)` / `$(try { } catch { })` (scripture 8.7 tolerate forms)
// imply a multi-statement / in-process body and are NotImplemented at
// v1.0; tolerate the failure at the OUTER level (`||`, an enclosing
// `try`) instead.

/// Run a command-substitution body (raw source) and return its stdout,
/// trimmed of trailing newlines. Used by the token/DqPart layer, where a
/// substitution body is stored as raw bytes (`TokenKind::Subst(String)`,
/// `DqPart::Subst(String)`) and re-parsed here. Sets `$status` to the
/// inner command's exit. Takes `&Env` (it sets `$status` through the
/// `Cell`), so it composes with nested substitution under the immutable
/// expression evaluator.
pub(crate) fn run_command_substitution(
    env: &Env,
    body: &str,
    span: Span,
) -> EvalResult<String> {
    use crate::parser::parse;

    let script = parse(body).map_err(|_e| {
        EvalError::new(
            EvalErrorKind::Internal("command substitution parse failed"),
            span,
        )
    })?;
    run_command_substitution_script(env, &script, span)
}

/// The substitution core, over an already-parsed `Script`. The expr-layer
/// (`ExprKind::Subst(Box<Script>)`) holds a pre-parsed body and calls here
/// directly; the raw-body wrapper above parses first. See the section
/// header for the v1.0 body-shape restriction + status semantics.
pub(crate) fn run_command_substitution_script(
    env: &Env,
    script: &Script,
    span: Span,
) -> EvalResult<String> {
    // Empty body (`$()` or all-whitespace): a no-op -- empty output, success.
    if script.statements.is_empty() {
        env.status_set(0);
        return Ok(String::new());
    }
    // Bound command-substitution recursion (scripture 8.1): a nested `$(...)`
    // in an element's words re-enters here via evaluate_argv, so without a cap
    // `$($($(...)))` overflows the 256 KiB EL0 stack into a guard-page
    // snare:segv. Shares Env's eval-depth counter with eval_block so the bound
    // holds across mixed function/source/subst nesting. eval_recursion_leave
    // runs on every return path (the inner fn's `?`/early returns all unwind
    // back through here).
    if env.eval_recursion_enter() {
        return Err(EvalError::new(EvalErrorKind::RecursionLimit, span));
    }
    let result = run_command_substitution_script_inner(env, script, span);
    env.eval_recursion_leave();
    result
}

fn run_command_substitution_script_inner(
    env: &Env,
    script: &Script,
    span: Span,
) -> EvalResult<String> {
    if script.statements.len() != 1 {
        return Err(EvalError::new(
            EvalErrorKind::NotImplemented("command substitution body must be a single pipeline (v1.0)"),
            span,
        ));
    }
    let p = match &script.statements[0].kind {
        StatementKind::Pipeline(p) => p,
        _ => {
            return Err(EvalError::new(
                EvalErrorKind::NotImplemented("command substitution body must be a command (v1.0)"),
                span,
            ));
        }
    };
    if p.background {
        return Err(EvalError::new(
            EvalErrorKind::NotImplemented("background in command substitution"),
            span,
        ));
    }

    // Every element must be a redirect-free EXTERNAL simple command.
    let mut argvs: Vec<Vec<String>> = Vec::with_capacity(p.elements.len());
    for elem in &p.elements {
        let cmd = &elem.command;
        if !cmd.redirects.is_empty() {
            return Err(EvalError::new(
                EvalErrorKind::NotImplemented("redirect in command substitution (v1.0)"),
                span,
            ));
        }
        let simple = match &cmd.kind {
            CommandKind::Simple(s) => s,
            _ => {
                return Err(EvalError::new(
                    EvalErrorKind::NotImplemented("non-simple command in command substitution (v1.0)"),
                    span,
                ));
            }
        };
        let argv = evaluate_argv(env, &simple.words)?;
        if argv.is_empty() {
            // An element that expands to nothing ($undef). With one
            // element this is an empty command (no output, success); a
            // multi-element pipeline with an empty element is degenerate.
            // Either way produce empty output + success rather than
            // spawning a nameless child.
            env.status_set(0);
            return Ok(String::new());
        }
        if env.fn_get(&argv[0]).is_some() {
            return Err(EvalError::new(
                EvalErrorKind::NotImplemented("function in command substitution (v1.0)"),
                span,
            ));
        }
        if builtin::is_builtin(&argv[0]) {
            return Err(EvalError::new(
                EvalErrorKind::NotImplemented("builtin in command substitution (v1.0)"),
                span,
            ));
        }
        argvs.push(argv);
    }

    let tolerate: Vec<bool> = p.elements.iter().map(|e| e.tolerate_failure).collect();
    Ok(capture_external_pipeline(env, &argvs, &tolerate))
}

/// Spawn an external pipeline (1+ elements), capturing the last element's
/// stdout and returning it trimmed of trailing newlines. Sets `$status`
/// to the pipefail-aggregated exit (127 on spawn failure).
///
/// Deadlock-free by drain-before-reap: the capture read end is drained to
/// EOF BEFORE any `wait`, so an element stalled on a full pipe is
/// unblocked by our read (the same discipline `exec_pipeline` uses for
/// spawn-all-before-wait). EOF arrives when the last element exits and
/// its write end -- the only remaining copy, ours was consumed into the
/// child by `spawn` -- closes. Reaping is BY PID (U-7-pre `wait_pid_for`),
/// so a coexisting background `&` job's zombie is never reaped here.
fn capture_external_pipeline(
    env: &Env,
    argvs: &[Vec<String>],
    tolerate: &[bool],
) -> String {
    let n = argvs.len();

    // The capture pipe: the last element writes here; we hold + drain the
    // read end. On allocation failure, report failure with empty output.
    let (cap_rd, cap_wr) = match pipe() {
        Ok(p) => p,
        Err(_) => {
            env.status_set(1);
            return String::new();
        }
    };

    // N-1 internal pipes: element j's stdout -> element j+1's stdin.
    let mut stdin_files: Vec<Option<File>> = (0..n).map(|_| None).collect();
    let mut stdout_files: Vec<Option<File>> = (0..n).map(|_| None).collect();
    for j in 0..n - 1 {
        match pipe() {
            Ok((rd, wr)) => {
                stdout_files[j] = Some(wr);
                stdin_files[j + 1] = Some(rd);
            }
            Err(_) => {
                env.status_set(1);
                return String::new();
            }
        }
    }
    // The last element's stdout is the capture write end.
    stdout_files[n - 1] = Some(cap_wr);

    // Spawn each element. Stdio::File ends are consumed into the child by
    // spawn(); the outer stdin (element 0) takes the v1.0 Piped+immediate-
    // drop convention; stderr inherits the console under LS-2 (env.stdio_-
    // inherit) so a failing substitution's error is visible, else Piped-drop.
    // A mid-pipeline spawn failure stops spawning; we still drain + reap what
    // launched (their ends + the un-spawned ends drop at return).
    let mut pids: Vec<i32> = Vec::with_capacity(n);
    let mut spawn_failed = false;
    for i in 0..n {
        let argv = &argvs[i];
        let mut spawn_cmd = build_command(argv);
        match stdin_files[i].take() {
            Some(f) => {
                spawn_cmd.stdin(Stdio::File(f));
            }
            None => {
                spawn_cmd.stdin(Stdio::Piped);
            }
        }
        match stdout_files[i].take() {
            Some(f) => {
                spawn_cmd.stdout(Stdio::File(f));
            }
            None => {
                // Unreachable: every element's stdout is a Some (an internal
                // pipe, or the capture write end for the last) -- the captured
                // stream is NEVER inherited. Kept Piped as a safe fallback.
                spawn_cmd.stdout(Stdio::Piped);
            }
        }
        // stderr is NOT captured ($(cmd) captures stdout only); it goes to the
        // terminal, so it inherits the console under LS-2 (a failing
        // substitution's error is visible) -- else the U-6c Piped-drop.
        spawn_cmd.stderr(out_stdio(env.stdio_inherit));
        match spawn_cmd.spawn() {
            Ok(mut child) => {
                drop(child.stdin.take());
                drop(child.stdout.take());
                drop(child.stderr.take());
                pids.push(child.pid());
            }
            Err(_) => {
                spawn_failed = true;
                break;
            }
        }
    }

    // Drop every un-consumed pipe end BEFORE draining the capture read end.
    // On the success path all entries are already None (taken into children),
    // so this is a no-op. On a NON-FINAL element spawn failure the capture
    // write end (stdout_files[n-1]) and the failed element's ends are still
    // owned here; unless they are closed first, read_to_end below blocks
    // forever -- the capture pipe delivers EOF only when its last write end
    // closes (kernel pipe cond_can_read = count>0 || write_eof), and the
    // never-spawned last element never received that write end.
    drop(stdin_files);
    drop(stdout_files);

    // Drain the capture read end to EOF (before reaping -- see fn note).
    let mut out_bytes: Vec<u8> = Vec::new();
    {
        let mut rd = cap_rd;
        let _ = rd.read_to_end(&mut out_bytes);
    }

    // Reap each spawned child BY PID (U-7-pre `wait_pid_for`). The output
    // was already drained to EOF above, so each element has exited (or is
    // exiting); a by-pid wait reaps exactly this substitution's children and
    // never a coexisting BACKGROUND zombie.
    let mut statuses: Vec<i32> = (0..pids.len()).map(|_| 0i32).collect();
    for (i, &pid) in pids.iter().enumerate() {
        let mut st: i32 = 0;
        // SAFETY: t_wait_pid_for is the SYS_WAIT_PID SVC wrapper; &mut st is
        // a valid writable i32.
        let rc = unsafe { t_wait_pid_for(pid, 0, &mut st as *mut i32) };
        if rc < 0 {
            continue;
        }
        statuses[i] = st;
    }

    if spawn_failed {
        env.status_set(127);
    } else {
        // pids.len() == n here (no early break), so tolerate[i] is in
        // bounds for every status.
        let pairs: Vec<(i32, bool)> = statuses
            .iter()
            .enumerate()
            .map(|(i, &st)| (st, tolerate[i]))
            .collect();
        env.status_set(aggregate_pipefail(&pairs));
    }

    trim_trailing_newlines(out_bytes)
}

/// Lossy-decode captured bytes to a String, stripping every trailing
/// newline (scripture 6.6). Lossy (like ReadDir) because command output
/// need not be valid UTF-8.
fn trim_trailing_newlines(bytes: Vec<u8>) -> String {
    let mut s = String::from_utf8_lossy(&bytes).into_owned();
    while s.ends_with('\n') {
        s.pop();
    }
    s
}

/// Split a captured substitution string on whitespace into fields,
/// dropping empties (rc's default `$ifs` = space/tab/newline). The
/// bare-word substitution path; the quoted path inserts the string whole.
pub(crate) fn split_fields(s: &str) -> Vec<String> {
    s.split_whitespace().map(String::from).collect()
}

// ---------------------------------------------------------------------
// Assignment + declarations
// ---------------------------------------------------------------------

fn eval_let(env: &mut Env, stmt: &LetStmt) -> EvalResult<StatementFlow> {
    // An assignment succeeds (status 0) UNLESS a command substitution in
    // the RHS fails -- in which case the substitution's non-zero exit
    // propagates (scripture 8.7: `let output = $(cmd)`). Set the default
    // 0 BEFORE evaluating, let a substitution override it, and do NOT
    // reset afterward (a post-eval reset would mask the failure).
    env.status_set(0);
    let v = eval_expr(env, &stmt.value)?;
    env.let_set(stmt.name.clone(), v);
    Ok(StatementFlow::Normal)
}

fn eval_assign(env: &mut Env, stmt: &AssignStmt) -> EvalResult<StatementFlow> {
    env.status_set(0);
    let v = eval_expr(env, &stmt.value)?;
    env.assign(stmt.name.clone(), v);
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
    let mut tick: u32 = 0;
    loop {
        // R2-F3: a loop whose body never blocks in a foreground wait (pure
        // shell eval -- `while (1==1) { }`) would otherwise never observe a
        // Ctrl-C `interrupt` note; poll the queue every LOOP_INTERRUPT_STRIDE
        // iterations and unwind to the prompt on one. A body that DOES run a
        // foreground command is already interruptible via the command's
        // wait_pids_interruptible, so this only governs the no-child case.
        if tick % LOOP_INTERRUPT_STRIDE == 0 && poll_loop_interrupt(env) {
            return Ok(StatementFlow::Return);
        }
        tick = tick.wrapping_add(1);
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
    let mut tick: u32 = 0;
    for el in elements {
        // R2-F3: same strided terminate-intent interrupt poll as eval_while.
        if tick % LOOP_INTERRUPT_STRIDE == 0 && poll_loop_interrupt(env) {
            return Ok(StatementFlow::Return);
        }
        tick = tick.wrapping_add(1);
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
    let name = eval_expr(env, &stmt.note_name)?.as_scalar();
    // Defer the named class for the body's duration (scripture 10.8). The
    // kernel Thread mask holds matching notes in the queue (when the shell
    // has opened its note fd) so a note arriving mid-body is dequeue-
    // deferred -- a polled wait inside the body sees no POLLIN for it and
    // cannot spin. Without an open fd this is pure book-keeping and
    // delivery is governed by sync-point timing alone. `note_mask_add`
    // returns false for an already-masked class (a nested re-mask), which
    // we must NOT unwind.
    let masked = note_class_for_name(&name).filter(|c| env.note_mask_add(*c));
    let flow = eval_block(env, &stmt.body);
    if let Some(c) = masked {
        env.note_mask_remove(c);
    }
    // Notes held during the mask are now deliverable (scripture 10.8:
    // "pending notes drain when the block exits"); fire their handlers.
    deliver_pending_notes(env);
    flow
}

/// Map a note name to its kernel-maskable class, for `mask note`. Only the
/// maskable kernel-known classes map; `kill` (non-catchable -- the kernel
/// ignores its mask bit), `snare:*` (kernel-synthetic faults that terminate
/// rather than enqueue), and user-defined names return `None` -- a
/// `mask note` over those is a body-only no-op (they either cannot be
/// deferred or cannot arrive at a userspace queue).
fn note_class_for_name(name: &str) -> Option<NoteClass> {
    match name {
        "interrupt" => Some(NoteClass::Interrupt),
        "pipe" => Some(NoteClass::Pipe),
        "child_exit" => Some(NoteClass::ChildExit),
        _ => None,
    }
}

/// Drain + dispatch every immediately-available note from the shell's own
/// queue (U-7c; scripture 10.7). Called at each REPL sync point (the prompt
/// cycle), at a `mask note` block exit, and -- LS-8c -- on an async idle-prompt
/// wake of the note fd (`Repl::on_notes_ready`). Bounded + non-blocking:
/// `Notes::try_read` polls with timeout 0, so an empty (or masked-only)
/// queue returns `None` and the drain stops -- this never blocks and never
/// spins. A registered `on note` handler fires synchronously here (main-loop
/// context, no async-signal constraints); an unhandled note takes its
/// built-in disposition. No-op until the consumer opens the note fd
/// (`Repl::open_notes`); the host + bare-spawn paths leave it inert.
///
/// Returns true iff an UNHANDLED `interrupt` was drained -- the reactive-Ctrl-C
/// signal the LS-8c idle poll loop uses to cancel the in-progress line. The
/// sync-point callers (the prompt cycle, the `mask note` exit) ignore the
/// return: an idle interrupt drained between commands is discarded there
/// (benign), exactly as before LS-8c.
pub fn deliver_pending_notes(env: &mut Env) -> bool {
    // Notes deferred during an interruptible foreground wait (U-7c-b) fire
    // first: they arrived before anything still sitting in the live queue.
    // Always drained -- even with the fd closed -- so a prior wait's residue
    // is never stranded.
    let deferred = env.take_deferred_notes();
    if deferred.is_empty() && env.notes().is_none() {
        return false;
    }
    // Note delivery is transparent to `$status` (bash saves `$?` around a
    // trap): a handler firing between commands must not clobber the status
    // the next prompt reports. Capture + restore around the whole drain.
    let saved_status = env.status();
    let mut unhandled_interrupt = false;
    for note in &deferred {
        unhandled_interrupt |= dispatch_note(env, note);
    }
    loop {
        // The immutable borrow of `env` for `notes()` lives only across the
        // `try_read` call; `next` is owned, so `env` is free for dispatch.
        let next = match env.notes() {
            Some(n) => n.try_read(),
            None => break,
        };
        match next {
            Ok(Some(note)) => {
                unhandled_interrupt |= dispatch_note(env, &note);
            }
            // Empty/masked-only queue (Ok(None)) or a transient read error
            // -> stop draining; the next sync point retries.
            _ => break,
        }
    }
    env.status_set(saved_status);
    unhandled_interrupt
}

/// How often (in iterations) `eval_while` / `eval_for` poll the note queue for
/// a terminate-intent interrupt (R2-F3). A strided poll keeps a hot pure-eval
/// loop from paying a `try_read` syscall every iteration while bounding Ctrl-C
/// latency to a handful of iterations -- microseconds for the tight-loop case
/// the fix targets.
const LOOP_INTERRUPT_STRIDE: u32 = 128;

/// Non-blocking poll of the shell's note queue for a pending `interrupt`
/// during a long-running shell-eval loop (R2-F3 / scripture 10.2). Returns
/// true (and latches `env.set_interrupt()`) when a terminate-intent
/// `interrupt` is queued, so the loop unwinds to the prompt. Non-interrupt
/// notes read past while looking are DEFERRED (`env.defer_note`) -- so an
/// `on note` handler still fires at the post-command drain and a `child_exit`
/// is not lost -- mirroring `drain_fg_wait_notes`. No-op (false) when the note
/// fd is closed (host tests / pre-`open_notes`): nothing posts a console
/// interrupt there, and `try_read` would have no queue to read.
fn poll_loop_interrupt(env: &mut Env) -> bool {
    if env.interrupt_pending() {
        return true;
    }
    if env.notes().is_none() {
        return false;
    }
    loop {
        // The immutable borrow for `notes()` ends with `try_read`; `note` is
        // owned, freeing `env` for `defer_note` / `set_interrupt`.
        let next = match env.notes() {
            Some(n) => n.try_read(),
            None => return false,
        };
        match next {
            Ok(Some(note)) => {
                if note.name == "interrupt" {
                    env.set_interrupt();
                    return true;
                }
                env.defer_note(note);
            }
            // Empty / masked-only queue or a transient read error -> no interrupt.
            _ => return false,
        }
    }
}

// ---------------------------------------------------------------------
// Interruptible foreground wait (U-7c-b)
// ---------------------------------------------------------------------
//
// A foreground command holds the shell until it exits. While it runs the
// shell is NOT reading the console (the line editor is idle), so a Ctrl-C
// cannot reach the shell as keystrokes -- the kernel console owner instead
// posts an `interrupt` note to the shell's own queue. `wait_pids_interruptible`
// turns the otherwise-blocking reap into a poll on that queue so the note is
// seen the moment it arrives and FORWARDED to the running job's pids
// (scripture 10.2: a foreground job present -> forward, do NOT run the shell's
// own `on note` handler).
//
// The reap ground truth stays the by-pid `wait_pid_for` (U-7-pre): each wake
// WNOHANG-reaps every still-live pid. The `child_exit` note (posted on every
// child exit) is the immediate poll wake; a bounded backstop timeout is the
// safety net for a coalesced / lost / mask-deferred child_exit so the wait can
// never hang. A note that is neither `interrupt` nor `child_exit` is deferred
// (Env::defer_note) to the post-command `deliver_pending_notes` -- `try_read`
// consumes the queue front, so a note read past while looking for an interrupt
// must be retained, not dropped, or its handler would be lost.
//
// DEGRADES to a plain blocking by-pid wait when the shell has no note queue
// open (host tests / the bare-spawn boot check / pre-`open_notes`): there is no
// console to deliver a Ctrl-C and nothing to forward.
//
// SCOPE at v1.0: the three direct foreground command waits route here. The
// command-substitution capture (`$(cmd)`) stays a blocking drain-before-reap
// (it polls a capture pipe, not the note fd) and the `wait` builtin stays a
// blocking bg-job reap; both gain forwarding when process groups land (U-PTY).

/// Backstop poll timeout for the foreground wait. The `child_exit` note is the
/// real wake (near-zero latency); this only bounds the rare case where that
/// note is coalesced / lost / mask-deferred, so a modest value keeps the wait
/// responsive while the kernel poll never becomes a busy loop.
const FG_WAIT_BACKSTOP_MS: u32 = 100;

/// Wait for every pid in `pids` to exit -- reaping each by pid -- while
/// forwarding any `interrupt` note (Ctrl-C) to the still-live foreground pids.
/// Returns the per-pid exit statuses in `pids` order (a vanished / not-our-
/// child pid yields 0, matching the prior reap loops). See the section note.
///
/// Re-exported from `eval` for the boot probe; the three foreground exec paths
/// (`exec_external`, `exec_external_redirected`, `exec_pipeline`) are its
/// in-tree callers.
pub fn wait_pids_interruptible(env: &mut Env, pids: &[i32]) -> Vec<i32> {
    let mut statuses: Vec<i32> = (0..pids.len()).map(|_| 0i32).collect();

    // DEGRADE: no note queue -> a plain blocking by-pid wait, no forwarding.
    if env.notes().is_none() {
        for (i, &pid) in pids.iter().enumerate() {
            let mut st: i32 = 0;
            // SAFETY: t_wait_pid_for is the SYS_WAIT_PID SVC wrapper; &mut st
            // is a valid writable i32.
            let rc = unsafe { t_wait_pid_for(pid, 0, &mut st as *mut i32) };
            statuses[i] = if rc < 0 { 0 } else { st };
        }
        return statuses;
    }

    let mut reaped: Vec<bool> = (0..pids.len()).map(|_| false).collect();
    let mut remaining = pids.len();
    while remaining > 0 {
        // 1. WNOHANG-reap every still-live pid (the reap ground truth).
        for (i, &pid) in pids.iter().enumerate() {
            if reaped[i] {
                continue;
            }
            let mut st: i32 = 0;
            // SAFETY: SVC wrapper; &mut st is a valid writable i32.
            let rc = unsafe { t_wait_pid_for(pid, T_WAIT_WNOHANG, &mut st as *mut i32) };
            if rc == 0 {
                continue; // still alive
            }
            // rc > 0: reaped -> st. rc < 0: not our child (vanished) -> 0.
            statuses[i] = if rc < 0 { 0 } else { st };
            reaped[i] = true;
            remaining -= 1;
        }
        if remaining == 0 {
            break;
        }
        // 2. Block on the note queue until a note arrives or the backstop fires.
        poll_notes_once(env);
        // 3. Drain the wake: forward interrupt, swallow child_exit, defer rest.
        drain_fg_wait_notes(env, pids, &reaped);
    }
    statuses
}

/// Block on the shell's note fd until a note is ready or `FG_WAIT_BACKSTOP_MS`
/// elapses. No-op (returns at once) if the queue is not open. A poll error
/// (closed fd / kernel reject) falls through to another WNOHANG sweep -- never
/// a hang, never a spin.
fn poll_notes_once(env: &Env) {
    let notes = match env.notes() {
        Some(n) => n,
        None => return,
    };
    let mut set = PollSet::with_capacity(1);
    set.add(notes, PollEvents::READ);
    let _ = set.poll(PollTimeout::Millis(FG_WAIT_BACKSTOP_MS));
}

/// Drain every immediately-available note during a foreground wait. `interrupt`
/// forwards to the still-live foreground pids; `child_exit` is swallowed (the
/// WNOHANG loop reaps, and consuming the note clears the fd's POLLIN so the
/// next poll genuinely blocks rather than spinning); any other note is deferred
/// to the post-command `deliver_pending_notes`. Bounded + non-blocking
/// (`try_read` polls with timeout 0).
fn drain_fg_wait_notes(env: &mut Env, pids: &[i32], reaped: &[bool]) {
    loop {
        // The immutable borrow for `notes()` ends with `try_read`; `note` is
        // owned, freeing `env` for `defer_note`.
        let next = match env.notes() {
            Some(n) => n.try_read(),
            None => return,
        };
        let note = match next {
            Ok(Some(note)) => note,
            // Empty / masked-only queue or a transient read error -> done.
            _ => return,
        };
        match note.name.as_str() {
            "interrupt" => {
                for (i, &pid) in pids.iter().enumerate() {
                    if !reaped[i] {
                        // Permission-gated to the shell's children (these pids
                        // are). Result ignored: a forward to an already-exited
                        // or native non-reader child is inert -- it matters to a
                        // pouch/musl child (async SIGINT).
                        let _ = send(NoteTarget::Pid(pid), "interrupt");
                    }
                }
            }
            // The poll wake; the WNOHANG loop is the reap truth. Swallow so the
            // fd stops advertising POLLIN.
            "child_exit" => {}
            // A handler-bearing note (user note / `pipe`): hold it for the
            // post-command drain so its `on note` body fires at the next sync
            // point -- we never run shell handlers mid-wait.
            _ => env.defer_note(note),
        }
    }
}

/// Dispatch one delivered note (U-7c). A registered `on note` handler wins
/// (scripture 10.7); otherwise the built-in disposition applies. At a sync
/// point no foreground job is running, so an unhandled `interrupt` is benign
/// (the running-foreground forward is U-7c-b); `child_exit` is informational
/// (the WNOHANG reaper is the reap ground truth); other classes are ignored.
/// Dispatch one drained note. A registered `on note <name>` handler fires
/// (best-effort, like a bash trap). Returns true iff this was an UNHANDLED
/// `interrupt` -- the LS-8c reactive-Ctrl-C-mid-edit signal the idle poll loop
/// reads to cancel the in-progress line edit. Every other unhandled note takes
/// its built-in disposition (a `child_exit` is reaped by `reap_jobs`; a
/// `pipe`/user note is a benign discard at this drain point).
fn dispatch_note(env: &mut Env, note: &Note) -> bool {
    if let Some(body) = env.note_handler_get(&note.name).cloned() {
        // A handler is best-effort, like an rc `on note` body / a bash trap:
        // evaluate it for its side effects, swallowing a body error (the
        // failing statement records it in $errstr). The body runs to
        // completion before the next note is drained, so handlers for
        // distinct notes never interleave (N-3-style serialization at the
        // shell layer).
        let _ = eval_block(env, &body);
        false
    } else {
        // A caught interrupt would have fired a handler above; an UNHANDLED
        // interrupt is the shell's own reactive-cancel signal (LS-8c).
        note.name == "interrupt"
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn trim_ascii_ws_strips_both_ends() {
        assert_eq!(trim_ascii_ws(b"  hi \t"), b"hi");
        assert_eq!(trim_ascii_ws(b"hi"), b"hi");
        assert_eq!(trim_ascii_ws(b"   "), b"");
        assert_eq!(trim_ascii_ws(b""), b"");
        assert_eq!(trim_ascii_ws(b"\t a b \t"), b"a b");
    }

    #[test]
    fn shebang_interp_only() {
        // D2: the common case -- `#!/bin/ut\n`.
        let (interp, arg) = parse_shebang_line(b"#!/bin/ut\necho hi\n").unwrap();
        assert_eq!(interp, "/bin/ut");
        assert_eq!(arg, None);
    }

    #[test]
    fn shebang_interp_with_one_arg() {
        // A single optional argument (Linux/BSD convention) is the trimmed
        // remainder of the line verbatim.
        let (interp, arg) = parse_shebang_line(b"#!/bin/awk -f\nBEGIN{}\n").unwrap();
        assert_eq!(interp, "/bin/awk");
        assert_eq!(arg.as_deref(), Some("-f"));
    }

    #[test]
    fn shebang_tolerates_leading_ws_and_crlf() {
        // Leading spaces after `#!` are skipped; a CR ends the line (CRLF file).
        let (interp, arg) = parse_shebang_line(b"#!   /bin/ut\r\n").unwrap();
        assert_eq!(interp, "/bin/ut");
        assert_eq!(arg, None);
    }

    #[test]
    fn shebang_arg_is_not_word_split() {
        // Everything after the interpreter + one space is ONE argument
        // (no further splitting), matching the kernel exec convention.
        let (interp, arg) = parse_shebang_line(b"#!/bin/ut --flag a b\n").unwrap();
        assert_eq!(interp, "/bin/ut");
        assert_eq!(arg.as_deref(), Some("--flag a b"));
    }

    #[test]
    fn not_a_shebang_is_none() {
        // An ELF (or any non-`#!` file) is not a script -> passes through.
        assert!(parse_shebang_line(b"\x7fELF\x02\x01\x01\x00").is_none());
        assert!(parse_shebang_line(b"echo hi\n").is_none());
        assert!(parse_shebang_line(b"").is_none());
        // `#!` with an empty interpreter line is not a script.
        assert!(parse_shebang_line(b"#!\n").is_none());
        assert!(parse_shebang_line(b"#!   \n").is_none());
    }

    #[test]
    fn shebang_without_trailing_newline() {
        // A file that is exactly the shebang line with no `\n` still parses
        // (the whole 128-byte read is the line).
        let (interp, arg) = parse_shebang_line(b"#!/bin/ut").unwrap();
        assert_eq!(interp, "/bin/ut");
        assert_eq!(arg, None);
    }
}
