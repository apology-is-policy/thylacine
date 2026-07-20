// libutopia::eval::builtin -- shell built-in commands (scripture 9.1).
//
// === Dispatch position ===
//
// `eval_command` resolves a SimpleCommand's argv[0] in the order
// fn -> BUILTIN -> external. A built-in runs IN-PROCESS because it
// mutates the shell's own state (cwd, variables, the scope stack) or
// evaluates source in the current scope (`source` / `eval`). External
// coreutils (echo, cat, ls, grep, ...) are NOT built-ins (scripture
// 9.6) -- they spawn as their own Procs.
//
// === Implemented at U-6e-a ===
//
// The scripture-9.1 "must be internal" set that needs no substrate
// beyond what the kernel exposes today:
//
//   cd          change the shell cwd ($cwd / $oldpwd; `cd -` toggle;
//               `cd` with no arg -> $home, falling back to "/").
//   pwd         print the shell cwd.
//   exit [code] terminate the shell (Env::request_exit + the
//               eval_block short-circuit).
//   true/false  set $status to 0 / 1.
//   unset NAME. remove variable bindings.
//   eval ARGS.  join ARGS and evaluate them in the current scope.
//   source FILE (also `.`) read FILE and evaluate it in the current
//               scope.
//   type/whence report whether a name is a builtin, function, or
//               external command.
//
// Built-in stdout (pwd / type output) goes via `Env::emit_line`
// (PTY-4b): fd 1 when the shell holds a live terminal -- the console
// session AND a pts-hosted session, where the old `t_putstr` (SYS_PUTS =
// the raw UART) would bypass the terminal entirely -- falling back to the
// UART on the fd-less boot check / host paths. This is the "switches to
// io::stdout() at PTY" the U-6e note promised.
//
// === Implemented at U-7b (job control) ===
//
// The job-control set, over the U-7a `JobTable` (in `Env`) + the U-7-pre
// `wait_pid_for` primitive (UTOPIA-SHELL-DESIGN.md section 10.6):
//
//   jobs        list tracked jobs as `[N]{+|-} Running|Stopped|Done cmd`
//               (refreshes against current truth via a WNOHANG poll, then
//               consumes the reported-Done jobs so the prompt cycle does
//               not re-announce them).
//   wait [id...] block until the named jobs/pids finish, or all jobs with
//               no argument. Interruptible: a ^C aborts the builtin (130,
//               bash) -- mandatory under jc, where a wait on a STOPPED job
//               would otherwise self-deadlock the shell (PTY-4e F1).
//   fg [%N]     foreground a job. Under job control (PTY-4b, a pts
//               session): terminal to the job's group + SYS_TTY_CONT
//               resume + the stop-aware wait (a re-^Z re-stops it into
//               the table). On the console: block on a RUNNING job to
//               completion + remove it silently (the U-7b behavior).
//   bg [%N]     resume a STOPPED job in the background (SYS_TTY_CONT,
//               no terminal handoff) under job control; else report the
//               job already running (bash-faithful).
//   kill {%N|pid} [interrupt|kill]
//               post a note to a job's pids / a pid. Default `interrupt`
//               (the catchable Ctrl-C analogue); `kill` is the non-
//               catchable force. The kernel's userspace-postable set is
//               {interrupt, kill} -- `snare:*` is kernel-synthetic-only,
//               so scripture's `snare:term` default is unsendable from
//               userspace and `interrupt` is the graceful default.
//
// === Deferred (need substrate absent at v1.0) ===
//
//   set / export        -- envp passing to children does not exist
//                          (SYS_SPAWN carries argv only); export is
//                          meaningless until it does.
//   alias / unalias     -- the alias TABLE + pre-dispatch expansion now
//                          exist (Env::aliases, seeded `la`/`ll`; expanded
//                          in eval_command). The `alias`/`unalias` BUILTINS
//                          that let a user define their own (+ a `~/.utrc`
//                          to seed them, + recursive-expansion loop guards)
//                          are the remaining later sub-chunk.
//   read                -- needs a terminal-backed fd 0 (U-6g/PTY).
//   history             -- needs the line editor's history (U-6g).
//   note                -- `note send/list/wait`, the richer notes argv
//                          surface (U-7c); `kill` already covers posting.
//   echo / printf / test / [ / time / palette / bind / mount / ... --
//                          their own surfaces; echo/printf already
//                          exist as external coreutils.
//
// A name outside the implemented set is NOT intercepted here
// (`try_builtin` returns `None`), so the caller falls through to an
// external spawn. `is_builtin` reports ONLY the implemented set, so
// `type` answers truthfully about what the shell actually intercepts.

use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;
use core::fmt::Write as _;

use libthyla_rs::fs::{self, Component, Path};
use libthyla_rs::io::Read as _;
use libthyla_rs::notes::{self, NoteTarget};
use libthyla_rs::{t_wait_pid_for, T_WAIT_WNOHANG};

use super::env::Env;
use super::error::EvalResult;
use super::stmt::{
    eval_source, poll_notes_once, run_foreground_jc, wait_pids_interruptible, StatementFlow,
};
use super::value::Value;

/// Whether `name` is a built-in the shell intercepts (the implemented
/// set). Used by `type`/`whence` and by the redirect/pipeline guards
/// in `stmt.rs`.
/// The built-in command names -- the single source of truth that `is_builtin`
/// tests membership of and that the #115a Tab-completion / coloring command
/// index enumerates (`builtin_names`). Keep aligned with `try_builtin`'s
/// dispatch arms.
pub const BUILTIN_NAMES: &[&str] = &[
    "cd", "pwd", "exit", "true", "false", "unset", "eval", "source", ".", "type",
    "whence", "jobs", "fg", "bg", "wait", "kill",
];

/// The built-in names, for the #115a completion command index.
pub fn builtin_names() -> &'static [&'static str] {
    BUILTIN_NAMES
}

pub fn is_builtin(name: &str) -> bool {
    BUILTIN_NAMES.contains(&name)
}

/// Try to run argv[0] as a built-in. Returns `Some(result)` if argv[0]
/// is an implemented built-in (the result carries its StatementFlow +
/// the $status / $cwd / scope side effects it imposed on `env`); `None`
/// if argv[0] is not a built-in, in which case the caller spawns it as
/// an external command.
///
/// A built-in's runtime failure (bad argument, missing directory,
/// unreadable file) is reflected via `$status` + `$errstr` and returns
/// `Ok(StatementFlow::Normal)` -- matching `exec_external`, so the
/// caller's implicit-fail discipline picks it up. Only `eval` / `source`
/// can return a non-Normal flow (the evaluated source's flow) or an
/// `Err` (a genuine eval error in the evaluated source).
pub fn try_builtin(env: &mut Env, argv: &[String]) -> Option<EvalResult<StatementFlow>> {
    if argv.is_empty() {
        return None;
    }
    let args = &argv[1..];
    let r = match argv[0].as_str() {
        "cd" => bi_cd(env, args),
        "pwd" => bi_pwd(env),
        "exit" => bi_exit(env, args),
        "true" => {
            env.status_set(0);
            Ok(StatementFlow::Normal)
        }
        "false" => {
            env.status_set(1);
            Ok(StatementFlow::Normal)
        }
        "unset" => bi_unset(env, args),
        "eval" => bi_eval(env, args),
        "source" | "." => bi_source(env, args),
        "type" | "whence" => bi_type(env, args),
        "jobs" => bi_jobs(env, args),
        "fg" => bi_fg(env, args),
        "bg" => bi_bg(env, args),
        "wait" => bi_wait(env, args),
        "kill" => bi_kill(env, args),
        _ => return None,
    };
    Some(r)
}

/// A runtime built-in failure: set `$errstr` + `$status` and report
/// Normal (the implicit-fail path takes it from there).
fn fail(env: &mut Env, msg: String, code: i32) -> EvalResult<StatementFlow> {
    env.errstr_set(msg);
    env.status_set(code);
    Ok(StatementFlow::Normal)
}

// ---------------------------------------------------------------------
// cd
// ---------------------------------------------------------------------

fn bi_cd(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.len() > 1 {
        return fail(env, "cd: too many arguments".to_string(), 1);
    }
    // Resolve the target absolute path + whether to echo it (bash
    // prints the destination on `cd -`).
    let (target, announce) = if args.is_empty() {
        let home = env.get("home").as_scalar();
        let raw = if home.is_empty() { "/".to_string() } else { home };
        (normalize_abs(env.cwd(), &raw), false)
    } else if args[0] == "-" {
        let old = env.get("oldpwd").as_scalar();
        if old.is_empty() {
            return fail(env, "cd: OLDPWD not set".to_string(), 1);
        }
        (old, true) // $oldpwd was stored already-normalized
    } else {
        (normalize_abs(env.cwd(), &args[0]), false)
    };

    // The root "/" is the territory root_spoor -- a directory by
    // construction, so it bypasses the fstat probe. (It must: libthyla_rs
    // File::open cannot open a zero-component path, so fs::is_dir("/")
    // is always false -- a separate fs gap tracked as #929. An fstat of
    // an invariant truth is unnecessary regardless.)
    if target != "/" && !fs::is_dir(&target) {
        let mut s = String::new();
        let _ = write!(&mut s, "cd: {}: not a directory", target);
        return fail(env, s, 1);
    }

    // LS-4: tell the KERNEL (SYS_CHDIR) so spawned children inherit this cwd and
    // resolve their relative paths against it. `target` is already absolute
    // (normalize_abs). The kernel re-validates (directory + search/X); a failure
    // here (e.g. no X on the target itself, which the is_dir probe above does not
    // catch) aborts the cd. On success $cwd mirrors the same target.
    if libthyla_rs::env::set_current_dir(&target).is_err() {
        let mut s = String::new();
        let _ = write!(&mut s, "cd: {}: cannot change directory", target);
        return fail(env, s, 1);
    }

    // Commit: $oldpwd <- the directory we are leaving; $cwd <- target.
    let old = env.cwd().to_string();
    env.cwd_set(target.clone());
    env.assign("oldpwd", Value::scalar(old));
    if announce {
        let mut line = target;
        line.push('\n');
        env.emit_line(&line);
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

/// Lexically resolve `arg` against `cwd` into a clean absolute path,
/// collapsing `.` and `..` and duplicate separators. The kernel stalk
/// resolver also resolves `..` (I-28), but storing the clean form keeps
/// `$cwd` tidy for display and for subsequent joins. An absolute `arg`
/// ignores `cwd`; `..` past the root is clamped at the root (matching
/// the kernel's containment).
fn normalize_abs(cwd: &str, arg: &str) -> String {
    let cwd_path = Path::new(cwd);
    let arg_path = Path::new(arg);
    let mut stack: Vec<&str> = Vec::new();
    if !arg_path.is_absolute() {
        for c in cwd_path.components() {
            match c {
                Component::ParentDir => {
                    stack.pop();
                }
                Component::Normal(s) => stack.push(s),
                Component::RootDir | Component::CurDir => {}
            }
        }
    }
    for c in arg_path.components() {
        match c {
            Component::RootDir => stack.clear(),
            Component::ParentDir => {
                stack.pop();
            }
            Component::Normal(s) => stack.push(s),
            Component::CurDir => {}
        }
    }
    if stack.is_empty() {
        return "/".to_string();
    }
    let mut s = String::new();
    for seg in &stack {
        s.push('/');
        s.push_str(seg);
    }
    s
}

// ---------------------------------------------------------------------
// pwd
// ---------------------------------------------------------------------

fn bi_pwd(env: &mut Env) -> EvalResult<StatementFlow> {
    let mut line = env.cwd().to_string();
    line.push('\n');
    env.emit_line(&line);
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

// ---------------------------------------------------------------------
// exit
// ---------------------------------------------------------------------

fn bi_exit(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    // No argument -> exit with the current $status (shell tradition).
    // A non-numeric argument -> status 2 (bash "numeric argument
    // required"); a numeric argument -> that code.
    let code = if args.is_empty() {
        env.status()
    } else {
        match args[0].parse::<i32>() {
            Ok(n) => n,
            Err(_) => {
                env.errstr_set("exit: numeric argument required");
                2
            }
        }
    };
    env.status_set(code);
    env.request_exit(code);
    Ok(StatementFlow::Normal)
}

// ---------------------------------------------------------------------
// unset
// ---------------------------------------------------------------------

fn bi_unset(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    for name in args {
        env.unset(name);
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

// ---------------------------------------------------------------------
// eval
// ---------------------------------------------------------------------

fn bi_eval(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.is_empty() {
        env.status_set(0);
        return Ok(StatementFlow::Normal);
    }
    let src = args.join(" ");
    // Evaluate in the CURRENT scope: assignments, exit, and control
    // flow all act on this Env (matching shell `eval`).
    eval_source(env, &src)
}

// ---------------------------------------------------------------------
// source / .
// ---------------------------------------------------------------------

fn bi_source(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.is_empty() {
        return fail(env, "source: filename required".to_string(), 1);
    }
    let path = &args[0];
    let mut f = match fs::File::open(path) {
        Ok(f) => f,
        Err(e) => {
            let mut s = String::new();
            let _ = write!(&mut s, "source: {}: {:?}", path, e);
            return fail(env, s, 1);
        }
    };
    let mut buf = Vec::new();
    if let Err(e) = f.read_to_end(&mut buf) {
        let mut s = String::new();
        let _ = write!(&mut s, "source: {}: read failed: {:?}", path, e);
        return fail(env, s, 1);
    }
    let src = match core::str::from_utf8(&buf) {
        Ok(s) => s,
        Err(_) => return fail(env, "source: file is not valid UTF-8".to_string(), 1),
    };
    eval_source(env, src)
}

// ---------------------------------------------------------------------
// type / whence
// ---------------------------------------------------------------------

fn bi_type(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    for name in args {
        let mut line = String::new();
        if let Some(exp) = env.alias_lookup(name) {
            let _ = write!(&mut line, "{} is aliased to `{}`\n", name, exp.join(" "));
        } else if is_builtin(name) {
            let _ = write!(&mut line, "{} is a shell builtin\n", name);
        } else if env.fn_defined(name) {
            let _ = write!(&mut line, "{} is a function\n", name);
        } else {
            // The kernel resolves external names dynamically at spawn
            // (devramfs / the pivoted root), so the shell cannot know
            // whether the binary exists without probing; report
            // "external" (the name would be spawned as a command).
            let _ = write!(&mut line, "{} is external\n", name);
        }
        env.emit_line(&line);
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

// ---------------------------------------------------------------------
// Job control (U-7b): jobs / fg / bg / wait / kill
// ---------------------------------------------------------------------

/// WNOHANG-poll every live background pid and feed the result into the job
/// table (the U-7-pre `wait_pid_for` ground truth). Marks reaped pids; does
/// NOT drain `[N]+ Done` notifications -- the REPL prompt cycle (`reap_jobs`)
/// and the `jobs` builtin own that, so a job is announced exactly once.
/// Shared by `Repl::reap_jobs` and the `jobs` refresh. ONE poll per pid --
/// never a busy-loop (U-7-pre F1: a hot WNOHANG loop can starve the awaited
/// child).
pub fn reap_background(env: &mut Env) {
    for pid in env.jobs().live_pids() {
        let mut st: i32 = 0;
        // SAFETY: t_wait_pid_for is the SYS_WAIT_PID SVC wrapper; &mut st is a
        // valid writable i32. WNOHANG -> pid (reaped) / 0 (alive) / -1 (gone).
        let rc = unsafe { t_wait_pid_for(pid, T_WAIT_WNOHANG, &mut st as *mut i32) };
        if rc > 0 {
            env.jobs_mut().mark_reaped(pid, st);
        } else if rc < 0 {
            // No longer our child (already gone). Treat as reaped so the job
            // can complete + be removed -- never left dangling.
            env.jobs_mut().mark_reaped(pid, 0);
        }
        // rc == 0: still running -- leave it for a later poll.
    }
}

/// The `wait`-builtin reap: block until every pid exits, feeding reaps into
/// the table -- INTERRUPTIBLE when the shell has a live note queue. A `^C`
/// (`interrupt` note) ABORTS the builtin (bash: `wait` is interruptible,
/// returning 128+SIGINT) rather than forwarding it -- background jobs are
/// keyboard-immune, and under job control a job-STOPPED pid never exits, so
/// an uninterruptible `wait` on one was an unrecoverable self-deadlock: the
/// wedged shell reads no input and is itself the only resumer of the stopped
/// job (PTY-4e F1). `child_exit` is swallowed (the WNOHANG sweep is the reap
/// ground truth; consuming it clears POLLIN so the next poll genuinely
/// parks); other notes defer to the post-command drain. DEGRADES to the
/// plain blocking by-pid wait when no note queue is open (host tests / the
/// bare-spawn boot check -- no console, so no `^C` can arrive). Returns
/// (last reaped status, interrupted); an already-reaped / vanished pid is
/// marked reaped + skipped (never spins), matching the prior loop.
fn wait_pids_reaping(env: &mut Env, pids: &[i32]) -> (i32, bool) {
    let mut last = 0;
    if env.notes().is_none() {
        for &pid in pids {
            let mut st: i32 = 0;
            // SAFETY: SYS_WAIT_PID SVC wrapper; &mut st is a valid writable i32.
            let rc = unsafe { t_wait_pid_for(pid, 0, &mut st as *mut i32) };
            if rc > 0 {
                env.jobs_mut().mark_reaped(pid, st);
                last = st;
            } else if rc < 0 {
                env.jobs_mut().mark_reaped(pid, 0);
            }
        }
        return (last, false);
    }
    let mut reaped: Vec<bool> = (0..pids.len()).map(|_| false).collect();
    let mut remaining = pids.len();
    while remaining > 0 {
        for (i, &pid) in pids.iter().enumerate() {
            if reaped[i] {
                continue;
            }
            let mut st: i32 = 0;
            // SAFETY: SYS_WAIT_PID SVC wrapper; &mut st is a valid writable i32.
            let rc = unsafe { t_wait_pid_for(pid, T_WAIT_WNOHANG, &mut st as *mut i32) };
            if rc == 0 {
                continue; // still alive (or stopped -- either way, not exited)
            }
            if rc > 0 {
                env.jobs_mut().mark_reaped(pid, st);
                last = st;
            } else {
                env.jobs_mut().mark_reaped(pid, 0);
            }
            reaped[i] = true;
            remaining -= 1;
        }
        if remaining == 0 {
            break;
        }
        poll_notes_once(env);
        loop {
            let next = match env.notes() {
                Some(n) => n.try_read(),
                None => break,
            };
            let note = match next {
                Ok(Some(note)) => note,
                _ => break,
            };
            match note.name.as_str() {
                "interrupt" => return (130, true),
                "child_exit" => {}
                _ => env.defer_note(note),
            }
        }
    }
    (last, false)
}

/// Resolve a `%`-prefixed jobspec to an EXISTING job's `[N]` spec. Accepts
/// `%N`, `%%`/`%+` (current job), `%-` (previous job). Err carries a
/// user-facing reason WITHOUT a builtin-name prefix (the caller adds it).
fn resolve_jobspec(env: &Env, arg: &str) -> Result<u32, String> {
    let rest = arg.strip_prefix('%').unwrap_or(arg);
    let spec = match rest {
        "" | "%" | "+" => env
            .jobs()
            .current_spec()
            .ok_or_else(|| "no current job".to_string())?,
        "-" => env
            .jobs()
            .previous_spec()
            .ok_or_else(|| "no previous job".to_string())?,
        n => n.parse::<u32>().map_err(|_| {
            let mut s = String::new();
            let _ = write!(&mut s, "invalid jobspec: {}", arg);
            s
        })?,
    };
    if env.jobs().spec_pids(spec).is_some() {
        Ok(spec)
    } else {
        let mut s = String::new();
        let _ = write!(&mut s, "no such job: %{}", spec);
        Err(s)
    }
}

/// Resolve `fg`/`bg`'s optional argument to an existing job spec. No argument
/// -> the current job. A `%`-prefixed arg goes through `resolve_jobspec`; a
/// bare decimal is a job NUMBER (bash treats fg/bg bare numbers as jobspecs,
/// unlike wait/kill where a bare number is a pid). The returned Err is already
/// builtin-name-prefixed.
fn resolve_target_job(env: &Env, arg: Option<&str>, builtin: &str) -> Result<u32, String> {
    let prefix = |m: &str| {
        let mut s = String::new();
        let _ = write!(&mut s, "{}: {}", builtin, m);
        s
    };
    match arg {
        None => env
            .jobs()
            .current_spec()
            .ok_or_else(|| prefix("no current job")),
        Some(a) if a.starts_with('%') => resolve_jobspec(env, a).map_err(|m| prefix(&m)),
        Some(a) => match a.parse::<u32>() {
            Ok(spec) if env.jobs().spec_pids(spec).is_some() => Ok(spec),
            Ok(spec) => {
                let mut s = String::new();
                let _ = write!(&mut s, "{}: no such job: {}", builtin, spec);
                Err(s)
            }
            Err(_) => {
                let mut s = String::new();
                let _ = write!(&mut s, "{}: invalid jobspec: {}", builtin, a);
                Err(s)
            }
        },
    }
}

/// `jobs` -- list the tracked background jobs. Refreshes against current truth
/// (a WNOHANG poll) so a job that finished since the last prompt shows as Done,
/// then CONSUMES the reported-Done jobs (one-shot `notified`) so the prompt
/// cycle does not re-announce them -- exactly-once reporting whether observed
/// here or at the prompt (bash semantics).
fn bi_jobs(env: &mut Env, _args: &[String]) -> EvalResult<StatementFlow> {
    reap_background(env);
    let current = env.jobs().current_spec();
    let previous = env.jobs().previous_spec();
    let mut out = String::new();
    for job in env.jobs().iter() {
        let mark = if Some(job.spec()) == current {
            '+'
        } else if Some(job.spec()) == previous {
            '-'
        } else {
            ' '
        };
        // PTY-4b: a live job may be job-stopped (`^Z`); Done only when fully
        // reaped (a stopped job structurally cannot be, see jobs.rs).
        let state = if !job.is_running() {
            "Done"
        } else if job.stopped() {
            "Stopped"
        } else {
            "Running"
        };
        let _ = write!(&mut out, "[{}]{}  {:7}  {}\n", job.spec(), mark, state, job.cmd());
    }
    if !out.is_empty() {
        env.emit_line(&out);
    }
    // Consume the Done jobs we just listed (discard the formatted lines -- the
    // listing above already reported them) so the prompt cycle stays quiet.
    let _ = env.jobs_mut().take_done_notifications();
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

/// `fg [%N]` -- bring a job to the foreground. Under job control (PTY-4b, a
/// pts session): the terminal goes to the job's process group, a stopped job
/// is resumed via `SYS_TTY_CONT`, and the wait is stop-aware -- a re-`^Z`
/// re-stops it back into the table. On a clean finish the job is removed
/// silently (its completion is this command's result, not a `[N]+ Done`
/// event); $status is the job's exit (bash). On the console (no jc) the
/// U-7b behavior is unchanged: block on a RUNNING job to completion.
fn bi_fg(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.len() > 1 {
        return fail(env, "fg: too many arguments".to_string(), 1);
    }
    let spec = match resolve_target_job(env, args.first().map(|s| s.as_str()), "fg") {
        Ok(s) => s,
        Err(m) => return fail(env, m, 1),
    };
    // Echo the command being foregrounded (bash prints the job's command line).
    let cmd = env.jobs().cmd_of(spec).unwrap_or("").to_string();
    {
        let mut line = cmd.clone();
        line.push_str("\r\n");
        env.emit_line(&line);
    }
    let jc_pgid = env
        .job_control
        .and_then(|_| env.jobs().pgid_of(spec))
        .filter(|&g| g != 0);
    if let Some(pgid) = jc_pgid {
        // PTY-4b: wait on the LIVE elements only (already-reaped ones keep
        // their recorded statuses). The resume (`SYS_TTY_CONT`) is posted
        // whether the job was Stopped or Running -- POSIX SIGCONT on a
        // running group is a harmless note.
        let pids = env.jobs().live_pids_of(spec).unwrap_or_default();
        if pids.is_empty() {
            // Everything already exited; consume the job silently.
            env.jobs_mut().remove(spec);
            env.status_set(0);
            return Ok(StatementFlow::Normal);
        }
        env.jobs_mut().set_stopped(spec, false);
        let statuses = run_foreground_jc(env, &pids, pgid, cmd, true, Some(spec));
        // A re-stop keeps the job in the table (run_foreground_jc re-marked
        // it); a finish removed it. $status: the last element's outcome.
        env.status_set(statuses.last().copied().unwrap_or(0));
        return Ok(StatementFlow::Normal);
    }
    // The console (non-jc) fallback: a foregrounded job IS a foreground wait,
    // so use the fg-semantics interruptible reap (a `^C` FORWARDS to the job,
    // exactly like any foreground command) rather than an unbreakable block.
    let pids = env.jobs().spec_pids(spec).unwrap_or_default();
    let statuses = wait_pids_interruptible(env, &pids);
    env.jobs_mut().remove(spec);
    env.status_set(statuses.last().copied().unwrap_or(0));
    Ok(StatementFlow::Normal)
}

/// `bg [%N]` -- resume a stopped job in the background. Under job control
/// (PTY-4b): `SYS_TTY_CONT` on the job's group, no terminal handoff -- the
/// job runs backgrounded and its exit surfaces as the usual `[N]+ Done`.
/// A job that is not stopped reports "already running" (bash-faithful).
/// On the console (no jc) there is never a stopped job to resume.
fn bi_bg(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.len() > 1 {
        return fail(env, "bg: too many arguments".to_string(), 1);
    }
    let spec = match resolve_target_job(env, args.first().map(|s| s.as_str()), "bg") {
        Ok(s) => s,
        Err(m) => return fail(env, m, 1),
    };
    let jc_pgid = env
        .job_control
        .and_then(|_| env.jobs().pgid_of(spec))
        .filter(|&g| g != 0);
    if env.jobs().is_stopped(spec) {
        if let Some(pgid) = jc_pgid {
            env.jobs_mut().set_stopped(spec, false);
            // SAFETY: t_tty_cont is the SYS_TTY_CONT SVC wrapper on fd 0
            // (the pts slave the session dance validated).
            let _ = unsafe { libthyla_rs::t_tty_cont(0, pgid) };
            let mut line = String::new();
            let _ = write!(
                &mut line,
                "[{}]+ {} &\r\n",
                spec,
                env.jobs().cmd_of(spec).unwrap_or("")
            );
            env.emit_line(&line);
            env.status_set(0);
            return Ok(StatementFlow::Normal);
        }
    }
    let mut line = String::new();
    let _ = write!(&mut line, "bg: job [{}] already running in background\n", spec);
    env.emit_line(&line);
    env.status_set(0);
    Ok(StatementFlow::Normal)
}

/// `wait [id...]` -- block until the named jobs/pids finish, or ALL background
/// jobs with no argument. A `%`-prefixed id is a jobspec; a bare decimal is a
/// child pid (bash). $status is the last id's reported exit (0 for the
/// wait-all form). The completed jobs stay in the table; the prompt cycle (or
/// a later `jobs`) emits their `[N]+ Done` lines -- bash prints those after
/// `wait` too.
fn bi_wait(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.is_empty() {
        let pids = env.jobs().live_pids();
        let (_, intr) = wait_pids_reaping(env, &pids);
        env.status_set(if intr { 130 } else { 0 });
        return Ok(StatementFlow::Normal);
    }
    let mut last = 0;
    for arg in args {
        let pids = if arg.starts_with('%') {
            let spec = match resolve_jobspec(env, arg) {
                Ok(s) => s,
                Err(m) => {
                    let mut s = String::new();
                    let _ = write!(&mut s, "wait: {}", m);
                    return fail(env, s, 1);
                }
            };
            env.jobs().spec_pids(spec).unwrap_or_default()
        } else {
            match arg.parse::<i32>() {
                Ok(p) if p > 0 => vec![p],
                _ => {
                    let mut s = String::new();
                    let _ = write!(&mut s, "wait: invalid target: {}", arg);
                    return fail(env, s, 1);
                }
            }
        };
        let (st, intr) = wait_pids_reaping(env, &pids);
        if intr {
            env.status_set(130);
            return Ok(StatementFlow::Normal);
        }
        last = st;
    }
    env.status_set(last);
    Ok(StatementFlow::Normal)
}

/// `kill {%N | pid} [interrupt | kill]` -- post a note to a job's element pids
/// or to a single pid. Default note `interrupt` (the catchable Ctrl-C
/// analogue); `kill` is the non-catchable force. The kernel's userspace-
/// postable set is {interrupt, kill}; `snare:*` is kernel-synthetic-only, so
/// scripture's `snare:term` default is unsendable from userspace -- `interrupt`
/// is the graceful default.
fn bi_kill(env: &mut Env, args: &[String]) -> EvalResult<StatementFlow> {
    if args.is_empty() || args.len() > 2 {
        return fail(
            env,
            "kill: usage: kill {%job | pid} [interrupt | kill]".to_string(),
            1,
        );
    }
    let target = &args[0];
    let name = if args.len() == 2 { args[1].as_str() } else { "interrupt" };
    if name.starts_with("snare:") {
        return fail(env, "kill: snare:* notes are kernel-reserved".to_string(), 1);
    }
    let pids = if target.starts_with('%') {
        let spec = match resolve_jobspec(env, target) {
            Ok(s) => s,
            Err(m) => {
                let mut s = String::new();
                let _ = write!(&mut s, "kill: {}", m);
                return fail(env, s, 1);
            }
        };
        env.jobs().spec_pids(spec).unwrap_or_default()
    } else {
        match target.parse::<i32>() {
            Ok(p) if p > 0 => vec![p],
            _ => {
                let mut s = String::new();
                let _ = write!(&mut s, "kill: invalid target: {}", target);
                return fail(env, s, 1);
            }
        }
    };
    if pids.is_empty() {
        return fail(env, "kill: no matching processes".to_string(), 1);
    }
    let mut send_err = false;
    for pid in pids {
        if notes::send(NoteTarget::Pid(pid), name).is_err() {
            send_err = true;
        }
    }
    if send_err {
        return fail(
            env,
            "kill: send failed (no such process or not a child)".to_string(),
            1,
        );
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}
