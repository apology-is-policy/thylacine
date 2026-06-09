# 94 — `libutopia::eval`

**Status:** U-6a..U-6e-a LANDED (evaluator scaffold + value model + expression eval; statement eval + control flow; external command spawn; multi-element pipeline + pipefail; I/O redirection `<` `>` `>>` heredoc; built-in dispatch `fn -> builtin -> external` + the must-be-internal builtins `cd`/`pwd`/`exit`/`true`/`false`/`unset`/`eval`/`source`/`type`). Glob argv expansion (U-6e-b: the directory-enumeration foundation `fs::read_dir` at -b-1, the `glob::expand` fs-walk + `evaluate_argv` wiring at -b-2) LANDED. Substitution/process-sub (U-6f) and the poll() main loop (U-6g) follow.

---

## 1. Purpose

The evaluator walks the AST produced by `libutopia::parser` and executes it. At U-6a only expression evaluation is implemented — the recursive walker that turns an `Expr` into a `Value`. The U-6 sub-chunks then build outward from this core:

- **U-6a (this chunk):** Value model + Env + `eval_expr` for the no-spawn subset.
- **U-6b:** Statement evaluation (Let / Assign / Return / If / While / For / Case / Try / Trace / OnNote / MaskNote — without external spawn).
- **U-6c:** External command spawn (SimpleCommand → `libthyla_rs::process::Command::spawn`).
- **U-6d:** Pipes + redirection.
- **U-6e:** Built-ins (cd, exit, set, source, fn, alias, eval, type).
- **U-6f:** Substitution (`$(cmd)`, `` `{cmd} ``, process substitution, heredoc).
- **U-6g:** poll() main loop + line-editor integration (the `ut` binary grows a REPL).
- **U-6-test:** Cumulative integration smoke.

This document is the binding reference for the U-6 arc; per-sub-chunk extensions append rows to the Status section.

---

## 2. Module layout

```
usr/utopia/libutopia/src/eval/
  mod.rs     -- module root + re-exports
  value.rs   -- Value type + helpers
  env.rs     -- scope stack + function table + special vars
  error.rs   -- EvalError + EvalErrorKind + EvalResult
  glob.rs    -- rc/POSIX-shape pattern matcher (no fs ops)
  expr.rs    -- the recursive eval_expr walker
```

Re-exports from `eval::mod`:

```rust
pub use env::Env;
pub use error::{EvalError, EvalErrorKind, EvalResult};
pub use expr::eval_expr;
pub use value::Value;
```

The `glob` module is sub-pub (`eval::glob`); future U-6e argv-time expansion will use the same matcher.

---

## 3. The unified-list value model

Per UTOPIA-SHELL-DESIGN.md §4.1 ("Variables are strings. Lists of strings are first-class but list-elements are strings."), §4.2 ("UTF-8 throughout."), and §6.3 ("A list is a list; a scalar is a scalar (a one-element list)."), every Utopia value is a flat list of UTF-8 strings. A "scalar" is the special case of a one-element list.

```rust
pub struct Value(pub Vec<String>);
```

### 3.1 Why unified rather than `enum Value { String, List }`

- Every consumer would have to match the enum to do anything useful; the match arms are nearly identical.
- Scripture explicitly says "a scalar is a scalar (a one-element list)" — the conceptual distinction is preserved, but the underlying representation is uniform.
- argv expansion is a `Vec<String>` by definition (each element → one argv item); having `Value` already shaped that way means argv expansion is `value.0.iter().cloned()`.
- `$"var` (no-split per scripture §6.8) collapses a multi-element value into one element by joining with space — a one-line `Value(vec![self.joined(" ")])`.
- `$#var` length is `Vec::len`.

The decision tracks rc's value model rather than bash/zsh's scalar-vs-array dichotomy.

### 3.2 Constructors + accessors

```rust
Value::empty()           // 0 elements; the result of looking up an undefined var
Value::scalar("hello")   // 1 element
Value::list(vec![…])     // N elements
Value::from(42i64)       // 1-element "42"
Value::from("hello")     // 1-element "hello"

v.len()                  // element count (scripture $#var)
v.is_empty()
v.joined(sep)            // String join
v.as_scalar()            // join with " " (default flatten)
v.as_elements()          // &[String] for argv expansion
v.as_int() -> Option<i64>// parse joined form as i64
v.is_truthy()            // false: empty, "", "0", "false"; else true
v.push(s)
v.extend_from(other)
```

### 3.3 Undefined-variable convention

Looking up an undefined variable evaluates to `Value::empty()` (zero elements), matching rc convention. This is silent in scripture §6 but well-established in rc/Plan 9.

### 3.4 Truthiness

Scripture is silent on expression-value truthiness (separate from `$status == 0` command truthiness). At U-6a:

- Empty value → false.
- One-element value `""`, `"0"`, or `"false"` → false.
- Anything else → true.

This matches the principle that "false-y" values in shells are empty / zero / the literal `false`.

---

## 4. `Env` — the evaluator's runtime state

### 4.1 Scope stack

`Env::scopes: Vec<BTreeMap<String, Value>>`. The bottom frame is the global scope; each function call pushes a new frame. Brace blocks `{ ... }` do NOT push a frame (they share scope with their caller per scripture §5.6). Subshells `( ... )` clone the entire `Env` at their fork point — that's a U-6c/d concern, not an `Env` API surface.

- `push_scope()` / `pop_scope()` — function-call entry/exit. `pop_scope` panics if only the global frame remains (a balance invariant the caller must uphold).
- `depth()` — current scope depth (1 = global only).

`BTreeMap` (not `HashMap`) is used because `alloc::collections::BTreeMap` is available in no_std + alloc; per-frame variable counts are small (< 50), so the O(log n) lookup is negligible.

### 4.2 Variable read/write

Scripture §6.2: "Dynamic by default: a variable assigned in a function is visible in callees. `let` inside a function declares a local that shadows any outer binding for the function's scope. Variables assigned at script-top-level are global."

| Method | Semantics |
|---|---|
| `get(name) -> Value` | Walks the scope stack top-down; returns the first hit. Undefined → `Value::empty()`. |
| `defined(name) -> bool` | Distinct from `get(name).is_empty()` because an explicitly-set empty value is still "defined". |
| `let_set(name, value)` | Writes the topmost frame, shadowing any outer binding. The `let` statement form. |
| `assign(name, value)` | Walks top-down looking for an existing binding; if found, writes there; otherwise writes the global frame. The bare `name = value` form. |
| `unset(name)` | Removes from all frames. The `unset` builtin (U-6e). |

### 4.3 Function table

`Env::fns: BTreeMap<String, FnDecl>`. Per scripture §5.5, function definitions are globally scoped (no function-local function definitions; same as rc).

- `fn_set(decl)` — registers/overwrites.
- `fn_get(name) -> Option<&FnDecl>`.
- `fn_defined(name) -> bool`.

### 4.4 Special variables

Dedicated accessors so that command-exit handling is a single point of contact rather than scattered through pipeline / spawn / wait code paths.

- `$status` (scripture §8.5): `status()` / `status_set(code: i32)`.
- `$errstr` (scripture §8.5; rc tradition): `errstr()` / `errstr_set(s)`.
- `$cwd`: `cwd()` / `cwd_set(p)`. Initialized to `/`; the `cd` builtin updates it.

**Special-variable read/write bridge (U-6e-a).** `Env::get`/`assign`/`let_set` special-case the names `status` / `errstr` / `cwd` and route them to the dedicated fields above (read: `special_get`; write: `special_set`). So `$status` / `$errstr` / `$cwd` read in a *script* resolve to the live field value (scripture §8.5 requires them queryable after every command), and they cannot be shadowed by an ordinary binding. Before U-6e-a these three were field-only — `env.get("status")` searched the scope frames and returned empty, so in-script `$status` was a silent gap since U-6b (the boot probes only ever asserted via the Rust `status()` accessor). The `cd` builtin is what first makes `$cwd` meaningful, so the uniform bridge landed alongside it.

- `Env::exit_requested() -> Option<i32>` / `request_exit(code)`: the `exit` builtin's pending-exit one-shot. `eval_block` short-circuits the whole statement stack to `Return` once it is set (U-6e-a; §9 of `eval::builtin`).

---

## 5. `eval_expr` — the recursive walker

```rust
pub fn eval_expr(env: &Env, expr: &Expr) -> EvalResult<Value>
```

Pure with respect to the AST. Side effects limited to errors raised through `EvalResult`. No mutation of `env` at U-6a (mutation belongs to statement eval at U-6b).

### 5.1 Coverage

| ExprKind | At U-6a | Notes |
|---|---|---|
| `Word(s)` | implemented | scalar in expr context; at argv-time a bare meta-bearing Word fs-expands via `glob::expand` (U-6e-b-2, see §6.3) |
| `SingleQuoted(s)` | implemented | literal scalar |
| `DoubleQuoted(parts)` | implemented | $var + $#var interp; `DqPart::Subst` → NotImplemented |
| `Integer(n)` | implemented | scalar via `core::fmt` |
| `Var(name)` | implemented | env lookup |
| `VarLen(name)` | implemented | length-as-string |
| `VarNoSplit(name)` | implemented | join-with-space scalar |
| `VarIndex(name, idx)` | implemented | 1-indexed; out-of-range → empty (rc) |
| `VarSlice(name, lo, hi)` | implemented | inclusive; clamps to length; lo > hi → empty |
| `Subst(_)` | NotImplemented | $(cmd) needs spawn — U-6f |
| `Backtick(_)` | NotImplemented | `{cmd} needs spawn — U-6f |
| `ProcSubIn(_)` | NotImplemented | <(cmd) needs spawn + fd plumbing — U-6f |
| `ProcSubOut(_)` | NotImplemented | >(cmd) — U-6f |
| `Regex(pat)` | implemented (literal) | returns the pattern as a scalar; matching is `=~` operator (NotImplemented) |
| `List(parts)` | implemented | flatten sub-expressions |
| `Concat(parts)` | implemented | rc-style cross-product (see §5.4) |
| `BinOp(op, l, r)` | implemented | arith / comparison / logical (see §5.2) |
| `UnOp(op, inner)` | implemented | Neg / BitNot / Not |
| `Match(op, l, r)` | partial | Glob + In implemented; Regex → NotImplemented |
| `Case(case_expr)` | implemented | case-as-expression; first-match-wins; NoCaseMatch if exhausted |

### 5.2 BinOp dispatch

The parser emits the same `BinOp` enum for arith / cond / value contexts; the evaluator picks behavior at runtime:

- **Logical (`And` / `Or`):** short-circuit evaluation; result is `"0"` or `"1"`.
- **Comparison (`Lt / Le / Gt / Ge / Eq / Ne`):** numeric if BOTH operands parse as `i64`; otherwise lexicographic string compare on `as_scalar()`. This collapses scripture §6.13 (arith int compare) and §6.14 (top-level `==` is string compare) into one rule and matches rc's behavior. Result is `"0"` or `"1"`.
- **Arithmetic + bitwise + shift:** both operands MUST parse as `i64` (else `NonNumeric` error). All arithmetic uses checked ops: overflow → `Overflow` error, div/mod by zero → `DivByZero`, shift count outside `[0, 63]` → `InvalidShift(n)`.

### 5.3 UnOp dispatch

- `Neg`: integer; `checked_neg` (catches `i64::MIN`).
- `BitNot`: integer; `!i`.
- `Not`: truthiness flip; result is `"0"` or `"1"`.

### 5.4 Concat — rc-style cross-product

Adjacent `^`-joined sub-expressions form a Concat. With list operands, the result is a cross-product (rc behavior):

| Operands | Result |
|---|---|
| `('a')` ^ `('b')` | `('ab')` |
| `('a' 'b')` ^ `('1')` | `('a1' 'b1')` |
| `('a' 'b')` ^ `('1' '2')` | `('a1' 'a2' 'b1' 'b2')` |
| `()` ^ `('x')` | `()` |

Note: bash/zsh do NOT do cross-product; they either concat to first element or error. rc's cross-product is the natural lift of string concatenation to lists, and Utopia adopts it.

### 5.5 Match operators

- `matches` (glob): evaluates both sides to scalars; runs the `glob::matches(pat, input)` matcher. Result `"0"` or `"1"`.
- `in` (list membership): evaluates left to scalar, right to a multi-element value; returns `"1"` if any element string-equals the scalar.
- `=~` (regex): `NotImplemented` at v1.0 (no regex engine in libthyla-rs; v1.x lift).

### 5.6 Case-as-expression

```
let kind = case $file {
    *.c  => 'C source'
    *.rs => 'Rust source'
    *    => 'unknown'
}
```

Per scripture §7.2: each arm produces a single VALUE expression. First match wins. No fallthrough. The arm pattern is evaluated to a scalar, used as a glob against the scrutinee's scalar form. If no arm matches, `EvalErrorKind::NoCaseMatch` is raised at the outer case-expression's span (rc behavior; scripture is silent but exhaustive case typing matches reader expectations).

### 5.7 1-indexed access discipline

`VarIndex` and `VarSlice` use 1-indexed semantics per scripture §6.9. Out-of-range, 0, and negative indices return `Value::empty()` (rc convention). The `EvalErrorKind::InvalidIndex` variant exists but is unused at v1.0 — reserved for a future strict mode.

### 5.8 Command substitution `$(cmd)` / `` `{cmd}` `` (U-6f)

Per scripture §6.6: run `cmd`, capture its stdout (trimmed of trailing newlines), substitute the result. The engine is `stmt::run_command_substitution(env, body, span)` (raw-source bodies — the token/DqPart layer's `TokenKind::Subst(String)` / `Backtick(String)` / `DqPart::Subst(String)`) and `stmt::run_command_substitution_script(env, &Script, span)` (the expr layer's pre-parsed `ExprKind::Subst(Box<Script>)` / `Backtick`). Both validate + drive `capture_external_pipeline`.

- **v1.0 body shape**: a SINGLE redirect-free pipeline of EXTERNAL simple commands (1+ elements). `$(echo x)`, `$(date)`, `$(seq 1 9)`, `$(a | b)` all fit. A **builtin** / function / control-flow / multiple-statement body / a per-element redirect → `NotImplemented`. Two reasons: capturing an in-process command's output needs a fork (or a UART-sink redirect) v1.0 lacks; AND running an in-process body here would leak its `Env` mutations into the parent shell, which a substitution must not (§6.6 "run cmd in a subshell"). Restricting to a pure external pipeline keeps the capture deadlock-free *and* the parent `Env` untouched.
- **Capture** (`capture_external_pipeline`): spawn the pipeline (`exec_pipeline`-style N-1 internal pipes), the LAST element's stdout → a capture pipe whose read end the shell holds; **drain it to EOF BEFORE reaping** (deadlock-free — the established discipline; EOF arrives when the last element exits and #926 closes its fd 1). Reap wait-any + pid-match (the only outstanding children are this op's — all argv, incl. nested substitutions, is evaluated before the outer spawn). Lossy-decode (`from_utf8_lossy`) + strip every trailing `\n`.
- **Field-split by context**: a bare-word substitution whitespace-splits the result into a list (rc's default `$ifs`, `split_fields` — drops empties); one inside `"..."` (or a heredoc body) is inserted verbatim, no split. `for f in $(ls)` iterates; `"x=$(cmd)"` is one word.
- **`$status` (§8.7)**: the inner pipeline's pipefail-aggregated exit becomes `$status` (a non-zero exit *propagates* — `let x = $(false-ish)` fails). This is why `$status` is a `Cell<i32>` (§4) — the substitution runs inside the `&Env` expression evaluator. A spawn failure reports 127; `$errstr` is left unchanged (v1.0 simplification — failure is observable via `$status`). `eval_command` / `eval_let` / `eval_assign` set `$status = 0` *before* evaluating their words/RHS so a substitution-free command/assignment reports 0 while a bare `$(failing)` or `let x = $(failing)` preserves the substitution's exit.
- **Process substitution `<(cmd)` / `>(cmd)`**: `NotImplemented` — §6.12 wants a `/proc/self/fd/N` namespace surface the kernel does not expose yet; a separable v1.x feature, not a dependency of `$(cmd)`.
- **Kernel prerequisite (#926)**: the capture's drain-before-reap only terminates because a process's fds close at *exit*, not reap — see `docs/reference/14-process-model.md` "fd-close-at-exit (#926)".

---

## 6. `glob` — the rc/POSIX-shape pattern matcher + fs expansion

`matches` / `has_meta` are pure pattern matching against a single input string (NO filesystem I/O) — used by case-as-expression and the `matches` operator. `expand` (§6.3, U-6e-b-2) is the argv-time **filesystem walk** that drives the matcher across a directory tree; it is the one entry that touches `fs::read_dir`.

### 6.1 Pattern syntax (per scripture §6.10)

- `*` — zero or more characters, NOT crossing `/`.
- `?` — exactly one character, NOT crossing `/`.
- `[abc]` — character class.
- `[!abc]` — negated character class.
- `[a-z]` — range within a class.
- `**` — recursive (crosses `/`); only valid as a complete path segment. Still NOT special-cased at U-6e-b-2: a `**` segment matches exactly one path component (behaves as `*`), so `expand` does single-level descent per segment; recursive `**` is a v1.x refinement.

### 6.2 Matcher API

```rust
pub fn matches(pattern: &str, input: &str) -> bool
pub fn has_meta(s: &str) -> bool
pub fn match_any(pattern: &str, candidates: &[String]) -> Vec<usize>  // unused at U-6a
```

Bytewise matching; UTF-8 is self-synchronizing so partial-byte matches cannot occur on valid UTF-8 input.

### 6.3 Filesystem expansion — `expand` (U-6e-b-2)

```rust
pub fn expand(env: &Env, pattern: &str) -> Vec<String>
```

The argv-time filesystem walk. `stmt::evaluate_argv` calls it for a bare unquoted word that carries a meta char (a single/double-quoted string, a `$var`, and a `^`-concat word are NOT candidates — rc-lineage: `echo "*.c"` and `echo a^*` stay literal). Algorithm:

1. **Split** the pattern on `/` (dropping empty segments, so `//`, leading `/`, and trailing `/` normalize away; a trailing-slash "directories only" refinement is a v1.x item) and record `leading_slash`.
2. **Literal prefix** — the leading run of meta-free segments is the start directory (named exactly once, not readdir-matched). The walk begins at the first segment with a meta char (the caller's `has_meta` gate guarantees one exists).
3. **Walk** one segment per level: `fs::read_dir(dir)` (resolved against `$cwd` for a relative pattern; the cwd only LOCATES the start dir — it is never prefixed onto the result), then for each entry `matches(seg, name)`. For the final segment, matches are pushed; for a non-final segment, only matching **directories** are descended (`DirEntry::is_dir`). Recursion depth is bounded by the segment count (not the tree depth) — no unbounded descent.
4. **Dotfile rule** (POSIX): a leading-`.` name is skipped unless the segment itself begins with `.`. (`.`/`..` are not emitted by any Dev's readdir, so no special-casing.)
5. **rc nullglob** (scripture §6.10): the match set is the result; if it is **empty** (no match, or an unreadable directory along the way), `expand` returns the empty `Vec` and `evaluate_argv` contributes no argv element — the literal is NEVER substituted.
6. **Sort** the full result once (bash whole-string order; a per-level sort would diverge around the `/` boundary).

The result preserves the pattern's shape: a relative pattern yields relative matches, an absolute pattern yields absolute matches.

---

## 7. Error taxonomy

Every error carries a `Span` from the offending Expr so the U-6 driver (the REPL or a future formatter) can attach the diagnostic to source.

| EvalErrorKind | When |
|---|---|
| `NonNumeric(String)` | arith on a value that fails `parse::<i64>()` |
| `DivByZero` | `/` or `%` with zero RHS |
| `Overflow` | checked arith returned `None` (Add/Sub/Mul/Div/Mod/Pow/Neg/Shl/Shr) |
| `InvalidShift(i64)` | shift count outside `[0, 63]` |
| `NoCaseMatch` | case-as-expression exhausted without matching |
| `InvalidIndex(i64)` | reserved (unused at v1.0; rc returns empty) |
| `NotImplemented(&'static str)` | $(cmd), `{cmd}, <(cmd), >(cmd), `=~` |
| `Internal(&'static str)` | evaluator invariant violation (should never reach user); bug |

`EvalResult<T>` is the convenience alias `Result<T, EvalError>`.

---

## 8. Tests + validation

### 8.1 cfg(test) host unit tests

`#[cfg(test)]` blocks in:

- `eval/value.rs` — (none at U-6a; covered by `expr.rs` indirectly)
- `eval/glob.rs` — 9 tests covering literal, star, no-`/` discipline, `?`, class, negation, range, complex, helpers.
- `eval/expr.rs` — 26 tests covering every implemented ExprKind via the parse-then-eval helper.

These can't run on host directly (libutopia is `no_std`; cargo test wants `std` linkage with global allocator + panic handler). Validation happens at boot via `/u-test`.

### 8.2 Boot probes

`/u-test::flow_eval_expr()` runs 15 composed probes against the live evaluator under `ThylaAlloc` on real arm64:

1. Integer atom in Arith context.
2. Arithmetic add + mul + precedence.
3. Div-by-zero error.
4. Var lookup (scalar).
5. Undefined var → empty Value.
6. VarLen on a 3-element list.
7. VarIndex (1-indexed).
8. VarIndex out-of-range → empty.
9. VarSlice inclusive.
10. VarNoSplit join-with-space.
11. List literal flattening.
12. Concat with var interp.
13. DoubleQuoted interpolation (`$var` + `$#var`).
14. case-as-expression first-match-wins.
15. `$(cmd)` substitution + `=~` regex → NotImplemented.

On success, prints `u-test: eval expr OK` between `u-test: parser full OK` and `u-test: all OK`. u-test now reports **12 cross-module flows**; flow_eval_expr is the new tail.

**`/u-glob-test` (U-6e-b-2).** A dedicated pre-pivot probe (joey-gated on status==0) drives `glob::expand` directly against the flat devramfs root — the load-bearing fs-walk — since argv echoes to a dropped pipe at v1.0 (no terminal-backed fd 1 until U-PTY). It asserts prefix-star (`u-*`), bare-star (`*`: count, sortedness, no `/` on a flat dir, no dotfile leak), single-char (`versio?`), char-class (`[vw]*`), absolute (`/u-*`), and rc nullglob (`no-match-* -> []`). The `evaluate_argv` wiring is then observed via `$status`: a bare glob matching nothing nullglobs to an empty command (`status 0`), while the same pattern quoted is a literal arg spawned NotFound (`status 127`) — the delta proves the bare branch globbed and the quoted branch did not. Prints `u-glob-test: all OK`.

### 8.3 Helper discipline

The boot probe's `ev(env, s, ctx)` helper tokenizes the source fragment, **strips the trailing `Eof` token** (the same discipline parse.rs uses at each placeholder site — `parse_expr_tokens` reports `TrailingTokensInExpr` if Eof is present), parses to an `Expr`, and evaluates. The `cfg(test)` helper in `eval/expr.rs::tests::eval_str` follows the same pattern.

### 8.4 The cumulative arc-close smoke (`/u-6-test`, U-6-test)

Where §8.1/§8.2 + the per-sub-chunk probes prove each U-6 surface in
ISOLATION, `/u-6-test` (the last sub-chunk of the U-6 arc; joey-gated,
pre-pivot, pure userspace) proves they **compose**: realistic
multi-feature scripts in one evaluation pass. The only pre-PTY
observables are `Env` state, `$status`, and -- the key integration lever
-- command substitution, which captures a real spawned/piped/globbed
command's stdout into a variable. Seven flows, each weaving >=3 features:

1. **subst-capture -> `for` -> `case` -> arith** -- `$(echo a.c b.rs c.c
   d.h)` captures + splits into a 4-element list; a `for`/`case`/`(( ))`
   counts the `*.c` matches.
2. **glob -> spawn -> subst-capture** -- `$(echo u-*)` runs the glob in
   echo's argv (the `evaluate_argv` fs-walk), captures the printed
   matches, splits them; asserts the full glob->argv->spawn->stdout->
   capture->split chain, including the `u-6-test` self-reference.
3. **var-interp -> pipeline -> subst** -- `$(echo hello $phrase | tr a-z
   A-Z)` -> `(HELLO WORLD)` (two externals, var in the upstream argv,
   bare-context field-split).
4. **builtin -> special-var -> control flow** -- `cd /srv` then `if
   ($status == 0) { let loc = "now at $cwd" }` (the §4.4 bridge read in
   both Cond and Dq contexts).
5. **var -> heredoc-interp -> redirect -> spawn** -- a `let mid = DATA`
   defined in the same script feeds an interpolated `<< EOF` body to
   pipe-sink's fd 0 (exits 0 iff the 13-byte payload arrives intact).
6. **the capstone: `Repl::feed` end-to-end** -- a multi-statement session
   driven through the REAL read-parse-eval loop (not `eval_source`):
   `let name`, `let up = $(echo $name | tr a-z A-Z)`, `let tag = "[$up]"`,
   `exit 0` -> the whole U-6 stack through the U-6g entrypoint, state
   carried across feeds.
7. **`Repl::feed` error survival + subst** -- a parse-error line draws a
   diagnostic but does not end the session (§8.9), and a subsequent
   `$(echo recovered)` line still evaluates.

Prints `u-6-test: all OK`; joey reports `U-6 evaluator arc integration
verified`. This is a dedicated probe (composition-focused, cleanly
revertable) rather than a `u-test` extension (whose `flow_eval_*` blocks
test in isolation).

### 8.5 The cumulative arc-close smoke (`/u-7-test`, U-7-test)

The U-7 analog of §8.4: where `/u-job-test` (19 scenarios) proves each
U-7 surface in ISOLATION -- the `&` background launch + the job table,
the jobs/fg/bg/wait/kill builtins, `on note`/`mask note` delivery, and
the interruptible foreground wait -- `/u-7-test` (the U-7 arc's last
sub-chunk; joey-gated, pre-pivot, pure userspace) proves they
**compose**, the way a real interactive session weaves them. Four flows,
each weaving >=2 U-7 surfaces:

1. **bg-demux + builtins (U-7a + U-7b)** -- two `echo &` jobs are
   registered; a foreground `seq` (exits 1) runs, and the U-7-pre by-pid
   foreground wait reaps ITS pid only, so both background jobs survive
   the demux with the foreground `$status` intact. `wait` (no args) then
   reaps every job and `jobs` consumes the now-Done entries exactly once,
   draining the table. No note queue is opened, so the foreground wait
   takes the DEGRADE path (a plain blocking by-pid reap).
2. **forward-vs-handle by fg-presence (U-7c-a + U-7c-b)** -- ONE
   `on note 'interrupt'` handler over one counter, observed in two
   contexts: at the idle prompt an `interrupt` runs the shell handler
   (`deliver_pending_notes`, the counter increments); during a foreground
   command the same note is FORWARDED to the child by
   `wait_pids_interruptible` and NEVER dispatched to the handler (the
   counter is unchanged across the wait -- deterministic in both the
   immediate-reap and the poll-and-forward timing arms, since
   `wait_pids_interruptible` has no `on note` dispatch).
3. **mask + reap (U-7c-a mask + U-7a/U-7b)** -- a background job is
   registered and an `interrupt` queued; a `mask note 'interrupt'` body
   then runs `wait %1`, reaping the job WHILE the note is held back by
   the kernel Thread mask. The deferred handler fires only at the
   post-block drain (scripture 10.8), AFTER the job-control op completed
   inside the masked region.
4. **the capstone: integrated `Repl::feed`** -- the whole stack driven
   through the REAL prompt cycle (`run_line` -> `reap_jobs` ->
   `deliver_notes`): `open_notes` arms the integrated note-delivery half,
   an `on note` handler is registered + delivered through feeds, a `&`
   job is backgrounded and drained by the integrated reaper, and `exit 0`
   ends the session -- all through the U-6g entrypoint, state carried
   across feeds.

The harness cannot inject a real `/dev/cons` Ctrl-C (A-4c), so a
foreground interrupt is self-posted (the `interrupt` the kernel console
owner would post); every spawned child is reaped so none orphans to joey.
Prints `u-7-test: all OK`; joey reports `U-7 job-control arc integration
verified`. A dedicated probe (matches the per-sub-chunk pattern, cleanly
revertable) rather than a `/u-job-test` extension (whose 19 scenarios
test in isolation).

---

## 9. Background jobs + job-control builtins (U-7a / U-7b)

`eval::jobs` (`jobs.rs`) is the **pure** half of job control: it records the
pids of `&`-launched jobs, decides when a job is "done", and formats the
`[N]+ Done` line. It performs **no syscalls** — the actual reaping
(`wait_pid_for` WNOHANG) is driven by the `ut` REPL's prompt-cycle reaper
(`Repl::reap_jobs`, `docs/reference/108-utopia-repl.md`), which feeds results
back via `mark_reaped`. Keeping the table pure makes it host-testable against
injected `(pid, status)` pairs. The table lives in `Env` (`jobs()` /
`jobs_mut()`), alongside the function table + note-handler registry.

### 9.1 The bash model, minus process groups

A `&`-launched **pipeline** (`a | b &`) is ONE job tracking N pids. The
`[N] pid` launch announcement prints the LAST element's pid (bash's `$!`); the
job is "done" only when EVERY element has been reaped. Process-group machinery
(`setpgid`, the controlling-terminal foreground pgid, `SIGTSTP`/`SIGCONT`
stop/resume) is U-PTY territory; at U-7a a job is just a set of pids the shell
reaps in the background. `JobTable::add` assigns `max(specs)+1`, or 1 when the
table is empty — numbers climb while jobs coexist and reset to 1 once the table
drains (bash-like); no spec is reused while its job is live.

### 9.2 `JobTable` API

```rust
pub fn add(&mut self, pids: Vec<i32>, cmd: String) -> u32;   // -> [N] spec
pub fn mark_reaped(&mut self, pid: i32, status: i32);        // idempotent; unknown pid ignored
pub fn live_pids(&self) -> Vec<i32>;                          // unreaped pids, for the WNOHANG poll
pub fn take_done_notifications(&mut self) -> Vec<String>;     // "[N]+ Done  cmd" + removes done jobs
pub fn is_empty(&self) -> bool;  pub fn len(&self) -> usize;  pub fn iter(&self) -> ...;
// U-7b jobspec resolution:
pub fn current_spec(&self) -> Option<u32>;   // %% / %+ -- highest spec (most recent)
pub fn previous_spec(&self) -> Option<u32>;  // %-     -- second-highest
pub fn spec_pids(&self, spec: u32) -> Option<Vec<i32>>;  // %N -> the job's element pids
pub fn cmd_of(&self, spec: u32) -> Option<&str>;         // for `fg`'s command echo
pub fn remove(&mut self, spec: u32) -> bool;             // `fg` consumes a job silently (no Done line)
```

### 9.3 `&` spawn + the foreground demux

`eval_pipeline` routes `p.background` to `eval_background_pipeline`, which
shares the spawn phase with the foreground path via `spawn_pipeline_elements`
(validate external-simple-only / resolve redirects / allocate the N-1 pipes /
spawn-all / feed heredocs — works for n == 1). The background path registers
ONE job, prints `[N] PID`, and sets `$status` 0; it does NOT wait.

The **foreground demux** is pid-precise: both `exec_pipeline` and
`capture_external_pipeline` reap each spawned element with
`wait_pid_for(pid, 0)` (the U-7-pre by-pid primitive) instead of the old
wait-any loop, and `exec_external` / `exec_external_redirected` use
`Child::wait` (also by-pid since U-7-pre). So a foreground command NEVER reaps
a coexisting background zombie — `cmd & ; producer | consumer` cannot lose the
bg child's exit to the pipeline's reap.

### 9.4 Reaping cadence + the note-delivery sync point

`Repl::reap_jobs` runs once per accepted line (after `run_line`, before the
fresh prompt — bash's `[N]+ Done`-before-prompt ordering, catching jobs that
finished while a foreground command was waiting). It is ONE WNOHANG poll per
live pid (never a busy-loop → cannot starve a child; U-7-pre F1). A bg job that
finishes while the user is idle at the prompt is reported at the next accepted
line. `Repl::deliver_notes` (§9.6) runs immediately after, at the SAME sync
point — the cons fd has no `.poll` hook until U-PTY, so notes are delivered
between commands rather than asynchronously mid-edit. The WNOHANG poll is the
complete reap ground truth, so this sync-point cadence loses no correctness,
only idle-time promptness.

Forwarding a `interrupt` note to a RUNNING foreground command (the
interruptible foreground wait) **landed at U-7c-b** — see §9.7.

### 9.5 Job-control builtins (U-7b)

`jobs` / `fg` / `bg` / `wait` / `kill` are dispatched through
`builtin::try_builtin` (the same `fn → builtin → external` order as the
U-6e-a builtins) and operate on `env.jobs_mut()` + the U-7-pre `wait_pid_for`
primitive. `builtin::reap_background(env)` is the shared WNOHANG poll-and-mark
half of reaping — `Repl::reap_jobs` and the `jobs` refresh both call it (the
table is pure, so the syscall-driven poll lives here, not in `jobs.rs`).

| Builtin | Behavior |
|---|---|
| `jobs` | Refresh (WNOHANG poll), list each job `[N]{+|-}  Running\|Done  cmd`, then CONSUME the reported-Done jobs — so a job is announced exactly once whether observed by `jobs` or by the prompt cycle (bash). |
| `wait [id...]` | Block (by-pid `wait_pid_for(pid, 0)`) on the named jobs/pids, or ALL jobs with no argument. `$status` = the last id's exit (0 for wait-all). Completed jobs stay in the table; the prompt cycle emits their Done lines (bash prints those after `wait` too). |
| `fg [%N]` | Block on a RUNNING job to completion (it becomes the foreground command), echo its cmd first (bash), then `remove` it silently — no `[N]+ Done`. `$status` = the job's exit. v1.0 has no STOPPED-job resume (U-PTY). |
| `bg [%N]` | v1.0 has no stopped jobs to resume (Ctrl-Z + SIGCONT = U-PTY), so it reports the named job is already running in the background (bash-faithful), `$status` 0. |
| `kill {%N\|pid} [interrupt\|kill]` | Post a note to a job's element pids / a pid. Default `interrupt` (catchable Ctrl-C analogue); `kill` is the non-catchable force. |

**Jobspec syntax.** `%N` / `%%` / `%+` (current) / `%-` (previous) are the
bash jobspecs; for `fg`/`bg` a bare decimal is also a job number, and for
`wait`/`kill` a bare decimal is a child pid (bash's split). `%N` lexes because
`%` is a command-context word char (`lexer.rs::is_word_char_byte`); it was a
hard `UnexpectedChar` before U-7b, so the addition is purely additive and also
fixes literal `%` arguments (`echo 100%`). Arith `%` (modulo) is unaffected —
the expression parser re-lexes word TEXT (`retokenize_arith_word`), splitting a
word on `%` back into the `Percent` operator in arith context.

**`kill` note set.** The kernel's userspace-postable note names are
`{interrupt, kill}` only — `snare:*` is reserved for kernel-synthetic fault
posters (`notes_post` rejects a userspace `snare:`-prefixed name). So
scripture §10.6's `snare:term` default is unsendable from userspace; `kill`
defaults to `interrupt` (the same note the shell's own Ctrl-C path will deliver
at U-7c), with `kill` for the non-catchable force.

### 9.6 `on note` / `mask note` runtime delivery (U-7c-a)

The `on note 'name' { body }` handler registry (`Env::note_handlers`,
registered since U-6b) is fired at runtime by `deliver_pending_notes(env)`
(`eval::stmt`, re-exported from `eval`). The REPL prompt cycle calls it (via
`Repl::deliver_notes`) at the same sync point as `reap_jobs`.

```rust
// eval::stmt — re-exported as eval::deliver_pending_notes
pub fn deliver_pending_notes(env: &mut Env);   // drain + fire handlers; no-op if no note fd

// Env note-queue API (the consumer opens the fd on-target):
fn set_notes(&mut self, notes: Option<libthyla_rs::notes::Notes>);
fn notes(&self) -> Option<&libthyla_rs::notes::Notes>;
fn note_mask(&self) -> NoteMask;
fn note_mask_add(&mut self, class: NoteClass) -> bool;  // true = newly masked
fn note_mask_remove(&mut self, class: NoteClass);
```

**Delivery.** `deliver_pending_notes` drains the shell's own note queue via
`Notes::try_read` (a `poll(timeout=0)` + `read`, so an empty OR masked-only
queue returns `None` and the drain stops — never blocks, never spins). Each
note: a registered `on note` handler wins (its body is `eval_block`'d
synchronously, best-effort — a body error is swallowed but recorded in
`$errstr`); an unhandled note takes its built-in disposition (an `interrupt`
with no handler is benign at the sync point — the running-foreground forward is
U-7c-b; `child_exit` is informational, the WNOHANG reaper is the ground truth).
Delivery is **transparent to `$status`** (saved + restored around the drain,
like bash saving `$?` around a trap), so a handler firing between commands
never clobbers the next prompt's status. The drain is unbounded (it runs until
the queue empties); a handler that re-posts its OWN note to self would loop —
the same user-pathology bash's `trap 'kill -INT $$' INT` has, and not reachable
today (no shell verb posts an arbitrary note to self; `kill` needs a pid/jobspec
target). The kernel note queue is otherwise bounded (it coalesces `child_exit`),
so the drain terminates.

**`mask note 'name' { body }`** (scripture §10.8) maps the name to a kernel
`NoteClass` (`note_class_for_name`: only `interrupt`/`pipe`/`child_exit` map;
`kill` is non-catchable, `snare:*`/user names cannot arrive) and, while the
shell's note fd is open, sets the per-Thread kernel mask via
`t::notes::set_mask` for the body's duration. A matching note arriving mid-body
is dequeue-deferred (`devnotes_poll` advertises POLLIN only for a mask-permitted
note, so a held note cannot spin a polled wait); on block exit the mask is
restored and `deliver_pending_notes` fires any held handler. Without an open fd
the mask is pure book-keeping and delivery is governed by sync-point timing
alone.

**The note fd is opened lazily.** `Repl::open_notes` (`Notes::open_self`) is
called by the `ut` main loop only after the first successful read confirms a
real input stream. A bare-spawned `ut` (the boot check) has an EMPTY handle
table, so opening the note fd eagerly would mint it at **fd 0** — the read loop
would then block on the note queue instead of EOFing. A session `ut` already
holds fd 0/1/2 from login, so its note fd lands at 3+. The note fd is NOT added
to the main poll set (the cons fd is unpollable until U-PTY, so it would
short-circuit any multi-fd poll there).

### 9.7 Interruptible foreground wait (U-7c-b)

A foreground command holds the shell until it exits. While it runs the shell is
NOT reading the console (the line editor is idle), so a Ctrl-C reaches the shell
as an `interrupt` NOTE (posted by the kernel console owner), not keystrokes. The
foreground reap becomes a poll on the note queue so the `interrupt` is seen the
moment it arrives and FORWARDED to the running job's pids — scripture §10.2: a
foreground job present → forward, do NOT run the shell's own `on note` handler.

```rust
// eval::stmt — re-exported as eval::wait_pids_interruptible
pub fn wait_pids_interruptible(env: &mut Env, pids: &[i32]) -> Vec<i32>;
```

The three direct foreground command waits route here: `exec_external` (a bare
`cmd`), `exec_external_redirected` (`cmd > f`), and `exec_pipeline` (`a | b`).
Each was a blocking by-pid reap; each now calls `wait_pids_interruptible` with
the spawned pid(s) and takes the returned status(es).

**The loop.** Each iteration WNOHANG-reaps every still-live pid (the by-pid
`wait_pid_for` is the reap ground truth, U-7-pre), then — if any pid remains —
blocks on the note fd for up to `FG_WAIT_BACKSTOP_MS` (100 ms) and drains
whatever the wake delivered:

| Drained note | Disposition |
|---|---|
| `interrupt` | Forward `send(Pid(pid), "interrupt")` to each still-live foreground pid. Permission-gated to the shell's children; the result is ignored (a forward to an exited / native non-reader child is inert — it matters to a pouch/musl child as async SIGINT). |
| `child_exit` | Swallowed (the WNOHANG loop is the reap truth). Consuming it clears the fd's POLLIN so the next poll genuinely blocks rather than spinning. |
| any other | Deferred (`Env::defer_note`) to the post-command `deliver_pending_notes` — `try_read` consumes the queue front, so a handler-bearing note read past while looking for an `interrupt` must be retained, not dropped, or its handler would be lost. §9.6's drain fires the deferred notes FIFO, before the live queue. |

The `child_exit` note (posted on every child exit) is the immediate wake; the
100 ms backstop is the safety net for a coalesced / lost / mask-deferred
`child_exit`, so the wait can never hang and the kernel poll never becomes a
busy loop. Forwarding `interrupt` does NOT kill the child — the child decides;
the loop keeps reaping until the child actually exits (bash's Ctrl-C semantics).

**Degrade.** With no note fd open (`env.notes() == None` — host tests, the
bare-spawn boot check, pre-`open_notes`) the helper is a plain blocking by-pid
wait with no forwarding: there is no console to deliver a Ctrl-C and nothing to
forward. A vanished / not-our-child pid yields status 0 (matching the prior
reap loops; the old single-command `wait failed → status 1` arm was unreachable
for a just-spawned child — nothing else reaps it).

**Scope at v1.0.** Only the three direct foreground command waits forward. The
command-substitution capture (`$(cmd)`, §5.8) stays a blocking
drain-before-reap (it blocks on a capture pipe, not the note fd) and the `wait`
builtin (§9.5) stays a blocking bg-job reap; both gain forwarding when process
groups land (U-PTY). The harness cannot inject a real `/dev/cons` Ctrl-C (the
A-4c constraint), so the probe self-posts the `interrupt` the console owner
would post (`/u-job-test` scenarios 16-19).

---

## 10. Open questions deferred to later sub-chunks

| Question | Owning sub-chunk |
|---|---|
| `$(cmd)` substitution: how is the subshell forked, stdout captured? | U-6f |
| ~~Redirects (`>` `<` `>>` `<<`)~~ -- **LANDED at U-6d-b** (resolve_redirects + the libthyla_rs::fs create-or-open/append lift) | ~~U-6d-b~~ done |
| Redirect on an in-process command (fn / BraceBlock); large-heredoc-to-non-reader; atomic O_APPEND | U-6f / v1.x |
| Job control: ~~`&` background~~ **LANDED at U-7a** (§9; `eval::jobs` + `eval_background_pipeline` + the by-pid demux + `Repl::reap_jobs`); ~~`jobs`/`fg`/`bg`/`wait`/`kill` builtins~~ **LANDED at U-7b** (§9.5); ~~`on note`/`mask note` runtime delivery~~ **LANDED at U-7c-a** (§9.6; `deliver_pending_notes` + the `Env` note-queue API); ~~Ctrl-C foreground forwarding~~ **LANDED at U-7c-b** (§9.7; `wait_pids_interruptible` -- the interruptible foreground wait + the deferred-note queue) | ~~U-7 / U-7b / U-7c-a / U-7c-b~~ done |
| ~~Glob expansion at argv-time: file-tree walk, no-match → empty (rc nullglob)~~ -- **LANDED at U-6e-b** (-b-1 `fs::read_dir`; -b-2 `glob::expand` + `evaluate_argv`, §6.3). Recursive `**` semantics + trailing-slash dirs-only + for-list/let-RHS expansion (expr context) remain v1.x | ~~U-6e-b~~ done |
| `set` / `export` (no envp to children yet), `alias`/`unalias` (alias table), `read` (terminal fd 0), `history`, `note` (`note send/list/wait`) -- deferred builtins. ~~`jobs`/`fg`/`bg`/`wait`/`kill`~~ LANDED U-7b (§9.5) | U-6g / U-7c / v1.x |
| Subshell `( cmds )`: fork + isolated env (libthyla-rs has spawn-then-exec, not fork) | U-6f or later |
| In-process pipeline elements (fn / BraceBlock / Arith as a stage) -- need fork | U-6f or later |
| Richer child exit status (literal code, not kernel's binary 0/1) -- needs kernel `exit_status` u64 | Phase 5+ kernel work (deferred) |
| `=~` regex: which regex engine? bytewise vs codepoint? | v1.x |
| Cross-dataset patterns (Stratum-aware globs?) | post-v1 |
| Interactive vs script implicit-fail mode (scripture §8.9) | U-6b |
| Stdio inheritance: replace v1.0 Piped-then-drop with `Stdio::Inherit` when fd 0/1/2 wire | U-PTY + U-6g |

---

## 11. Status

| Sub-chunk | Commit | What |
|---|---|---|
| **U-7-test** | `70c1916` | The U-7 job-control arc **cumulative composition smoke** (§8.5) -- **closes the U-7 arc**. New dedicated pre-pivot `/u-7-test` (joey-gated on status==0; 4 composed flows, each weaving >=2 U-7 surfaces): (1) bg-demux + builtins -- two `echo &` jobs survive a foreground `seq`'s by-pid demux, then `wait` + `jobs` drain them (the foreground wait takes the no-note-queue DEGRADE path); (2) forward-vs-handle -- ONE `on note 'interrupt'` handler + counter, fired at the idle prompt (`deliver_pending_notes`) but NOT during a foreground wait, where the same note is forwarded to the child by `wait_pids_interruptible` (the counter is unchanged across the wait in both timing arms); (3) mask + reap -- a `mask note 'interrupt'` body runs `wait %1` while the note is deferred, the handler firing only at block exit AFTER the reap; (4) the capstone -- the whole stack through the real prompt cycle via `Repl::feed` (`open_notes` + handler register/deliver + a backgrounded job drained by the integrated reaper + `exit 0`). Where `/u-job-test` (19 scenarios) tests each U-7 surface in ISOLATION, this proves they COMPOSE the way a real session weaves them. A foreground interrupt is self-posted (A-4c blocks `/dev/cons` injection); every spawned child is reaped. PURE USERSPACE -- zero kernel files (kernel binary byte-identical); no SMP gate, not a formal-audit-trigger surface (self-reviewed: every flow's timing arms traced deterministic, no orphaned children, bounded reap loops). Verified: `make test-tcg` x2 + `u-7-test: all OK` (4 flows) + `joey: /u-7-test reaped status=0; U-7 job-control arc integration verified` + `u-test`/`u-6-test`/`u-job-test`/all probes `all OK` + login E2E + `Thylacine boot OK`, **0 EXTINCTION** both boots. |
| **U-7c-b** | `1218615` | **Interruptible foreground wait + Ctrl-C forward** (§9.7) -- the second half of U-7c. New `wait_pids_interruptible(env, &[pid])` (`eval::stmt`, re-exported) replaces the blocking by-pid reap in all THREE foreground command waits (`exec_external`, `exec_external_redirected`, `exec_pipeline` -- the handoff named two; the bare-`cmd` path is the most common Ctrl-C case, so completeness took all three). Each iteration WNOHANG-reaps every still-live pid (the U-7-pre by-pid `wait_pid_for` ground truth) then, if any remain, blocks on the note fd for up to `FG_WAIT_BACKSTOP_MS` (100 ms) and drains the wake: `interrupt` -> `send(Pid(pid), "interrupt")` to each still-live fg pid (scripture §10.2: forward, don't run the shell handler); `child_exit` -> swallow (clears POLLIN so the next poll blocks, not spins); any other note -> `Env::defer_note` (new deferred-note queue) so its handler fires at the post-command `deliver_pending_notes` (which now drains deferred FIFO before the live queue) rather than being lost (`try_read` consumes the queue front). DEGRADES to a plain blocking by-pid wait when `env.notes() == None` (host / boot-check / pre-open). **PURE USERSPACE** (libutopia + `/u-job-test`; built on U-2e `t::notes` + U-7-pre `wait_pid_for`) -> NOT a formal-audit-trigger surface (self-reviewed: backstop bounds every coalesced/lost/mask-deferred `child_exit` so no hang + the kernel poll never spins; swallowing `child_exit` is consistent with the bg reaper, which WNOHANG-polls `live_pids` not notes; degrade path identical to the prior blocking reap; borrow-clean drain/defer). **Scope**: command-substitution capture + the `wait` builtin stay blocking (no forward) at v1.0 -- both gain forwarding with process groups (U-PTY). `/u-job-test` +4 scenarios (16 forward-to-live-child SEND; 17 `wait_pids_interruptible` drains a pre-queued interrupt + reaps without hanging; 18 the degrade path; 19 a non-interrupt note deferred-not-lost). Verified: `make test-tcg` x2 + `u-job-test: all OK` (19 scenarios) + `u-test`/`u-6-test`/all probes `all OK` + `Thylacine boot OK` + login E2E OK, **0 EXTINCTION** both boots. The U-7c arc is COMPLETE; the U-7 arc closes at U-7-test. |
| **U-7c-a** | `377b866` | `on note` / `mask note` runtime delivery (§9.6). New `deliver_pending_notes(env)` (`eval::stmt`, re-exported) drains the shell's own note queue (`Notes::try_read`) at the REPL prompt-cycle sync point (`Repl::deliver_notes`, beside `reap_jobs`) and fires registered `on note` handlers (`eval_block`, best-effort, `$status`-transparent). `eval_mask_note` rewritten from a U-6b body-only pass-through to set the per-Thread kernel mask (`t::notes::set_mask` via the new `Env::note_mask_add/remove`) for the body's duration -> a matching note is dequeue-deferred (`devnotes_poll` POLLIN gates on mask) and fires at the post-block drain (scripture §10.8). `note_class_for_name` maps `interrupt`/`pipe`/`child_exit` -> `NoteClass` (`kill`/`snare:*`/user names don't map). New `Env` note-queue API (`set_notes`/`notes`/`note_mask*`); the note fd is `Option<Notes>`, opened lazily by `Repl::open_notes` from the `ut` main loop AFTER the first read (a bare-spawn `ut` has an empty handle table -> eager open would mint the note fd at fd 0 and the read loop would block on it). **PURE USERSPACE** (libutopia + `ut` + `/u-job-test`; built on the U-2e `t::notes` substrate) -> NOT a formal-audit-trigger surface (self-reviewed: try_read non-blocking drain never spins, masked-queue returns None, borrow-clean drain/dispatch, `$status` save/restore, no fd-0 clobber). The running-foreground Ctrl-C forward is split to **U-7c-b**. `/u-job-test` +3 scenarios (13 on-note-fires-on-delivery + exactly-once; 14 unhandled-note-benign; 15a parsed `mask note` defer+drain, 15b the mask mechanism directly). Verified: `make test-tcg` x2 + `u-job-test: all OK` (15 scenarios) + `Thylacine boot OK` + login E2E OK, **0 EXTINCTION** both boots. |
| **U-7a** | `4808ea0` | Background jobs (§9). New pure `eval::jobs` (`JobTable` + `Job` in `Env`); `eval_pipeline` routes `&` -> `eval_background_pipeline`; the shared `spawn_pipeline_elements` helper factored out of `exec_pipeline`; both `exec_pipeline` + `capture_external_pipeline` reaps converted wait-any -> **by-pid** (the pid-precise foreground demux); `render_pipeline_cmd`; `Repl::reap_jobs` (the prompt-cycle WNOHANG reaper, §9.4). `cmd &` / `a | b &` register ONE job, print `[N] PID`, status 0; finished jobs emit `[N]+ Done  cmd` before the next prompt. A bg builtin/fn (needs fork) stays NotImplemented. **PURE USERSPACE** -- zero kernel files; built on the U-7-pre `wait_pid_for` primitive -> NOT a formal-audit-trigger surface (self-reviewed: no-spin reaps, by-pid demux, no orphaned bg children, JobTable borrow-clean). The notes-fd / multi-fd-poll integration (async `[N]+ Done` while idle, on/mask delivery, Ctrl-C) is U-7c. New `/u-job-test` (registration, one-job-per-pipeline, reap-to-`Done`, spec-reset, the by-pid foreground demux, bg-builtin rejection, the integrated `Repl` reaper) + `u-test` probes 4/7 updated (`&` now WORKS + reaps, was the stale U-6 `NotImplemented` assertion). Verified: `make test-tcg` x2 + `u-job-test: all OK` + `joey: /u-job-test reaped status=0; U-7a background jobs verified` + `u-test: all OK` + `Thylacine boot OK`, **0 EXTINCTION** both boots. |
| **U-6-test** | `e4b5100` | The U-6 evaluator arc **cumulative integration smoke** (§8.4) -- **closes the U-6 arc**. New dedicated pre-pivot `/u-6-test` (joey-gated; 7 composed flows weaving subst-capture + `for`/`case`/arith, glob->spawn->capture, var-interp pipelines, builtin->special-var->control flow, var-fed heredoc redirect, and the whole stack through the real `Repl::feed` loop + error recovery). Proves the U-6 surfaces COMPOSE (vs the per-sub-chunk probes' isolation), landing assertions on Env state / `$status` / substitution-captured output. PURE USERSPACE -- zero kernel files (784/784 unchanged); no SMP gate, not a formal-audit-trigger surface (self-reviewed: no-spin loops, bounds-safe, borrow-clean). Verified: kernel **784/784**, `make test-tcg` x2 + `u-6-test: all OK` + `joey: /u-6-test reaped status=0; U-6 evaluator arc integration verified` + login E2E + `Thylacine boot OK`, **0 EXTINCTION** both boots. |
| **U-6f** | `e9e0aa9` | Command substitution `$(cmd)` / `` `{cmd}` `` (§5.8) + its kernel prerequisite **#926** (a single-thread Proc closes its fds at EXIT, not reap — `docs/reference/14-process-model.md`). `stmt::run_command_substitution[_script]` + `capture_external_pipeline` (drain-before-reap, deadlock-free) + `split_fields` + `trim_trailing_newlines`; `$status` -> `Cell<i32>` (interior mutability so the `&Env` evaluator can record the inner exit, §8.7); the `eval_value_token` Subst/Backtick arms + `eval_dq_in_word`/`render_heredoc_body` DqPart::Subst + the `expr.rs` Subst/Backtick arms; `eval_command`/`eval_let`/`eval_assign` status-before-eval. v1.0 scope = a single redirect-free EXTERNAL pipeline; builtin/fn/control-flow/multi-statement bodies + `<(cmd)`/`>(cmd)` procsub are `NotImplemented` (documented). Bare → whitespace field-split (rc `$ifs`); `"$(cmd)"`/heredoc → inline. **#926 is the audit-bearing death-path surface** (ARCH §25.4): SMP-gated (default+UBSan x smp4/smp8 N=10, 0 corruption) + a focused Opus prosecutor round CLEAN (0 P0 / 0 P1 / 0 P2 / 3 P3 doc); `memory/audit_926_u6f_closed_list.md`. New `/u-subst-test` (9 probes: capture+trim, bare split, `seq` newline-split, `"[$(…)]"` inline, `echo|tr` pipeline capture, backtick form, `$status` propagation, builtin-in-subst NotImplemented, procsub NotImplemented, script-mode implicit-fail) + the `u-test::flow_process_pipe` part-(c) #926 regression (read child stdout to EOF before reap) + the rewritten `test_sys_spawn_with_fds_child_rights_subset_of_parent`. Verified: kernel **784/784**, `u-subst-test: all OK`, login E2E + `Thylacine boot OK`, 0 EXTINCTION. |
| **U-6e-b-2** | `aba3b7d` | Glob argv fs-expansion. `glob::expand(env, pattern) -> Vec<String>` (§6.3) -- splits on `/`, accumulates the meta-free literal prefix as the start dir, walks one segment per level via `fs::read_dir` + `matches` (descending only matching dirs for non-final segments; recursion bounded by segment count, not tree depth), applies the POSIX dotfile rule, returns the SORTED match set or the EMPTY vec (rc nullglob -- never the literal). `stmt::evaluate_argv` gains a `glob_candidate(w)` gate: only a bare `Word::Single(TokenKind::Word)` with `has_meta` expands (quoted / `$var` / `^`-concat stay literal); an empty match set drops the word entirely. `**` stays per-segment (== `*`); recursive `**`, trailing-slash dirs-only, and expr-context (for-list / let-RHS) globbing are v1.x. PURE USERSPACE -- zero kernel files; rides the audited U-6e-b-1 `read_dir` surface; not formal-audit-bearing (bounded read-only enumeration; self-audited: bounded recursion, owned strings across recursion, nullglob, sort-once determinism). New pre-pivot `/u-glob-test` (joey-gated; §8.2). Verified: kernel 784/784 (unchanged -- no kernel delta), `make test-tcg` x2 + `u-glob-test: all OK` + 0 EXTINCTION. |
| **U-6e-a** | `fbba6d2` | Built-in dispatch + the must-be-internal builtins (scripture §9.1). New `eval::builtin` module: `try_builtin(env, &argv) -> Option<EvalResult<StatementFlow>>` (Some = handled in-process; None = fall through to external spawn) + `is_builtin(name) -> bool`. `eval_command`'s Simple arm resolves argv[0] in the order **fn -> builtin -> external** (a user `fn cd { ... }` shadows the builtin). Implemented: **`cd`** (validates the target dir via `fs::is_dir`, lexically normalizes `..`/`.` against `$cwd` via `normalize_abs`, sets `$cwd` + `$oldpwd`; `cd` no-arg -> `$home` falling back to `/`; `cd -` toggles + echoes; the root `/` bypasses the fstat probe -- it is the territory root_spoor, a directory by construction, and libthyla_rs `File::open` cannot open a zero-component path anyway, #929), **`pwd`** (prints `$cwd` to the UART -- the v1.0 no-terminal-fd1 convention, like trace), **`exit [code]`** (sets the `Env::pending_exit` one-shot + `$status`; `eval_block` short-circuits the whole statement stack to `Return`, and `invoke_function` does NOT normalize the exit-Return away at the function boundary; no-arg = exit with current `$status`; non-numeric arg -> 2), **`true`/`false`** (`$status` 0/1), **`unset NAME...`**, **`eval ARGS...`** (joins + `eval_source` in the CURRENT scope), **`source FILE` / `.`** (reads FILE + `eval_source` in the current scope -- the assignment + fn registration persist into the caller's Env), **`type`/`whence`** (reports builtin / function / external). A built-in with a redirect or as a pipeline element -> `NotImplemented` (no spawned fd to retarget; honest, not a confusing 127). **Folded-in fix**: the `$status`/`$errstr`/`$cwd` special-var read/write bridge (§4.4) -- a pre-existing U-6b gap that `cd`'s `$cwd` observability surfaced. **Deferred** (own substrate): `set`/`export` (no envp), `alias`/`unalias`, `read`, `jobs`/`fg`/`bg`/`wait`, `history`, `kill`/`note`, `echo`/`printf`/`test`/`palette`/`bind`/`mount` (the namespace + coreutil surfaces). New pre-pivot `/u-builtin-test` (12 probes: true/false, cd to a synthetic dir + the `$cwd` bridge, normalize + root-clamp, `cd -` toggle, missing-dir fail + `$errstr`, unset, eval-in-scope, `$status` bridge, type, source-of-`/builtin-test.rc` [assignment + fn persist], `exit 7` flow + pending, exit-through-a-function unwind); joey gates the boot on status==0. **Surfaced + tracked**: #929 (libthyla_rs `File::open`/`metadata`/`is_dir` cannot open the root `/`). |
| **U-6d-b** | `cc8b88d` | I/O redirection (`<` `>` `>>` heredoc), lone-command + per-pipeline-element. New `resolve_redirects(env, &[Redirect]) -> Result<ResolvedStdio, RedirError>` opens every target UP FRONT (any failure aborts before a spawn), producing per-fd `Stdio` overrides: `< f` -> `File::open` at stdin; `> f` -> `OpenOptions::write+create+truncate`; `>> f` -> `...+append`; `<< TAG` -> render the body + a kernel pipe (rd at stdin, wr fed post-spawn then dropped for EOF). The shell OPENS-and-HANDS-OFF (`Stdio::File`); it does no redirect I/O itself, so Loom (the async 9P data path) is not this layer's concern (the child's; #916 FS-open-via-Loom stays a tracked v1.x seam). Last-wins per fd (a later same-fd redirect drops the earlier `Stdio::File`). `exec_external_redirected` (lone command) + `exec_bare_redirect` (no command -> open/truncate the target, run nothing, status 0); `exec_pipeline` per-element redirects OVERRIDE the pipe wiring for that fd (the displaced pipe end is dropped). `RedirError::Eval` propagates as an eval error (a redirect on a fn -> `NotImplemented("redirect on function")`; on a non-Simple in-process command -> `NotImplemented("redirect on in-process command")` -- defensive, the parser only attaches redirects to Simple); `RedirError::Runtime` (ambiguous multi-word target / open failure) -> `$status`+`$errstr`. **Dependency pulled forward**: `libthyla_rs::fs` create-or-open + append -- `OpenOptions::{create,create_new,append}` were `NotImplemented` stubs, now `File::open_create_at_path` drives `SYS_WALK_CREATE` (CREATE-FIRST, mirroring joey's `mkdir_or_open`: `t_walk_create` is exclusive + the kernel walk_open has no distinguishable not-found code, so "try create; on failure open existing with truncate" -- never depends on a missing-file errno); `File::create` becomes std-shaped; `append` = seek-to-end-at-open (the Spoor offset is shared into the child via the handle bump). **Lexer fix folded in**: `<<`/`<<-` now skip horizontal whitespace before the tag, so `cat << EOF` works as well as `cat <<EOF`. `/u-test::flow_eval_redirect` (7 pre-pivot probes: 3 heredoc forms + fn-redirect-NotImplemented + ambiguous-target + create-failure + bare-redirect) + the new post-pivot `/u-redir-test` (the real `>`/`>>`/`<` round-trip on the writable dev9p root: create+write -> read-back, append -> 2x, truncate -> 1x, `pipe-sink < f` -> 0). u-test now reports **16 cross-module flows** (`u-test: eval redirect OK`). |
| **U-6d-a** | `24eafac` | Multi-element pipeline. Lifts `eval_pipeline`'s `p.elements.len() > 1` NotImplemented branch via new `exec_pipeline(env, p)`. For `cmd1 | cmd2 | ... | cmdN`: validates every element is a redirect-free EXTERNAL Simple command (a fn / BraceBlock / Arith / Subshell as a stage -> NotImplemented, since in-process eval has no fd to wire to a pipe without fork; per-element redirect -> NotImplemented, U-6d-b), evaluates all argvs up front (any rejection aborts before a single spawn), then allocates N-1 pipes via `libthyla_rs::process::pipe()`, assigns each pipe's write end to the upstream element's stdout slot (`Stdio::File`) and read end to the downstream element's stdin slot, gives the OUTER ends (element 0 stdin, element N-1 stdout, all stderr) the U-6c v1.0 convention (`Stdio::Piped` + immediate drop -- the shell has no terminal-backed fd 0/1/2 yet), spawns ALL elements before the first wait (waiting between spawns would deadlock a `producer | consumer` -- producer blocks on a full pipe while the parent blocks before spawning the consumer that drains it), then reaps via wait-any + pid-match (SYS_WAIT_PID reaps any zombie; we collect the N spawned pids and match each reaped pid to its element index; at v1.0 there are no background children so exactly N waits reap the pipeline), and finally aggregates `$status` per pipefail. **Pipefail (scripture 8.4)** = new pure `pub fn aggregate_pipefail(&[(i32, bool)]) -> i32`: the RIGHTMOST non-zero exit among elements NOT marked `?|` (the `?|` modifier sets `tolerate_failure` on the LEFT element, whose status is then ignored), or 0 if every non-tolerated element succeeded. **v1.0 binary exit status**: the kernel's `sys_exits_handler` normalizes any non-zero child exit to `exit_status = 1` (x0==0 -> "ok"/0; x0!=0 -> "fail"/1; the literal code is lost -- a richer u64 status is a Phase 5+ deferral). So a real pipeline element's reaped status is 0 or 1; the rightmost-VALUE distinction in `aggregate_pipefail` is exercised by the pure-function probe (synthetic statuses 7/3), not by real binaries. **Stdio direction**: `Stdio::File(end)` installs a specific pipe end at the child's slot, then libthyla-rs drops the parent's copy after the spawn -- so once the upstream exits, the downstream reader sees EOF (the parent holds no write-end copy). **Two new test fixtures**: `pipe-src` (writes the fixed payload `"PIPE-DATA-OK\n"` to fd 1) + `pipe-sink` (reads fd 0 to EOF, exits 0 iff it got exactly that payload, else 1). cpio 40 -> 42 entries. **Deferred at U-6d-a**: redirects (U-6d-b), background `&` (U-7), in-process pipeline stages (U-6f+). ~150 LOC stmt.rs (exec_pipeline + aggregate_pipefail) + 2 fixtures (~40 + ~55 LOC) + ~170 LOC u-test extension (8 probes). New `/u-test::flow_eval_pipe`: `hello-rs \| hello-rs` -> 0 (2-element mechanics); 3-element -> 0; `pipe-src \| pipe-sink` -> 0 (REAL byte transfer + correct wiring direction); `hello-rs \| pipe-sink` -> 1 (EOF + real non-zero through pipefail, kernel-normalized); aggregate_pipefail rightmost-non-zero (synthetic 7/3 -> 3); aggregate_pipefail `?|` tolerate; background pipeline -> NotImplemented; fn-in-pipeline -> NotImplemented. u-test now reports **15 cross-module flows**; new `u-test: eval pipe OK` line. |
| **U-6c** | `c2661a8` | External command spawn. Lifts `eval_command`'s SimpleCommand `NotImplemented("external command spawn or built-in")` branch -- when argv[0] does not resolve to a defined fn (and built-ins land at U-6e), `exec_external(env, &argv, span)` builds a `libthyla_rs::process::Command`, sets all three Stdio slots to `Piped` (v1.0 convention; see Stdio-inheritance note below), spawns, immediately drops the parent-side pipe ends, waits, and reflects the result in `$status` + `$errstr`. **Status semantics**: clean reap -> `ExitStatus::raw()`; spawn failure -> 127 + `$errstr = "spawn failed: <Error>"` (bash command-not-found convention); wait failure -> 1 + `$errstr = "wait failed: <Error>"`. All three paths return `Ok(StatementFlow::Normal)` -- a runtime non-zero exit is not an eval-time error; the caller's implicit-fail discipline handles propagation. **Trace echo (closes U-6b deferral)**: when `env.trace_depth > 0`, `trace_echo(argv)` emits `+ argv[0] argv[1] ...\n` via `t_putstr` (UART) BEFORE the spawn. Bash's `set -x`-style argv quoting is a v1.x refinement; v1.0 space-separates. **Stdio inheritance (v1.0 convention)**: `Stdio::Inherit` would tell the kernel to install the parent's fd 0/1/2 into the child's handle table, but the shell at v1.0 has no terminal-backed fd 0/1/2 (joey/u-test/ut all write via SYS_PUTS direct to the UART). The kernel's `SYS_SPAWN_FULL_ARGV` then rejects the spawn (the parent's handle table has no entry at those slots). Until U-PTY + U-6g land fd 0/1/2 wiring, exec_external uses the established v1.0 convention (matches `alloc-smoke` + `u-test::flow_process_pipe`): `Stdio::Piped` on all three slots followed by an immediate drop of every parent-side pipe end. The child gets kernel-installed pipes at 0/1/2 (so the kernel accepts the spawn); native binaries (SYS_PUTS-writers) don't touch them; ported binaries get EOF on read / EPIPE on write when they try -- the correct v1.0 behavior for a shell without a terminal. When PTY lands, this whole block flips to `Stdio::Inherit` cleanly. **Subshell + background**: still NotImplemented (Subshell needs a fork primitive libthyla-rs doesn't expose; `&` needs job control at U-7). **Built-ins**: deferred to U-6e (when they land, they're tried ahead of `exec_external`). **Argv glob fs expansion**: deferred to U-6e -- libthyla-rs has no `ReadDir` surface yet (per `fs/mod.rs` "deferred -- kernel directory-read mechanism not yet exposed"); U-6e lands both. The glob pattern matcher in this crate is unchanged (case + matches operators continue to work; only fs-walk is the new add). ~95 LOC stmt.rs additions + ~170 LOC u-test extension (7 probes). New `/u-test::flow_eval_exec` validates at boot: hello-rs spawn-and-reap, spawn-failure status=127 path, trace { hello-rs } echo, background `&` rejection, subshell `( ... )` rejection, fn shadowing of external lookup, spawn-failure errstr prefix. u-test now reports **14 cross-module flows**; new `u-test: eval exec OK` line. |
| **U-6b** | `0dc28d9` / `9063322` | Statement evaluation: walks `Vec<Statement>` via `eval_statement(env, stmt) -> EvalResult<StatementFlow>` and `eval_block(env, stmts)`. `StatementFlow` is `Normal | Return | Break | Continue`. New `eval_source(env, src)` parse+eval convenience. Implements every StatementKind except external `SimpleCommand` (U-6c): Let/Assign/FnDecl/Return/Break/Continue/If/While/For/Case (statement form)/Try/Trace/OnNote/MaskNote, plus Pipeline whose single element resolves to `CommandKind::Simple` (argv[0] in `Env::fns` → function invocation) OR `CommandKind::BraceBlock` (executes in current scope per scripture 5.6) OR `CommandKind::Arith` (status=0 if non-zero result else 1). Multi-element pipeline / external SimpleCommand / Subshell / background `&` / redirects → NotImplemented at U-6b. **Function-call discipline**: push_scope; bind $0 = name, $1..$N positional, $* full-args-list, FnDecl::args by name; eval_block; pop_scope. A `Return` inside the body normalizes to `Normal` at the call site; unconsumed Break/Continue across the function boundary likewise. **Implicit-fail**: `eval_block` checks after each Normal statement -- if `$status != 0` AND (Pipeline first-element has fail_propagate set OR `!env.interactive && implicit_fail_suppressed == 0`), returns Return. `Env::interactive` defaults to false (script mode); the U-6g main loop will set it true. **Try/Catch**: enter increments `implicit_fail_suppressed`; body executes with implicit-fail off; on body err OR post-body `$status != 0`, set $errstr from the error kind, $status=1, run catch. **Trace**: increments `trace_depth` around body; actual echo to stderr deferred to U-6c (when external commands land alongside a coherent argv-to-stderr surface). **OnNote**: registers handler in `Env::note_handlers` (BTreeMap by name); runtime delivery wires at U-7. **MaskNote**: pass-through at U-6b (executes body; masking-state recording wires at U-7). **Case statement form**: same glob matcher as case-as-expression; no-match returns Normal + status=0 (NOT error -- distinct from case-as-expression which raises NoCaseMatch). Env extensions: `interactive: bool`, `implicit_fail_suppressed: u32`, `trace_depth: u32`, `note_handlers: BTreeMap<String, Vec<Statement>>` + accessors. New `eval_word`/`eval_value_token` mirrors `eval_expr`'s atom dispatch on Token-shaped Word nodes (since `Word` holds raw Tokens, not Exprs). New `eval_dq_in_word` duplicates `eval_dq` for Token-anchored DQ interpolation. ~620 LOC stmt.rs + ~290 LOC u-test extension. 14-probe `/u-test::flow_eval_stmt` validates at boot (Let / Assign / fn-call / If/elif/else / While+Break / For / Case match + no-match / Try/Catch on $status / Try/Catch on eval err / Trace / OnNote registration / BraceBlock / script-mode implicit-fail). **Known v1.x deferral**: a list literal `(a b c)` inline inside a `for` header (`for (x in (a b c)) { ... }`) currently triggers a parse-time `UnexpectedEof { expected: "`)`" }`; the `for (x in $xs) { ... }` idiom with a pre-bound list var works. Investigation deferred -- low-priority since the working idiom is the common case and the list-as-var-binding pattern is what users will reach for. |
| **U-6a** | `3be8cba` / `34f564a` | Evaluator scaffold + Value (unified Vec<String>) + Env (BTreeMap scope stack + fns + $status/$errstr/$cwd) + EvalError/EvalErrorKind/EvalResult + glob matcher (no fs) + eval_expr for the no-side-effect subset (atoms / Var* / List / Concat / arith / cond / Match::Glob / Match::In / case-as-expression). Subst/Backtick/ProcSub/Match::Regex → NotImplemented. 15-probe `/u-test::flow_eval_expr` validates at boot. |

---

## 12. Naming rationale

The module is `eval`, not a thematic name. Per CLAUDE.md's thematic-naming discipline ("If the rename makes the code less obvious to a reader who doesn't know the project's identity, the standard name wins"), `eval` is the unambiguous Unix term and stays. The thematic moniker for the AST walker step in the U-6 main loop ("the shell stalks the AST") was considered (`stalk`, `hunt`) but rejected because the standard term is what readers expect.

---

## 13. References

- `docs/UTOPIA-SHELL-DESIGN.md` §4 (data model), §6 (variables / interpolation / substitution), §7 (pattern matching / control flow), §8 (error handling).
- `docs/reference/93-utopia-parser.md` — the AST taxonomy U-6 consumes.
- `usr/utopia/libutopia/src/eval/*.rs` — implementation.
- `usr/u-test/src/main.rs::flow_eval_expr` — boot probes.
