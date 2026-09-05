---
id: chg-2026-08-03-utopia-eval-sweep
type: chg
title: "the ut evaluator — one counter where the parser needed three, and a pure function that spawns processes"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-utopia-eval
  - moc-userspace-shell-tui
established:
  - sub-utopia-eval
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-03
---
Batch 39: the `ut` shell's evaluator — stmt, builtin, expr, env, glob, jobs,
console, value, error, mod. 10 files, 6953 lines. Main unchanged at `2f7cbc83`
(already an ancestor; no sync needed). L-1 absent on the TWENTY-SEVENTH check.

**THE SUBJECT IS THE SHELL'S WHOLE CONSEQUENCE SURFACE.** Where the parser
(batch 38) touches nothing outside its own arguments, this layer spawns
processes, opens files, wires pipes, forwards notes, forms process groups, hands
the terminal to a foreground job and takes it back, and flips the console line
discipline around a full-screen child. Still `audit: light` — the kernel gates
every syscall it issues, so a bug costs the user their session and nothing else
— but within that session the blast radius is the whole thing.

**THE COUNTERWEIGHT IS THE DIRECT ANSWER TO LAST BATCH'S, AND IT IS THE OPPOSITE
ANSWER.** The parser needed THREE recursion counters because no single one saw
all three shapes. The evaluator has ONE — `EVAL_MAX_DEPTH` = 64, charged from
two entry points — and the reason is written at both:

> Shares Env's eval-depth counter with eval_block so the bound holds across
> mixed function/source/subst nesting.

A shell mixes its recursion shapes in one expression (`fn f { echo $(f) }`
recurses through a function call, a substitution and a block on every turn), so
per-shape counters would each stay comfortably under their own bound while the
stack overflowed anyway. **Three counters where the shapes are independent, one
where they compose — both right, for opposite reasons**, and each says which
case it is. Supporting detail, all verified: the counter is a `Cell` so the
`&Env` substitution path can charge it; `eval_recursion_leave` saturates so a
stray leave cannot relax the bound; both guards split into an outer
`enter`/`leave` wrapper around an inner body so the leave is unconditional
across every `?`; and `eval_block` returns early for an EMPTY block *before*
charging, stating why — the error would otherwise need `stmts[0].span`, which an
empty slice does not have.

Also traced sound and worth recording: the reap ground truth is the `wait`, never
the `child_exit` note (which is only a poll wake), so a coalesced or
mask-deferred note is a latency event and not a hang; both note scanners DEFER
what they read past (`try_read` consumes the queue front, so dropping would lose
an `on note` handler); `run_foreground_jc` restores the terminal and the prompt
line discipline BEFORE it branches on the outcome, which makes the stop case
correct by construction; `push_scope`/`pop_scope` have exactly one call-site
pair and it binds the block result rather than `?`-propagating it, so the frame
always pops; and `BUILTIN_NAMES` agrees exactly with `try_builtin`'s dispatch
arms, one of the few hand-maintained mirrors in the tree that has not drifted.

**F1 -- THE MAIN EXPRESSION ENTRY POINT IS DOCUMENTED AS A PURE FUNCTION AND IT
SPAWNS PROCESSES.** `eval_expr`'s doc comment:

> Pure function with respect to the AST; side effects are limited to errors
> raised through `EvalResult`.

Since command substitution landed, an expression containing `$(cmd)` spawns
children, captures their stdout, and writes `$status`. **And the crate knows** —
`Env`'s `status` field comment calls command substitution *"the first
side-effecting expression atom -- it spawns a child + captures its stdout"* and
explains that the field is a `Cell` PRECISELY so a `&Env` evaluation can have
that effect. Two files, one crate, opposite claims about the same function; the
wrong one is the doc comment on the function, which is what rustdoc renders and
what a caller reads.

**F2 -- `export` IS DECLINED FOR WANT OF A MECHANISM THAT EXISTS, AND THE SHELL
ALREADY LINKS THE MODULE THAT SAYS SO (#106).** builtin.rs's deferred list:

> set / export -- envp passing to children does not exist (SYS_SPAWN carries
> argv only); export is meaningless until it does.

The premise is still literally true and the conclusion is false. Since G15 the
environment is the per-Proc `/env` Dev: `kernel/env.c` + `devenv.c`, registered
in `dev.c`, mounted by joey (which *extincts* if the mount fails, so it is on
every boot), and inherited through `env_clone_into` in `rfork_internal` — which
is exactly the property `export` needs. libthyla-rs's own `env.rs` header opens
*"ENVIRONMENT (G15, closed by the /env device)"* and exposes `var()`. **And ut
imports that module**: `bi_cd` calls `libthyla_rs::env::set_current_dir` about a
hundred and fifty lines below the deferred list, with two more call sites in the
REPL.

This is the THIRD instance of the arc's stale-reason family (#89's L-6a refusal,
#99's `Stdio::Null`), and the sharpest, because of *how* it survived: **the
stated reason never became false, only irrelevant.** SYS_SPAWN really does carry
argv only. A reader who checks the sentence confirms it and moves on — unlike
#99, where the cited fact itself went stale and looking would have caught it.
A justification can rot without a single word of it becoming untrue.

**F3 -- A CONSOLE-DETECTION COMMENT NAMES A DEFENSE THE KERNEL REMOVED, AND A
SECOND ONE SILENTLY CARRIES THE CASE (#107).** `pts_slave_n_of_fd0` documents
*"`None` on the console (devcons has no `stat_native` -> fstat fails)"*. Since
#55 devcons HAS `stat_native`, so the call succeeds — and it reports
`T_S_IFCHR`, so the mode gate the function checks FIRST also passes. The correct
answer now rests entirely on the qid check, and only because the kernel side
deliberately made the console's flag bit 41 disjoint from ptyfs's bit 40 and
documented that choice where it made it.

So this is NOT the two-sides-drifting-into-danger shape: the kernel author
removed one defense and built the replacement in the same breath. The defect is
directional — **the userspace comment tells a maintainer that the surviving gate
is redundant**, so a simplification on its authority breaks console detection.

**F4 -- EIGHT OF TEN HEADERS ARE CONSTRUCTION SNAPSHOTS, AND THE STALE HALF IS
THE DANGEROUS ONE (#108).** Every file opens with a header scoped to the
sub-chunk that created it ("Scope at U-6a", "Scope at U-6d-a"). expr.rs is the
sharpest: it lists `Subst` and `Backtick` as DEFERRED (NotImplemented) five
lines above the `use` that imports the substitution runner, and both arms are
live. stmt.rs's deferred list is three-fifths stale — redirects, background `&`
and filesystem glob expansion have all shipped. mod.rs, the module's front door,
still says `$(cmd)` is unimplemented. jobs.rs says process-group machinery is
future work and its own struct carries `pgid` and `stopped` forty lines later.

The asymmetry is the finding: **a stale *implemented* entry is harmless — the
reader goes looking and finds the feature. A stale *deferred* entry tells a
reader that a working feature does not exist**, and there is nothing marking
which half of a half-true list is which. The counter-example is in the same
directory: builtin.rs's deferred list was UPDATED IN PLACE when the alias table
landed, which is the proof that maintaining these is possible and that nobody
did it eight times.

Same family, small: `dispatch_note` carries two STACKED doc comments — the
superseded one left above its replacement — so the rendered documentation says
an unhandled `interrupt` is benign and then that it is the signal the idle poll
loop reads to cancel a line edit.

**#105 GAINS ITS BEST ILLUSTRATION AND ITS FIRST WITNESS.** jobs.rs's header
gives a DESIGN RATIONALE: the table performs no syscalls, so the REPL must drive
the reaping and feed results back, because *"keeping the table pure makes it
host-testable against injected `(pid, status)` pairs."* Its fifteen tests cannot
compile. **A real design constraint was accepted to buy a property that has
never existed.** And console.rs shows the problem being MET and worked around
locally rather than escalated — four `const _: () = assert!(...)` guards mirror
four `#[cfg(test)]` assertions, under a comment that diagnoses #105 exactly:
*"libutopia has no host test harness (the crate is unconditionally `#![no_std]`),
so the `#[cfg(test)]` literal asserts below never run. These do."* Someone
understood it precisely, in writing, and routed around it.

**PATTERN, SIXTEEN BATCHES.** b37 the claim is false and nothing reads it; b38
the claim is false and what reads it is a second implementation written because
the first never worked; **b39 the claim's PREMISE is still true and only its
conclusion died.** That is the failure mode a re-check cannot catch, and it now
has three instances across two batches — enough to call it the userspace plane's
characteristic decay rather than a coincidence: on a plane where nothing is
load-bearing, a justification outlives the world it was written about, and the
more precisely it was argued the longer it survives.

LEDGER, read off the rendered view. Corpus 832 -> **834** (two notes: an
established area needs no new MOC, unlike the last two batches). Coverage
206 -> **216 owned of 421**, 48% -> **51%** — the sweep passes half; unswept
lines 79138 -> **72185** (-8.8%). `usr/utopia` 8/18 -> **18/8**, 11980 -> 5027.
Both metrics moved together for the third batch running.

The owned count was drafted as 215 and rendered 216 — I added ten files to 206
and got the arithmetic wrong. Third batch in a row where a ledger number written
before the render was wrong (b37 predicted the corpus, b38 predicted it again).
The number is cheap to read and apparently impossible to guess; the rule is not
"predict carefully" but **do not write a ledger number until `render` has been
run**, and this line is the third piece of evidence for it.
