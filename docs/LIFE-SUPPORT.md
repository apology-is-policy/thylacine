# LIFE-SUPPORT.md — the Life Support arc

Binding scripture for the **Life Support (LS)** arc: the minimal-yet-complete
set of fixes + features that make Thylacine genuinely usable for **standard,
useful, human-driven interactive workflows** — log in, navigate, list, inspect,
create / edit / delete files, search, pipe, interrupt, run jobs.

This arc was scoped 2026-06-09 after the first hands-on interactive test by the
user surfaced that the kernel UART console **never received keystrokes** (LS-1,
root-caused + fixed). That single bug had hidden a whole class of
never-exercised interactive gaps — the pieces of the textual OS were all
unit-tested and composition-tested, but **never driven by a real keyboard**.
Life Support closes that gap end to end.

> Methodological note (the meta-lesson of LS-1): the interactive console was the
> one surface CI structurally could not exercise (the A-4c harness cannot inject
> a UART keystroke). The fix is not just code — it is **LS-CI**, an expect/PTY
> interactive harness so the human path is regression-tested from here on. Every
> LS chunk lands with an LS-CI interactive assertion.

---

## State of the world (post-LS-1, 2026-06-09)

What a logged-in human gets **today** (tip `6e533d6`):

- **Login works** over a real terminal (`michael` / the seeded passphrase), proven over a PTY.
- **The shell takes input**: line editor, history (in-session), Tab completion, multi-line, Ctrl-R.
- **Builtins show output**: `cd`, `pwd`, `exit`, `true`/`false`, `unset`, `eval`, `source`, `type`, and the U-7 job-control builtins `jobs`/`fg`/`bg`/`wait`/`kill`.
- **Files can be created + read**: `echo hi > f`, `cat > f << EOF`, `>>` append, `cat f`, `<` — the FS-mutation path is landed and works.
- **Pipes + redirection** compose: `a | b`, `<`, `>`, `>>`, heredoc.
- **16 coreutils** exist + run: `echo cat wc head tail true false seq sort uniq tr cut grep basename dirname pwd`.

What is **missing or broken** for real human use (the LS arc):

| Gap | Effect on the human | Chunk |
|---|---|---|
| ~~External command stdout is **dropped** (`Stdio::Piped`-then-drop)~~ stdout/stderr inherit the console (`env.stdio_inherit`) -- DONE | `echo`/`cat`/`grep`/coreutils output is now **visible** | **LS-2 [done]** |
| No interactive regression net | the UARTEN bug shipped silently; no test drives a keyboard | **LS-CI** |
| No `ls`/`stat`/`clear` | can't list a directory or inspect a file | **LS-3a** |
| No `mkdir`/`rm`/`cp`/`mv`/`touch`/`tee` | can't create/delete/copy/move files by command | **LS-3b** |
| Relative paths don't resolve for externals (no per-Proc cwd; G07) | `cat foo.txt` from a cwd fails; only absolute paths work | **LS-4** |
| Interactive Ctrl-C unverified | unknown whether Ctrl-C interrupts a runaway command | **LS-5** |
| Login/prompt: neither username nor password **echoes** (raw cons, no line discipline) | you type blind; password is masked-by-accident, username should echo | **LS-6** |
| No interactive editor | can't edit an existing file (only rewrite via redirection) | **LS-7** |
| No `id`/`whoami`/`date` (no `t_getuid`/`t_getpid`/`t_clock_gettime`; G13/G14) | can't ask "who am I" / "what time is it" | **LS-K** |
| Cons not pollable; no termios | no async "job done" while idle; no reactive Ctrl-C mid-edit; password mask is accidental not enforced; no Ctrl-Z; no real editors (helix) | **LS-8 (U-PTY)** |
| `ln`/`readlink`/`export`/`env`/`find -exec`/full cwd | the long tail | **LS-9 (v1.x)** |

---

## Chunks

Each chunk lands independently with: status-row, tests (incl. an **LS-CI**
interactive assertion where user-visible), and the standard close. Pure-
userspace chunks = `make test-tcg` x2 floor; kernel chunks = SMP gate + focused
audit. The arc is **MVP-first** — LS-2 -> LS-CI -> LS-3a -> LS-4 -> LS-5 makes the
shell genuinely usable; the rest is breadth (LS-3b/c, LS-6, LS-7, LS-K) then
depth (LS-8).

### LS-1 — Console RX restoration [DONE @6e533d6, closes #943]

The foundation. Kernel `uart_rx_init` now sets `CR.UARTEN|RXE` (the UART was
never master-enabled, so QEMU received nothing); login's `read_line` accepts CR
as well as LF (a raw terminal sends CR on Enter). Regression guard:
`uart.rx_path_enabled`. Proven over a PTY. **Without LS-1 nothing below works.**

### LS-2 — See command output (external stdio inherits the console) [DONE @pending, closes #944]

**As-built**: `Env::stdio_inherit` (default false), set by `ut::main` via
`io::stdout_is_live()` (a zero-length `SYS_WRITE` to fd 1 -- the kernel validates
the handle even for len 0; the design fork "has-tty flag vs fd-1 probe" collapsed
to "the entry point probes once, threads a bool" -- the probe lives in `ut::main`,
the flag on `Env`, so fd-less test harnesses default false). When true, the
non-redirected stdout/stderr slots use `Stdio::Inherit` (the `out_stdio` helper) in
`exec_external` / `exec_external_redirected` / `spawn_pipeline_elements`, plus
`capture_external_pipeline`'s stderr. **stdin stays Piped-drop** at LS-2 (interactive
child stdin is LS-5/LS-8). Proven via expect/PTY (`echo` output visible; `cat` error
visible). The original design notes (below) record the fork that was resolved.

**Userspace** (`usr/utopia/libutopia/src/eval/stmt.rs`). Today `exec_external` /
`exec_external_redirected` / `exec_pipeline` set `Stdio::Piped` on the child's
stdout/stderr then **drop** the parent ends (the v1.0 "Piped-then-drop"
convention from U-6c, chosen when fd 1 was never terminal-backed). Now that the
session shell holds the console as fd 0/1/2, flip to **`Stdio::Inherit`** for the
child's stdout/stderr (and stdin) so `echo`/`cat`/coreutils output reaches the
terminal. **Design decision to surface**: the flip must be *conditional* — the
boot-check `ut` (bare-spawned, empty handle table) has no fd 1 and `Inherit`
would error its spawns. Detect a terminal-backed fd 1 (e.g. an `Env`/`Repl`
"has-tty" flag set by `ut` when launched with real std fds, or a probe of fd 1)
and inherit only then; else keep Piped-then-drop. This is **THE** unblocker —
without it the coreutils are mute. Comments at `stmt.rs:71-81` already note
"this whole block flips to `Stdio::Inherit` cleanly." Tests: a probe + LS-CI
(`echo HELLO` shows `HELLO`; `cat /welcome` shows the file).

### LS-CI — Interactive E2E harness (the regression net)

**Host tooling** (`tools/test-interactive.sh` + an `expect` driver;
non-audit-bearing). Boot under TCG, drive a real PTY via `expect`: log in as the
seeded user (`michael`), run a scripted human sequence, assert the rendered
output. The `expect`/PTY method (proven in LS-1) is the ONLY way to inject real
console input — piped stdin hits EOF and closes QEMU's `mon:stdio` chardev. This
is the test that would have caught LS-1. Land it EARLY (right after LS-2) so
every later LS chunk gets an interactive assertion. Wire it as an optional gate
(it needs `expect`; degrade gracefully if absent). Deliver an `expect` library
of helpers (login, send-line, expect-output, logout).

### LS-3 — Essential coreutils (adopt from the aux branch)

**Userspace.** The aux track (`aux/userspace-apps` branch, `usr/apps/**`) already
has ~23 coreutils **written + compiling**; adoption = copy each into
`usr/coreutils/src/bin/`, substitute `aux-rt` -> `libthyla-rs` (`env::args` +
`io::{stdin,stdout,stderr}` + the print macros — the exact U-6e-pre-b pattern),
wire `tools/build.sh usr_rs_bins` + the cpio. Their deps already exist
(`fs::read_dir`, `SYS_WALK_CREATE`, `SYS_UNLINK`, `SYS_RENAME`, `SYS_FSTAT`).
Closes most of #925. Split:

- **LS-3a — navigate + inspect**: `ls` (the headline; uses `fs::read_dir`), `stat` (uses `t_fstat`), `clear` (ANSI). Verify `cat`/`head`/`tail`/`wc` against real files.
- **LS-3b — fs-mutation**: `mkdir`, `rmdir`, `rm`, `touch`, `cp`, `mv`, `tee`.
- **LS-3c — misc**: `sleep`, `hexdump`, `cmp`, `yes`, `realpath` (lexical), `which`, `env` (degenerate, no envp yet), `uname` (static).

Each verified by `coreutil-smoke` + an LS-CI assertion (`ls /` lists the root).

### LS-4 — Relative paths (the cwd)

**Userspace MVP** (G07). The shell tracks cwd in `Env` (`cd` updates it), but
externals receive raw args and the kernel has **no per-Proc cwd**, so a relative
path (`cat foo.txt`, `ls subdir`) cannot resolve. MVP: the shell **resolves
relative path arguments against `Env.cwd`** (expand to absolute) before spawning
externals, and builtins resolve against `Env.cwd`. **Design decision to
surface**: shell-side resolution (userspace, MVP, ~one chunk) vs a kernel
per-Proc cwd + `getcwd`/`chdir` syscalls (deeper, correct-for-all-Procs, kernel,
audit-light). Recommend the userspace MVP first; the kernel cwd is LS-9/v1.x.
Tests: LS-CI (`cd /home/michael; echo hi > f; cat f` round-trips with relative
`f`).

### LS-5 — Interactive Ctrl-C (verify + prompt-cancel)

**Userspace + verification.** The machinery exists: the console owner is the
session shell (set via `SYS_CONSOLE_OWNER` at login), Ctrl-C is cooked to an
`interrupt` note, and U-7c forwards it to the running foreground command. LS-1
unblocked the input. LS-5 = (a) **verify via LS-CI** that Ctrl-C interrupts a
runaway foreground command interactively; (b) ensure Ctrl-C at the idle prompt
**cancels the line + redraws** (the editor's `Cancel` action). The *reactive
mid-line-edit* Ctrl-C (clear the line while still typing) needs the pollable cons
-> **LS-8**; note that boundary. Tests: LS-CI (`sleep 50` then Ctrl-C returns to
the prompt).

### LS-6 — Login + prompt UX (echo username, mask password)

**Userspace** (`usr/login/src/main.rs`). The raw cons echoes nothing, so the user
types **blind** — fine (masked) for the password, wrong for the username. Login
should **echo each username char** as typed (write it back to fd 1) but NOT the
password. A small `read_line(echo: bool)` flag. (Real ECHO control belongs in
termios -> LS-8; this is the v1.0 hand-rolled interim, sufficient + safe.) Plus
any prompt polish. Tests: LS-CI (the username appears as typed; the password does
not).

### LS-7 — A minimal native editor

**Userspace** (~300 LOC, native libthyla-rs). A small editor so a human can edit
an existing file interactively without U-PTY. Wrap `libutopia::line_editor`
(battle-tested: UTF-8, history, multi-line) in a load -> edit-buffer -> save
loop. Line-mode (`ed`-style) OR a simple full-screen editor that works in the
cooked-ish v1.0 console (no raw mode needed). Full editors (helix via Pouch,
`nora` native ratatui-fork) need U-PTY raw mode -> deferred. Name it
thematically. Tests: LS-CI (open a file, edit a line, save, `cat` shows the
edit).

### LS-K — Kernel introspection syscalls (id / whoami / date)

**Kernel** (small, audit-light). Three tiny read-only syscalls that unblock
useful tools: `t_getpid` / `t_getuid` / `t_getgid` (-> `id`, `whoami`; the Proc
already carries `principal_id`/`primary_gid` from A-1a) + `t_clock_gettime` (->
`date`; the kernel already has `CNTVCT`/the virtual timer — expose monotonic +
a wall-clock base). Each = a syscall + a `libthyla-rs` wrapper + the coreutil
(adopt from aux). Closes G13/G14. Tests: kernel unit + the coreutils via LS-CI.

### LS-8 — U-PTY: the line-discipline substrate [KERNEL + audit-bearing]

**The depth chunk.** Everything async/raw/secure converges here. **Spec-first
re-enabled for this surface** (`specs/pty.tla`, the ARCH 25.2 gate-9 module —
not yet written; write it FIRST). Splittable:

- **LS-8a — pollable cons**: a `.poll` hook on `/dev/cons`. The blocker is IRQ-safety — `poll_waiter_list_wake` is not IRQ-safe (only `wakeup()` on a `Rendez` is), so the IRQ handler currently can only wake the data Rendez. Design an IRQ-safe poll-waiter wake (defer to the `console_mgr` kthread, or a poll-on-Rendez bridge). Audit-bearing (poll wait/wake missing-wakeup, `specs/poll.tla` family). Unblocks async multi-fd poll.
- **LS-8b — termios / consctl**: raw vs cooked, `ECHO` (real enforced password masking, supersedes LS-6's interim), `ISIG`, `ICRNL`/`ONLCR`. Via `/dev/consctl` writes or a cons ioctl. Audit-bearing (termios state machine, `specs/pty.tla`).
- **LS-8c — the shell multi-fd poll loop**: `ut` polls cons + the notes fd simultaneously -> async `[N]+ Done` while idle + reactive Ctrl-C mid-edit. Userspace, rides LS-8a.

Deferred to **Phase 8** (beyond LS): process groups + controlling terminal +
Ctrl-Z (stopped thread state) + SIGWINCH + `/dev/ptmx`+`/dev/pts` -> full job
control + helix/nora/bash via Pouch.

### LS-test — Arc-close cumulative interactive workflow probe

The LS analog of `/u-7-test`: a full human-session E2E driven through LS-CI —
log in -> `ls` -> `cd` -> `mkdir` -> `touch`/`cp`/`mv` -> `echo > f` -> `cat`/`grep`
-> pipe -> edit (LS-7) -> Ctrl-C -> `jobs`/`fg` -> logout. Proves the whole arc
composes for a real workflow. Closes the LS arc.

### LS-9 — The long tail [v1.x]

`ln`/`readlink` (no `t_link`/`t_symlink`/`t_readlink`; G11), `export`/`env`-real
(no envp at spawn; G15), kernel per-Proc cwd (full G07), `find -exec`,
atime/mtime setter (G12), `uname`-real sysinfo (G16). Each a named kernel/lib
surface; mostly v1.x.

---

## Sequencing + the MVP line

```
LS-1 [done] ─► LS-2 ─► LS-CI ─► LS-3a ─► LS-4 ─► LS-5    ◄── MVP: usable shell
                                  │
                                  ├─► LS-3b ─► LS-6 ─► LS-3c ─► LS-7 ─► LS-K   ◄── breadth
                                  │
                                  └─► LS-8 (8a ─► 8b ─► 8c) ─► LS-test         ◄── depth + close
                                                                  │
                                                                  └─► LS-9 [v1.x]
```

**MVP (LS-2 + LS-CI + LS-3a + LS-4 + LS-5)**: a human can log in, `ls`, `cd`,
`cat`, `grep`, create/inspect files, pipe, see output, and Ctrl-C a runaway
command. That is "standard useful workflows" at the floor. Everything after is
breadth (more tools, editing, identity) and depth (U-PTY: async, real masking,
editors, eventually Ctrl-Z).

## Design decisions to surface (per chunk, before coding)

- **LS-2**: how to detect a terminal-backed fd 1 for the conditional `Inherit` (Env/Repl has-tty flag vs fd-1 probe).
- **LS-4**: shell-side relative-path resolution (MVP) vs kernel per-Proc cwd (deeper).
- **LS-8**: the IRQ-safe poll-waiter wake design + the `specs/pty.tla` model (spec-first re-enable) + the consctl-vs-ioctl termios surface.

## References

- LS-1 root-cause + fix: commit `6e533d6`; `arch/arm64/uart.c` (`uart_rx_init`, `uart_rx_path_enabled`), `usr/login/src/main.rs` (`read_line`).
- Interactive-terminal gap survey + U-PTY scope: `docs/ARCHITECTURE.md` 23.5, `docs/UTOPIA-SHELL-DESIGN.md` 10.1-10.8 + 13 (editor), `specs` gate-9 (`pty.tla`).
- Coreutils gap: task #925; the aux `usr/apps/**` ready set.
- The external-stdio flip seam: `usr/utopia/libutopia/src/eval/stmt.rs:71-81`, 476-532.
