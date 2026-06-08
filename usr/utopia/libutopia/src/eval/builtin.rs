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
// Built-in stdout (pwd / type output) goes via `t_putstr` (the kernel
// UART), the same v1.0 convention the rest of the shell's diagnostic
// output uses -- the shell has no terminal-backed fd 1 until U-6g/PTY,
// at which point this switches to `io::stdout()`.
//
// === Deferred (need substrate absent at v1.0) ===
//
//   set / export        -- envp passing to children does not exist
//                          (SYS_SPAWN carries argv only); export is
//                          meaningless until it does.
//   alias / unalias     -- needs an alias table + pre-dispatch
//                          expansion (a later U-6 sub-chunk).
//   read                -- needs a terminal-backed fd 0 (U-6g/PTY).
//   wait / jobs / fg / bg -- job control (U-7).
//   history             -- needs the line editor's history (U-6g).
//   kill / note         -- the notes argv surface.
//   echo / printf / test / [ / time / palette / bind / mount / ... --
//                          their own surfaces; echo/printf already
//                          exist as external coreutils.
//
// A name outside the implemented set is NOT intercepted here
// (`try_builtin` returns `None`), so the caller falls through to an
// external spawn. `is_builtin` reports ONLY the implemented set, so
// `type` answers truthfully about what the shell actually intercepts.

use alloc::string::{String, ToString};
use alloc::vec::Vec;
use core::fmt::Write as _;

use libthyla_rs::fs::{self, Component, Path};
use libthyla_rs::io::Read as _;
use libthyla_rs::t_putstr;

use super::env::Env;
use super::error::EvalResult;
use super::stmt::{eval_source, StatementFlow};
use super::value::Value;

/// Whether `name` is a built-in the shell intercepts (the implemented
/// set). Used by `type`/`whence` and by the redirect/pipeline guards
/// in `stmt.rs`.
pub fn is_builtin(name: &str) -> bool {
    matches!(
        name,
        "cd" | "pwd" | "exit" | "true" | "false" | "unset" | "eval" | "source" | "." | "type"
            | "whence"
    )
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

    // Commit: $oldpwd <- the directory we are leaving; $cwd <- target.
    let old = env.cwd().to_string();
    env.cwd_set(target.clone());
    env.assign("oldpwd", Value::scalar(old));
    if announce {
        let mut line = target;
        line.push('\n');
        t_putstr(&line);
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
    t_putstr(&line);
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
        if is_builtin(name) {
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
        t_putstr(&line);
    }
    env.status_set(0);
    Ok(StatementFlow::Normal)
}
