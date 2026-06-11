// libutopia::eval::env -- the evaluator's runtime state.
//
// === The scope stack ===
//
// Scripture (UTOPIA-SHELL-DESIGN.md section 6.2): "Dynamic by
// default: a variable assigned in a function is visible in callees.
// `let` inside a function declares a local that shadows any outer
// binding for the function's scope. Variables assigned at
// script-top-level are global."
//
// Concretely:
//   - The `let x = v` statement creates a new binding in the
//     TOPMOST frame.
//   - Bare `x = v` assignment writes to the topmost frame that
//     ALREADY has the name; if no frame has it, writes to the
//     bottom (global) frame.
//   - `$x` lookup walks the stack from top (innermost) to bottom
//     (global) and returns the first hit.
//   - Function-call entry pushes a new frame. Function-call exit
//     pops it. Brace-block `{ ... }` does NOT push a frame (it
//     shares scope with its caller per scripture 5.6).
//   - Subshell `( ... )` is handled by a separate Env clone at the
//     subshell's spawn site -- not by frame push (the subshell is
//     a forked process with its own Env).
//
// The frame is a `BTreeMap<String, Value>`. We pick BTreeMap (not
// HashMap) because:
//   - `alloc::collections::BTreeMap` is available in no_std + alloc;
//     HashMap is in std and would require pulling in hashbrown.
//   - Variable counts per frame are typically small (< 50); the
//     O(log n) lookup is negligible.
//   - Deterministic iteration order is a nice property for debug
//     printing.
//
// === Function table ===
//
// `fn name { ... }` registers a function in the Env's `fns` table.
// Function definitions are globally scoped per scripture 5.5 (no
// mention of function-local function definitions; the rc tradition
// is global). When a SimpleCommand's argv[0] matches a name in
// `fns`, the evaluator invokes the function (U-6b).
//
// === Special variables ===
//
// Scripture 8.5: `$status` and `$errstr` are queryable after every
// command. They live as ordinary variables in the global frame, but
// the evaluator has dedicated accessors (`status_set` / `errstr_set`)
// so that command-exit handling is a single point of contact rather
// than scattered through pipeline / spawn / wait code paths.
//
// `$cwd` likewise has a dedicated accessor; `cd` updates it
// atomically with the syscall.
//
// === U-6a scope ===
//
// At U-6a only `let`/`assign` aren't wired (they're the U-6b
// statement work). But Env's setters are public so that tests can
// pre-populate the env with named values for expression eval.

use alloc::collections::BTreeMap;
use alloc::string::{String, ToString};
use alloc::vec;
use alloc::vec::Vec;
use core::cell::Cell;

use crate::parser::ast::{FnDecl, Statement};

use super::jobs::JobTable;
use super::value::Value;

/// The evaluator's runtime state. One Env per shell process / per
/// subshell (a subshell is implemented as a fork with a cloned Env).
pub struct Env {
    /// The variable scope stack. Top of stack is innermost; bottom
    /// is global. Always at least one frame (the global).
    scopes: Vec<BTreeMap<String, Value>>,
    /// Function definitions, name -> AST. Per scripture 5.5,
    /// function defs are globally scoped.
    fns: BTreeMap<String, FnDecl>,
    /// `on note 'name' { body }` handler registry (scripture 9.5 +
    /// 10.7). The body is invoked synchronously at the next main-loop
    /// sync point after the named note arrives -- `deliver_pending_notes`
    /// (U-7c) drains the queue and fires the matching body.
    note_handlers: BTreeMap<String, Vec<Statement>>,
    /// $status -- exit code of the last command. Initialized to 0
    /// at construction. A `Cell` so a command substitution (the first
    /// side-effecting expression atom -- it spawns a child + captures
    /// its stdout) can record the inner command's exit through the
    /// `&Env` expression evaluator (scripture 8.7: a non-zero exit
    /// inside `$(cmd)` propagates), without rippling `&mut Env` through
    /// the whole expression tree. $status is the shell's result
    /// register, not a scope binding -- interior mutability models
    /// "every evaluated command updates the register".
    status: Cell<i32>,
    /// $errstr -- last command's error string (rc tradition;
    /// scripture 8.5). Initialized to "".
    errstr: String,
    /// $cwd -- current working directory. Initialized to "/" until
    /// the first cd or until the caller sets it.
    cwd: String,
    /// Interactive-shell mode (scripture 8.9: "the implicit-fail
    /// model applies to scripts and functions. At the interactive
    /// prompt, non-zero exits do NOT terminate the session"). When
    /// true, a non-zero $status after a command in eval_block does
    /// NOT propagate as Return. The U-6g main loop sets this true;
    /// script execution sets it false.
    pub interactive: bool,
    /// External-command stdout/stderr inherit the shell's fd 1/2 (LS-2).
    /// True only when the shell holds a terminal-backed fd 1 (a session
    /// `ut`, which login spawns with the console inherited as fd 0/1/2);
    /// `ut::main` probes fd 1 and sets it. Default false: a fd-less
    /// boot-check/test harness has no 0/1/2 to inherit, so externals keep
    /// the U-6c `Stdio::Piped`-then-drop convention (Inherit would fail
    /// their spawns). Read by `exec_external` / `exec_external_redirected`
    /// / `spawn_pipeline_elements`. stdin stays Piped-drop regardless at
    /// LS-2 -- interactive child stdin is LS-5/LS-8.
    pub stdio_inherit: bool,
    /// Nesting depth of `try { ... }` blocks. While > 0, implicit-
    /// fail is suppressed regardless of `interactive` -- the
    /// enclosing try will pick up the failure via its post-body
    /// $status check (scripture 8.6).
    pub implicit_fail_suppressed: u32,
    /// Nesting depth of `trace { ... }` blocks (scripture 7.8).
    /// While > 0, each command is conceptually echoed to stderr
    /// before execution. At U-6b only the depth is tracked; the
    /// actual echoing wires at U-6c when external commands land
    /// alongside a coherent argv-to-stderr path. Tests can
    /// introspect `trace_depth` to verify scoping.
    pub trace_depth: u32,
    /// Pending `exit` request (U-6e-a). Set by the `exit` builtin to
    /// the requested exit code; once set, `eval_block` short-circuits
    /// every enclosing block/function/loop to `Return` so the whole
    /// statement stack unwinds, and the driver (REPL or script runner)
    /// reads it to terminate. `None` = no exit pending. One-shot: the
    /// first `exit` wins.
    pending_exit: Option<i32>,
    /// Background-job registry (U-7a). `&`-launched pipelines register
    /// here; the REPL prompt-cycle reaper polls + reports them. Pure
    /// state (no syscalls); the syscall-driven reaping lives in `ut`.
    jobs: JobTable,
    /// The shell's own note queue, fd-shaped (U-7c; scripture 10.1 +
    /// 10.7). Opened on-target by the consumer (`Repl::open_notes` or a
    /// probe) AFTER construction -- `Env::new` stays syscall-free so host
    /// construction and the bare-spawn boot check pay nothing. `None`
    /// until opened; while `None`, `deliver_pending_notes` is inert and
    /// `mask note` skips the kernel Thread mask (delivery is then governed
    /// purely by sync-point timing).
    notes: Option<libthyla_rs::notes::Notes>,
    /// The shell's intended note mask (U-7c; scripture 10.8). A
    /// `mask note 'name' { ... }` block adds the named class for the
    /// body's duration; a polled foreground wait (U-7c-b) consults it to
    /// defer forwarding a masked note. Mirrored into the per-Thread kernel
    /// mask via `t::notes::set_mask` only while `notes` is open, so a held
    /// note is dequeue-deferred (no busy poll).
    note_mask: libthyla_rs::notes::NoteMask,
    /// Notes consumed during an interruptible foreground wait (U-7c-b) that are
    /// neither `interrupt` (forwarded to the running job) nor `child_exit` (the
    /// reap wake): held here so their `on note` handlers fire at the next
    /// `deliver_pending_notes` sync point rather than being lost. `try_read`
    /// consumes the kernel queue front, so a note the foreground drain reads
    /// past -- to reach a possible `interrupt` queued behind it -- must be
    /// retained somewhere; this is that somewhere. Drained FIFO, before the
    /// live queue, by `deliver_pending_notes`.
    deferred_notes: Vec<libthyla_rs::notes::Note>,
    /// Eval-stack recursion depth -- the count of nested `eval_block` +
    /// command-substitution frames currently live. Bounded by
    /// `EVAL_MAX_DEPTH` so a runaway recursion (`fn f { f }`, a self-
    /// `source`, an `eval`-bomb, or executed `$($($(...)))` nesting) yields a
    /// graceful `RecursionLimit` error instead of overflowing the 256 KiB
    /// EL0 stack into a guard-page `snare:segv` (scripture 8.1). A `Cell`
    /// because the `&Env` command-substitution path must inc/dec it.
    eval_depth: Cell<u32>,
    /// One-shot "a terminate-intent `interrupt` arrived inside a running
    /// command" flag (R2-F3 / scripture 10.2). Set by the loop interrupt poll
    /// (`poll_loop_interrupt`) when Ctrl-C reaches a runaway shell-eval loop
    /// that has no foreground child to forward it to; checked by `eval_block`
    /// after every statement so it unwinds every enclosing loop / block /
    /// function -- the same propagation `pending_exit` uses -- returning the
    /// shell to the prompt. The REPL clears it (and sets `$status = 130`)
    /// once the command has unwound.
    interrupt_pending: bool,
}

/// Maximum eval-stack recursion depth (function calls / command-substitution
/// nesting / source / eval). Conservative against the 256 KiB EL0 stack: the
/// heaviest eval frame (a command-substitution level) burns a few KiB, so 64
/// nested levels stays well under the stack while sitting far beyond any real
/// script's nesting (the deepest realistic recursion is a handful of levels).
pub(crate) const EVAL_MAX_DEPTH: u32 = 64;

impl Env {
    /// Construct a fresh Env with a single empty global frame, all
    /// special vars at defaults. Default `interactive = false` (the
    /// strict default; implicit-fail active). The U-6g main loop
    /// flips it to true; tests opt in explicitly.
    pub fn new() -> Self {
        Env {
            scopes: vec![BTreeMap::new()],
            fns: BTreeMap::new(),
            note_handlers: BTreeMap::new(),
            status: Cell::new(0),
            errstr: String::new(),
            cwd: "/".to_string(),
            interactive: false,
            stdio_inherit: false,
            implicit_fail_suppressed: 0,
            trace_depth: 0,
            pending_exit: None,
            jobs: JobTable::new(),
            notes: None,
            note_mask: libthyla_rs::notes::NoteMask::NONE,
            deferred_notes: Vec::new(),
            eval_depth: Cell::new(0),
            interrupt_pending: false,
        }
    }

    // === Eval-stack recursion bound (scripture 8.1) ===

    /// Enter one eval-recursion level. Returns `true` if the level is at the
    /// limit -- in that case NO frame is pushed and the caller MUST bail
    /// (returning a `RecursionLimit` error) WITHOUT calling `eval_recursion_leave`.
    /// Returns `false` on success: a frame was pushed and the caller MUST call
    /// `eval_recursion_leave` on every exit path (success and error).
    pub(crate) fn eval_recursion_enter(&self) -> bool {
        let d = self.eval_depth.get();
        if d >= EVAL_MAX_DEPTH {
            return true;
        }
        self.eval_depth.set(d + 1);
        false
    }

    /// Leave one eval-recursion level. Saturating so a stray leave can never
    /// underflow the counter (which would falsely relax the bound).
    pub(crate) fn eval_recursion_leave(&self) {
        self.eval_depth.set(self.eval_depth.get().saturating_sub(1));
    }

    // === Scope manipulation ===

    /// Push a new (empty) scope frame. Called on function-call entry
    /// (U-6b).
    pub fn push_scope(&mut self) {
        self.scopes.push(BTreeMap::new());
    }

    /// Pop the topmost scope frame. Called on function-call exit
    /// (U-6b). Panics if only the global frame remains -- a balance
    /// invariant the caller must uphold.
    pub fn pop_scope(&mut self) {
        if self.scopes.len() <= 1 {
            // Internal invariant violation; treat as fatal because
            // reaching this means push/pop are mismatched. At U-6a
            // tests don't exercise push/pop, so this can't fire.
            panic!("Env::pop_scope: only global frame remains");
        }
        self.scopes.pop();
    }

    /// Current scope depth (1 = only global; N+1 = N function calls
    /// deep). Diagnostic only.
    pub fn depth(&self) -> usize {
        self.scopes.len()
    }

    // === Variable read/write ===

    /// Look up a variable by name. The special variables `$status`,
    /// `$errstr`, and `$cwd` bridge to their dedicated Env fields
    /// (scripture 8.5 requires them queryable after every command);
    /// they cannot be shadowed by an ordinary binding. All other names
    /// search the scope stack from innermost to outermost and return
    /// the first frame's binding. Undefined names return
    /// `Value::empty()` per rc convention (see value.rs module docs).
    pub fn get(&self, name: &str) -> Value {
        if let Some(v) = self.special_get(name) {
            return v;
        }
        for frame in self.scopes.iter().rev() {
            if let Some(v) = frame.get(name) {
                return v.clone();
            }
        }
        Value::empty()
    }

    /// Whether a variable is defined in any frame. Distinct from
    /// `get(name).is_empty()` because an explicitly-set empty value
    /// is "defined" in scripture's sense.
    pub fn defined(&self, name: &str) -> bool {
        self.scopes.iter().rev().any(|frame| frame.contains_key(name))
    }

    /// `let name = value` -- create a binding in the TOPMOST frame
    /// (innermost scope), shadowing any outer binding. Scripture 6.2.
    /// Assignment to a special variable (`status`/`errstr`/`cwd`)
    /// routes to its dedicated field rather than a scope binding, so
    /// the read bridge stays coherent.
    pub fn let_set<S: Into<String>>(&mut self, name: S, value: Value) {
        let name = name.into();
        if self.special_set(&name, &value) {
            return;
        }
        let frame = self.scopes.last_mut().expect("global frame present");
        frame.insert(name, value);
    }

    /// `name = value` -- bare assignment. Writes to the topmost
    /// frame that already has the name; if no frame has it, writes
    /// to the global (bottom) frame. Scripture 6.2. A special
    /// variable routes to its dedicated field (see `let_set`).
    pub fn assign<S: Into<String>>(&mut self, name: S, value: Value) {
        let name = name.into();
        if self.special_set(&name, &value) {
            return;
        }
        // Walk top-down looking for an existing binding.
        for frame in self.scopes.iter_mut().rev() {
            if frame.contains_key(&name) {
                frame.insert(name, value);
                return;
            }
        }
        // No frame has it -- write to the global (bottom) frame.
        let global = self.scopes.first_mut().expect("global frame present");
        global.insert(name, value);
    }

    /// Unconditional unset from all frames. Used by `unset` builtin
    /// (U-6f).
    pub fn unset(&mut self, name: &str) {
        for frame in self.scopes.iter_mut() {
            frame.remove(name);
        }
    }

    // === Function table ===

    /// Register a function definition. Scripture 5.5 -- globally
    /// scoped, overwrites any prior definition with the same name.
    pub fn fn_set(&mut self, decl: FnDecl) {
        self.fns.insert(decl.name.clone(), decl);
    }

    /// Look up a function definition by name.
    pub fn fn_get(&self, name: &str) -> Option<&FnDecl> {
        self.fns.get(name)
    }

    /// Whether a function is defined.
    pub fn fn_defined(&self, name: &str) -> bool {
        self.fns.contains_key(name)
    }

    // === Note handler registry ===

    /// Register a `on note 'name' { body }` handler. Last registration
    /// wins (rc convention). At U-6b only registration is wired;
    /// runtime delivery wires at U-7.
    pub fn note_handler_set<S: Into<String>>(&mut self, name: S, body: Vec<Statement>) {
        self.note_handlers.insert(name.into(), body);
    }

    /// Look up a handler by note name.
    pub fn note_handler_get(&self, name: &str) -> Option<&Vec<Statement>> {
        self.note_handlers.get(name)
    }

    /// Whether a handler is registered for the given note name.
    pub fn note_handler_defined(&self, name: &str) -> bool {
        self.note_handlers.contains_key(name)
    }

    // === Note delivery (U-7c) ===

    /// Install the shell's own note queue. The consumer opens it on-target
    /// (`Notes::open_self`) once after construction; host tests and
    /// note-less probes leave it `None`. Replacing an existing queue drops
    /// the prior fd (closing it).
    pub fn set_notes(&mut self, notes: Option<libthyla_rs::notes::Notes>) {
        self.notes = notes;
    }

    /// Borrow the shell's note queue, if opened. `None` => note delivery is
    /// inert (the pre-U-7c / host path).
    pub fn notes(&self) -> Option<&libthyla_rs::notes::Notes> {
        self.notes.as_ref()
    }

    /// The shell's current note mask (U-7c).
    pub fn note_mask(&self) -> libthyla_rs::notes::NoteMask {
        self.note_mask
    }

    /// Add `class` to the shell note mask (a `mask note` block entry) and,
    /// when the queue is open, push the new mask to the kernel Thread so the
    /// class is dequeue-deferred (a held note advertises no POLLIN, so a
    /// polled wait cannot spin on it). Returns `true` iff the class was
    /// newly added -- the caller pairs a `true` with `note_mask_remove` on
    /// block exit and must NOT unwind a `false` (an already-masked re-entry).
    pub fn note_mask_add(&mut self, class: libthyla_rs::notes::NoteClass) -> bool {
        if self.note_mask.contains(class) {
            return false;
        }
        self.note_mask = self.note_mask.with(class);
        if self.notes.is_some() {
            let _ = libthyla_rs::notes::set_mask(self.note_mask);
        }
        true
    }

    /// Remove `class` from the shell note mask (a `mask note` block exit) and
    /// restore the kernel Thread mask when the queue is open. Pairs with a
    /// `true` from `note_mask_add`.
    pub fn note_mask_remove(&mut self, class: libthyla_rs::notes::NoteClass) {
        self.note_mask = self.note_mask.without(class);
        if self.notes.is_some() {
            let _ = libthyla_rs::notes::set_mask(self.note_mask);
        }
    }

    /// Hold a note consumed during an interruptible foreground wait (U-7c-b)
    /// for delivery at the next sync point. Used for notes that are neither
    /// `interrupt` (forwarded to the running job) nor `child_exit` (the reap
    /// wake), so a handler-bearing note read past during the foreground drain
    /// is not lost.
    pub fn defer_note(&mut self, note: libthyla_rs::notes::Note) {
        self.deferred_notes.push(note);
    }

    /// Take (and clear) the notes deferred during a foreground wait, oldest
    /// first. `deliver_pending_notes` fires these before draining the live
    /// kernel queue, preserving arrival order.
    pub fn take_deferred_notes(&mut self) -> Vec<libthyla_rs::notes::Note> {
        core::mem::take(&mut self.deferred_notes)
    }

    // === Special variables ===

    pub fn status(&self) -> i32 {
        self.status.get()
    }

    /// Set `$status`. Takes `&self` (not `&mut self`) because `$status`
    /// is a `Cell`: a command substitution running inside the `&Env`
    /// expression evaluator must be able to record the inner command's
    /// exit (scripture 8.7). `&mut self` callers coerce to `&self`
    /// transparently, so every existing call site is unaffected.
    pub fn status_set(&self, code: i32) {
        self.status.set(code);
    }

    pub fn errstr(&self) -> &str {
        &self.errstr
    }

    pub fn errstr_set<S: Into<String>>(&mut self, s: S) {
        self.errstr = s.into();
    }

    pub fn cwd(&self) -> &str {
        &self.cwd
    }

    pub fn cwd_set<S: Into<String>>(&mut self, p: S) {
        self.cwd = p.into();
    }

    /// Read bridge for the special variables. Returns `Some` for
    /// `status`/`errstr`/`cwd` (their dedicated field value), `None`
    /// for any other name (the caller then searches scope frames).
    fn special_get(&self, name: &str) -> Option<Value> {
        match name {
            "status" => Some(Value::from(self.status.get() as i64)),
            "errstr" => Some(Value::scalar(self.errstr.clone())),
            "cwd" => Some(Value::scalar(self.cwd.clone())),
            _ => None,
        }
    }

    /// Write bridge for the special variables. Routes a write to
    /// `status`/`errstr`/`cwd` into its dedicated field and returns
    /// `true`; returns `false` for any other name (the caller then
    /// writes a scope binding).
    fn special_set(&mut self, name: &str, value: &Value) -> bool {
        match name {
            "status" => {
                self.status.set(value.as_int().unwrap_or(0) as i32);
                true
            }
            "errstr" => {
                self.errstr = value.as_scalar();
                true
            }
            "cwd" => {
                self.cwd = value.as_scalar();
                true
            }
            _ => false,
        }
    }

    // === Pending exit (U-6e-a) ===

    /// Request shell termination with `code` (the `exit` builtin).
    /// One-shot: the first request wins (a nested `exit` while one is
    /// already pending does not overwrite it).
    pub fn request_exit(&mut self, code: i32) {
        if self.pending_exit.is_none() {
            self.pending_exit = Some(code);
        }
    }

    /// The pending exit code, if `exit` has fired. The driver checks
    /// this after evaluation to terminate; `eval_block` checks it to
    /// short-circuit the statement stack.
    pub fn exit_requested(&self) -> Option<i32> {
        self.pending_exit
    }

    // === Pending interrupt (R2-F3 / scripture 10.2) ===

    /// Flag a pending terminate-intent interrupt (Ctrl-C inside a runaway
    /// shell-eval loop). Idempotent; `eval_block` unwinds on it.
    pub fn set_interrupt(&mut self) {
        self.interrupt_pending = true;
    }

    /// Whether a terminate-intent interrupt is pending. `eval_block` checks
    /// this after each statement to unwind every enclosing block to the
    /// prompt (the `pending_exit` propagation, but transient per-command).
    pub fn interrupt_pending(&self) -> bool {
        self.interrupt_pending
    }

    /// Consume the pending-interrupt flag (one-shot). The REPL calls this once
    /// the interrupted command has unwound so the next command starts clean.
    pub fn take_interrupt(&mut self) -> bool {
        core::mem::replace(&mut self.interrupt_pending, false)
    }

    // === Background jobs (U-7a) ===

    /// Borrow the background-job table (the `jobs` builtin + the reaper's
    /// `live_pids` read).
    pub fn jobs(&self) -> &JobTable {
        &self.jobs
    }

    /// Mutable background-job table (the `&` register path + the reaper's
    /// `mark_reaped` / `take_done_notifications`).
    pub fn jobs_mut(&mut self) -> &mut JobTable {
        &mut self.jobs
    }
}

impl Default for Env {
    fn default() -> Self {
        Env::new()
    }
}
