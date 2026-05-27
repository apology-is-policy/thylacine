# 94 — `libutopia::eval`

**Status:** U-6a LANDED. The U-6 arc opens with the evaluator scaffold + value model + expression evaluation for the no-side-effect subset. Statement evaluation, external command spawn, pipes/redirection, substitution, built-ins, and the poll() main loop land at U-6b through U-6h.

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
- `$cwd`: `cwd()` / `cwd_set(p)`. Initialized to `/`; the `cd` builtin (U-6e) updates atomically with the syscall.

---

## 5. `eval_expr` — the recursive walker

```rust
pub fn eval_expr(env: &Env, expr: &Expr) -> EvalResult<Value>
```

Pure with respect to the AST. Side effects limited to errors raised through `EvalResult`. No mutation of `env` at U-6a (mutation belongs to statement eval at U-6b).

### 5.1 Coverage

| ExprKind | At U-6a | Notes |
|---|---|---|
| `Word(s)` | implemented | scalar; argv-time glob expansion deferred to U-6e |
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

---

## 6. `glob` — the rc/POSIX-shape pattern matcher

Pure pattern matching against a single input string. NO filesystem walks. Used by case-as-expression and the `matches` operator at U-6a; will be reused by argv-time expansion at U-6e.

### 6.1 Pattern syntax (per scripture §6.10)

- `*` — zero or more characters, NOT crossing `/`.
- `?` — exactly one character, NOT crossing `/`.
- `[abc]` — character class.
- `[!abc]` — negated character class.
- `[a-z]` — range within a class.
- `**` — recursive (crosses `/`); only valid as a complete path segment. Not special-cased at U-6a (case-arm and `matches` rarely match paths); the U-6e filesystem expansion will lift it.

### 6.2 API

```rust
pub fn matches(pattern: &str, input: &str) -> bool
pub fn has_meta(s: &str) -> bool
pub fn match_any(pattern: &str, candidates: &[String]) -> Vec<usize>  // unused at U-6a
```

Bytewise matching; UTF-8 is self-synchronizing so partial-byte matches cannot occur on valid UTF-8 input.

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

### 8.3 Helper discipline

The boot probe's `ev(env, s, ctx)` helper tokenizes the source fragment, **strips the trailing `Eof` token** (the same discipline parse.rs uses at each placeholder site — `parse_expr_tokens` reports `TrailingTokensInExpr` if Eof is present), parses to an `Expr`, and evaluates. The `cfg(test)` helper in `eval/expr.rs::tests::eval_str` follows the same pattern.

---

## 9. Open questions deferred to later sub-chunks

| Question | Owning sub-chunk |
|---|---|
| `$(cmd)` substitution: how is the subshell forked, stdout captured? | U-6f |
| Pipeline pipefail (multi-element `cmd1 | cmd2`) + redirects | U-6d |
| Job control: `&` background, `jobs` / `fg` / `bg`, note-fd polling | U-7 |
| Glob expansion at argv-time: file-tree walk, `**` semantics, no-match → empty | U-6e |
| Subshell `( cmds )`: fork + isolated env (libthyla-rs has spawn-then-exec, not fork) | U-6f or later |
| `=~` regex: which regex engine? bytewise vs codepoint? | v1.x |
| Cross-dataset patterns (Stratum-aware globs?) | post-v1 |
| Interactive vs script implicit-fail mode (scripture §8.9) | U-6b |
| Stdio inheritance: replace v1.0 Piped-then-drop with `Stdio::Inherit` when fd 0/1/2 wire | U-PTY + U-6g |

---

## 10. Status

| Sub-chunk | Commit | What |
|---|---|---|
| **U-6c** | `c2661a8` | External command spawn. Lifts `eval_command`'s SimpleCommand `NotImplemented("external command spawn or built-in")` branch -- when argv[0] does not resolve to a defined fn (and built-ins land at U-6e), `exec_external(env, &argv, span)` builds a `libthyla_rs::process::Command`, sets all three Stdio slots to `Piped` (v1.0 convention; see Stdio-inheritance note below), spawns, immediately drops the parent-side pipe ends, waits, and reflects the result in `$status` + `$errstr`. **Status semantics**: clean reap -> `ExitStatus::raw()`; spawn failure -> 127 + `$errstr = "spawn failed: <Error>"` (bash command-not-found convention); wait failure -> 1 + `$errstr = "wait failed: <Error>"`. All three paths return `Ok(StatementFlow::Normal)` -- a runtime non-zero exit is not an eval-time error; the caller's implicit-fail discipline handles propagation. **Trace echo (closes U-6b deferral)**: when `env.trace_depth > 0`, `trace_echo(argv)` emits `+ argv[0] argv[1] ...\n` via `t_putstr` (UART) BEFORE the spawn. Bash's `set -x`-style argv quoting is a v1.x refinement; v1.0 space-separates. **Stdio inheritance (v1.0 convention)**: `Stdio::Inherit` would tell the kernel to install the parent's fd 0/1/2 into the child's handle table, but the shell at v1.0 has no terminal-backed fd 0/1/2 (joey/u-test/ut all write via SYS_PUTS direct to the UART). The kernel's `SYS_SPAWN_FULL_ARGV` then rejects the spawn (the parent's handle table has no entry at those slots). Until U-PTY + U-6g land fd 0/1/2 wiring, exec_external uses the established v1.0 convention (matches `alloc-smoke` + `u-test::flow_process_pipe`): `Stdio::Piped` on all three slots followed by an immediate drop of every parent-side pipe end. The child gets kernel-installed pipes at 0/1/2 (so the kernel accepts the spawn); native binaries (SYS_PUTS-writers) don't touch them; ported binaries get EOF on read / EPIPE on write when they try -- the correct v1.0 behavior for a shell without a terminal. When PTY lands, this whole block flips to `Stdio::Inherit` cleanly. **Subshell + background**: still NotImplemented (Subshell needs a fork primitive libthyla-rs doesn't expose; `&` needs job control at U-7). **Built-ins**: deferred to U-6e (when they land, they're tried ahead of `exec_external`). **Argv glob fs expansion**: deferred to U-6e -- libthyla-rs has no `ReadDir` surface yet (per `fs/mod.rs` "deferred -- kernel directory-read mechanism not yet exposed"); U-6e lands both. The glob pattern matcher in this crate is unchanged (case + matches operators continue to work; only fs-walk is the new add). ~95 LOC stmt.rs additions + ~170 LOC u-test extension (7 probes). New `/u-test::flow_eval_exec` validates at boot: hello-rs spawn-and-reap, spawn-failure status=127 path, trace { hello-rs } echo, background `&` rejection, subshell `( ... )` rejection, fn shadowing of external lookup, spawn-failure errstr prefix. u-test now reports **14 cross-module flows**; new `u-test: eval exec OK` line. |
| **U-6b** | `0dc28d9` / `9063322` | Statement evaluation: walks `Vec<Statement>` via `eval_statement(env, stmt) -> EvalResult<StatementFlow>` and `eval_block(env, stmts)`. `StatementFlow` is `Normal | Return | Break | Continue`. New `eval_source(env, src)` parse+eval convenience. Implements every StatementKind except external `SimpleCommand` (U-6c): Let/Assign/FnDecl/Return/Break/Continue/If/While/For/Case (statement form)/Try/Trace/OnNote/MaskNote, plus Pipeline whose single element resolves to `CommandKind::Simple` (argv[0] in `Env::fns` → function invocation) OR `CommandKind::BraceBlock` (executes in current scope per scripture 5.6) OR `CommandKind::Arith` (status=0 if non-zero result else 1). Multi-element pipeline / external SimpleCommand / Subshell / background `&` / redirects → NotImplemented at U-6b. **Function-call discipline**: push_scope; bind $0 = name, $1..$N positional, $* full-args-list, FnDecl::args by name; eval_block; pop_scope. A `Return` inside the body normalizes to `Normal` at the call site; unconsumed Break/Continue across the function boundary likewise. **Implicit-fail**: `eval_block` checks after each Normal statement -- if `$status != 0` AND (Pipeline first-element has fail_propagate set OR `!env.interactive && implicit_fail_suppressed == 0`), returns Return. `Env::interactive` defaults to false (script mode); the U-6g main loop will set it true. **Try/Catch**: enter increments `implicit_fail_suppressed`; body executes with implicit-fail off; on body err OR post-body `$status != 0`, set $errstr from the error kind, $status=1, run catch. **Trace**: increments `trace_depth` around body; actual echo to stderr deferred to U-6c (when external commands land alongside a coherent argv-to-stderr surface). **OnNote**: registers handler in `Env::note_handlers` (BTreeMap by name); runtime delivery wires at U-7. **MaskNote**: pass-through at U-6b (executes body; masking-state recording wires at U-7). **Case statement form**: same glob matcher as case-as-expression; no-match returns Normal + status=0 (NOT error -- distinct from case-as-expression which raises NoCaseMatch). Env extensions: `interactive: bool`, `implicit_fail_suppressed: u32`, `trace_depth: u32`, `note_handlers: BTreeMap<String, Vec<Statement>>` + accessors. New `eval_word`/`eval_value_token` mirrors `eval_expr`'s atom dispatch on Token-shaped Word nodes (since `Word` holds raw Tokens, not Exprs). New `eval_dq_in_word` duplicates `eval_dq` for Token-anchored DQ interpolation. ~620 LOC stmt.rs + ~290 LOC u-test extension. 14-probe `/u-test::flow_eval_stmt` validates at boot (Let / Assign / fn-call / If/elif/else / While+Break / For / Case match + no-match / Try/Catch on $status / Try/Catch on eval err / Trace / OnNote registration / BraceBlock / script-mode implicit-fail). **Known v1.x deferral**: a list literal `(a b c)` inline inside a `for` header (`for (x in (a b c)) { ... }`) currently triggers a parse-time `UnexpectedEof { expected: "`)`" }`; the `for (x in $xs) { ... }` idiom with a pre-bound list var works. Investigation deferred -- low-priority since the working idiom is the common case and the list-as-var-binding pattern is what users will reach for. |
| **U-6a** | `3be8cba` / `34f564a` | Evaluator scaffold + Value (unified Vec<String>) + Env (BTreeMap scope stack + fns + $status/$errstr/$cwd) + EvalError/EvalErrorKind/EvalResult + glob matcher (no fs) + eval_expr for the no-side-effect subset (atoms / Var* / List / Concat / arith / cond / Match::Glob / Match::In / case-as-expression). Subst/Backtick/ProcSub/Match::Regex → NotImplemented. 15-probe `/u-test::flow_eval_expr` validates at boot. |

---

## 11. Naming rationale

The module is `eval`, not a thematic name. Per CLAUDE.md's thematic-naming discipline ("If the rename makes the code less obvious to a reader who doesn't know the project's identity, the standard name wins"), `eval` is the unambiguous Unix term and stays. The thematic moniker for the AST walker step in the U-6 main loop ("the shell stalks the AST") was considered (`stalk`, `hunt`) but rejected because the standard term is what readers expect.

---

## 12. References

- `docs/UTOPIA-SHELL-DESIGN.md` §4 (data model), §6 (variables / interpolation / substitution), §7 (pattern matching / control flow), §8 (error handling).
- `docs/reference/93-utopia-parser.md` — the AST taxonomy U-6 consumes.
- `usr/utopia/libutopia/src/eval/*.rs` — implementation.
- `usr/u-test/src/main.rs::flow_eval_expr` — boot probes.
