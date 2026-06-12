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
- **LS-5b — P2 default disposition. [done]** `interrupt` default-terminate at
  the EL0-return tail + the self-managing-fd gate. New `PROC_FLAG_SELF_MANAGING_-
  NOTES` (proc_flags bit 6) set by `SYS_NOTE_OPEN` (the shell qualifies); the
  EL0-return dispatcher's `handler_va == 0` arm now calls a pure decision
  `notes_interrupt_should_terminate_locked` (no handler + not self-managing +
  a deliverable [queued AND unmasked] `interrupt` present) and, on true, drops
  the queue lock and `exits(NOTE_NAME_INTERRUPT)` -- the same primitive the
  `kill` branch uses, which already routes single-thread -> ZOMBIE and multi-
  thread -> the #811 `proc_group_terminate` cascade. A self-managing Proc, a
  handler-bearing Proc, or a masked interrupt is exempt; only `interrupt` newly
  default-terminates (child_exit / pipe stay queued; `kill`/N-4 unchanged).
  I-19 extension. Kernel-unit-tested (`notes.interrupt_terminate_gate` truth
  table + `notes.self_managing_flag`); **797/797** (HVF + TCG), u-7-test +
  u-job-test all OK, boot OK, 0 EXTINCTION. **Semantic ripple (intended):** a
  forwarded Ctrl-C to a dumb foreground coreutil (e.g. `echo`) now TERMINATES
  it (status 1) instead of being a no-op -- the two U-7 arc probes that pinned
  the forwarded child's status to 0 were relaxed to "reaped (no hang)" (their
  stated intent); the forward-not-handle assertion is untouched.
- **LS-5c — P3-terminate. [done]** The #811 wake predicate widened to
  death-or-terminate-interrupt; a blocked child wakes + dies. As-built:
  `PROC_FLAG_INTR_TERMINATE_PENDING` (proc_flags bit 7) is the LOCK-FREE
  wake-hint — set by `notes_post`'s interrupt arm (no handler + not
  self-managing + never kproc, under q->lock), cleared under the same q->lock
  by handler registration (`notes_set_handler`, which also closes the
  notify-vs-post arm race), the self-managing mark
  (`notes_mark_self_managing`), and draining the last queued interrupt (both
  dequeue helpers). `thread_die_pending` (notes.c) = `group_exit_msg` set OR
  latch-armed-and-unmasked-for-this-thread; it replaces every direct
  group_exit_msg load in the sleep paths — sleep/tsleep x4
  register-then-observe (sched.c), torpor's post-register check, and the 9P
  client's `client_self_dying` (a Ctrl-C'd coreutil blocked in a 9P RPC takes
  the audited #845 Tflush unwind). The WAKE (`proc_interrupt_terminate_wake`,
  internally latch-gated, caller holds g_proc_table_lock) is the #811 death
  walk verbatim minus `torpor_wake_all_for_proc` (torpor waiters are
  reachable via `rendez_blocked_on`; tsleep's widened check closes the
  register-after-walk race) and minus `smp_resched_others` (the IRQ-from-EL0
  tail evaluates only group_exit_msg, so the IPI buys nothing — the
  never-syscalling-spinner gap is tracked, #964); wired at all four
  interrupt-post sites (console post, SAK, `postnote_walk_cb`, the
  SYS_POSTNOTE self-post, which now takes the table lock for the walk). The
  EL0-return tail stays the TRUTH (the LS-5b predicate re-runs against the
  live queue); a stale-positive latch costs one spurious `*_INTR` unwind,
  never a wrong termination; a masked thread is woken but re-sleeps (masking
  defers; death still overrides the mask). Kernel-unit-tested (+6:
  `rendez.intr_terminate_{interrupts_sleep,register_observe,
  masked_sleeps_through,interrupts_tsleep}` — the first drives the REAL waker
  under the deterministic harness, possible because it has no IPI — +
  `notes.intr_latch_lifecycle` + `notes.die_pending_predicate`); **803/803**
  (HVF + TCG), 0 EXTINCTION, login E2E OK. Runtime regression: u-7-test
  flow 5 (`sleep 3600 &` + `kill <pid>` + `wait` reaps promptly) — proven
  NON-VACUOUS by stash + rebuild on the base kernel, where it hangs the boot
  check (Error 1) exactly as pre-LS-5c semantics predict.
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

### LS-K — Kernel introspection syscalls (id / whoami / date) [done; KERNEL + audit-light; scripture ARCH §22.6]

**LANDED** (scripture `9dcd9d0` -> LS-K-a `a7ca2dc` kernel -> LS-K-b `3cc7cea`
userspace -> audit close). 866/866 kernel tests (+5 `test_clock.c`) +
coreutil-smoke 46 (+4) + the `ls-k` LS-CI (id/whoami/date over a real PTY) +
the SMP gate (0 corruption). Fable focused audit **0 P0 / 0 P1 / 1 P2 / 3 P3**,
all fixed (F1 the `clock_gettime` `ts_va` alignment guard -- benign at SCTLR.A=0,
latent extinction if set; F2 the RTC plausibility ceiling -- a floating
`0xFFFFFFFF` = 2106 defeated the low-only floor; F3 the rtc.h fail-soft-scope
doc; F4 the public `timer_wallclock_offset_ns` underflow guard). Closed list
`audit_lsk_closed_list.md`. Owed: a non-vacuous EL0 odd-`ts_va` regression for
F1 (the in-kernel harness has no mapped-unaligned user VA + SCTLR.A=0).

**Kernel.** Four tiny read-only syscalls that unblock useful tools. Closes
G13/G14. **Design RESOLVED 2026-06-12** (research-collapsed to the Plan 9 +
POSIX idiom — no fork; canonical statement ARCH §22.6):

- **`SYS_GETPID = 72` / `SYS_GETUID = 73` / `SYS_GETGID = 74`** — return the
  calling Proc's `pid` / `principal_id` / `primary_gid` (all durable Proc fields
  since A-1a). No args, no memory write, no capability; the value is the return.
- **`SYS_CLOCK_GETTIME = 75`**`(clk_id, timespec_va)` — fill a `struct t_timespec
  { i64 tv_sec; i64 tv_nsec; }` (16 B, `_Static_assert`-pinned, the musl/arm64
  layout) for `T_CLOCK_MONOTONIC = 1` (nanoseconds since boot, from
  `timer_now_ns()` / `CNTVCT_EL0` — already the tsleep timebase) or
  `T_CLOCK_REALTIME = 0` (nanoseconds since the Unix epoch). `-EINVAL` on a bad
  `clk_id`; `-EFAULT` on a bad `timespec_va`. The `T_CLOCK_*` ids match Linux
  `clockid_t` so a future pouch boundary-line maps `clock_gettime` 1:1.

**The wall-clock base (the one genuinely-new mechanism).** MONOTONIC is free
(the kernel already has `CNTVCT`). REALTIME needs a real epoch. The kernel reads
the **PL031 RTC once at boot** (`arch/arm64/rtc.c`: `dtb_get_compat_reg("arm,pl031",
…)` + the QEMU-`virt` `0x09010000` fallback, the PL011 pattern; the 32-bit
`RTCDR` = host Unix seconds), snapshots the monotonic counter at the same instant
(`timer_set_wallclock_anchor`), and computes `realtime(now) = epoch_anchor_ns +
(mono_now − mono_anchor)`. The slow RTC is touched exactly once; the fast counter
gives the delta + sub-second resolution. **Fail-soft**: no PL031 / implausible
read -> `epoch_anchor = 0` -> REALTIME reads `1970 + uptime` (the honest "no
wall-clock" signal), never an extinction. The PL031 MMIO slot is reserved (I-5);
the device derives from the DTB (I-15). **No new §28 invariant** (read-only;
covered by I-15 + I-5).

**Userspace.** `usr/lib/libthyla-rs`: `time::now()` / `Instant` / `SystemTime`
(realizing the pre-documented `time.rs` v1.x plan) + the `clock_gettime` wrapper;
the `getpid`/`getuid`/`getgid` wrappers. The three native coreutils `id` / `whoami`
/ `date` (authored native — the aux branch has none, since `date` needs the
not-yet-existing clock surface). **v1.0 limitation**: `whoami` / `id` render the
*numeric* `principal_id` / `primary_gid` — uid->name resolution (a corvus
`NAME_LOOKUP` verb or a kernel `principal_name` stamped at `CAP_SET_IDENTITY`) and
`getgroups` (the supplementary-group set) are recorded **v1.x seams** (ARCH §22.6),
not LS-K scope.

**Split**: LS-K scripture (ARCH §22.6 + this section + the §25.4 audit row +
CLAUDE.md mirror; no code) -> **LS-K-a** (kernel: `rtc.c` + the anchor + the 4
syscalls + the I-5 reservation + kernel unit tests + the `11-timer.md` wall-clock
section; audit-bearing — new MMIO/I-15/I-5 + new ABI) -> **LS-K-b** (userspace:
the lib wrappers + `id`/`whoami`/`date` + build wiring + the `ls-k` LS-CI) -> one
focused audit over the kernel surface + the SMP gate. Tests: kernel unit (the RTC
anchor; MONOTONIC strictly advances; REALTIME > a 2020 floor when the RTC is
present; `clock_gettime` `EINVAL`/`EFAULT`; `getpid`/`getuid`/`getgid` return the
Proc fields) + the `ls-k` LS-CI (`id` -> `uid=…`, `whoami` -> the principal,
`date` -> a plausible wall-clock line).

### LS-8 — U-PTY: the line-discipline substrate [KERNEL + audit-bearing]

**The depth chunk.** Everything async/raw/secure converges here. **Design RESOLVED 2026-06-12** (research-collapsed; one user vote on the termios granularity; canonical statement ARCH §23.5.1). **Spec-first re-enabled for this surface** — but the LS-8 spec is **`specs/cons_poll.tla`** (the LS-8a deferred poll-wake, filed under **I-9**), NOT `pty.tla`: ARCH §28 I-20 / §23.5 reserve `pty.tla` for the Phase-8 PTY *master/slave* atomicity, which LS-8 excludes. The naming reconciles a scripture inconsistency (this section formerly said "write pty.tla FIRST"; the load-bearing LS-8 invariant is I-9's deferred wake, not I-20's master/slave). `cons_poll.tla` is TLC-green (clean + liveness + the `BUGGY_MGR_LOST_WAKE` counterexample); written FIRST (done). Splittable:

- **LS-8a — pollable cons (the deferred poll-wake).** A `.poll` hook on `/dev/cons`. The blocker was IRQ-safety: `poll_waiter_list_wake` is not IRQ-safe (a plain non-irqsave lock + a nested `wakeup`); only `wakeup()` on a `Rendez` is. **Resolved: defer to the `console_mgr` kthread** (Linux's tty `flush_to_ldisc` model — NOT lock-widening `poll_waiter_list_wake`, which would widen IRQ-off windows across every pipe/notes producer for a console-only need). The RX IRQ sets `poll_wake_pending` under `g_cons.lock` + wakes `g_cons_mgr_rendez`; `console_mgr` drains it + walks the hook list in process context. Audit-bearing (the I-9 deferred wake; `cons_poll.tla` is the gate). Unblocks async multi-fd poll.
- **LS-8b — termios / consctl (B, fine-grained).** The kernel-side line discipline (cooking in the cons layer — kernel-owned for the I-27 trusted path, NOT a userspace `consd`), toggled by stty-style `/dev/consctl` writes — **not a cons ioctl** (Thylacine has no ioctl; Plan 9 lineage + the modern capability-microkernel SOTA both use a control channel, not ioctl). **Five independent flags** (granularity B, user-voted 2026-06-12 over the Plan 9 coarse `rawon/rawoff`): `ICANON`, `ECHO` (the enforced password mask, a hard kernel guarantee — supersedes LS-6's interim), `ISIG` (Ctrl-C → the `interrupt` note vs a `0x03` byte), `ICRNL`, `ONLCR`. Independent bits make cbreak representable + the Phase-8 Pouch `tcsetattr` mapping 1:1. Accepted cost: icrnl/onlcr-independence + cbreak have no native v1.0 consumer (unit-tested; driven when Pouch lands). Audit-bearing (the termios state machine; prose + unit-tested per the suspension). **As-built (kernel mechanism landed):** the five-flag cooking in `cons_rx_input` + `/dev/consctl` parse/render + the ECHO-off hard guarantee + the `CONS_ISIG`-only boot default (breaks nothing), 9 unit tests. **The login-echo consumer (the LS-6 fold-in) is owed pending a console-mode access decision** — login is *not* console-attached, so it cannot open the I-27-gated `/dev/consctl`; resolve via either relaxing the consctl I/O re-gate (`CWALKONLY`/#81 already closes the O_PATH bypass, so open-gate + an inherited consctl fd suffices) or a capability-keyed `SYS_CONSOLE_MODE(fd)` (deviates from the consctl-file decision). Mechanism complete + tested independent of this; `ut` (LS-8c) drives cooked mode as the console *owner* without the gate question.
- **LS-8c — the shell multi-fd poll loop [done].** `ut` polls cons + the notes fd simultaneously -> async `[N]+ Done` while idle + reactive Ctrl-C mid-edit. Userspace, rides LS-8a. **As-built**: the `ut` binary's loop (`usr/utopia/shell/src/main.rs`) adds the shell's own note fd to the `PollSet` beside cons (fd 0), dispatches on WHICH fd is ready, and on a note-fd wake calls the new `Repl::on_notes_ready` (`usr/utopia/libutopia/src/repl.rs`) -- which reaps finished bg jobs (`[N]+ Done`), drains the queue (firing `on note` handlers), and on an UNHANDLED `interrupt` resets the in-progress edit (the note-path analogue of the editor's 0x03 Cancel; `deliver_pending_notes` now returns that signal). The foreground-command interrupt forwarding stays in `wait_pids_interruptible` (the shell is not in the idle loop while a child runs). No mode change is involved: the boot default (ISIG-only) already cooks Ctrl-C to the `interrupt` note, which the multi-fd poll services. Tests: `u-job-test` blocks 20-22 (the `deliver_pending_notes` bool + `on_notes_ready` reactive-cancel / preserve, over the real per-Proc note queue) + repl host `#[cfg(test)]` + the `ls-8c` LS-CI (`sleep 2 &` -> `[N]+ Done` appears with NO keystroke). **The raw/cooked dance is NOT in LS-8c -- it folds into #94.** The LS-8b "Consumers" note assumed `ut` could set cooked mode for a foreground child via `/dev/consctl`, but `ut` is the console *OWNER* (LS-5a), NOT console-*attached* (spawn does not confer the attach bit; during a session corvus is the sole console-attached Proc), so it cannot open the I-27-gated `/dev/consctl` -- the SAME gate as the login-echo consumer. So interactive child stdin + line discipline (the raw/cooked dance) is gated on the #94 B-vs-C console-mode-access vote, not separable from it.

Deferred to **Phase 8** (beyond LS): process groups + controlling terminal +
Ctrl-Z (stopped thread state) + SIGWINCH + `/dev/ptmx`+`/dev/pts` -> full job
control + helix/nora/bash via Pouch. (That is I-20 / `pty.tla`.)

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
