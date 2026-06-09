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
| ~~No interactive regression net~~ `tools/test-interactive.sh` (expect/PTY) logs in + asserts output -- DONE | the LS-1/LS-2 class is now regression-tested from a real keyboard | **LS-CI [done]** |
| ~~No `ls`/`stat`/`clear`~~ adopted from the aux branch (native libthyla-rs) -- DONE | can list a directory (`ls /var`) + inspect a file (`stat`) + clear the screen, AND list the pivot root (`ls /`) -- the #955 readdir-cookie sign-clamp is fixed | **LS-3a [done]** |
| ~~No `mkdir`/`rm`/`cp`/`mv`/`touch`/`tee`~~ authored native on the new libthyla-rs `fs::` mutation API -- DONE | can create/delete/copy/move files by command | **LS-3b [done]** |
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

### LS-2 — See command output (external stdio inherits the console) [DONE @8d3c13b, closes #944]

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

### LS-CI — Interactive E2E harness (the regression net) [DONE @6ea7452, closes #945]

**As-built**: `tools/test-interactive.sh` (the wrapper) + `tools/interactive/lib.exp`
(the reusable `expect` helper library: `lc_boot`/`lc_login`/`lc_send`/`lc_expect`/
`lc_run_expect`/`lc_quit`) + `tools/interactive/ls-ci.exp` (the headline scenario);
`make test-interactive`. Optional gate -- SKIPs (exit 0) without `expect`. Default
`THYLACINE_ACCEL=tcg` (portable compat run); `THYLACINE_ACCEL=hvf` is the fast local
override. The `ls-ci` scenario proves **LS-1** (reaching the shell banner requires
login to have received every keystroke) + **LS-2** three ways: `echo` stdout
(`exec_external`), `echo | tr a-z A-Z` upper-cased stdout (`spawn_pipeline_elements`),
and `cat /missing` -> `cat:` stderr.

Three hard-won portability facts are encoded in the harness (and matter for every
future LS-CI scenario):
- **Run `expect` under `script(1)`** -- macOS expect 5.45 corrupts its own std
  channels inside `spawn` when its stdout is not a tty (a `>file` redirect OR a
  pipe), aborting with "Tcl_RegisterChannel: duplicate channel names" (SIGABRT) or
  breaking `puts` with "bad file number". `script -q <file> <cmd>` gives a PTY,
  captures the transcript, and propagates the exit code.
- **`global spawn_id`** in any proc that `spawn`s -- else the spawn is proc-local and
  later procs' expect/send see a spurious immediate EOF.
- **Match command OUTPUT, never typed input** -- the `ut` line editor redraws the
  prompt per keystroke via cursor positioning and does NOT emit the typed line as
  plain bytes, so only the command's output (clean text on its own line) is matchable.

The kernel is proven stable at idle (a no-input boot survives indefinitely), so an
unexpected qemu exit before a terminal result is a host-timing artifact
(TCG-under-oversubscription); the wrapper retries each scenario up to
`LS_CI_ATTEMPTS` (default 3). A real regression fails every attempt deterministically,
so the retry tolerates flakes without masking a break. Reference: `docs/reference/09-test-harness.md`
"Interactive E2E harness (LS-CI)".

The `expect`/PTY method is the ONLY way to inject real console input -- piped stdin
hits EOF and closes QEMU's `mon:stdio` chardev, which is why CI never exercised the
interactive path and LS-1 shipped silently. Every later LS chunk lands an `ls-ci`-style
scenario here.

### LS-3 — Essential coreutils (adopt from the aux branch)

**Userspace.** The aux track (`aux/userspace-apps` branch, `usr/apps/**`) already
has ~23 coreutils **written + compiling**; adoption = copy each into
`usr/coreutils/src/bin/`, substitute `aux-rt` -> `libthyla-rs` (`env::args` +
`io::{stdin,stdout,stderr}` + the print macros — the exact U-6e-pre-b pattern),
wire `tools/build.sh usr_rs_bins` + the cpio. Their deps already exist
(`fs::read_dir`, `SYS_WALK_CREATE`, `SYS_UNLINK`, `SYS_RENAME`, `SYS_FSTAT`).
Closes most of #925. Split:

- **LS-3a — navigate + inspect [done]**: `ls` (uses `fs::read_dir`), `stat` (uses `t_fstat`), `clear` (ANSI), adopted from `usr/apps/{ls,stat}` (aux branch) onto the libthyla-rs surface; `clear` authored native. Built into the cpio + LS-CI `ls-3a.exp` (`ls /var`->lib, `ls /`->thylacine-version, `stat /thylacine-version`->regular file, `clear`+alive). **#955 fixed** (the `ls /` headline): `ls` was the first consumer to ever readdir the pivot ROOT in a paginating loop, which surfaced a kernel readdir-cookie sign-truncation -- Stratum's Treaddir resume cookies for real dirents exceed INT64_MAX (bit 63 set), but the kernel carried the cookie through the SIGNED `s64 Spoor.offset` and `dev9p_readdir` clamped any "negative" offset to 0, restarting enumeration so the reader re-fetched batch 0 forever. Fixed by treating the cookie as an opaque u64 (no clamp) + a non-advancing-cursor EOD guard. NOT a Stratum bug, NOT an ls defect. `cat`/`head`/`tail`/`wc` already verified against real files (coreutil-smoke).
- **LS-3b — fs-mutation [done]**: `mkdir` (+`-p`), `rmdir`, `rm` (+`-r`/`-f`), `touch`, `cp` (+`-r`), `mv`, `tee` (+`-a`), authored native on a NEW libthyla-rs `fs::` mutation surface -- `fs::create_dir` / `remove_file` / `remove_dir` / `rename` (path-based wrappers over the audited SYS_WALK_CREATE / SYS_UNLINK / SYS_RENAME, via a shared `with_parent_dir` parent-walk). The walk opens intermediates with `T_OPATH` (born R|W since A-3b), which **also fixed a latent `File::create` depth>=2 bug** (the old `T_OREAD` intermediate yielded a RIGHT_READ-only parent that SYS_WALK_CREATE rejects -- pulled forward, the proper-completion dependency). `rm -r` / `cp -r` skip `.`/`..` (fs::read_dir yields them). Verified by a new always-on `fs-mut-smoke` (joey post-pivot, 13 checks incl. depth-3 File::create) + the `ls-3b` LS-CI scenario. **#957 [done] (kernel, audit-bearing):** wiring ls-3b surfaced that a logged-in user could not write to their own `/home/<user>` -- the single-hop `SYS_WALK_OPEN` that `fs::` mutations navigate parent dirs with did NOT cross mounts (only multi-component `stalk`/SYS_OPEN did), so create/rename/unlink hit the SYSTEM-owned placeholder under the per-user home mount, not the mounted user-owned 0700 root. Fixed by making `sys_walk_open_handler` cross mounts at source + result like `stalk` does (the Plan 9 idiom; `cross_mounts` -> public `stalk_cross_mounts`); crossing into `/srv` also required making the devsrv registry root openable + stat-able as a directory (the #932 readdir prerequisite). Focused audit 0 P0/1 P1 (pre-existing devsrv open-return adopt)/0 P2/2 P3, all fixed/tracked; 791/791 + boot + ci-smp-gate 0 corruption. **This unblocks LS-4** (a user's cwd writes land in their own home). The ls-3b multi-step interactive net is PTY-render-flaky under host load (#958, a harness issue -- the write itself passes: ls-3b step (a) /home-write is reliable).
- **LS-3c — misc [done]**: `sleep` (rides `libthyla-rs::time::sleep` ->
  `SYS_TORPOR_WAIT`; magnitude-guarded so a pathological operand can't overflow
  `Duration::from_secs_f64`), `hexdump` (`hexdump -C`-style), `cmp` (byte
  compare), `yes`, `realpath`, `which`, `env` (degenerate, no envp -- G15),
  `uname` (static -- G16). Adopted from the aux branch onto the libthyla-rs
  surface (the LS-3a/3b pattern: `aux-rt` -> `env::args` + `io::` + the print
  macros). Two now-satisfied dependencies were folded in (LS-4 cwd):
  **realpath** anchors a relative input against `env::current_dir()` instead of
  rejecting it; **which**'s explicit-path probe (`fs::exists`) likewise resolves
  relative-to-cwd. Verified by `coreutil-smoke` (13 new checks; 41 total) +
  `ls-3c` LS-CI (`uname -a`, `realpath`, `yes | head`, `echo | hexdump` through
  the real shell). **`yes` is deliberately excluded from `coreutil-smoke`**: it
  is an unbounded producer, and the capture harness holds the pipe read-end open
  (no BrokenPipe), so its write never errors and `wait()` would deadlock -- it is
  covered interactively (`yes | head`, where the reader closes the pipe).

Each verified by `coreutil-smoke` + an LS-CI assertion (`ls /` lists the root).

### LS-4 — Relative paths (the kernel per-Proc cwd) [done; KERNEL + audit-bearing]

**Kernel** (G07; ABI = two syscalls + a resolution-semantics change). The shell
already tracks cwd in `Env` (`cd`/`pwd`/glob/the prompt use it), but externals
receive raw args and the kernel has **no per-Proc cwd**, so a relative path
(`cat foo.txt`, `./config`, *a child program's own internal relative open*)
cannot resolve. **Design decision RESOLVED 2026-06-09 (user-voted "kernel
per-Proc cwd"):** the kernel owns one authoritative cwd per Proc that *every*
program shares — not a shell-side argv-expansion (which can't reliably tell
which argv tokens are paths — `grep foo bar.txt` — and never fixes a child's
*internal* relative open). This is the Plan 9 model (kernel "dot" +
`chdir`/`getwd`), correct-for-all-Procs by construction, and the only option
that doesn't confuse users and developers with a cwd that works for some opens
but not others.

As-built design:
- **`Territory` gains `dot_path`** — a cleaned, absolute cwd path *string*
  (`NULL` == `"/"`; heap-allocated only once `cd`'d away from root; bounded by
  `SYS_OPEN_PATH_MAX`). It lives on the per-Proc `Territory` beside `root_spoor`,
  so a Proc's threads **share** it (POSIX: cwd is per-process) and a child
  **inherits** the parent snapshot via `territory_clone` (independent
  thereafter); freed at `territory_unref` final release.
- **`SYS_CHDIR = 69`**`(path, len)`: form the candidate `clean(path[0]=='/' ?
  path : join(dot_path, path))`, resolve it from `root_spoor` via `stalk`,
  require it is a **directory** (`QTDIR`) the caller has **X** (search) on (the
  open-path `perm_check`), then swap `dot_path` under the territory lock (like
  `pivot_root`) and clunk the verification Spoor. Errors:
  `ENOTDIR`/`ENOENT`/`EACCES`.
- **`SYS_GETCWD = 70`**`(buf, len)`: copy out `dot_path`; `-ERANGE` if too small.
- **Resolution:** for a path-taking syscall (`SYS_OPEN`) with the sentinel
  start-fd and a **relative** path, the handler joins `clean(join(dot_path,
  path))` before `stalk` — exactly POSIX `openat(AT_FDCWD, …)`. Absolute paths
  and an explicit start-fd are unchanged. `..` falls out of the lexical clean.
  **I-28 is PRESERVED with zero new mechanism**: `stalk` is *always* handed an
  absolute-from-`root_spoor` path and clamps `..` at `root_spoor` itself, so a
  maliciously un-cleaned join cannot escape. Native `libthyla-rs` `fs::`
  mutations (which navigate parent dirs single-hop) join via `SYS_GETCWD`; the
  kernel `SYS_OPEN` join covers `File::open` + every ported (musl) program's
  `AT_FDCWD`-relative open.

**Why name-based (a string), not handle-based (a `dot` Spoor), for v1.0:**
holding cwd as a live Spoor is the deeper Plan 9/Linux form (rename-robust; cwd
follows the *directory*, not the name), BUT (a) `stalk` contains `..` at its
`start` trail-floor, so a handle-`dot` start would strand `cd ..` (can't pop
above dot) unless `..` becomes a device parent-walk — a *new mechanism on the
I-28 containment surface*, where bugs are CVEs; and (b) the strongest
correctness argument for handle-based — the symlink/`..` interaction — is
**moot in v1.0** (no `t_symlink`/`t_readlink`; G11/LS-9). So name-based reuses
the fully-audited `stalk`-from-root verbatim, adds no I-28 mechanism, and fully
delivers the requirement. **Handle-based is the v1.x upgrade, landing with
symlinks** (which force it). The only v1.0 compromise: renaming an ancestor of a
live cwd makes the `dot_path` stale (the Proc re-`cd`s) — rare, and not the
confusion this chunk fixes.

Tests: kernel unit (`chdir`/`getcwd` round-trip; relative `SYS_OPEN` resolves
against dot; `..` clamps at root; X-search enforced; `ENOTDIR`/`EACCES`) + LS-CI
(`cd /home/michael; echo hi > f; cat f` round-trips with relative `f`).

### LS-5 — Interactive Ctrl-C: `interrupt` as a real note [KERNEL + audit-bearing]

**Ground-truth correction (2026-06-09).** The original premise — "the machinery
exists; LS-5 = verify + idle-cancel" — was FALSE. A live probe (log in, run
`yes`, hit Ctrl-C) flooded 1.9M lines unfazed. Three independent gaps, all
confirmed in code; foreground Ctrl-C is **fully dead** today:

1. **No console owner in the session.** `proc_console_post_interrupt` posts the
   `interrupt` note to `g_console_owner`, set only by joey at bringup + the SAK
   re-grant to corvus. joey *relinquishes* it at the bringup->session boundary;
   login spawns `ut` WITHOUT re-taking it -> `g_console_owner == NULL` all session
   -> every Ctrl-C is dropped at a NULL owner.
2. **An uncaught `interrupt` doesn't terminate.** `notes_deliver_at_el0_return`
   (notes.c) LEAVES an uncaught non-`kill` note QUEUED (Thylacine is NOT Plan 9's
   "uncaught note kills"). So even forwarded, a dumb coreutil just queues it.
3. **Blocked children aren't note-interruptible.** A child in `SYS_TORPOR_WAIT`
   (`sleep`) or a blocking read wakes only on death (`*_INTR`, #811) — not a
   regular note; notes deliver only at the EL0-return tail. So `sleep 50` ignores
   Ctrl-C until it finishes (the old `sleep 50` example was wrong for v1.0).

**The proper systemic fix — make `interrupt` a real (catchable) note**, not the
hard-kill workaround. The notes subsystem IS the Plan 9 note mechanism; LS-5 makes
`interrupt` behave like a real Plan 9 note / Unix SIGINT, in three properties (the
canonical statement is ARCH §8.8.2):

- **P1 — delivery.** The session shell owns the console (`g_console_owner = ut`)
  via a new **`SPAWN_PERM_CONSOLE_OWNER`** spawn perm, conferred by trusted login.
  This is console-*owner* ("who receives Ctrl-C"), strictly DISTINCT from
  console-*attach* (I-27, the SAK/elevation gate) — a per-bit separation like
  `SPAWN_PERM_MAY_POST_SERVICE`; the owner bit NEVER confers attach, so I-27 is
  untouched. The shell forwards `interrupt` to its foreground child (the existing
  U-7c-b `wait_pids_interruptible` notes-fd poll). Real process groups +
  controlling terminal are Phase-8 job control; shell-forwards is the v1.0 interim.
- **P2 — default disposition: uncaught `interrupt` terminates.** Generalize the
  `snare:*` default-terminate to `interrupt`: an `interrupt` with NO registered
  handler, NOT masked, on a NON-self-managing Proc default-terminates the Proc at
  the EL0-return tail (SIGINT's "default = die, catchable"). The **self-managing
  gate** is the discriminator the fd-read note model needs: a Proc that has opened
  its notes fd (`devnotes`) has declared it consumes its own notes -> exempt (the
  shell qualifies via `open_notes`; a dumb coreutil never opens it -> dies). A
  program catches Ctrl-C by registering an `on note interrupt` handler or masking.
- **P3-terminate — a terminate-disposition `interrupt` wakes a blocked child,
  reusing #811.** §8.8.1's wake machinery (per-Thread `wait_lock` +
  `rendez_blocked_on`, every rendez site, unwind at the EL0-return tail) is widened
  from "group-exit death" to "death OR a pending terminate-disposition
  `interrupt`." A single-threaded blocked child with no handler / no notes-fd /
  unmasked cannot change its disposition while blocked (it is not running to call
  `notify()`), so the disposition is decided at post time and it takes the #811
  death path: wake + terminate. One more trigger on an already-audited mechanism —
  NOT a parallel kill path, and not the uncatchable-`kill` collapse.

Composed (P1 + P2 + P3-terminate): **Ctrl-C terminates any foreground command —
CPU-bound, output-bound, OR blocked in sleep/read — catchably.** The complete,
correct v1.0 model.

**Why NOT the hard-kill shortcut (forward `kill`):** it "works" but makes Ctrl-C
permanently uncatchable, collapses the deliberate `interrupt`(catchable) /
`kill`(N-4 non-catchable) distinction, and leaves P2/P3 unfixed — so "uncaught
notes silently queue forever" stays a system-wide footgun for every future note
consumer. The proper fix removes that footgun.

**The LS-5 / LS-8 boundary (corrected).** Deferred to **LS-8** (U-PTY: pollable
cons + termios ISIG): **P3-deliver** — promptly delivering a *caught* (handler-
bearing / self-managing) `interrupt` to a *blocked* program via an interrupted-
syscall (`-EINTR`) return WITHOUT terminating it. Its sole v1.0 consumer is
**idle-prompt-cancel** (the shell's own blocked cons-read returning so the line
editor's `Cancel` fires) + the reactive *mid-edit* Ctrl-C — both inherently LS-8
(the editor never sees a `0x03` byte today; the kernel cooks it to the note). So
idle-cancel moves wholly to LS-8; LS-5 delivers the foreground-command interrupt.

**Sub-chunks (DFS-inserted at this roadmap point; build all, then emerge):**
- **LS-5a — P1 console owner. [done]** `SPAWN_PERM_CONSOLE_OWNER` (kernel: a new
  spawn-perm bit + `g_console_owner = child` at spawn, gated like
  `MAY_POST_SERVICE`); login spawns `ut` with it; joey's relinquish unchanged.
  Owner-set wiring + the grant gate are kernel-unit-tested (795/795); the
  interactive "Ctrl-C reaches the shell" proof rides the `ls-5.exp` LS-CI at
  LS-5-audit (needs P2/P3 for a visible effect).
- **LS-5b — P2 default disposition.** `interrupt` default-terminate at the
  EL0-return tail + the self-managing-fd gate; the shell exempt. Notes-delivery
  change (I-19).
- **LS-5c — P3-terminate.** Widen the #811 wake predicate to
  death-or-terminate-interrupt; the blocked single-thread child wakes + dies.
  Re-validate every #811 site (I-9 generalized).
- **LS-5-audit** — one focused adversarial round over the whole surface (the
  death-path + notes lineage — #788/#806/#807/#808/#860/#809/#811/#926, the most
  bug-prone in the tree), then the LS-CI `ls-5` scenario (`yes` + Ctrl-C ->
  prompt; `sleep 30` + Ctrl-C -> prompt; an `on note interrupt` catch survives).

Scripture: this section + ARCH §8.8.2 (the model + invariants) + ARCH §25.4 (the
audit row) + CLAUDE.md (mirror). Spec-reasoned in prose per the 2026-05-23
suspension; the rigor is the focused audit + the #811 per-site re-validation +
LS-CI.

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

> **Scheduled revisit at arc close: Imperium** (`docs/IMPERIUM-DESIGN.md`,
> accepted 2026-06-08). Once LS-test closes the arc and the shell is *usable*,
> the next capstone is **imperium** -- the fork-propagating legate scope +
> trusted-path `lex curiata` that turns the A-4 clearance substrate into a
> `sudo -s`-style power-user mode (an elevated shell *safer* than a root shell:
> abdication de-escalates the whole subtree atomically). It is a post-LS
> identity-arc + Utopia capstone (scripture fold-in + spec-first kernel lift +
> corvus per-level keys + shell builtins + a hard audit), NOT LS-arc work.

### LS-9 — The long tail [v1.x]

`ln`/`readlink` (no `t_link`/`t_symlink`/`t_readlink`; G11), `export`/`env`-real
(no envp at spawn; G15), handle-based cwd (a rename-robust `dot` Spoor; LS-4
ships the name-based form, and the handle-based upgrade lands *with* symlinks/G11
— they force it), `find -exec`, atime/mtime setter (G12), `uname`-real sysinfo
(G16). Each a named kernel/lib surface; mostly v1.x.

---

## Sequencing + the MVP line

```
LS-1 [done] ─► LS-2 [done] ─► LS-CI [done] ─► LS-3a [done] ─► LS-4 [done] ─► LS-5    ◄── MVP: usable shell
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
- **LS-4**: RESOLVED 2026-06-09 — kernel per-Proc cwd (name-based `dot_path` for v1.0; `SYS_CHDIR`=69 / `SYS_GETCWD`=70; handle-based `dot` Spoor is the v1.x upgrade, landing with symlinks). Chosen over shell-side argv-expansion (can't tell which args are paths; never fixes a child's internal open).
- **LS-8**: the IRQ-safe poll-waiter wake design + the `specs/pty.tla` model (spec-first re-enable) + the consctl-vs-ioctl termios surface.

## References

- LS-1 root-cause + fix: commit `6e533d6`; `arch/arm64/uart.c` (`uart_rx_init`, `uart_rx_path_enabled`), `usr/login/src/main.rs` (`read_line`).
- Interactive-terminal gap survey + U-PTY scope: `docs/ARCHITECTURE.md` 23.5, `docs/UTOPIA-SHELL-DESIGN.md` 10.1-10.8 + 13 (editor), `specs` gate-9 (`pty.tla`).
- Coreutils gap: task #925; the aux `usr/apps/**` ready set.
- The external-stdio flip seam: `usr/utopia/libutopia/src/eval/stmt.rs:71-81`, 476-532.
