# UTOPIA-SHELL-DESIGN

The full design scripture for Thylacine's shell — `ut` — and the surrounding userspace surface that makes the Utopia milestone (`VISION.md §13`) feel real, not broken.

**STATUS**: COMMITTED — scripture commit, U-1 chunk.

This document is the binding design for the shell, its line editor, its coreutils companions, its runtime substrate, and its integration with Thylacine's existing kernel surfaces (notes, poll, Spoor, Territory, capabilities). It is consumed by every U-* implementation chunk under Phase 7 (the project's Phase 7 — execution-phase numbering per `ROADMAP.md §2.1`; corresponds to `ROADMAP.md §8` and `ARCHITECTURE.md §23`).

---

## 1. Scope

In scope:

- The `ut` shell binary's full language semantics: data model, syntax, error handling, expansion, pattern matching, control flow, built-ins, job control, line editing.
- The `libutopia` Rust library (shared by `ut` + coreutils): palette helpers, ANSI emission, terminal raw-mode, line editor, prompt-emit, common error formatting.
- The `usr/utopia/` Cargo workspace layout.
- The native libthyla-rs runtime substrate the shell depends on, and what extensions libthyla-rs needs to grow to host it.
- The Helix port as the default `$EDITOR`.
- The Plan 9-style split between native Thylacine programs and Pouch-ported programs, made explicit so future contributors know which path to take.

Out of scope:

- Halcyon (Phase 8). The shell is the Utopia surface; Halcyon will eventually render the same ANSI bytes natively on Thylacine's own framebuffer.
- The network stack and Linux binary compat (Phase 8, ROADMAP §9). The shell does not depend on either; the Utopia milestone deliberately exists before them.
- Containers (`thylacine-run`); they are a Phase 8 deliverable.
- POSIX `epoll` / `inotify` / `io_uring`. Deferred per `ARCHITECTURE.md §23.8`.

---

## 2. Mission

The shell is the daily interface to Thylacine. Its job:

1. Make a developer connected via UART or SSH feel productive and at home.
2. Express Thylacine's identity — Plan 9 lineage, namespace-first thinking, capability awareness, the visual identity declared in `UTOPIA-VISUAL.md`.
3. Compose with the Unix tradition: pipes work, redirects work, glob works, classical tools work.
4. Refine the historically-painful parts of POSIX shells (`set -e`, quoting hell, signal-handler race conditions, IFS surprises, PS1 escape arithmetic) without breaking familiarity.
5. Be implementable, auditable, and small enough that one engineer can read the whole codebase in a week.

Non-goals:

- POSIX shell compliance. The shell is not `sh`. Scripts written for `sh` do not run unchanged; that is by design.
- A radical departure from shell semantics. Users who know `sh` or `rc` should be able to read `ut` code on first look and understand most of it; the refinements are surgical, not revolutionary.
- Embedding a programming language. The shell is for orchestration; logic-heavy code belongs in Rust, Python, or whatever the user prefers.

---

## 3. The Plan 9 split — native vs ported

The single most load-bearing scripture decision under U-1: every Thylacine userspace program is in one of two camps, and the boundary determines which runtime substrate it builds against.

### 3.1 Native code (authored within Thylacine)

Built against **`libthyla-rs`** (the existing no_std Rust crate at `usr/lib/libthyla-rs/`). Uses Thylacine syscalls directly. Speaks Thylacine concepts natively (Spoor, KObj_*, notes, capabilities, Territories). No musl, no POSIX shim, no Pouch boundary-line patches.

Programs in this camp:
- `ut` — the shell.
- `libutopia` — the shared Rust library for palette, ANSI, line editor, common helpers.
- `cat`, `ls`, `echo`, `grep`, `sed`, `awk`, and the other coreutils for Utopia v1.
- corvus — the key agent (existing precedent; Phase 5).
- The virtio-* userspace drivers (existing precedent; Phase 3).
- mmio-probe, irq-probe, hello-rs (existing precedents; Phase 4).
- Future Thylacine-shaped daemons and tools authored within the project.

### 3.2 Ported code (foreign code adapted to Thylacine)

Built against **Pouch** (the cross-compilation environment delivered by Phase 6). Uses musl + the `usr/lib/pouch/patches/*` boundary-line patches that bridge musl syscalls to Thylacine syscalls and adapt POSIX semantics (notes-as-signals, SrvConn-as-AF_UNIX, etc.).

Programs in this camp:
- stratumd — the filesystem daemon (existing precedent; Phase 6).
- libsodium — the cryptographic library (existing precedent; Phase 6).
- The pouch-hello-* probing binaries (existing precedents).
- **Helix** — the default `$EDITOR` (Utopia chunk U-Helix).
- Future ports of foreign programs (ssh, git, python, etc.).

### 3.3 The decision rule

When adding a new Thylacine program, ask:

> Is this program authored within the Thylacine project, OR is it a port of an existing codebase that already expects POSIX?

- **Authored within Thylacine** → native libthyla-rs. No Pouch.
- **Ported from elsewhere** → Pouch. The boundary-line patches grow to cover whatever new POSIX surface the port touches.

This rule is enforced by code review and by the structure of `tools/build.sh` (which has separate build paths for the two camps).

### 3.4 Rationale

Plan 9 made this split explicit. Native Plan 9 programs used `libc.h` — a thin syscall layer + a few utilities. Ported POSIX programs used APE (the ANSI POSIX Environment) — a heavier translation layer. The two coexisted; neither pretended to be the other; both worked.

Thylacine inherits the pattern. Native programs get clean Thylacine semantics — direct access to notes-as-fds, Spoor-as-handle, capability-as-syscall-gate, Territory-as-namespace. Ported programs get POSIX-shape via the pouch boundary-line, which is the right place to do the translation work once per surface rather than at every program's syscall site.

Programs we author benefit from being Thylacine-shaped: smaller binaries, faster startup, no impedance mismatch, fewer patches to maintain. The shell in particular benefits because the shell talks to the kernel constantly — every prompt redraw, every command spawn, every fd poll.

### 3.5 What this means for `ut`

The shell links libthyla-rs and grows whatever extensions libthyla-rs needs to host it. It does not depend on musl, does not require pouch patches, does not have a `_start` outside libthyla-rs's own. Its panic handler is libthyla-rs's. Its allocator is one libthyla-rs provides (per §15 below).

Helix is the only Pouch consumer in Utopia. The shell launches Helix as an external program (via standard `exec` path); the boundary is fd-level, not library-level.

---

## 4. Data model — plain text streams

Pipes carry bytes. Programs interpret. Composable with every classical Unix tool out of the box. Structured output is achieved by programs emitting JSON / TSV / msgpack and parsing downstream.

This is the Unix tradition and is preserved without reservation. Typed-stream shells (nushell, PowerShell) buy structured pipelines at the cost of every command emitting structured records; Thylacine declines the trade because classical Unix tools are not going away, and Thylacine programs that want structured output emit JSON, which any modern tool can consume.

### 4.1 Implications

- Variables are strings. Lists of strings are first-class (per §6 below) but list-elements are strings.
- A pipeline element's stdout is bytes. The reader interprets the bytes' encoding (UTF-8 by default, configurable per program).
- No type annotations. No type checking. The shell trusts the programs.
- Programs that want structured output emit it (typically JSON); the shell offers no built-in structured-data primitives.

### 4.2 Encoding

UTF-8 throughout. Every Utopia program emits UTF-8. Filesystem paths are bytes (as is Plan 9 / Unix tradition) but conventionally UTF-8.

---

## 5. Familiarity anchor — rc-shaped, with refinements

The shell reads like `rc` (Plan 9's shell) to an rc user, like a-cleaner-`sh` to an sh user. Refinements target specific pain points (error handling, prompt format, interpolation, pattern matching) without breaking the rc shape.

### 5.1 Reserved words

```
fn  let  if  else  case  for  while  in  try  catch
return  break  continue  on  mask  trace
```

### 5.2 Comments

`#` to end-of-line. No block comments.

### 5.3 Line continuation

Trailing `\` joins the next physical line. Auto-continuation when a bracket pair (`{` / `(` / `[`) or a quote is unclosed.

### 5.4 Statement separator

Newline OR semicolon. `cmd1; cmd2` runs `cmd1` then `cmd2` on the same logical line.

### 5.5 Function declaration

```
fn name {
    body
}
# or
fn name args {
    body
}
```

`name` is callable thereafter. `$1`, `$2`, ... refer to positional args; `$*` is the list. `args` declaration is sugar that names positional args for the body's scope.

### 5.6 Subshell and brace block

- `( cmds )` — forks (`rfork`); runs `cmds` in a child shell; the parent waits.
- `{ cmds }` — brace block; runs `cmds` in the current shell.

The two have different scoping: subshells have a copy of variable bindings; brace blocks share.

---

## 6. Variables, interpolation, and substitution

### 6.1 Assignment

```
x = value             # bare assignment
let x = value         # explicit declaration; equivalent shape, slightly more visible
files = (a b c)       # list assignment
let files = (a b c)   # same
```

`let` is optional. Code-style guidance: use `let` when defining a variable for the first time inside a function; use bare assignment for re-assignment or in script scope.

### 6.2 Scoping

Dynamic by default: a variable assigned in a function is visible in callees. `let` inside a function declares a local that shadows any outer binding for the function's scope. Variables assigned at script-top-level are global.

### 6.3 Lists are first-class

```
files = (a b c)
echo $#files       # length: 3
echo $files(1)     # first element: a
echo $files(2 3)   # slice: b c
echo $files        # all elements separated by space: a b c
```

No `IFS`. No word-splitting surprises. A list is a list; a scalar is a scalar (a one-element list).

### 6.4 Single-quoted strings — literal

`'text'` is literal text. No expansion. No escapes. The only way to include a single quote inside a single-quoted string: double the quote. `'it''s a quote'` = `it's a quote`.

### 6.5 Double-quoted strings — interpolating

`"text"` interpolates `$var` and `$(cmd)`. Embedded escapes: `\n`, `\t`, `\\`, `\"`, `\$`.

```
let user = (whoami)
let greeting = "Hello, $user from $(pwd)"
echo $greeting
```

### 6.6 Substitution

Two forms, both supported:

- `$(cmd)` — POSIX-familiar shape. Recommended for new code.
- `` `{cmd} `` — rc-traditional shape. Supported for rc-script compatibility.

Both run `cmd` in a subshell, capture stdout, and substitute the result (trimmed of trailing newlines) at the substitution site.

### 6.7 Concatenation operator `^`

Adjacent words concatenate when joined by `^`:

```
let prefix = 'pre-'
let suffix = '-post'
echo $prefix^$var^$suffix   # pre-VALUE-post (one word)
```

Without `^`, `$prefix $var $suffix` is three separate words.

### 6.8 No-split prefix `$"var`

```
let args = (a 'b c' d)
echo $args        # three words: a, "b c", d
echo $"args       # one word: "a b c d" — the list joined with spaces
```

This is rc's clean answer to bash's `"$@"`-vs-`$@` mess.

### 6.9 Built-in length and slicing

```
echo $#var       # length (1 for scalar, N for list)
echo $var(N)     # element N (1-indexed)
echo $var(M N)   # slice from M to N inclusive
```

### 6.10 Globbing

- `*` — zero or more characters (no path-separator crossing).
- `?` — one character.
- `[abc]` — character class; `[!abc]` is negation.
- `**` — recursive (path-separator-crossing); only valid as a complete path segment (`**/*.rs` = all `.rs` files under cwd).
- A glob that matches nothing expands to the empty list (NOT to itself; this is rc's behaviour, not bash's default).

### 6.11 Heredocs

```
cat <<EOF
multi-line
literal text
EOF

cat <<-EOF       # leading tabs stripped (indented heredoc)
    indented
    text
EOF

cat <<"EOF"      # no expansion (quoted heredoc)
literal $var stays as bytes
EOF
```

`<<` performs interpolation by default; `<<"` (quoted) suppresses it; `<<-` strips leading tabs to allow indentation.

### 6.12 Process substitution

```
diff <(cmd1) <(cmd2)         # treats cmd's stdout as a filename
tee >(filter) >output.txt    # treats filter's stdin as a filename
```

Implemented via anonymous pipes + `/proc/self/fd/N` (Plan 9 has `/fd/N`; Thylacine inherits the form). The shell creates the pipe, spawns the subcommand attached to one end, and substitutes the path to the other end at the call site.

### 6.13 Arithmetic

```
let n = (( 1 + 2 * 3 ))           # arithmetic context; n = 7
let m = (( $x + $y ))             # variables auto-dereferenced inside
let p = (( $#files - 1 ))         # length expressions
```

Integers only at v1; floats are deferred. Operators: `+ - * / % ** & | ^ ~ << >> < <= > >= == != && ||`.

### 6.14 String comparison

There is no `[[ $a == $b ]]`. Use pattern matching (§7) or the `match` operator:

```
if ($a matches $b) { echo equal }
if ($a in $list) { echo found }
```

For plain equality:

```
if ($a == $b) { ... }      # textual equality; both sides treated as strings
```

The `==` operator is shell-syntactic, not a builtin. It compares left and right as strings.

---

## 7. Pattern matching and control flow

### 7.1 The `case` block

```
case $file {
    *.c *.h       => echo 'C source'
    *.rs          => echo 'Rust source'
    *.md          => echo 'Markdown'
    *             => echo "unknown: $file"
}
```

Multi-pattern per branch (space-separated). No fallthrough; first match wins. `*` is the catch-all. No `;;`. No `esac`.

### 7.2 Match as expression

```
let kind = case $file {
    *.c  => 'C source'
    *.rs => 'Rust source'
    *    => 'unknown'
}
```

When `case` appears in an expression context (right of `=`), each branch must produce a value; the chosen branch's value becomes the case-expression's value.

### 7.3 Inline match operators

```
if ($var matches *.c) echo C
if ($var in (yes y true on)) echo enabled
if ($var =~ /^foo/) echo starts-with-foo
```

- `matches` — glob match.
- `in` — set membership against a list.
- `=~` — regex match. Delimited by `/`.

### 7.4 The legacy `~` operator

rc's `~ $var pattern` is supported for rc-compat:

```
if (~ $var foo bar) echo found
```

It is not idiomatic in new code. Use `matches` or `in` instead.

### 7.5 If / else

```
if ($x == 0) {
    echo zero
} else if ($x < 10) {
    echo small
} else {
    echo big
}
```

### 7.6 Loops

```
for (f in $files) {
    cat $f
}

while ($cond) {
    body
}
```

`break` and `continue` work as expected.

### 7.7 Try / catch

```
try {
    risky_operation
} catch {
    echo "operation failed: $errstr"
}
```

`catch` runs if any command in the `try` block exits non-zero. The `$errstr` shell variable is set to the last error message produced by the failing command (rc-style; see §8 below).

### 7.8 Trace block

```
trace {
    cmd1
    cmd2
} 
```

Equivalent of `set -x` scoped to a block: each command is printed to stderr before it runs, in a recognizable trace format. Exits the trace mode when the block ends.

---

## 8. Error handling

### 8.1 The model

**Implicit fail.** Every command in a script auto-fails-the-enclosing-function on non-zero exit. The `?` suffix marks explicit propagation. `try { ... } catch { ... }` wraps a block to suppress. Pipelines pipefail-by-default. `$status` and `$errstr` remain queryable.

This is `set -e`'s intent, done correctly: well-defined semantics in functions, pipelines, command substitutions, subshells.

### 8.2 The `?` operator

```
build?            # if build fails, the enclosing function returns build's exit code
```

`?` is a postfix operator on a command (or pipeline). Reads as "fail-propagate." Visible at the call site, so a reader can see "this command's failure stops the function."

### 8.3 Function exit on error

By default, when a command in a function exits non-zero, the function returns immediately with that exit code. To suppress, wrap in `try { ... } catch { ... }`.

### 8.4 Pipefail

```
cmd1 | cmd2 | cmd3
```

The pipeline's exit code is the rightmost non-zero exit of any element, or 0 if all succeeded. To opt out for a specific element (allow it to fail without affecting the pipeline):

```
cmd1 ?| cmd2 | cmd3      # tolerate cmd1's failure
```

The `?|` modifier on the pipe means "the preceding command's exit is ignored for pipefail purposes."

### 8.5 `$status` and `$errstr`

After each command, `$status` is the exit code and `$errstr` is the textual error message (the program's last stderr line, or a structured error per `ERRORS.md`).

`$errstr` is rc's `errstr` variable, lifted into ut with the same semantics.

```
some_cmd?    # auto-fail
some_cmd     # succeeds or sets $status / $errstr
echo $status $errstr
```

### 8.6 Explicit suppression

```
some_cmd; if ($status != 0) { ... }   # explicit handling
try { some_cmd } catch { ... }         # block-level
some_cmd || echo 'failed'              # || tolerates non-zero exit
```

### 8.7 Implicit-fail in command substitution

Inside `$(cmd)` or `` `{cmd} ``, a non-zero exit DOES propagate up. To tolerate:

```
let output = $(cmd?)       # propagates if cmd fails
let output = $(try { cmd } catch { echo default })
```

### 8.8 Implicit-fail in subshell

`( cmds )` — if the subshell exits non-zero, the parent's `$status` is set; whether the parent auto-fails depends on whether the parent itself is in a function with implicit-fail (yes by default in functions, no in script-top-level interactive use).

### 8.9 Interactive-shell behaviour

The implicit-fail model applies to scripts and functions. At the interactive prompt (typing commands one at a time), non-zero exits do NOT terminate the session; `$status` is set, and the next prompt is drawn. This is what users expect from interactive use; the fail-fast model is for scripts.

---

## 9. Built-ins

### 9.1 The standard set (modern middle)

Must be internal (no other choice — they modify the shell's own state):

- `cd` — change directory. `cd` with no args goes to `$home`. `cd -` toggles to previous.
- `pwd` — print working directory.
- `exit` — exit the shell with optional code.
- `return` — return from a function.
- `set`, `unset`, `export` — variable management.
- `source` (also `.`) — execute a script in the current shell's scope.
- `eval` — evaluate a string as commands.
- `alias`, `unalias` — manage aliases.
- `fn` — function declaration (keyword, processed by parser, not builtin per se; listed for completeness).
- `wait` — wait for child processes.
- `jobs`, `fg`, `bg` — job control (§10).
- `read` — read a line into a variable.
- `type`, `whence` — query: is this command a builtin, alias, function, or external?
- `history` — history access.
- `echo` — write args to stdout. Internal for speed.
- `printf` — formatted output. Internal.
- `test`, `[` — legacy POSIX-style test. Largely supplanted by `case`/`matches`. Kept for rc-script compatibility.
- `true`, `false` — exit 0 / exit 1.
- `time` — time a command or pipeline. Internal for pipeline-aware timing.
- `kill` — send a note (§10). Default note: `snare:term`.

### 9.2 The Thylacine extensions

Cannot sensibly be external — they modify the calling process's namespace, capabilities, or note delivery state:

- `bind` — `bind [-a|-b|-r] OLD NEW` — Plan 9 namespace bind. Calls the `rfork` + bind syscall path.
- `mount` — `mount [-c] SERVER NEW` — Plan 9 namespace mount. Mounts a 9P server at a namespace point.
- `unmount` — `unmount [OLD] NEW` — undo a mount.
- `pivot_root` — `pivot_root NEW` — atomic root swap. Rare; used by containers and bootstrap.
- `rfork` — `rfork [FLAGS]` — explicit fork with namespace-control flags.
- `cap` — `cap [ls|get|drop]` — inspect or drop capabilities held by the current process.
- `note` — `note [send|list|wait] [PID] [NAME] [BODY]` — send a note, list pending notes, wait for one.

These are the user-facing surface of Thylacine's per-process namespace + capability + notes model. Without them as builtins, users would have to write Rust or C to compose namespaces; with them, namespace composition is a one-line shell builtin.

### 9.3 The trace builtin

- `trace { ... }` — see §7.8.

### 9.4 The palette builtin

- `palette ROLE TEXT...` — emit `TEXT` in the named role's colour per `UTOPIA-VISUAL.md §4`.

### 9.5 The `on` and `mask` keywords

Not exactly builtins — they're language-level constructs for note handler registration. See §10 for semantics.

### 9.6 External programs (NOT built-in)

Everything else. Including:

- All coreutils: `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `grep`, `sed`, `awk`, `find`, etc.
- `mk` — make-equivalent (Plan 9 tradition).
- `ps`, `top`, `htop` — process inspection (depend on `/proc`).
- Network tools (once Phase 8 lands the stack).

Each of these is its own Rust crate under `usr/utopia/coreutils/`, built native against libthyla-rs.

---

## 10. Job control and signals

### 10.1 The model

The shell's main event loop is a single `poll()` across:

- stdin (line editor).
- One notes fd per child process the shell has spawned and is tracking.
- A notes fd for the shell itself (so it sees PTY line-discipline-routed notes).
- Any other fds the shell is interested in (script-watched fds, etc.).

Everything is poll-driven. No async signal handlers. No `trap` fragility. The shell wakes when something is readable, dispatches in synchronous, lock-clean code, returns to the poll.

This is the natural Thylacine model — Phase 6 made notes fd-readable; `poll()` is the existing multi-fd wait primitive; the shell uses what's there.

### 10.2 Ctrl-C

1. PTY line discipline detects Ctrl-C (`ISIG` flag set on slave).
2. Kernel posts note `snare:int` to the foreground process group.
3. The shell's notes fd becomes readable.
4. `poll()` returns. The shell reads the note. It decides:
   - Foreground job is a child process → forward the note to it; wait for it to handle or kill.
   - No foreground job → clear the line being edited; print fresh prompt.
5. Loop continues.

### 10.3 Ctrl-Z

Same as Ctrl-C but with `snare:stop`. The shell suspends the foreground job (records its state, removes its fd from the active poll set), prints fresh prompt. `fg N` un-suspends.

### 10.4 Ctrl-D

Line discipline returns EOF on the input fd. If the input buffer is empty, the shell treats this as a request to exit. If the buffer has content, EOF is ignored (or, optionally, signals end-of-input to a command reading via `read`).

### 10.5 Background jobs

```
long_running_command &
```

Spawns the command; the shell prints `[N] PID` where N is the jobspec; adds the child's notes fd to the poll set; continues the prompt. When the child exits, the shell prints `[N]+ Done <cmd>` at the next prompt cycle (fish-style lazy notification, not bash-style immediate-interrupt).

### 10.6 Job control builtins

- `jobs` — list tracked children with status (running, stopped, done).
- `fg N` — un-suspend and resume in foreground; shell waits.
- `bg N` — un-suspend and continue async.
- `wait N` — block until child N exits.
- `kill N [snare:NAME]` — post a note to child N. Default note: `snare:term`.

### 10.7 The `on note` construct

```
on note 'snare:int' {
    echo
    echo 'interrupted; cleaning up'
    rm $tmpfile
}
```

Registers a handler that fires when a note of the named class arrives in the shell's notes fd. The handler runs in the main loop, not in a signal context. There are no async-signal-safe constraints; `printf`, `malloc`, `read` are all safe.

Multiple handlers for the same note class accumulate (in registration order). Handlers fire synchronously when poll returns the note.

### 10.8 The `mask note` block

```
mask note 'snare:int' {
    body
}
```

Suppresses delivery of the named note class during the body. Pending notes drain when the block exits (the handler for that class fires, if any, immediately after `mask` returns).

### 10.9 The Plan 9 spirit

This entire surface is "Thylacine's notes substrate, exposed at the shell." Programs that consume notes via the kernel substrate (libthyla-rs's `t::notes::Notes` API) see exactly the same model. The shell is not novel; it's idiomatic on top of what's already there.

---

## 11. Prompt and line editor

### 11.1 Prompt as a function

The prompt is the output of the user's `prompt` shell function (per `UTOPIA-VISUAL.md §3.3`). The shell calls `prompt` before each readline cycle and captures its stdout. The captured bytes become the prompt rendered by the line editor.

The default `prompt` (shipped in `/etc/utopia/utopia.rc`) is:

```
fn prompt {
    palette path (pwd)
    palette glyph ' ⊢ '
}
```

Users override `prompt` in `~/.config/utopia/utopia.rc`.

### 11.2 The line editor

The line editor is implemented in `libutopia::line_editor`, hand-rolled. **Not reedline** (which assumes std and Pouch); the line editor is native libthyla-rs, no_std + alloc.

Approximate scope: 1500-2500 LOC across the following capabilities:

- Raw-mode terminal handling: `termios`-equivalent via `/dev/consctl` writes (see `ARCHITECTURE.md §23.5`).
- Display: glyph + cursor management within a single line, with prompt-length tracking that accounts for ANSI escapes (`palette` helpers register zero-width markers).
- Editing: standard cursor motion (Ctrl-A, Ctrl-E, Ctrl-B, Ctrl-F, Alt-B, Alt-F), kill/yank (Ctrl-K, Ctrl-U, Ctrl-W, Ctrl-Y), insert/delete (backspace, delete), single-character commands.
- Multi-line input: continuation when brackets/quotes unbalanced; per-line cursor tracking; continuation glyph from `UTOPIA-VISUAL.md §3.2`.
- History: up/down arrows walk history; Ctrl-R initiates incremental search.
- Tab completion: pluggable completion source (the shell provides paths, command names, fn names, env vars, capability names).

Emacs keybindings are the default. `set editor vi` (in `~/.config/utopia/utopia.rc`) switches to vi mode. Both are within the same line-editor module — vi mode is a different state machine over the same primitives.

### 11.3 No syntax highlighting at v1

The Pale Fire palette is deliberately minimal. Syntax-highlighting the prompt would require introducing semantic colour distinctions (valid command, argument, redirect, etc.) that the palette does not allow.

Programs that prefer rich line-editor highlighting can override the editor; the default is plain.

Syntax highlighting can be added in v1.x if it earns the complexity.

### 11.4 Multi-line editing

When the user has open brackets or unclosed quotes at the end of a line, the shell continues to the next line with the `⋮` continuation glyph (per `UTOPIA-VISUAL.md §3.2`). The line editor tracks the multi-line buffer and supports navigation within it.

### 11.5 Tab completion source

Completions come from:

- **File paths** — when the cursor is positioned in a position that takes a file argument (the shell knows from the command being typed; for unknown commands it assumes file argument).
- **Command names** — for the first word of a command. Searches `$path`, plus the alias and function tables.
- **Function names** — same.
- **Variable names** — when the cursor is positioned after `$`.
- **Capability names** — when the command is `cap`.
- **Bind/mount paths** — when the command is `bind` or `mount`.
- **Third-party completions** — programs may declare completions via a `--ut-complete` flag (see §17.5 below).

### 11.6 History at the prompt

Up arrow / down arrow walk the in-memory history (loaded from the history file at shell startup). Ctrl-R initiates incremental fuzzy search. Pressing Enter accepts the matched history entry.

History storage is the next section.

---

## 12. History

### 12.1 Storage

A flat file at `~/.config/utopia/history`, one command per line. The shell appends after each command in interactive mode. Atomic-append-with-fsync to prevent corruption from concurrent shells.

Pragmatic for v1. Stratum-native history (snapshots, cross-host sync, structured query) is a v1.x consideration, not a v1 deliverable.

### 12.2 Format

One command per line. UTF-8. No metadata (timestamps, exit codes, cwd) at v1; the metadata-bearing variant is v1.x.

Multi-line commands are stored as a single history entry; newlines within a command are escaped as `\n` (with `\\` for literal backslash).

### 12.3 Search

In-memory, on shell startup. Ctrl-R walks the entries; reedline-style fuzzy is hand-rolled in the line editor.

### 12.4 Configuration

- `$HISTSIZE` — max entries kept in memory (default: 10000).
- `$HISTFILE` — path to the history file (default: `~/.config/utopia/history`).

Set in `~/.config/utopia/utopia.rc`.

### 12.5 Privacy

Commands beginning with a single space are not added to history (common interactive convention).

---

## 13. Naming

### 13.1 The shell binary: `ut`

Two letters. Mnemonic for Utopia. Concise; daily-typing-cheap; doesn't collide with common Unix commands.

The binary is installed as both `/bin/ut` and `/bin/utopia` (symlink to `ut`). `$SHELL=/bin/ut` is the conventional setting.

### 13.2 The default editor: `hx`

`hx` is Helix's standard binary name. Helix is the default `$EDITOR` for Thylacine; users who set `$EDITOR=hx` get the Thylacine-shipped Helix. Programs that launch an editor (git commit, mk-edit, etc.) honour `$EDITOR`.

### 13.3 The Rust workspace: `usr/utopia/`

Cargo workspace root. Members: `shell/`, `libutopia/`, `coreutils/*`. Helix is vendored separately at `usr/helix/` (its own workspace; tracks Helix upstream releases).

### 13.4 The crate names

- `utopia-shell` (binary `ut`) — the shell.
- `libutopia` (lib) — the shared Rust library.
- `utopia-cat`, `utopia-ls`, `utopia-echo`, etc. — coreutils.

The `utopia-` prefix prevents crate-name collisions in the workspace registry; the binaries lose the prefix on install (`/bin/cat`, not `/bin/utopia-cat`).

### 13.5 The configuration file

```
~/.config/utopia/utopia.rc          — per-user
/etc/utopia/utopia.rc               — site-wide default
```

XDG Base Directory convention. The `utopia.rc` extension matches the rc-shape: it's an rc-style script the shell sources at startup.

### 13.6 The visual identity name: Pale Fire

Per `UTOPIA-VISUAL.md`. Named after Nabokov's novel.

---

## 14. Rust crate layout

### 14.1 The workspace

```
usr/utopia/                           Cargo workspace root
├── Cargo.toml                        workspace manifest
├── shell/                            utopia-shell crate (binary "ut")
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs
│       ├── parser.rs
│       ├── ast.rs
│       ├── eval.rs
│       ├── builtins.rs
│       └── ...
├── libutopia/                        libutopia crate (shared lib)
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs
│       ├── palette.rs                Pale Fire constants + ANSI helpers
│       ├── ansi.rs                   escape emission, length-tracking
│       ├── line_editor.rs            hand-rolled line editor (~2000 LOC)
│       ├── notes.rs                  high-level notes API atop libthyla-rs
│       ├── ninep.rs                  high-level 9P client API
│       ├── tty.rs                    raw-mode + termios wrappers
│       ├── prompt.rs                 prompt-emit helpers
│       ├── path.rs                   path abbreviation + display
│       ├── errors.rs                 common error formatting
│       └── ...
├── coreutils/                        coreutils group
│   ├── Cargo.toml                    sub-workspace OR cargo group
│   ├── common/                       shared coreutils helpers (arg-parse, exit-code)
│   ├── cat/                          one binary per command
│   ├── ls/
│   ├── echo/
│   ├── grep/
│   ├── ...
└── helix/                            (NOT here; see §14.3)
```

### 14.2 The Helix port

Vendored separately at `usr/helix/`. Its own Cargo workspace. Builds via Pouch, NOT via the libthyla-rs path. Lives under `usr/helix/` to keep its dependency graph separate from `usr/utopia/`.

### 14.3 The libthyla-rs dependency

Every Utopia crate depends on `libthyla-rs` (at `usr/lib/libthyla-rs/`) and on `libutopia` (which itself depends on libthyla-rs).

### 14.4 Build wiring

`tools/build.sh` learns a new build target for the Utopia workspace. Approximate shape:

```bash
# new target: 'utopia'
tools/build.sh utopia          # builds usr/utopia/ workspace via aarch64-thylacine target
tools/build.sh helix           # builds usr/helix/ via Pouch
tools/build.sh all             # both
```

The host-bake step (`tools/build.sh::populate_stratum_pool`) is extended to copy:

```
target/aarch64-thylacine/release/ut          -> /bin/ut
target/aarch64-thylacine/release/cat         -> /bin/cat
target/aarch64-thylacine/release/ls          -> /bin/ls
... (one per coreutils binary)
usr/helix/target/aarch64-thylacine/release/hx -> /bin/hx
```

Plus the configuration files:

```
share/utopia/utopia.rc                       -> /etc/utopia/utopia.rc
```

### 14.5 The terminal-config files (host-side)

```
share/terminal-configs/ghostty.config
share/terminal-configs/kitty.conf
share/terminal-configs/alacritty.toml
share/terminal-configs/wezterm.lua
share/terminal-configs/iterm2.itermcolors
```

These ship in the repo. Users of Thylacine via UART/SSH apply them to their host terminal to get Pale Fire colours and the typeface recommendation.

---

## 15. The native runtime — `libthyla-rs` uplift to the library Thylacine deserves

### 15.1 Reframing

The existing `libthyla-rs` (857 lines as of `218feb0`) was right-sized for three Rust binaries (`hello-rs`, the virtio-* drivers, corvus). It is materially behind Thylacine's current kernel surface — Phase 6 added at least eight new syscalls (BURROW_ATTACH/DETACH, TORPOR_WAIT/WAKE, THREAD_SPAWN/EXIT, POST_SERVICE_BYTE) and 16c added two more (ATTACH_9P_SRV, PIVOT_ROOT). And the existing wrappers are bare: raw `i64` returns, no error types, no RAII for handles, no Path type, no notes API, no poll wrapper. It is not right-sized for `ut` + `libutopia` + a dozen coreutils + the future Phase 8 daemons + Halcyon.

U-1 (this scripture) commits to a different framing for the work that comes between now and `ut`-skeleton: **the libthyla-rs uplift to the library Thylacine deserves.** Not just the extensions Utopia needs — a complete, coherent, idiomatic Rust API for every kernel surface Thylacine exposes today. Future native Rust programs will look at libthyla-rs to learn "how is Rust on Thylacine done?" They should find something inspiring.

The uplift is the gating prerequisite to U-3+. Every U-* chunk that follows inherits the patterns established here.

### 15.2 Lead-by-example

Three concrete arguments:

1. **Current libthyla-rs is behind.** Every U-* chunk past U-2 would have to drag the library forward as a side-effect of its own work, in dribs and drabs, with no consistency. Better to do the uplift coherently up front.
2. **Every future Thylacine Rust program will copy these patterns.** Phase 8 (Linux compat + network) adds userspace daemons; Phase 10 (Halcyon) is a major Rust userspace; v1.x is more daemons. If `t::fs::File` has a beautiful RAII shape, every future program inherits that. If we leave it as bare wrappers + raw i64, that's what gets copied.
3. **Investment pays back across the whole remaining project.** v1.0 is multi-month; a ~10-session libthyla-rs uplift at the start of Phase 7 is high-leverage per session for the rest of v1.0 and into v1.x.

### 15.3 The two-library structure (no intermediate tier)

| Library | Audience | Shape |
|---|---|---|
| `libthyla-rs` | Every native Thylacine Rust program | Kernel surface + idiomatic Rust primitives. Generic. The library this section defines. |
| `libutopia` | Utopia (the shell, coreutils, future Utopia tools) | Application helpers — Pale Fire palette, ANSI emission, the line editor, prompt-format. Specific. Depends on libthyla-rs. |

No third-layer "helpers between libthyla-rs and libutopia." If something proves to be useful beyond Utopia (e.g., a logging framework), it migrates up to libthyla-rs at that time. Premature layering is worse than late layering.

Things that belong in libthyla-rs (per this scripture): Path/PathBuf, Error/Result, File/Metadata/ReadDir, Read/Write traits, Command/Child, Pipe, Notes, PollSet, Caps, Territory ops, Thread, Torpor, Time, Rand, Tty, 9P client, hardware-handle surfaces.

Things that belong in libutopia: Pale Fire palette + helpers, the line editor, prompt-emit, terminal-config knowledge.

### 15.4 The module structure

The libthyla-rs uplift target:

```
usr/lib/libthyla-rs/src/
├── lib.rs              re-exports; #[global_allocator] hook; #[panic_handler]; _start
├── syscall.rs          raw SVC wrappers; kept private for the most part
├── alloc.rs            #[global_allocator] backed by burrow_attach
├── err.rs              t::Error enum + t::Result<T> alias + From<i64>
├── handle.rs           Handle RAII newtype; rights tracked at type level
├── fs/                 filesystem surface
│   ├── mod.rs          File, Metadata, OpenOptions, SeekFrom
│   ├── path.rs         Path, PathBuf (std::path-shaped, no_std + alloc)
│   └── dir.rs          ReadDir, DirEntry, Treaddir-backed
├── io.rs               Read, Write, BufRead, Seek traits; Cursor; copy/read_to_*
├── process/            spawn + wait
│   ├── mod.rs          Command, Child, ExitStatus
│   ├── stdio.rs        Stdio enum (Inherit, Piped, File, Null)
│   └── pipe.rs         pipe() -> (Reader, Writer)
├── notes.rs            Notes RAII (open_self / open_pid), Note struct, mask guards
├── poll.rs             PollSet ergonomic wrapper, PollEvents bitflags
├── territory.rs        bind / mount / unmount / pivot_root / rfork
├── cap.rs              Caps set, query, drop; elevation-only marker types
├── thread.rs           Thread spawn / exit (peers within a Proc)
├── torpor.rs           wait/wake on address (futex shape)
├── time.rs             Instant, Duration, sleep
├── rand.rs             getrandom wrapper
├── tty.rs              /dev/consctl raw-mode; termios-equivalent ops
├── ninep/              9P client (lifted from corvus's internal impl)
│   ├── mod.rs          Session high-level API
│   ├── attach.rs       Tversion / Tattach helpers
│   ├── walk.rs         Twalk / Tlopen / Tlcreate
│   └── ops.rs          Tread / Twrite / Tgetattr / Tsetattr / Tremove / Treaddir / Treadlink
├── hardware/           hardware-handle surfaces (for the virtio-* drivers)
│   ├── mod.rs          common types
│   ├── mmio.rs         Mmio handle + map + read/write helpers
│   ├── irq.rs          Irq handle + wait
│   └── dma.rs          Dma handle + map
└── prelude.rs          `use t::prelude::*;` brings in commonly-used items
```

Every module is idiomatic Rust: RAII, typed errors, builders for complex constructors, lifetimes on borrowed references, marker types where invariants need compile-time enforcement.

The top-level crate is re-exported as the short name `t` for ergonomic use (`use t::fs::File;` reads cleaner than `use libthyla_rs::fs::File;`). The actual Cargo package name stays `libthyla-rs` for compat with existing callers.

### 15.5 The before/after taste

What "delightful" means concretely:

```rust
// Before (current libthyla-rs shape — every callsite does this):
let h: i64 = unsafe { svc4(T_SYS_WALK_OPEN, ...) };
if h < 0 { return Err(...); }
let mut buf = [0u8; 4096];
let n: i64 = unsafe { svc3(T_SYS_READ, h as u64, buf.as_mut_ptr() as u64, 4096) };
if n < 0 { unsafe { svc1(T_SYS_CLOSE, h as u64); } return Err(...); }
let s = core::str::from_utf8(&buf[..n as usize]).ok();
unsafe { svc1(T_SYS_CLOSE, h as u64); }
// Manual cleanup; raw error codes; no type guarantees.

// After (libthyla-rs uplift target):
let mut f = t::fs::File::open("/etc/hosts")?;
let mut buf = String::new();
f.read_to_string(&mut buf)?;
// Drop closes the handle. `?` propagates t::Error. Rights tracked at the type level.
```

```rust
// Notes + poll, the way the shell main loop wants to read:
let notes = t::notes::Notes::open_self()?;
let mut poll = t::poll::PollSet::new();
poll.add(notes.as_fd(), t::poll::READ);
poll.add(stdin.as_fd(), t::poll::READ);

loop {
    for r in poll.poll(None)? {
        match r.fd {
            fd if fd == notes.as_fd() => handle_note(notes.read()?),
            fd if fd == stdin.as_fd() => handle_stdin(),
            _ => {}
        }
    }
}
```

```rust
// Process spawn with builder + Stdio:
let child = t::process::Command::new("ls")
    .args(&["-l", "/etc"])
    .stdout(t::process::Stdio::Piped)
    .spawn()?;
let output = child.wait_with_output()?;
```

```rust
// Namespace composition (Thylacine-specific; no std equivalent):
t::territory::bind("/srv/stratum-ctl", "/n/stratum", t::territory::BindFlags::REPLACE)?;
let entries: Vec<_> = t::fs::read_dir("/n/stratum")?.collect::<t::Result<Vec<_>>>()?;
```

These read like idiomatic Rust to anyone who knows `std::fs` / `std::process` / `mio`, AND route every call through Thylacine syscalls. That's the moat.

### 15.6 Module API sketches

Indicative; exact signatures iterate during impl. These are commitments to shape, not to spelling.

#### 15.6.1 `t::err` — Error + Result

```rust
pub type Result<T> = core::result::Result<T, Error>;

#[non_exhaustive]
pub enum Error {
    // POSIX-aligned per docs/ERRORS.md
    InvalidArgument,        // T_E_INVAL
    PermissionDenied,       // T_E_ACCES (NOT T_E_PERM — see CLAUDE.md trap re: pouch -1 collision)
    NotFound,               // T_E_NOENT
    Interrupted,            // T_E_INTR
    NotImplemented,         // T_E_NOSYS
    TimedOut,               // T_E_TIMEDOUT
    BadHandle,              // T_E_BADF
    NoMemory,               // T_E_NOMEM
    IsDirectory,            // T_E_ISDIR
    NotDirectory,           // T_E_NOTDIR
    Exists,                 // T_E_EXIST
    // Thylacine-specific
    CapabilityRequired,     // attempted op needs a cap not held
    RightInsufficient,      // attempted op on a handle with insufficient rights
    NoteInvalidName,        // tried to use a reserved `snare:` prefix
    NamespaceCycle,         // bind would create a cycle (I-3 violation)
    // Pass-through for codes we haven't mapped
    Other(i64),
}

impl Error {
    pub fn from_syscall_return(rc: i64) -> Option<Self>;
    pub fn as_errno(&self) -> i32;
}

impl core::fmt::Display for Error { /* ... */ }
```

#### 15.6.2 `t::alloc` — heap allocator

```rust
// Set at the top of each native binary by the libthyla-rs prelude;
// burrow_attach-backed.
pub struct ThylaAlloc;

unsafe impl core::alloc::GlobalAlloc for ThylaAlloc { /* ... */ }

#[global_allocator]
static ALLOC: ThylaAlloc = ThylaAlloc;
```

Gating: without this, `alloc::Box` / `Vec` / `String` are unusable across Utopia.

#### 15.6.3 `t::handle` — Handle RAII

```rust
pub struct Handle { idx: i32, rights: Rights }
impl Handle {
    pub fn rights(&self) -> Rights;
    pub fn raw(&self) -> i32;
    pub(crate) fn from_raw(idx: i32, rights: Rights) -> Self;
}
impl Drop for Handle { fn drop(&mut self) { let _ = sys::close(self.idx); } }

bitflags::bitflags! {
    pub struct Rights: u32 {
        const READ      = 1 << 0;
        const WRITE     = 1 << 1;
        const EXECUTE   = 1 << 2;
        const TRANSFER  = 1 << 3;
        const SIGNAL    = 1 << 5;  // per kernel/include/thylacine/handle.h
        // ...
    }
}
```

Foundation for File, Notes, Mmio, Irq, Dma, etc. — every kernel-object-backed type composes a Handle.

#### 15.6.4 `t::fs` — File + Path + ReadDir + Metadata

```rust
pub struct File { /* private Handle */ }
impl File {
    pub fn open(path: impl AsRef<Path>) -> Result<File>;
    pub fn create(path: impl AsRef<Path>) -> Result<File>;
    pub fn options() -> OpenOptions;
    pub fn metadata(&self) -> Result<Metadata>;
    pub fn sync(&self) -> Result<()>;
    pub fn set_len(&self, len: u64) -> Result<()>;
}
impl Read for File { /* ... */ }
impl Write for File { /* ... */ }
impl Seek for File { /* ... */ }

pub struct OpenOptions { /* ... */ }
impl OpenOptions {
    pub fn new() -> OpenOptions;
    pub fn read(self, b: bool) -> Self;
    pub fn write(self, b: bool) -> Self;
    pub fn create(self, b: bool) -> Self;
    pub fn truncate(self, b: bool) -> Self;
    pub fn open(self, path: impl AsRef<Path>) -> Result<File>;
}

pub struct Metadata { /* fields per kernel struct t_stat */ }
impl Metadata {
    pub fn is_file(&self) -> bool;
    pub fn is_dir(&self) -> bool;
    pub fn len(&self) -> u64;
    // ... mtime, mode, uid/gid, etc.
}

pub struct ReadDir { /* ... */ }
impl Iterator for ReadDir { type Item = Result<DirEntry>; }
pub struct DirEntry { /* ... */ }
pub fn read_dir(path: impl AsRef<Path>) -> Result<ReadDir>;
```

Path/PathBuf shape-mirror std::path (component iteration, joins, parent, file_name, extension); no_std + alloc.

#### 15.6.5 `t::process` — Command + Child

Per §15.5 above; matches `std::process`.

#### 15.6.6 `t::notes`

```rust
pub struct Notes { /* private Handle */ }
impl Notes {
    pub fn open_self() -> Result<Notes>;
    pub fn open_pid(pid: Pid) -> Result<Notes>;
    pub fn read(&self) -> Result<Note>;          // blocking
    pub fn try_read(&self) -> Result<Option<Note>>; // non-blocking
    pub fn send(&self, target: NoteTarget, name: &str, body: &[u8]) -> Result<()>;
    pub fn mask<'a>(&'a self, classes: &[&str]) -> MaskGuard<'a>;
}
impl AsFd for Notes { /* allows use in PollSet */ }

pub struct Note {
    pub name: String,
    pub body: Vec<u8>,
    pub from: Option<Pid>,
}

pub enum NoteTarget { Pid(Pid), Group(Pgid), Process(Pid) }
```

#### 15.6.7 `t::poll`

```rust
pub struct PollSet { /* ... */ }
impl PollSet {
    pub fn new() -> Self;
    pub fn add<F: AsFd>(&mut self, fd: &F, events: PollEvents);
    pub fn remove<F: AsFd>(&mut self, fd: &F);
    pub fn poll(&mut self, timeout: Option<Duration>) -> Result<PollResults>;
}

bitflags::bitflags! {
    pub struct PollEvents: i16 {
        const READ  = 0x001;
        const WRITE = 0x004;
        const ERROR = 0x008;
        const HUP   = 0x010;
    }
}

pub struct PollResults { /* iterable */ }
impl Iterator for PollResults { type Item = PollResult; }
pub struct PollResult { pub fd: RawFd, pub revents: PollEvents }
```

#### 15.6.8 `t::territory`

```rust
pub fn bind(old: impl AsRef<Path>, new: impl AsRef<Path>, flags: BindFlags) -> Result<()>;
pub fn mount(server: impl AsRef<Path>, new: impl AsRef<Path>, flags: MountFlags) -> Result<()>;
pub fn unmount(new: impl AsRef<Path>) -> Result<()>;
pub fn pivot_root(new_root: impl AsRef<Path>) -> Result<()>;

bitflags::bitflags! {
    pub struct BindFlags: u32 { const BEFORE = 1; const AFTER = 2; const REPLACE = 4; }
    pub struct MountFlags: u32 { const BEFORE = 1; const AFTER = 2; const CACHE = 4; }
}

pub fn rfork(flags: RForkFlags) -> Result<RForkResult>;
```

#### 15.6.9 `t::cap`

```rust
pub fn current() -> Caps;
pub fn drop(caps_to_drop: Caps) -> Result<()>;

bitflags::bitflags! {
    pub struct Caps: u64 {
        const HW_CREATE     = 1 << 0;
        const LOCK_PAGES    = 1 << 1;
        const CSPRNG_READ   = 1 << 2;
        // Note: HOSTOWNER is elevation-only; not in this fork-grantable set
        const GRANT_HOSTOWNER = 1 << 4;
    }
}
```

#### 15.6.10 `t::thread`, `t::torpor`, `t::time`, `t::rand`, `t::tty`

Each a small focused module wrapping the corresponding syscall surface. Sketches:

```rust
// t::thread
pub fn spawn(f: impl FnOnce() + Send + 'static) -> Result<JoinHandle>;
pub struct JoinHandle { /* ... */ }
impl JoinHandle { pub fn join(self) -> Result<()>; }

// t::torpor (wait-on-address; futex shape)
pub fn wait(addr: &AtomicU32, expected: u32, timeout: Option<Duration>) -> Result<()>;
pub fn wake(addr: &AtomicU32, count: u32) -> Result<u32>;

// t::time
pub struct Instant(u64);
impl Instant { pub fn now() -> Instant; pub fn elapsed(&self) -> Duration; }
pub struct Duration(u64);  // nanoseconds; or wrap a representation
pub fn sleep(d: Duration) -> Result<()>;

// t::rand
pub fn getrandom(buf: &mut [u8]) -> Result<()>;

// t::tty
pub struct RawMode<'a> { tty: &'a Tty, /* saved state */ }
impl<'a> RawMode<'a> { pub fn enter(tty: &'a Tty) -> Result<Self>; }
impl Drop for RawMode<'_> { /* restore */ }
pub struct Tty { /* opens /dev/cons + /dev/consctl */ }
```

#### 15.6.11 `t::ninep` — 9P client

Lifted from corvus's internal client. High-level shape:

```rust
pub struct Session { /* ... */ }
impl Session {
    pub fn attach(transport: impl Transport, aname: &str, uname: &str) -> Result<Root>;
}

pub struct Root { /* Spoor handle for the attached root */ }
impl Root {
    pub fn walk(&self, path: &[&str]) -> Result<File>;
    pub fn create(&self, path: &[&str], perm: u32) -> Result<File>;
}
// File implements Read/Write/Seek as in t::fs::File.
```

#### 15.6.12 `t::hardware`

Existing surface from the virtio-* drivers; kept for them; cleaned up + properly typed during the uplift.

```rust
pub struct Mmio { /* Handle */ }
impl Mmio {
    pub fn create(addr: PhysAddr, size: usize) -> Result<Mmio>;
    pub fn map(&self) -> Result<MmioMap>;
}
pub struct MmioMap { /* mapped Vmo with read/write helpers */ }

pub struct Irq { /* Handle */ }
impl Irq {
    pub fn create(num: u32) -> Result<Irq>;
    pub fn wait(&self) -> Result<()>;
}

pub struct Dma { /* Handle */ }
impl Dma {
    pub fn create(size: usize) -> Result<Dma>;
    pub fn map(&self) -> Result<DmaMap>;
}
```

### 15.7 Sub-chunk decomposition

The U-2 work splits into focused sub-chunks. Each ships independently; each is auditable on its own; each preserves existing callers' build.

| Chunk | Scope | Estimated |
|---|---|---|
| **U-2** scripture amendment | This §15 rewrite. Plus the matching §19 + phase7-status.md updates. NO code. | 0.5 session (already happening) |
| **U-2a** err + handle | `t::err` (Error + Result + From<i64>) + `t::handle` (Handle RAII + Rights bitflags). The foundational types every other module composes. | 1 |
| **U-2b** alloc | `#[global_allocator]` via burrow_attach. Smoke binary using `alloc::Box`/`Vec`/`String`. | 1 |
| **U-2c** fs (Path + File + ReadDir + io) | `t::fs::{Path, PathBuf, File, OpenOptions, Metadata, ReadDir}` + `t::io::{Read, Write, Seek, BufRead}`. The fs + io modules together. | 1-2 |
| **U-2d** process + pipe + stdio | `t::process::{Command, Child, ExitStatus, Stdio}` + `t::process::pipe()`. | 1 |
| **U-2e** notes + poll | `t::notes::{Notes, Note, MaskGuard, NoteTarget}` + `t::poll::{PollSet, PollEvents}`. | 1 |
| **U-2f** territory + cap | `t::territory::{bind, mount, unmount, pivot_root, rfork}` + `t::cap::{Caps, current, drop}`. | 1 |
| **U-2g** thread + torpor + time + rand + tty | The smaller scope modules grouped. | 1 |
| **U-2h** ninep + hardware | Lift the 9P client out of corvus into `t::ninep`; consolidate the virtio-* hardware-handle surface into `t::hardware`. Migration commit for corvus + virtio-* in the same PR. | 1-2 |
| **U-2-test** | Cross-module smoke binary running on Thylacine: opens a file, reads, computes, writes, spawns a child, pipes, polls, sends a note, drops caps. Validates the library end-to-end. | 1 |

Total: ~9-12 sessions. Each sub-chunk is independent enough that a context-compaction at any boundary is recoverable.

### 15.8 Backwards compatibility

Existing libthyla-rs callers (corvus, virtio-*, hello-rs, mmio-probe, irq-probe, irq-bench, virtio-blk-probe, virtio-blk-rw, virtio-input, virtio-gpu, virtio-net-*) MUST continue building after each U-2X sub-chunk. Strategy:

1. **Add new modules without removing the existing constants/wrappers.** During the uplift, the old `T_SYS_*` constants + the bare svc wrappers stay public so callers don't break.
2. **Where a sub-chunk restructures something a caller uses (e.g., U-2h moves the 9P client out of corvus), the migration lands in the same commit as the restructure.** A single atomic commit; never a half-migrated tree.
3. **The U-2-test sub-chunk validates that the existing callers still pass their own tests** after the uplift. If a caller regresses, that's a U-2X bug.
4. **Once U-2-test is green and the next U-* chunk (U-3) lands, we audit which old-shape exports are still in use.** Anything unused gets removed in a separate cleanup commit; anything still in use stays public (or migrates its callers in the same cleanup).

### 15.9 Discipline — what we don't do

- **Don't import the entire std API surface.** Some std things are dubious on Thylacine (Mutex with poisoning at v1 — no). Add what's load-bearing.
- **Don't invent novel Rust idioms.** Where std has a shape, mirror it (File, Path, Command). Where mio or crossterm has a shape, mirror it. Invent only where Thylacine is genuinely novel (notes, caps, namespaces).
- **Don't add abstractions for hypothetical future programs.** Two real uses > one premature abstraction. The litmus test: does any near-term U-* chunk use this? If yes, add; if no, defer.
- **Don't make libthyla-rs into a framework.** It's a library — small, focused, no global state beyond the allocator, no startup hooks beyond what's already there.
- **Don't reach for unsafe except in the syscall.rs glue.** Every module above syscall.rs should be safe by default; unsafe usage in higher modules needs explicit justification.

### 15.10 What stays out

- **Threading as an async runtime.** Native Thylacine programs use poll() for concurrency; no tokio, no futures, no async-trait.
- **Network.** Phase 8 deliverable (ROADMAP §9). When network lands, libthyla-rs grows a `t::net` module then.
- **std::sync::Mutex with poisoning semantics.** A simple `t::sync::Mutex` may be added if multi-thread programs need it; the poison-on-panic story is deferred until there's a clear need.
- **Logging framework.** v1 programs emit to stderr. A structured logging facility is v1.x.
- **Serde-style serialization.** Programs that need JSON emit it directly; serde is v1.x.
- **Backwards-compat shims for POSIX shapes.** Programs that want POSIX go via Pouch. libthyla-rs is Thylacine-shaped; the boundary is explicit.

---

## 16. The Helix port

### 16.1 Why Helix

Modal text editor in Rust, Kakoune-inspired (verb-after-object semantics), multi-cursor as a first-class concept, tree-sitter syntax + LSP built in. Aligns with the Rust userspace commitment and provides the tree-sitter substrate Halcyon will likely want.

### 16.2 The port path

Helix uses full Rust std + tokio + ratatui + crossterm + ~150 dependent crates. Porting to no_std + libthyla-rs is impractical. Helix uses **Pouch**, like stratumd does.

The pouch path for Helix:
1. Vendor Helix at `usr/helix/` (likely a git submodule or a vendored snapshot pinned to a release).
2. Cross-compile via Pouch with the `aarch64-thylacine` target.
3. Resolve any pouch-patch growth required (likely minor; Helix's syscall surface is mostly file I/O + termios + spawn).
4. Bundle tree-sitter grammars (audit licenses; vendor as part of the helix port).
5. Bake `hx` into the pool fixture via `tools/build.sh::populate_stratum_pool`.

### 16.3 LSP

Helix has built-in LSP client. Language servers (rust-analyzer, clangd, etc.) are separate ports — each is its own pouch arc when added. v1 ships Helix without language servers; LSP works the moment a user installs a server.

### 16.4 Themes

A Pale Fire theme for Helix ships with the port. The colour mapping covers:

- Editor background → `bg`.
- Editor foreground → `fg`.
- Selection, status bar, line numbers, etc. → palette-derived shades (TBD; documented in `usr/helix/themes/pale-fire.toml`).

Helix's theme system is flexible; the Pale Fire constraint relaxes slightly here (a code editor needs more semantic colour than a shell prompt) but the four canonical roles remain visible.

---

## 17. Minor design decisions resolved in scripture

These are spelled out for completeness; each was minor enough that the U-1 design conversation did not separately question them.

### 17.1 Globbing

Per §6.10 above.

### 17.2 Process substitution

Per §6.12 above.

### 17.3 Heredocs

Per §6.11 above.

### 17.4 Background job notifications

Lazy at next prompt (fish-style), not immediate (bash-style). Less jarring during interactive use.

### 17.5 Tab completion authoring (third-party programs)

A program may opt into ut tab-completion by recognizing a `--ut-complete` flag. When invoked as `prog --ut-complete [current-word]`, it emits structured completion data on stdout (one candidate per line; lines may be tab-separated for `candidate<TAB>description`). The shell caches the completion source per program; the program is responsible for keeping its completion behaviour consistent with its CLI.

This is sketch-level at U-1; details land with `U-7` (tab-completion source plumbing).

### 17.6 Plugin model

Just sourceable scripts at v1. `source ~/some/script.rc` or `. ~/some/script.rc`. No dynamic loading of compiled plugins. v1.x can consider a plugin model if real users want one.

### 17.7 Shell exit on Ctrl-D

Yes (provided the input buffer is empty). See §10.4.

### 17.8 Variable scoping

Dynamic by default; `let` declares local in fn scope. See §6.2.

### 17.9 `set -x` equivalent

`trace { ... }` block-scoped tracing. See §7.8.

### 17.10 Empty pipeline element

Syntax error. `cmd1 | | cmd2` is rejected at parse time, not silently ignored.

### 17.11 Comment syntax

`#` to end-of-line. No block comments. See §5.2.

### 17.12 Line continuation

Trailing `\` joins lines; auto-continue when brackets/quotes unbalanced. See §5.3.

### 17.13 Word splitting on unquoted variables

rc-style: lists are lists; scalars are scalars. No IFS. See §6.3.

### 17.14 `set` builtin with no args

Prints all variables in alphabetical order, one per line, in `name=value` shape with single quotes around `value` if it contains spaces.

### 17.15 `cd` with no args

Goes to `$home`.

### 17.16 `cd -`

Toggles to the previous directory. The previous directory is the shell variable `$OLDPWD`, written by `cd` each time.

### 17.17 Multi-line input continuation

Auto-continue when brackets (`{`/`(`/`[`) or quotes are unbalanced or when a trailing `\` is present. The continuation glyph (`⋮`) appears at the start of each continuation line.

### 17.18 The `errstr` variable

Per §8.5.

### 17.19 String comparison

Use pattern matching or `==`. See §6.14.

### 17.20 Numeric arithmetic

`(( expr ))`. Integer only at v1. See §6.13.

---

## 18. The integration test (Phase 7 exit gate)

The shell ships when this test passes:

1. Boot a fresh Thylacine VM.
2. Connect via UART (or SSH, once the network is up).
3. Land at the Pale Fire `ut` prompt. `pwd` returns `$home` (typically `/home/<user>` or similar).
4. Run a multi-stage shell pipeline: `cat /etc/passwd | grep root | cut -d: -f1` produces the expected output.
5. Background a long command: `sleep 100 &`. Verify `jobs` lists it. Ctrl-Z a foreground command; verify it appears stopped in `jobs`. `fg` resumes it.
6. Define a fn; call it; verify error propagation: a fn with `cmd1?; cmd2?; cmd3?` where `cmd2` exits 1 returns immediately without running `cmd3`.
7. `bind /srv/stratum-ctl /n/stratum`; `ls /n/stratum` shows the Stratum admin surface.
8. `note send $$ snare:user1`; `on note 'snare:user1' { echo got it }` registered earlier in the session; verify the handler fires.
9. Launch `hx /etc/hosts`; verify Helix opens; edit; save; close; the file change is observable in the file.
10. Run a script that exercises rc-shape syntax: `for (f in *.md) { wc -l $f }`; output is right.
11. The Pale Fire prompt renders correctly: path in `#8898b4`, glyph `⊢` in `#e07840`, command in `#d8e4f4`. The visual identity is visible.
12. Assert no kernel extinctions, no driver crashes, no zombie processes.

When this passes, Utopia v1 ships at Phase 7 exit.

---

## 19. Implementation roadmap

The U-* chunks; the order honours dependencies.

| Chunk | Scope | Depends on |
|---|---|---|
| **U-1** | Scripture: this doc + UTOPIA-VISUAL.md + UTOPIA.md + ARCH §3.5 + ROADMAP §8 + phase7-status.md + CLAUDE.md updates. No code. | — |
| **U-2** | Scripture amendment: §15 (the libthyla-rs uplift framing) + this §19 update + phase7-status.md U-* arc refresh. No code. | U-1 |
| **U-2a** | `t::err` (Error + Result) + `t::handle` (Handle RAII + Rights bitflags). Foundational types. | U-2 |
| **U-2b** | `t::alloc` (`#[global_allocator]` via burrow_attach). Enables `alloc` crate. Smoke binary with Box/Vec/String. | U-2a |
| **U-2c** | `t::fs::{Path, PathBuf, File, OpenOptions, Metadata, ReadDir}` + `t::io::{Read, Write, Seek, BufRead}`. The fs + io modules. | U-2b |
| **U-2d** | `t::process::{Command, Child, ExitStatus, Stdio}` + `t::process::pipe()`. | U-2c |
| **U-2e** | `t::notes::{Notes, Note, MaskGuard}` + `t::poll::{PollSet, PollEvents}`. | U-2c |
| **U-2f** | `t::territory::{bind, mount, unmount, pivot_root, rfork}` + `t::cap::{Caps, current, drop}`. | U-2a |
| **U-2g** | `t::thread` + `t::torpor` + `t::time` + `t::rand` + `t::tty`. The smaller-scope modules grouped. | U-2c |
| **U-2h** | `t::ninep` (lift 9P client from corvus) + `t::hardware::{Mmio, Irq, Dma}` (consolidate virtio-* surface). Migrates corvus + virtio-* callers in the same commit. | U-2c, U-2d |
| **U-2-test** | Cross-module smoke binary exercising every libthyla-rs module on Thylacine. Validates the uplift end-to-end. | U-2a..U-2h |
| **U-3** | Utopia workspace skeleton: `usr/utopia/{Cargo.toml,shell,libutopia,coreutils}`; libutopia palette/ansi/path modules; `ut` skeleton (version + exit); `tools/build.sh utopia` wiring; host-bake `/bin/ut`. | U-2-test |
| **U-4** | Line editor in libutopia: raw mode + emacs keybindings + line buffer + multi-line + tab hook + Ctrl-R history hook. | U-3 |
| **U-5** | Parser + AST. Pure logic; unit-testable on host. | U-3 |
| **U-6** | Evaluator core + main loop: poll() main loop; built-ins (cd, exit, set, source, fn, alias, eval, type, etc.); external command spawn; pipes; redirection; `?`/try-catch; pipefail. | U-4, U-5 |
| **U-7** | fd-notes job control: Ctrl-C/Ctrl-Z handling; `&`; jobs/fg/bg; on note / mask note. | U-6 |
| **U-8** | Thylacine builtins: bind, mount, unmount, pivot_root, rfork, cap, note. | U-6 |
| **U-9..N** | Coreutils, one or two per chunk: cat, ls, echo, grep, sed, awk, cp, mv, rm, mkdir, find, wc. | U-3 (each is independent thereafter) |
| **U-Helix** | Helix port via Pouch. Parallel to U-6..N. | U-3 (for the host-bake path); independent of shell impl. |
| **U-Z** | The integration test (§18 above). Multiple full-suite passes; perf measurements; doc final pass. | All above. |

Rough scale: 27-37 sessions across the arc. The libthyla-rs uplift (U-2..U-2-test) is the heaviest sub-arc — ~9-12 sessions — and is investment in the library every subsequent chunk builds on. After U-2-test, the U-3+ chunks parallelize across multiple sub-arcs.

---

## 20. Open design questions

None at U-1.

---

## 21. References

- `docs/UTOPIA.md` — the top-level Utopia experience.
- `docs/UTOPIA-VISUAL.md` — Pale Fire palette + glyph + prompt format.
- `docs/ARCHITECTURE.md §3` — language and toolchain (extended in U-1 with the native vs ported split).
- `docs/ARCHITECTURE.md §23` — POSIX surfaces and the Utopia milestone (substantially updated in U-1).
- `docs/ROADMAP.md §8` — the execution-phase definition (updated in U-1).
- `docs/POUCH-DESIGN.md` — the ported-code substrate.
- `docs/ERRORS.md` — the errno + snare:* note-name registry.
- `docs/VISION.md §13` — the Utopia milestone framing.
- `docs/phase7-status.md` — per-chunk progress tracking.
- `usr/lib/libthyla-rs/src/lib.rs` — the native runtime crate.
