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
  - usr/utopia/libutopia/src/eval/pathname.rs
  - usr/utopia/libutopia/src/eval/jobs.rs
  - usr/utopia/libutopia/src/eval/console.rs
  - usr/utopia/libutopia/src/eval/discipline.rs
  - usr/utopia/libutopia/src/eval/value.rs
  - usr/utopia/libutopia/src/eval/error.rs
  - usr/u-glob-test/src/main.rs
audit: light
guarded-by: [inv-i19, inv-i20, inv-i27, inv-i28]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design:
  - "docs/UTOPIA-SHELL-DESIGN.md sections 5-10"
created: 2026-08-03
updated: 2026-09-23
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

**Haul and Imperium (2026-09-17).** `mount /srv/NAME PATH [ANAME]` connects
a byte service, attaches it through SYS_ATTACH_9P_SRV, and mounts the returned
root in the calling shell's Territory. `unmount PATH` removes that entry.
Failures set `$status` and `$errstr`; they are not automatically printed.
`abdicate` exits the elevated shell, allowing the imperium root to revoke the
whole scope. Foreground external execution passes console bytes through while
the child owns input, so the imperium key reaches the trusted episode.

Argument expansion now preserves the previous command's `$status` (including
`exit $status`). `Env.status_revision` detects whether expansion ran a
status-producing substitution: an empty expansion with no such write succeeds,
whereas an empty substitution retains its result. Boot probes cover prior
failure, empty plain expansion, and empty successful/failed substitutions.


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
`$path` handled shell-side (a bare name is searched over `/bin`, `/`,
`/goroot/bin`, `/clade/bin`, `/viv/bin`, `/viv/abin` in order — first existing
hit wins, a miss falls back to `/bin/<name>` for a clean spawn error; the
toolchain and phenotype dirs come last so `/bin` stays authoritative, and the
two `/viv` dirs are `MPHENO_LINUX` mounts so a Linux binary there runs
seamlessly; a `/`-bearing name is used as-is) and the actual resolution done by
the kernel against the caller's namespace. There are seventeen builtins under
nineteen names (`source` / `.` and `type` / `whence` are pairs; `cd --` ends
option processing — the one way to enter a directory whose name begins with
`-`), and `BUILTIN_NAMES` — the list
`is_builtin` tests and `type` answers from — agrees exactly with `try_builtin`'s
dispatch arms. `mount` and `unmount` are builtins for a structural reason their
module states: Thylacine clones the namespace at every fork, so a mount made by
a spawned command would land in the child's namespace and vanish with it. They
landed at `5c22f90d`, whose title says they compile but do not yet work in the
guest.

### The external spawn is one chokepoint, and `#!` is shell-side

Every external spawn — foreground, the raw-mode TUI path, a redirected command,
a pipeline element, and a background pipeline (five sites) — routes through
`build_command(argv)`, so a script runs identically in every position
(`./s.ut`, `./s.ut | grep x`, `./s.ut > out`, `./s.ut &`). Centralizing it is
what lets the one interesting thing it does happen everywhere at once.

That thing is `#!`. The kernel loads ELF only (ARCH §9.6.8); the shebang is a
**shell-side** convention, kept out of the kernel exactly as Plan 9 keeps it out
of its exec. `prepare_argv` resolves `argv[0]` through `$path`, then
`peek_shebang` reads the first 128 bytes and `parse_shebang_line` (pure,
host-tested) recognizes `#!interp [arg]`: a match rewrites the argv to
`[resolve($path, interp), arg?, prog, args…]` — the interpreter itself
`$path`-resolved, so `#!ut` works as well as `#!/bin/ut` — with at most **one**
argument after the interpreter (the Linux/BSD convention, the trimmed remainder
of the first line, no further word-splitting). A non-match (an `\x7fELF` head, an
unreadable file, any non-`#!` head) passes through unchanged; the shell never
re-peeks the interpreter, so there is no recursion (a non-ELF interpreter just
fails the kernel's ELF load with status 127).

The permission shape is Unix's, and it falls out of the mechanism rather than
being re-checked: reading the script to peek needs the caller's **R**, and the
kernel still gates the actual exec on the `OEXEC` **X**-bit — so a script needs
both, and the shell adds no gate of its own. A spawn failure (bad `$path`, a
kernel rejection) sets `$status = 127`, the bash convention.

### Implicit-fail is a mode, not a flag on the command

In script mode outside a `try`, a non-zero `$status` after a statement converts
the block's flow to `Return`, so the failure propagates out of the enclosing
function or script. Interactive mode suppresses it; the `?` postfix forces it
regardless of mode. This is `set -e`'s intent with the modes made explicit
rather than global.

### `$status` is read before it is settled

A statement's words are expanded against the status the *previous* command
left: `echo $status`, `let saved = $status`, `exit $status`, and a bare `exit`
(which exits with the current status) all read it. So nothing may reset the
register before expanding, and what a statement reports is settled afterwards:

- **A command name reports its own exit.** A function body, and the text an
  `eval` or `source` runs, starts from the caller's status, as in rc and bash,
  so `fn f { return }` after a failure returns that failure.
- **A statement with no command of its own** (a `let`, a bare assignment, a
  line whose words expand to nothing) succeeds, unless a command substitution
  ran while expanding it. In that case the substitution's exit stands
  (scripture 8.7: `let output = $(cmd)`). `succeed_unless_substituted` decides
  this from `Env.substitutions`, a monotonic count bumped once per substitution
  at `run_command_substitution_script` and read before and after the expansion.
  Comparing `$status` before and after cannot decide it, because a substitution
  may exit with the value already in the register.
- **A body that runs nothing** succeeds: an empty function, an `eval` or
  `source` whose text holds no statement (`eval_source_as_command`), and an
  empty `try` body, which therefore never runs its `catch`. `if`, `while`,
  `for` and `case` already reported 0 when they ran nothing. The REPL calls the
  plain `eval_source`, so an empty line leaves `$status` alone.

From 2026-06-08 (`e9e0aa92`, command substitution) to 2026-09-16 the order was
the other way round in `eval_command`, `eval_let` and `eval_assign`: each reset
`$status` to 0 before expanding, so every read listed above saw 0 while the
status after each command stayed correct. The multi-element pipeline, redirect
and substitution-body expansions never had the reset, so `echo $status | tr ...`
read the true value throughout. Every boot probe read the register from Rust
(`Env::status`, `Env::get`) rather than through a statement, which is why none
of them failed. `u-builtin-test` 8b and `u-subst-test` 6b now read it the way a
script does, one leg per site, and print every failing leg with the value it
saw.

### `&&` / `||` short-circuit lists

`eval_and_or` runs the first pipeline, then each later pipeline only if its
connector is satisfied by the running `$status` (`&&` → 0, `||` → non-zero). A
link's non-zero exit is consumed by its connector rather than propagated, so
only the list's *final* `$status` reaches the block loop's implicit-fail check —
which is why `a || b` tolerates `a`'s failure when `b` succeeds (scripture 8.6).
A control-flow escape (`return` / `break` / `continue`) from any operand wins
immediately, and a visible `?` on the leading command still forces propagation
even interactively (the `should_propagate_failure` AND-OR arm). The parser
builds the `AndOr` node only when a connector is present, so a lone pipeline
never reaches this path.

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

The pts slave is not directly pollable — `dev9p.poll` reports it always-ready —
so for fd-0 input readiness the shell polls a side fd, `/dev/pts/<n>ready`
(`JobControlState.poll_in_fd`), while still reading fd 0 itself; a failed
ready-open degrades to polling fd 0, the prior behaviour. That bridge is what
lets a native `ut` service Ctrl-C at an otherwise-idle pts prompt.

### The raw-mode set is a closed allowlist, and joining it is a deliberate act

Programs that need the console as an unprocessed byte pipe — the editor, the
pseudoterminal host, the process monitor, the graphics bench launcher, and now
the deck presenter — are named in a **fixed list** matched on the command's
basename, with the path form covered too.

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

The deck presenter (2026-09-22) is the first entry that is **not** a full-screen
renderer, and it pays the sentence the paragraph above asks for. It joins for the
**input half only**: it needs byte-at-a-time reads with no echo so a keystroke
turns a slide, and it needs signal cooking off so its own quit handling runs
rather than a note terminating a talk. It deliberately never enters the alternate
screen — that is the mode in which a tile paints its raw character grid instead
of the rich document, which would discard the very rendering it exists to show —
so the restore backstop's leave-alt-screen escape is inert for it, harmlessly.

That splits the entry's cost in two, and only the second half is obvious. Signal
cooking off means **the habitual interrupt key becomes a byte the program must
answer itself**, so a member that does not answer it cannot be left by the key
every user reaches for first. Output translation off means a member emitting a
**document** rather than positioned cells must supply its own line-ending
translation, because the argument for turning it off — that a full-screen program
owns every byte it writes — is an argument about renderers, and a document's
lines end in a bare feed. Both are properties of *membership*, not of being a
TUI, which is why a non-renderer can hold the entry at all.

### A note read while looking for something else is held, not dropped

`try_read` consumes the queue front, so any path that scans for a particular
note must retain what it reads past. Both scanners do: the foreground drain
forwards `interrupt`, swallows `child_exit` (so the fd stops advertising
`POLLIN` and the next poll genuinely blocks), and defers everything else; the
strided loop poll defers every non-`interrupt` note it passes. `Env` holds them
FIFO and `deliver_pending_notes` fires them **before** draining the live queue,
preserving arrival order across the boundary.

### The note mask mirrors the kernel's, and `mask note` swaps it whole

The shell keeps a model of the kernel note mask (`Env.note_mask`) seeded to
`just(Pipe)` — the process default is pipe-masked (a pipe write to a dead reader
is EPIPE, not death), and `mask note` SWAPS the whole kernel mask, so the model
must carry `pipe` or the first `mask note` block would clobber it and a later
shell pipe-write would terminate instead (#237). Registering a handler with `on
note <name>` *unmasks* that note's class, so an `on note 'pipe'` handler can
actually receive the note it was written for — the mirror of pouch's
`sigaction(SIGPIPE, handler)` clearing the mask bit. Names map to kernel classes
through `note_class_for_name`, where any `tty:`-prefixed name (`tty:susp`,
`tty:winch`, …) resolves to the single `NoteClass::Tty` bit, so `mask note
'tty:susp'` masks the whole tty family — matched by prefix rather than by
enumerating the five, because before this arm existed that exact `mask note
'tty:susp'` parsed, ran its body, and masked nothing.

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
  because `alloc` has it and `HashMap` would pull in a dependency). `$status`,
  `substitutions` (a `u64` count of substitutions run, compared only across two
  readings) and `eval_depth` are `Cell`s so the `&Env` expression path can
  write them; every other field is plain.
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

Three interior-mutability points exist for one reason each: `$status` so a
command substitution reached through `&Env` can record the inner command's exit,
`substitutions` so the same path can count itself, and `eval_depth` so it can
charge the shared recursion counter.

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
- **A statement settles `$status` after expanding its own words, never before.**
  A reset placed before the expansion passes every test that reads the register
  from Rust and silently zeroes every `$status` a script reads. That is exactly
  how the `e9e0aa92` ordering went unnoticed for three months. A new statement
  kind, or a new path through an existing one, earns a leg in `u-builtin-test`
  8b that reads `$status` through a statement.
- **"A substitution ran" is counted, never inferred.** Any test of the form
  "`$status` changed during the expansion" misreports a substitution that exits
  with the value already in the register. `u-subst-test` 6b pins that case
  (`false` then a bare `$(seq)` line reports 1).

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
- **Positional parameters are bound but cannot be referenced.**
  UTOPIA-SHELL-DESIGN 5.5 says *"`$1`, `$2`, ... refer to positional args; `$*`
  is the list"*, and `invoke_function` and `Repl::run_script` do bind `1`..`N`
  and `*`. But the lexer starts a variable name only at `[a-zA-Z_]`, so `$1` is
  a parse error (`EmptyVarName`) and `$*` never lexes. Functions use named
  parameters (`fn f a b { ... }`). Tracked as #138, a v1.x item in
  `docs/UT-NORA-ERGONOMICS.md`.

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

- **The job table was made pure specifically to be host-testable, and for its
  whole life that property did not exist. It does now (2026-09-22).** The module
  header gives the design rationale: the table performs no syscalls, so the REPL
  must drive the reaping and feed results back, *"keeping the table pure makes
  it host-testable against injected `(pid, status)` pairs."* The crate's tests
  did not compile at all (task #105) — a real design constraint accepted to buy
  a property that had never existed. The `backend` feature split fixed that:
  `eval::jobs` is on the pure side of the line and its tests run.

  **`eval::expr` and the syscall-bearing modules are still behind the gate, and
  for `expr` the reason is worth stating.** It makes no syscall of its own, but
  expansion genuinely reaches into `stmt` (command substitution) and `env` —
  `$(...)` runs a pipeline. That is coupling in the shell's design, not an
  accident of imports, so its 29 tests stay stranded until someone restructures
  the evaluator. (Its use of `glob` is the pure matcher only -- the `matches`
  operator and case-as-expression -- which stopped being a coupling on
  2026-09-23.) `builtin`, `console`, `env`, `pathname` and `stmt` are gated
  because they do call syscalls, which is exactly the layering rule the crate
  already states: only the built-ins whose purpose is to mutate THIS Proc reach
  for one. At 2026-09-23 the crate still strands 71 of its 409 tests: `eval::expr`
  29, `eval::stmt` 14, `eval::env` 5, and `repl` 23 ([[sub-utopia-interactive]]).
  `tools/test-rust.sh` prints the figure per crate ([[sub-substrate-gates]]).

  **Globbing was split the same way (2026-09-23).** `eval::glob` had grown the
  argv-time filesystem walk beside the pattern matcher, so the matcher's eleven
  tests -- every one of them about matching, none about the filesystem -- were
  stranded with the one function that calls `fs::read_dir`. `glob` is the pure
  matcher again, as its own header always said it was, and its tests run on the
  host; `eval::pathname` (POSIX's "pathname expansion") holds `expand` and its
  walk, moved byte-for-byte and gated, and `u-glob-test` witnesses it at boot.

  **`console`'s vocabulary was lifted out** into `eval::discipline`
  (2026-09-22), and it is the clearest case for why the `backend` split was
  worth making. The mode strings, the screen-restore sequence and the two name
  predicates are pure; `is_raw_command` in particular is the hardcoded basename
  set deciding whether a child gets the raw-mode dance — extended for `lantern`
  in that same arc — and it sat in a module gated for three unrelated
  `t_write`/`t_fstat` calls, so its tests ran nowhere. The module had already
  met the problem and worked around it locally rather than escalating: four
  compile-time asserts mirroring four `#[cfg(test)]` assertions, under a
  comment saying the crate *"has no host test harness ... so the `#[cfg(test)]`
  literal asserts below never run. These do."*

  A pure sibling replaces the mirrors with execution. The `const _: ()` guards
  are KEPT anyway, deliberately: they fire on the device build, where a host
  test cannot, so the two now cover different machines rather than the same one
  twice. Measured — dropping `lantern` from the allowlist fails
  `is_raw_command_matches_nora_by_basename` on the host in milliseconds, where
  before it could only have been caught by a boot.

## Provenance

[[chg-2026-08-03-utopia-eval-sweep]].

[[chg-2026-08-16-seven-small-surfaces]] records this interval.

[[chg-2026-09-06-utopia-eval-shell-arc-notes]] folds the `&&`/`||` eval half,
the six-entry `$path`, `cd --`, and the settled notes-mask changes.
