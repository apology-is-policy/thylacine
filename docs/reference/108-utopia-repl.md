# 108 — `libutopia::repl` + the `ut` REPL main loop (U-6g)

The as-built reference for the Utopia shell's read-parse-eval loop. Landed
at Phase 7 **U-6g**. Binding designs: `docs/UTOPIA-SHELL-DESIGN.md` §10
(job control + main loop), §11 (prompt + line editor), §486 (interactive
implicit-fail). Companion references: `92-utopia-line-editor.md` (U-4),
`93-utopia-parser.md` (U-5), `94-utopia-eval.md` (U-6a..f).

## 1. Purpose

U-6g is the chunk that turns `ut` from a banner-and-exit skeleton into a
working interactive shell. It glues the three earlier halves into one loop:

```
fd 0 bytes ──> U-4 line editor ──Accept(line)──> U-5 parser + U-6 evaluator
                  (feed_bytes)                       (eval::eval_source)
     ^                                                      │
     └──────────── render prompt/echo to fd 1 <─────────────┘
```

The loop lives in two pieces:

- **`libutopia::repl::Repl`** — the fd-agnostic core. Owns the `Env` + the
  `LineEditor`; `feed`s it already-read input bytes and writes rendering to
  an injected `io::Write` sink. No fd, no `poll`, no syscalls — so it is unit-
  testable against in-memory byte streams.
- **The `ut` binary** (`usr/utopia/shell/src/main.rs`) — owns the real fd-0
  read (and the single-fd `poll()`), feeding chunks into `Repl::feed` with
  `io::stdout()` as the sink.

### 1.1 Scope boundary (binding)

| Concern | Chunk | Why |
|---|---|---|
| Read-parse-eval cycle; Ctrl-D/`exit`/EOF exit; prompt render | **U-6g** (here) | The loop itself |
| `&`; jobs/fg/bg/wait/kill | **U-7a/b** | §10.5 / §10.6 (LANDED) |
| `on note`/`mask note` runtime delivery (sync-point, §3.7) | **U-7c-a** | LANDED — drained at the prompt cycle; the cons fd is unpollable until U-PTY |
| Ctrl-C foreground forwarding | **U-7c-b** | LANDED — `eval::wait_pids_interruptible` forwards `interrupt` to the running fg job (§10.2 as-built) |
| Ctrl-Z; multi-fd `poll()` across a pollable cons + notes fds | **U-PTY** | needs a kernel stopped thread-state + a pollable cons `.poll` hook |
| Pollable cons (`.poll` hook) + termios (raw/cooked, ECHO, ISIG) + per-Proc fd 0/1/2 | **U-PTY** | The line-discipline substrate |

At v1.0 `/dev/cons` is a blocking-read-only Dev with **no `.poll` hook**
(`kernel/cons.c`), so the `ut` loop blocks in `read()`; the single-fd
`PollSet` over fd 0 is the U-7 seam, not a load-bearing wait. The cons
`.poll` hook is deliberately **not** pulled forward — it is an audit-bearing
kernel surface whose IRQ-context producer can only `wakeup()` a `Rendez`
(the non-IRQ-safe poll-waiter-list is exactly what `cons.c` avoids), and
scripture stages it to U-PTY.

## 2. Public API

### `libutopia::repl::Repl`

```rust
pub struct Repl { /* env + editor */ }

impl Repl {
    /// A fresh interactive REPL. Env::interactive is set (scripture 8.9).
    pub fn new() -> Self;

    /// Borrow the evaluator state (inspect $status, variables).
    pub fn env(&self) -> &Env;
    /// Mutable evaluator state (pre-seed vars / source an rc file pre-loop).
    pub fn env_mut(&mut self) -> &mut Env;

    /// Emit the current prompt + buffer. Call once before the first feed.
    pub fn draw_prompt(&self, out: &mut dyn io::Write);

    /// Feed one chunk of input bytes through the editor + evaluator.
    /// Returns Some(exit_code) iff the session should terminate (the user
    /// ran `exit`, or pressed Ctrl-D on an empty buffer). None = read more.
    pub fn feed(&mut self, input: &[u8], out: &mut dyn io::Write) -> Option<i32>;

    /// The exit code when the input stream ends (EOF) without an explicit
    /// `exit`: the last command's $status.
    pub fn exit_code(&self) -> i32;

    /// D2: evaluate a whole script `src` non-interactively (`ut SCRIPT`).
    /// Binds the positionals (names "0"/"1"/"2"/"*") at the global scope,
    /// sets script mode (interactive=false -> fail-fast, scripture 8.9),
    /// eval_sources the source, returns the exit code (`exit N` wins, else
    /// the last $status). No line editor / prompt / notes loop. See
    /// 94-utopia-eval.md §13.3.
    pub fn run_script(&mut self, arg0: &str, args: &[String], src: &str) -> i32;

    /// #115a: install namespace-driven Tab completion. Scans /bin once and
    /// installs the production ShellCompletionSource into the editor. Called
    /// ON-TARGET (gated on a live console, like open_notes); new() stays
    /// syscall-free so host tests + the bare-spawn boot check pay nothing.
    pub fn install_completion(&mut self);

    /// #115b: load + enable ~/.ut_history persistence (after set_home). Reads
    /// any existing file into the editor's history; records the path so
    /// accepted lines append. No-op + in-memory-only when $home is unset.
    pub fn install_history(&mut self);
}
```

### `impl io::Write for Vec<u8>` (libthyla-rs)

A new in-memory sink in `usr/lib/libthyla-rs/src/io.rs` — `write` appends,
`flush` is a no-op. Mirrors `std::io::Write for Vec<u8>`. Used as the
`Repl::feed` sink in the host unit tests + `u-repl-test`, and useful for
buffering composed output before a single syscall.

## 3. Implementation

### 3.1 The `ut` main loop

`usr/utopia/shell/src/main.rs::rs_main`:

1. `print_banner()` — the Pale Fire version banner (`v0.6-dev`) via
   `t_putstr` (UART direct, so it shows even when fd 1 is absent).
2. `Repl::new()` + `draw_prompt(&mut io::stdout())`.
3. A `PollSet` with fd 0 (`io::stdin()`) added for `READ`.
4. Loop: `poll.poll(Block)` → `io::stdin().read(&mut buf)`:
   - `Ok(0) | Err(_)` → `break repl.exit_code()` (EOF / no fd 0 / error).
   - `Ok(n)` → on the FIRST such read, `repl.open_notes()` (U-7c; lazy, see
     below); then `repl.feed(&buf[..n], &mut out)`; if it returns `Some(code)`,
     `break code`.
5. Return the exit code as the process status.

The `poll()` is correct on a pipe (real readiness) and harmless on cons
(no `.poll` → always-ready → the subsequent blocking `read` waits). It
cannot spin: `read` returning `Ok(0)`/`Err` breaks; only `Ok(n>0)` (genuine
input) continues.

**Lazy note-fd open (U-7c).** `open_notes` (`Notes::open_self`) is deferred to
the first successful read instead of running before the loop. A bare-spawned
`ut` (joey's boot check, which verifies `ut` runs + exits 0) has an EMPTY handle
table — an eager open would mint the note fd at **fd 0**, and the read loop
would then block on the note queue forever instead of EOFing. Deferring to a
confirmed input stream means the boot-check `ut` never opens notes (it EOFs on
the first read and exits 0), while a session `ut` (login gives it fd 0/1/2)
opens its note fd at 3+. The note fd is deliberately NOT added to the `PollSet`:
the cons fd is unpollable until U-PTY, so notes are delivered at the prompt-cycle
sync point (§3.7), not via this poll.

### 3.2 `Repl::feed` action dispatch

`feed` calls `editor.feed_bytes(input)` (returns an **owned** `Vec<EditorAction>`,
so the loop body may freely take `&mut self`) and dispatches each action:

| `EditorAction` | Effect |
|---|---|
| `NoChange` | nothing |
| `Redraw` | `render(prompt)` → sink |
| `Accept(line)` | `\r\n` → sink; **`deliver_notes` FIRST (the RW-9 R3-F1 stale-idle drain, `cd7eae2` — a Ctrl-C queued while idle at the prompt must not deliver mid-next-command)**; push non-empty line to history; `run_line` (which clears a consumed interrupt via `take_interrupt()` → `$status=130`); if `env.exit_requested()` → return `Some(code)`; else `reap_jobs` (U-7a) then `deliver_notes` (U-7c, §3.7) then fresh prompt |
| `Cancel` (Ctrl-C) | `\r\n` + fresh prompt (the editor already reset its buffer) |
| `Eof` (Ctrl-D, empty buffer) | `\r\n`; return `Some(env.status())` |
| `ClearScreen` (Ctrl-L) | `\x1b[2J\x1b[H` + render |
| `MenuShow { candidates, selected }` (D4) | redraw prompt+buffer, then a one-line `render_menu_strip` candidate strip below (reverse-video on `selected`), cursor restored to the prompt via DECSC/DECRC; `menu_shown = true` |

(D4) Any action OTHER than `MenuShow`/`NoChange`, when `menu_shown`, first clears
the strip line below the prompt (`\x1b7\r\n\x1b[K\x1b8`) and clears `menu_shown`,
so a completion strip never lingers under a fresh prompt or accepted line.

`exit` short-circuits the whole input batch: the first `Accept` whose
`run_line` sets the exit request returns immediately, skipping any later
actions in the same chunk.

### 3.3 `run_line` + error policy

`run_line(line, out)` calls `eval::eval_source(&mut env, line)`. On `Err`,
it writes a `ut: <diagnostic>\r\n` line to the sink (preferring the rich
`$errstr` that `eval_source` sets on a parse error, falling back to the
`EvalErrorKind` Debug form) and **continues** — at the interactive prompt a
parse/eval error does not end the session (scripture 8.9). The
`StatementFlow` result is irrelevant at the top level.

Evaluation OUTPUT (`echo`, external command stdout) does NOT flow through
`out` — it goes to fd 1 from the builtin / spawned child directly. On a real
terminal `out` IS fd 1, so the rendering and the eval output interleave
correctly (the `\r\n` after Accept precedes the eval output, which precedes
the fresh prompt).

### 3.4 The prompt

`Repl::prompt()` builds the default prompt each call from `env.cwd()`:
path-coloured cwd + glyph-orange ` ⊢ ` (the §11.1 default `prompt` function's
output shape). The cwd segment is HOME-abbreviated to `~`/`~/...` via
`path::abbreviate_home(cwd, $home)` (#118) — so `/home/cora/src` renders
`~/src`; an empty `$home` (a bare-spawned `ut`) leaves the cwd verbatim.
`render` strips the SGR escapes for width (`ansi::visible_width`), so the
colour does not disturb cursor positioning. Capturing the user's own `prompt`
shell function's stdout is deferred to a later chunk (it needs rc loading +
function-output capture).

`$home` itself is established by `Repl::set_home(path)` (#113): a session `ut`
learns its home from login's `--home <path>` argument (there is no kernel
envp), sets `$home` (which `cd` with no argument resolves to, and the prompt
abbreviates), and `chdir`s into it so the shell starts in the user's home,
syncing `$cwd` on success. A home that cannot be entered leaves `ut` at `/`
rather than failing startup; a bare-spawned `ut` gets no `--home` and runs at
`/` unchanged.

### 3.5 The input substrate

`ut` reads **fd 0**. Two live sources:

- **Console** — login (`/sbin/login`, A-5) spawns `ut` with `Stdio::Inherit`
  over the console handle joey opened via `SYS_CONSOLE_OPEN` (and inherited
  through the getty). The blocking byte read is the same path login's own
  `read_line` uses.
- **Pipe** — the CI login E2E seeds creds into a pipe as login's fd 0; `ut`
  inherits it.

When joey spawns `/ut` bare for the boot smoke, the child has **no fd 0**
(`rfork` supports only `RFPROC`; every Proc gets a fresh empty handle table
— `kernel/proc.c`), so the first `read` errors and `ut` exits 0 after the
banner. That is the link + boot + banner smoke; the real loop is exercised
deterministically by `/u-repl-test`.

### 3.6 Background-job reaping (U-7a; reaper shared at U-7b)

`Repl::reap_jobs(&mut self)` is the **syscall-driven** half of job tracking
(the `JobTable` in `Env` is pure — `docs/reference/94-utopia-eval.md` §9). It
runs in the `Accept` branch after `run_line` and before the fresh prompt
(bash's `[N]+ Done`-before-prompt ordering, so a job that finished while the
just-run foreground command was waiting is reported now). The poll-and-mark
half is `builtin::reap_background(env)` (extracted at U-7b and shared with the
`jobs` builtin's refresh — `94-utopia-eval.md` §9.5): for each
`env.jobs().live_pids()` it does ONE `wait_pid_for(pid, T_WAIT_WNOHANG)` —
reaped → `mark_reaped`, `-1` (gone) → `mark_reaped(pid, 0)` so the job still
completes, `0` (alive) → leave it. `reap_jobs` then `t_putstr`s each
`take_done_notifications()` line. One poll per live pid per prompt, never a
busy-loop (U-7-pre F1: a hot WNOHANG loop can starve the awaited child). It is
`pub` so the `/u-job-test` probe can drive the real reap path. A bg job that
finishes while the user is idle at the prompt is reported at the next accepted
line; the async-while-idle path (a pollable cons + the notes fd in the poll
set) is U-PTY / U-7c-b.

### 3.7 Note delivery (U-7c-a)

`Repl::deliver_notes(&mut self)` runs in the `Accept` branch right after
`reap_jobs` (the same prompt-cycle sync point) and delegates to
`eval::deliver_pending_notes(&mut self.env)` — see `94-utopia-eval.md` §9.6.
It drains the shell's own note queue (opened EAGERLY at REPL start when
stdout is live — the LS-5/RW-0 HT00.F1 fix `f145ce8`; the bare-spawn boot
check deliberately stays lazy so the note fd is not minted as fd 0) and
fires any registered `on note` handler; `mask note` defers a class for its
body via the kernel Thread mask. Delivery is at this sync point (not async)
because the cons fd is unpollable until U-PTY. `deliver_notes` is `pub` so the
`/u-job-test` probe drives delivery directly; it is a no-op when the note fd is
unopened (host tests, the bare-spawn boot check) AND no notes are deferred.

Since U-7c-b, `deliver_pending_notes` also fires the notes **deferred** during
an interruptible foreground wait (a handler-bearing note that arrived while a
foreground command ran — `94-utopia-eval.md` §9.7) — drained FIFO, before the
live queue. So a `pipe`/user note arriving mid-command runs its handler at this
prompt-cycle sync point, never mid-command.

### 3.8 Tab completion install (#115a)

`install_completion` (called once by `ut` after `set_home`, gated on
`io::stdout_is_live()` like `open_notes`) scans `/bin` into the cached
`bin_commands` (regular files only — the #58 exec-namespace external command
set, static for the session) and calls `refresh_command_index`. The latter
merges that cache with the live builtins + `Env::alias_names()` +
`Env::fn_names()` into a sorted, deduped command index and installs a fresh
`completion::ShellCompletionSource` into the editor — and hands the SAME index
to `editor.set_known_commands` for #115c command-line validity coloring. `feed`
re-runs
`refresh_command_index` after every accepted line so an interactively-defined
`fn`/alias is completable at the next prompt; it is a no-op until
`install_completion` has run (`completion_installed` gate), so host tests + the
bare-spawn boot check keep the inert-Tab behaviour. See `92-utopia-line-editor.md`
"Production source: `ShellCompletionSource`" for the per-context completion logic.

### 3.9 History persistence (#115b)

`install_history` (called once by `ut` after `set_home`, same session-only gate)
resolves `$home/.ut_history` (`None` when `$home` is unset → in-memory-only),
reads any existing file via `File::open` + `read_to_string`, and replays its
non-empty lines through `editor.push_history` (capped at the editor's
`HISTORY_CAP` by eviction, newest kept). It records the path; `feed` then
appends each accepted non-empty line via `append_history` —
`OpenOptions::new().write().append().create().mode(0o600)` + `write_all`. The
file is **append-only** (never rewritten), so a torn write loses at most the
trailing line, and **0600** keeps history owner-only (it can carry sensitive
command args; the encrypted home already gates access — this is
defense-in-depth). v1.0 persists **single-line commands only** (`append_history`
skips a line containing `\n`, which would split into separate entries on
reload); multi-line commands stay in-memory for the session. The file grows
unbounded (in-memory is capped, the file is not) — a v1.x atomic trim
(write-temp + rename) bounds it. The cross-session round-trip (type → logout →
login → Up) is covered by the interactive LS-CI harness (blocked by #105, the
same posture as the Phase-1 visible-ergonomics); build + boot proves
`install_history` runs non-crashing against the real `/home/<user>`.

## 4. Exit semantics

`exit` (builtin, U-6e) calls `env.request_exit(code)`; `feed` returns
`Some(code)` after the `Accept` that ran it. Ctrl-D on an empty buffer
returns `Some(env.status())`. Input EOF (read 0/Err) → the `ut` loop breaks
with `repl.exit_code()` = `exit_requested().unwrap_or(env.status())`.

## 5. Tests

- **Host `#[cfg(test)]`** (`repl.rs`, 7 tests): assignment eval + Env state;
  split-across-reads accumulation; `exit N` → `Some(N)`; Ctrl-D → exit;
  Ctrl-C discard + recovery; Redraw renders to the sink; parse-error survival.
- **`usr/u-repl-test`** (the boot probe; joey-gated on status==0): the same
  7 cases driven through `Repl::feed` with a `Vec<u8>` sink. The interactive
  keystroke path (fd 0 = `/dev/cons`) cannot be driven non-interactively in
  the harness — QEMU offers no UART-RX injection without disturbing the boot-
  banner ABI (the A-4c constraint) — but `feed` is fd-agnostic: a pipe/cons
  delivers the same bytes the editor consumes, so the probe exercises the
  full loop. Prints `u-repl-test: all OK`.
- **Boot E2E**: joey's bare `/ut` spawn (banner + clean exit) + the login E2E
  (login spawns `ut` stamped, inheriting the creds pipe).

## 6. Status

| Item | State |
|---|---|
| Read-parse-eval loop on fd 0 | DONE (U-6g) |
| Line editor + parser + evaluator wired | DONE |
| Ctrl-D / `exit` / EOF exit; interactive error survival | DONE |
| Default prompt (cwd + glyph) | DONE |
| Background `&` + the job table + prompt-cycle `[N]+ Done` reaping (`reap_jobs`, §3.6) | DONE (U-7a) |
| `jobs`/`fg`/`bg`/`wait`/`kill` builtins | DONE (U-7b) |
| `on note`/`mask note` runtime delivery (`deliver_notes`/`open_notes`, §3.7) | DONE (U-7c-a) |
| Ctrl-C foreground forwarding (interruptible foreground wait) | DONE (U-7c-b) |
| Multi-fd job-control `poll()` over a pollable cons | U-PTY |
| Ctrl-Z (stop) + termios | U-PTY |
| Pollable cons + termios (raw/ECHO) + per-Proc fd 0/1/2 | U-PTY |
| `prompt`-shell-function capture | later U-* |

## 7. Known caveats / footguns

- **No echo/raw mode pre-PTY.** Whatever the terminal does in cooked mode is
  what the user sees plus the editor's `render`. Real raw-mode line editing
  (the editor owning echo + cursor) needs U-PTY's termios surface.
- **`poll()` on cons is decorative at v1.0.** Cons has no `.poll` hook, so
  `PollSet` over fd 0 returns ready immediately and the blocking `read` does
  the waiting. The structure is the U-7 seam (add notes fds), not a real
  multi-fd wait yet.
- **Eval output bypasses the `feed` sink.** Builtins/children write to fd 1
  directly; the sink only carries the editor's rendering + `ut:` diagnostics.
  Correct on a real terminal (sink == fd 1); in a test the eval output is not
  captured by the sink (assert on Env state instead).

## 8. Naming rationale

No new thematic name — "REPL" is the expected term for a read-parse-eval
loop and renaming it would obscure intent for any reader. The module is
`libutopia::repl`; the binary remains `ut` (Utopia).

## 9. References

- `docs/UTOPIA-SHELL-DESIGN.md` §10 (main loop + job control), §11 (prompt),
  §486 (interactive implicit-fail), ROADMAP rows U-6/U-7.
- `docs/reference/92-utopia-line-editor.md`, `93-utopia-parser.md`,
  `94-utopia-eval.md`.
- `kernel/cons.c` (the blocking-read `/dev/cons`), `kernel/proc.c` (rfork
  fresh handle table).
