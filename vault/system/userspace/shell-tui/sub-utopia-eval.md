---
id: sub-utopia-eval
type: sub
title: "The ut evaluator — one recursion counter for shapes that compose, and a pure function that spawns processes"
parent: moc-userspace-shell-tui
code:
  - usr/utopia/libutopia/src/eval/mod.rs
  - usr/utopia/libutopia/src/eval/stmt.rs
  - usr/utopia/libutopia/src/eval/builtin.rs
  - usr/utopia/libutopia/src/eval/expr.rs
  - usr/utopia/libutopia/src/eval/env.rs
  - usr/utopia/libutopia/src/eval/glob.rs
  - usr/utopia/libutopia/src/eval/jobs.rs
  - usr/utopia/libutopia/src/eval/console.rs
  - usr/utopia/libutopia/src/eval/value.rs
  - usr/utopia/libutopia/src/eval/error.rs
audit: light
guarded-by: [inv-i19, inv-i20, inv-i27, inv-i28]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design:
  - "docs/UTOPIA-SHELL-DESIGN.md sections 5-10"
created: 2026-08-03
updated: 2026-08-16
---
## Purpose

AST to effects. Where [[sub-utopia-parser]] touches nothing outside its own
arguments, this layer is the shell's whole consequence surface: it spawns
processes, opens files, wires pipes, forwards notes, forms process groups, hands
the terminal to a foreground job and takes it back, and flips the console's line
discipline around a full-screen child.

None of that is a privilege boundary — the kernel gates every syscall the
evaluator issues, so a bug here costs the user their session and nothing else.
But *within* that session the blast radius is the whole thing, and the layer's
own account of itself is the least reliable part of it: see Caveats, where the
main entry point's doc comment and a field comment one file away describe the
same function in opposite terms.

## Contract

Six public entry points, all over `&mut Env`:

- `eval_script(env, &Script)` / `eval_block(env, &[Statement])` — walk a parsed
  program or a statement sequence, returning a `StatementFlow`
  (`Normal` / `Return` / `Break` / `Continue`).
- `eval_statement(env, &Statement)` — one statement.
- `eval_expr(env, &Expr)` — the expression walker, over `&Env`.
- `eval_source(env, &str)` — parse and evaluate in one call.
- `deliver_pending_notes(env)` / `wait_pids_interruptible(env, pids)` /
  `aggregate_pipefail(...)` — the pieces the REPL drives directly.

`Env` is the runtime state: a scope stack, a function table, an alias table, a
note-handler registry, the job table, `$status` / `$errstr` / `$cwd`, and the
mode flags (`interactive`, `stdio_inherit`, `consctl_fd`, `job_control`).

## Mechanism

### Command resolution is three-way, in order

`eval_command` expands argv[0] through the alias table, then resolves
**function → builtin → external**. A function runs in a pushed scope; a builtin
runs in-process because it mutates shell state; anything else is spawned, with
`$path` handled shell-side (a bare name becomes `/bin/<name>`, a `/`-bearing
name is used as-is) and the actual resolution done by the kernel against the
caller's namespace. There are sixteen builtins, and `BUILTIN_NAMES` — the list
`is_builtin` tests and `type` answers from — agrees exactly with `try_builtin`'s
dispatch arms.

### Implicit-fail is a mode, not a flag on the command

In script mode outside a `try`, a non-zero `$status` after a statement converts
the block's flow to `Return`, so the failure propagates out of the enclosing
function or script. Interactive mode suppresses it; the `?` postfix forces it
regardless of mode. This is `set -e`'s intent with the modes made explicit
rather than global.

### Two foreground wait paths, chosen by whether the session dance succeeded

On the console the kernel routes a Ctrl-C to the console *owner* — the shell —
so `wait_pids_interruptible` turns the blocking reap into a poll on the shell's
own note queue and **forwards** an arriving `interrupt` to the still-live
foreground pids. Reap truth stays a per-pid `WAIT_WNOHANG` sweep; the
`child_exit` note is only the wake, and a bounded backstop timeout covers a
coalesced or mask-deferred one so the wait can never hang. With no note queue
open the whole path degrades to a plain blocking by-pid wait.

On a pts the routing inverts — the kernel fans a terminal signal to the
foreground *process group* — so `run_foreground_jc` places the job in its own
group, hands that group the terminal, waits stop-aware (`WAIT_UNTRACED`), and
restores the terminal and the prompt line discipline on **every** outcome
including a stop. No note forwarding happens on this path. Every job-control arm
is inert while `job_control` is `None`, so the console path is unchanged by the
existence of the other one.

### The raw-mode set is a closed allowlist, and joining it is a deliberate act

Programs that need the console as an unprocessed byte pipe — the editor, the
pseudoterminal host, the process monitor, and now the graphics bench launcher —
are named in a **fixed list** matched on the command's basename, with the path
form covered too.

**The default is cooked, and joining requires an edit plus a test.** Nothing
infers raw mode from what a program does, so a new full-screen program gets the
line discipline until someone says otherwise — the same absence-as-safe-default
shape as the substrate's watchpoint exemption, and it holds for the same reason:
the exemption is keyed to the programs that need it rather than to a list of the
ones that do not, so a new arrival inherits nothing by accident.

The per-entry justifications are worth keeping in the list rather than
compressed away, because they are not the same reason. The pseudoterminal host
wants the outer console as a raw pipe **because the terminal it hosts is the one
line discipline** — two disciplines in series would double-cook. The others are
full-screen renderers. A future entry owes its own sentence; "it looks like a
TUI" is not the criterion.

### A note read while looking for something else is held, not dropped

`try_read` consumes the queue front, so any path that scans for a particular
note must retain what it reads past. Both scanners do: the foreground drain
forwards `interrupt`, swallows `child_exit` (so the fd stops advertising
`POLLIN` and the next poll genuinely blocks), and defers everything else; the
strided loop poll defers every non-`interrupt` note it passes. `Env` holds them
FIFO and `deliver_pending_notes` fires them **before** draining the live queue,
preserving arrival order across the boundary.

### The recursion bound is ONE counter with two entry points

`EVAL_MAX_DEPTH` (64) bounds the eval stack the way the parser's three counters
bound the parse stack — but here a single counter is the deliberate choice, and
the reason is written at both charge sites: *"Shares Env's eval-depth counter
with eval_block so the bound holds across mixed function/source/subst nesting."*
A shell mixes its recursion shapes in one expression (`fn f { echo $(f) }`
recurses through a function call, a substitution and a block on each turn), so
per-shape counters would each stay under their own bound while the stack
overflowed anyway. See Prosecution for what that buys and what it costs.

## Data structures

- **`Value(Vec<String>)`** — the unified list model: a scalar is a one-element
  list, so argv expansion is the identity on the representation rather than a
  conversion. `as_int` treats an empty value as 0 and a multi-element value as
  its space-joined form.
- **`Env`** — the scope stack is a `Vec<BTreeMap<String, Value>>` (BTreeMap
  because `alloc` has it and `HashMap` would pull in a dependency). `$status`
  and `eval_depth` are `Cell`s so the `&Env` expression path can write them;
  every other field is plain.
- **`JobTable` / `Job`** — a job is a `&`-launched *pipeline*, so it tracks N
  pids and is Done only when all of them are reaped. Specs climb while jobs
  coexist and reset to 1 when the table drains.
- **`StatementFlow`** — the four-way control-flow result the block walker
  interprets.
- **`EvalErrorKind`** — ten variants, each carrying the span of the offending
  expression.

## Concurrency

None inside the evaluator: `ut` is single-threaded and the whole layer runs on
one stack. The interesting concurrency is *external* — the shell is racing its
own children — and it is handled by making the reap the ground truth rather than
the notification. A `child_exit` note is treated purely as a wake; every
decision about whether a child has finished comes from a `wait_pid_for` call.
That ordering is what makes a lost, coalesced or mask-deferred note a latency
event instead of a hang.

Two interior-mutability points exist for one reason each: `$status` so a
command substitution reached through `&Env` can record the inner command's exit,
and `eval_depth` so the same path can charge the shared recursion counter.

## Invariants enforced

**None from the enumerated set** — this is client code over a validating kernel.
It *depends* on four:

- **[[inv-i19]]** — note delivery order and exactly-once consumption. The
  deferred-note queue exists because `try_read` consumes; the shell preserves
  arrival order across its own scan-and-hold.
- **[[inv-i20]]** — the pts stop leg. `WAIT_UNTRACED` stop reports and
  `SYS_TTY_CONT` resumes are the shell's half of job control.
- **[[inv-i27]]** — console *owner* is distinct from console *attach*. The shell
  sets line discipline through a forwarded ctl fd and is never console-attached;
  the child it hands the console to touches neither.
- **[[inv-i28]]** — path resolution. The shell does `$path` selection only; the
  kernel resolves and X-searches.

The layer does hold one property of its own, on no list: **no script, however
recursive, overflows the EL0 stack** — the liveness twin of the parser's, one
level up.

## Error paths

Two dispositions, deliberately separated. A *command's* failure — a bad
argument, a missing file, a failed spawn — is not an error: it sets `$status`
(127 for a spawn failure, the bash convention) plus `$errstr` and returns
`Ok(Normal)`, so the implicit-fail discipline decides what happens next. An
*evaluator* error — non-numeric arithmetic, division by zero, an unimplemented
form, recursion exhaustion — is an `Err` carrying a span.

Depth exhaustion goes through the ordinary error channel, so a runaway
recursion is a message and a prompt rather than a dead shell.

## Performance

Not a measured surface. One deliberate cost control: the loop interrupt poll is
strided (every 128 iterations) so a hot pure-eval loop does not pay a `try_read`
syscall per turn, bounding Ctrl-C latency to a handful of iterations instead of
one.

## Prosecution

- **The recursion counter must stay shared.** Splitting it per shape — one for
  functions, one for substitutions — would let each stay under its own bound
  while the composed stack overflowed. That is the property the single counter
  buys, and it is the opposite of the parser's answer to a superficially
  identical problem.
- **Every charge site must pair with a leave on every path.** Both do it the
  same way: an outer `enter` / `leave` wrapper around an inner body function, so
  the leave is unconditional across every `?` in the body. A new charge site
  written inline, with `?` between enter and leave, leaks depth until the shell
  wedges at 64.
- **`eval_block`'s empty-slice early return is load-bearing, not an
  optimization.** It returns before charging depth because the error it would
  otherwise raise needs `stmts[0].span`, which an empty slice does not have.
- **A new note scanner must defer what it reads past.** `try_read` consumes;
  both existing scanners hold non-matching notes for the post-command drain. A
  third that dropped them would silently lose `on note` handlers.
- **A new foreground wait path must restore the terminal and the line
  discipline on every outcome, stop included.** `run_foreground_jc` does the
  restore before it branches on the result, which is what makes the stop case
  correct by construction rather than by remembering.
- **Reap truth stays the wait, never the note.** A path that concluded a child
  had exited from a `child_exit` note would break the moment one was coalesced.

## Seams

- **Subshells and in-process pipeline elements are unimplemented**, and the
  reason is structural: both need a fork that re-runs a parsed body in the
  child, which the spawn-then-exec process model does not offer.
- **Command substitution accepts only a single pipeline of redirect-free
  external simple commands** — no builtin, no function, no background, no
  redirect, no control flow. Seven explicit refusals, each its own message.
- **Process substitution** needs a `/proc/self/fd/N` surface the kernel does not
  expose; the comment saying so is current and correct, which is worth noting
  given its neighbours.
- **Aliases expand one pass**, so alias-of-alias does not fully resolve. The
  `alias` / `unalias` builtins that would let a user create the loop hazard are
  the deferred half; the table and the expansion already exist.
- **`**` in a glob is not special-cased** — it behaves as `*`, matching one path
  component, so recursive descent is future work.

## Caveats

- **The main expression entry point is documented as a pure function and it
  spawns processes.** `eval_expr`'s doc comment says *"Pure function with
  respect to the AST; side effects are limited to errors raised through
  `EvalResult`"*. Since command substitution landed, an expression containing
  `$(cmd)` spawns children, captures their stdout, and writes `$status`. The
  crate knows: `Env`'s `status` field comment calls command substitution *"the
  first side-effecting expression atom -- it spawns a child + captures its
  stdout"*, and explains that the field is a `Cell` precisely so a `&Env`
  evaluation can have that effect. Two files describe the same function's purity
  in opposite terms, and the wrong one is the doc comment on the function.

- **`export` is declined for want of a mechanism that exists, and the shell
  already links the module that says so.** The builtin module's deferred list
  reads *"set / export -- envp passing to children does not exist (SYS_SPAWN
  carries argv only); export is meaningless until it does."* The premise is
  still true and the conclusion is not: the environment is a per-Proc `/env`
  device, mounted by init (which extincts if the mount fails) and inherited by
  children through the kernel clone at fork. The runtime library's own module
  header calls the gap *closed by the /env device* and exposes a reader — and
  the shell imports that module, calling it from the `cd` builtin about a
  hundred and fifty lines below the deferred list. What makes this the sharpest
  instance of a shape the arc keeps finding is that **the stated reason never
  became false, only irrelevant**, so anyone who re-checks it confirms it and
  moves on. Task #106.

- **A console-detection comment names a defense the kernel removed; a second
  one silently carries the case.** The pts-detection helper documents its
  console answer as *"devcons has no `stat_native` -> fstat fails"*. The console
  Dev gained `stat_native`, so the call now succeeds and reports a character
  device — which means the mode gate this function checks *first* also passes.
  The correct answer rests entirely on the qid check, and only because the
  kernel side deliberately made the console's flag bit disjoint from the pts
  one and documented that choice. The code is right; the comment tells a
  maintainer that the surviving gate is redundant. Task #107.

- **Eight of the ten files open with a header describing the sub-chunk that
  created them, and the stale halves are the "deferred" lists.** The expression
  module is the sharpest: it lists command substitution and backtick as
  deferred, five lines above the `use` that imports the substitution runner, and
  both arms are live. The statement module's deferred list is three-fifths
  stale — redirects, background and filesystem glob expansion have all shipped.
  The asymmetry is what matters: a stale *implemented* entry is harmless, since
  a reader looking for the feature finds it; a stale *deferred* entry tells a
  reader that a working feature does not exist. The counter-example sits in the
  same directory — the builtin module's deferred list was updated in place when
  the alias table landed, which is the proof that maintaining these is possible.
  Task #108.

- **The note dispatcher carries two stacked doc comments that contradict each
  other.** The superseded one was left above its replacement, so the rendered
  documentation states that an unhandled `interrupt` at a sync point is benign
  and then that it is the return value the idle poll loop reads to cancel an
  in-progress line edit. Both paragraphs describe the same function; only the
  second is true.

- **The job table was made pure specifically to be host-testable, and its
  fifteen tests have never run.** The module header gives the design rationale:
  the table performs no syscalls, so the REPL must drive the reaping and feed
  results back, *"keeping the table pure makes it host-testable against injected
  `(pid, status)` pairs."* The crate's tests do not compile (task #105). A real
  design constraint was accepted to buy a property that has never existed. The
  console module shows the same problem being met and worked around locally
  rather than escalated: four compile-time asserts mirror four `#[cfg(test)]`
  assertions, with a comment explaining that the crate *"has no host test
  harness ... so the `#[cfg(test)]` literal asserts below never run. These do."*

## Provenance

[[chg-2026-08-03-utopia-eval-sweep]].

[[chg-2026-08-16-seven-small-surfaces]] records this interval.
