# Git on Thylacine -- a plan toward self-hosting

Status: **PLAN / design-first, awaiting operator signoff.** No code lands against
this until the milestone ladder + the flagged decisions are ratified.

The goal (operator, 2026-08-27): **complete, practical git under Thylacine** --
not just `git init`, but the whole developer workflow a person actually hits:
clone/fetch/push over https, rebase, merge, diff, log, worktree, the automatic
housekeeping git does when `.git` grows, and `nora` as the editor for commit
messages and diff/merge. The strategic frame: **a working git is a large step
toward Thylacine self-hosting its own development.**

This document is the research + plan. It is grounded in three parallel research
passes (the tree's ground truth; the static-git-with-https build; the full
workflow's OS-integration surface + heritage), each cited inline.

---

## 1. It is proven achievable -- aim high, with a small floor

Thylacine runs unmodified Linux binaries via the VIVARIUM **phenotype** (a
syscall-translation layer; `docs/VIVARIUM.md`). That is the *exact* architecture
of three production systems that already run **unmodified upstream git**:

- **gVisor / Sentry** -- a Linux application-kernel in Go, ~250/294 arm64
  syscalls implemented-or-graceful-ENOSYS; git is a mainstream `runsc` workload.
- **WSL1** -- pico processes, Linux ELF translated to NT; git was a headline use
  case. Its lessons are about *performance* (the `stat`-heavy index-refresh path
  and process-spawn cost), never correctness.
- **illumos LX-branded zones** -- a non-Linux kernel running stock GNU/Linux git
  natively via syscall emulation.

And **9front's `git9`** (Ori Bernstein's native reimplementation) **self-hosts
the entire Plan 9 fork's development** with *no index/staging, no
interactive-rebase todo editor, no difftool/mergetool, no auto-gc* -- porcelain
is small `rc` scripts over a handful of C primitives. That calibrates the
**self-hosting floor**: it sits far below "all of upstream git."

**Two lessons frame the whole plan.** (1) Running *stock* upstream git over a
translation layer is the status quo elsewhere, not a research risk -- so we aim
to run the real thing, not reimplement. (2) The feature set required to
*self-host* is genuinely small; "complete practical git" (the end goal) is a
superset we reach in tiers.

---

## 2. Current state -- ground truth (file:line-anchored)

### 2.1 What works today

The **local core is proven at every boot** by the git-probe gate
(`gitprobe.sh`, `tools/build.sh:1412-1451`), spawned by joey as
`PRINCIPAL_SYSTEM`. The proven chain: `git init` / `add` / `commit` / **`log`** /
clone-`file://` (spawns `git-upload-pack` via a dashed symlink + `sh -c` -- a
real fork+exec+pack transfer) / reflog 2-line **O_APPEND** / second-commit /
worktree-materialize-verify. Markers `GITPROBE-INIT..DONE`.

Supported FS/process primitives (all grounded in `kernel/vivarium.c`):
`O_CREAT` (#50), `O_EXCL` (`SYS_WALK_OPEN_OEXCL` -> Stratum `EEXIST` -- git's
lockfile primitive), `O_APPEND` (delegated to Stratum, positioned at EOF),
`rename`/`renameat2` (git's atomic install-by-rename), `mkdir`/`unlink`/
`getdents64`/`fsync`/`chdir`/`getcwd`/`faccessat`/`readlinkat`/`fchmodat`;
child spawn (`clone`->`rfork_core`, `execve`->`execve_core`, `wait4`; fds 0/1/2
inherited across both, `vivarium.c:517-519`); `getrandom` (keeps its
`CAP_CSPRNG_READ` gate under I-43).

Already in place for https: `/etc/ssl/certs/ca-certificates.crt` (Mozilla roots,
Mozilla CA bundle) baked into the pool (`tools/build.sh:3075-3086`); netd +
slirp networking; `/etc/resolv.conf` + `/etc/hosts` in the net-granted bundle;
TLS proven in-guest by the curl demo.

### 2.2 The gaps, by kind

**(a) https is blocked at THREE independent layers -- a build problem, not a
syscall bug.**

1. `NO_CURL` -- git speaks http(s) only through the external helper
   `git-remote-https`, built only when libcurl is linked. `NO_CURL` sets
   `EXCLUDED_PROGRAMS += git-http-fetch git-http-push` and empties
   `REMOTE_CURL_*`, so the helper does not exist -> "unable to find remote helper
   for 'https'."
2. `NO_OPENSSL` -- no TLS in git itself. (This turns out *not* to block https:
   git's https is via libcurl's own TLS backend, so `NO_OPENSSL` can stay -- see
   3.1.)
3. **OpenSSL armcap SIGILL** -- OpenSSL's aarch64 init SIGILL-probes CPU
   features under a handler it expects to catch; Thylacine's `SIGILL -> snare:ill`
   is terminal/non-deliverable (`vivarium.c:1385`), so the probe is fatal.
   Worked around by `OPENSSL_armcap=0` in the env (STILL-OPEN as a general gap).

**(b) No terminal control -> no interactive git.** The phenotype translates no
`ioctl`/termios/`setsid`/`setpgid`/`TIOCGWINSZ` (all FORWARD->ENOSYS;
`vivarium.h:21` "ioctl forwards"). Consequences: `isatty()` fails -> git uses
non-interactive defaults (no editor, no pager, no color); a spawned editor/pager
cannot enter raw mode; no controlling terminal (`SYS_TTY_ACQUIRE` is never
reached from the phenotype). **This is the gate on every interactive feature**
(editor, `rebase -i`, `add -p`, pager) -- a kernel chunk, not a config line.

**(c) FS gaps, all graceful (none fatal).** No `linkat`/`link` (loose-object
finalize falls back to `rename`; `core.createObject=rename` already set,
`build.sh:1364`; `clone --local` falls back to copy). No `symlinkat`/`symlink`
(checkout of a symlink tree writes the target as a plain file under
`core.symlinks=false`; **worktrees use pointer FILES, not symlinks**, so they are
unaffected). No `flock` (git uses `O_EXCL` lockfiles; only `gc.pid` touches
flock). No `ftruncate`/`utimensat` (pack/midx edge; racy-git falls back to
content compare).

**(d) `exit(N)` is boolean (#91).** A nonzero container exit collapses to 1
(`sys_exit_group_handler -> exits("fail")`). A shell reading `$?` sees only 0/1,
which breaks git wrappers/hooks and the shell-script tool paths
(mergetool/difftool) that branch on specific codes. A real correctness fix.

**(e) The A-3 wall.** The pool 9P mount is `PRINCIPAL_SYSTEM`-owned, so git only
works run **as SYSTEM** today; a real uid-1000 user is denied the chmod on its
own lockfiles and `git init` dies. Per-principal 9P ownership (A-3) is unbuilt at
v1.0. This bounds "a *user* runs git," not "git works." Tracked separately.

### 2.3 Curl-demo wall status (git-remote-https drives libcurl the same way)

FIXED: `getsockopt(SO_ERROR)` (`vivarium.c:1597`), `send`/`recv`
(`vivarium.c:1609`), `getrandom` (`vivarium.c:446`). STILL-OPEN: `eventfd2` (no
`VIV_LINUX_EVENTFD`; curl>=8.20 fatal on it -> pin a tolerant libcurl);
`pthread_create`/`CLONE_THREAD` (refused by design, `vivarium.c:1262` admits only
fork+vfork shapes).

---

## 3. The milestone ladder

**B (network transport) -> C1 (non-interactive complete workflow = the
self-hosting floor) -> C2 (interactive tier).** Self-hosting is achieved at
**B + C1**; C2 is the "complete practical" polish. A-3 (user-git) is a parallel
dependency, separately tracked.

### Milestone B -- network git (clone / fetch / push over https)

> **STATUS 2026-08-27 (aux): clone + fetch over HTTPS ACHIEVED under the phenotype**
> (`joey: git-https gate PASS`; commits `70e19dd8` dup + the gate infra). The B3 gate
> proved the full transport: external TLS-over-netd (the first on Thylacine) + HTTP +
> smart-http + packfile + checkout + `--unshallow` fetch. Three items REMAIN, surfaced
> as decisions: **(a)** git's DEFAULT protocol **v2** aborts silently under the phenotype
> (reads the capability advertisement, writes nothing back -- a phenotype bug); forced
> **`protocol.version=0`** (fully functional) in both gitconfigs; v2 root-cause tracked
> (getsockname-ENOSYS during v2 connection-reuse is the leading hypothesis). **(b)**
> **DNS-by-name** needs the phenotype's unconnected-UDP + non-blocking path (= **net-4d**,
> general -- every networked Linux binary needs it); the gate IP-pins `/etc/hosts` fresh.
> **(c)** **push** over https needs a writable/authenticated remote (deferred). The dup(2)
> translation was the one kernel change; a phenotype-networking arc (v2 + net-4d) would
> complete "network git" fully.

A **build + staging** problem, precisely scoped by the research. Built on
thyla-pi (the existing aarch64-musl cross-builder that produced the current git).

**B1 -- build the toolchain, bottom-up, all static aarch64-linux-musl:**
1. A **TLS backend** (one only -- backends do not co-link). **OpenSSL** is the
   low-risk default (proven with musl, best cert compatibility, reuses the
   toolchain that already builds our static curl). **mbedTLS** is the size winner
   (~0.3-0.6 MB vs OpenSSL's ~3-5 MB) with no functional loss for https
   clone/fetch/push. *Decision D1 below.*
2. A **minimal static libcurl** -- http/https only; **`--disable-threaded-resolver`**
   (CRITICAL: libcurl's default DNS resolver spawns one `pthread_create` per
   lookup -- a hidden thread layer *separate* from git's, which would hit the
   CLONE_THREAD refusal even with a NO_PTHREADS git). Drop
   nghttp2/ngtcp2/idn2/psl/ssh2/brotli/zstd; `--with-ca-bundle=/etc/ssl/certs/ca-certificates.crt`.
3. **git 2.51.2**: **drop `NO_CURL`**; **keep** `NO_OPENSSL` (https is via
   libcurl, decoupled from git's own openssl), `NO_PTHREADS` (see 5.1), `NO_EXPAT`
   (drops the obsolete dumb-DAV `git-http-push`), `NO_GETTEXT`, `NO_ICONV`,
   `NO_REGEX=NeedsStartEnd`; add `CURLDIR` + **`CURL_LDFLAGS=$(curl-config
   --static-libs)`** (the #1 static-git build gotcha -- the default `--libs` omits
   the transitive `-lssl -lcrypto -lz` and the static link fails); `prefix=/usr`
   (so the compiled exec-path is `/usr/libexec/git-core`) or
   `RUNTIME_PREFIX=YesPlease` (relocatable).
   - Emits into `libexec/git-core/`: `git-remote-http` (the one real libcurl ELF)
     + `git-remote-https` (a link to it; reads `argv[0]` for the protocol). That
     pair is the entire minimum for `clone https`.

**B2 -- stage it (the concrete miss).** Our current staging
(`tools/build.sh:1500-1518`) copies only the `git` binary. It must ALSO unpack
`libexec/git-core/git-remote-http` + the `git-remote-https` link -- git finds
them via its exec-path (`GIT_EXEC_PATH` env / compiled `gitexecdir` /
`RUNTIME_PREFIX`), and `git-remote-https` is NOT a builtin (unlike the
upload/receive-pack symlinks that work today). Wire `/etc/gitconfig`
`[http] sslCAInfo=/etc/ssl/certs/ca-certificates.crt` + `OPENSSL_armcap=0` in
git's spawn env (if OpenSSL backend). Ship the templates dir (cheap; nice, not
required). Re-pin `gittar_sha` (`build.sh:1384`).

**B3 -- prove it under the phenotype.** A boot-gate marker: `git clone
https://github.com/apology-is-policy/thylacine.git` (or a small repo) succeeds
through netd + the CA bundle. Pre-flight `nm git-remote-http | grep
pthread_create` is empty. Watch for `eventfd2` -- git's libcurl usage is the
simple easy-interface path (not curl-the-binary's parallel multi-socket eventfd
path), so it likely dodges it; verify, and if it bites, that becomes a small
phenotype translation (a real eventfd2 -> a native poll/pipe wakeup).

**Exit criteria:** clone + fetch + push over https succeed under the phenotype,
proven by a boot gate; `git-remote-http` provably thread-free. **MET (2026-08-31):**
clone + fetch gated by `tools/test-git-https.sh` (milestone B, `70e19dd8`/`cc0d1e68`);
**push** gated by `tools/test-git-push.sh` (N-6 -- `GITHTTPS-PUSH` to a real writable
github remote, first try; push rides clone's transport, `readv` [N-5] was the whole
prerequisite, no new syscall). Milestone B is COMPLETE.

**Build vs prebuilt (decided):** no reputable, current, sha-pinnable prebuilt
fully-static aarch64-musl git-with-https + templates exists (minos-static /
tiiuae are unverified + likely stale; Alpine's git is dynamic). We **build**.
The one auditable shortcut is reusing **stunnel/static-curl's `-dev` tarball**
(ships `libcurl.a` + headers, sha-pinned -- we already consume its curl binary)
for the libcurl half -- but verify its resolver mode (it bundles c-ares; confirm
c-ares-or-`--disable-threaded-resolver` before trusting it under CLONE_THREAD).

### Milestone B2 -- the phenotype networking + threading arc (the "make it literal" cluster)

> **STATUS 2026-08-28 (aux): PLANNED.** Milestone B proved HTTPS clone + fetch,
> but behind three asterisks -- `-c protocol.version=0`, an `/etc/hosts` IP-pin
> (no DNS-by-name), SYSTEM-only -- and only for *single-threaded* programs. Those
> asterisks plus the `pthread` wall are **one cluster** of VIVARIUM gaps, all
> "unconnected-UDP / non-blocking-socket / thread" machinery that **most**
> networked or concurrent Linux binaries need, not just git. This arc closes it.

Two things fall out of closing this cluster: **(a)** `git clone
https://github.com/apology-is-policy/thylacine.git` becomes *literal* -- by URL,
default protocol, verified TLS, no hosts-pin -- and **(b)** real
multithreaded/networked Linux programs run under `viv` (npxf, stock curl with
its threaded resolver, git's parallel index-pack). This is a VIVARIUM arc that
git's networking *depends on*; it is sequenced here because the git README is
its most legible exit criterion.

**The two headline unlocks:** DNS-by-name (**net-4d**) makes the clone URL
literal; threads (**CLONE_THREAD**) run the multithreaded majority.

**Sequence** (dependency + impact order; N-1 is the shared substrate):

- **N-1 -- non-blocking sockets (`SOCK_NONBLOCK`).** The shared substrate: musl's
  DNS resolver, npxf's connect-with-timeout, and curl all need it. Today
  `socket(..., SOCK_NONBLOCK, ...)` is refused outright
  (`kernel/vivarium.c:1628-1630`). Scope: admit the flag + give the socket Dev a
  non-blocking mode -- the `CNONBLOCK` pipe fill from the git-stash chunk is the
  near-template (same idea, the netd/weft socket layer instead of devpipe), plus
  `fcntl(F_SETFL, O_NONBLOCK)` on a socket fd. Audit-bearing (I-9, the socket
  readiness wait/wake).

- **N-2 -- net-4d: unconnected UDP + DNS-by-name.** *The README unlock.* musl's
  resolver `sendto`s a query to the nameserver on an **unconnected** UDP socket
  and `recvfrom`s the reply under a poll timeout; the phenotype serves only the
  **connected** datagram shape (`vivarium_sendto_decide`/`recvfrom_decide` report
  ENOSYS for unconnected, `kernel/vivarium.c:1691-1698`). Scope: build the
  unconnected `sendto(addr)`/`recvfrom(addr)` datagram path (a per-datagram dial
  to the named address) on top of N-1. Exit: `getaddrinfo("github.com")` resolves
  via `/etc/resolv.conf` -> slirp DNS (10.0.2.3), so the clone URL is literal.

- **N-3 -- phenotype threads (`CLONE_THREAD`).** The npxf unlock + curl's threaded
  resolver + git's parallel index-pack. **Already fully scoped:
  `docs/PHENOTYPE-THREADS-GUIDE.md`** (mental model; the one genuinely-new core
  `thread_create_forked_in_proc`; the `futex`/`gettid` rows; build order; the
  test ladder). The largest single piece and the highest-leverage -- it unblocks
  "most multithreaded Linux binaries," not just git.

- **N-4 -- AF_UNIX sockets.** npxf's local transport (the phenotype admits
  AF_INET only, `kernel/vivarium.c:1622`). Lower priority -- many programs run
  fine over AF_INET loopback -- but real for anything that hard-codes AF_UNIX.

- **N-5 -- protocol v2 root-cause. DONE + AUDIT-CLOSED (`05616d9c` + the P0 close).**
  Removed the `protocol.version=0` asterisk so git's *default* protocol works. The
  leading `getsockname` hypothesis was **REFUTED** -- it is FORWARD->ENOSYS but
  NON-FATAL, called at connect time on a path shared with the working v0 clone. ROOT
  CAUSE (found via the always-on kernel census, not git-side traces): the phenotype
  never served **readv(65)** while its twin **writev(66)** was served -- git v2's
  stateless-connect path reads the helper response through readv -> ENOSYS -> silent
  abort ("reads the caps, writes nothing back"); v0 reads the inline advertisement
  with plain read(). FIX: served readv (the exact mirror of `viv_writev`); retired
  forced v0 from both gitconfigs; the git-net gate now clones on git's default v2
  (the E2E v2 regression net). The audit (self-audit + the Fable-5 holotype,
  converged) found + fixed a **P0**: `viv_readv` AND `viv_writev` copy_in the iovec
  ARRAY at an unvalidated `iov_va` -> a kernel-range array pointer extincts the
  kernel (unprivileged DoS; the uaccess fault-fixup covers only the user half); both
  twins guarded with one whole-span `sys_validate_user_buf`.

- **N-6 -- push over https.** Completes milestone B's *own* exit criteria (clone +
  fetch + **push**; push is currently deferred). Needs a writable/authenticated
  remote + the smart-push path (`git-remote-https` POST `git-receive-pack`) +
  credential handling.

**Exit criteria (the arc):** `git clone https://github.com/apology-is-policy/thylacine.git`
succeeds by URL, default protocol, over verified TLS, in a net-granted bundle; a
multithreaded musl binary (the pthread-guide probe, then npxf) runs; milestone
B's push criterion is met. The README line becomes literally true, not asterisked.

**Build-system integration (an N-6 graduation step).** The git bundle's inputs
(`static-curl`, `static-git`) are ALREADY first-class **forage** targets
(`tools/forage.sh static-curl|static-git` -> `build/cache/`, with a `forage_hint`
emitted on absence, exactly like `alpine`) -- so forage is done. What is NOT done
is the **build-config** side: `GOROOT`/`CLADE`/`ALPINE` are first-class
`bc_def bake CHUNK_*` symbols (togglable via `tools/configure.sh`, `docs/BUILD-CONFIG-DESIGN.md`),
but the git bundles are still raw `THYLACINE_BAKE_GITNET=1` / `_GITWF=1` env
flags, outside the configurator -- correct while they are in-flight *gates*, but
the moment https-git is a shipped *feature* it should graduate to a
`CHUNK_GITNET` symbol mirroring `CHUNK_ALPINE` (a one-line `bc_def bake`), so it
is a configured option with the standard "needs the tarball foraged; absent ->
skipped" note rather than an ad-hoc env flag. That promotion is the concrete
"graduate into the shipped image" step §2.3/B allude to.

**Suggested first two:** **N-1 -> N-2** (the README unlock, small + high-visibility)
and **N-3** (threads, the general unlock, guide already written) can proceed in
parallel -- they share only N-1's non-blocking substrate. Cross-refs: the socket
`*_decide` functions are `kernel/vivarium.c:1522-1710`; the gap census that
surfaced this cluster is the curl/git mission record in `docs/AUX-ROADMAP.md`.

### Milestone C1 -- the non-interactive complete workflow (the self-hosting floor)

> **STATUS 2026-08-27 (aux): COMPLETE -- all 13 of 13 verbs VERIFIED under the
> phenotype** (`joey: git-workflow gate PASS`, 17 GITWF-* markers; the hermetic
> `git-workflow` bundle + `tools/test-git-workflow.sh`, THYLACINE_BAKE_GITWF=1).
> PASS: branch, checkout, **diff (a same-size line edit IS detected -- racy-git
> works; the pool mtime is a real wall clock)**, status, commit, log, **merge
> (fast-forward AND a 3-way with a real conflict resolved by editing the marked
> file -- git spawns NO editor)**, **rebase (non-interactive)**, reset,
> **stash (save + pop, a round-trip witness: save reverts the change, pop
> restores it)**, worktree, **manual gc (fork-self -> repack/prune)**. Both fills
> LANDED:
>
> **(a) `git stash` -- LANDED 2026-08-27.** git's async pump sets its subprocess
> pipe non-blocking via `fcntl(F_SETFL, O_NONBLOCK)`; both were missing -- the
> phenotype declined F_SETFL (ENOSYS) and devpipe was blocking-only. The fill adds
> per-Spoor POSIX O_NONBLOCK (`CNONBLOCK`, a new Spoor flag) end to end:
> `vivarium_fcntl_decide` now serves F_GETFL/F_SETFL; the shell reads/writes the
> bit via `handle_get_status_flags`/`handle_set_nonblock`; and `devpipe_read`/
> `devpipe_write` return `-EAGAIN` instead of sleeping when the op would fully
> block. The EAGAIN guards are PRE-SLEEP early returns placed AFTER the data / EOF
> / space checks, so a ready op, EPIPE, and EOF are untouched and the blocking
> wait/wake path is byte-unchanged (I-9 holds trivially). Audit-bearing
> (kernel/pipe.c); holotype + SMP gate on the death/wait surface.
>
> **(b) exit(N) boolean (#91) -- LANDED 2026-08-27.** The real exit byte now
> reaches `$?` end-to-end: `sys_exits_handler`/`sys_exit_group_handler` pass
> `status & 0xff`; `exits_code`/`proc_group_terminate_code` carry it; a new
> `group_exit_code` on struct Proc feeds the last-thread-out ZOMBIE status.
> Witnessed on three layers (native `SYS_EXIT_GROUP(42)`->42, pouch `return 3`->
> `WEXITSTATUS 3`, viv `linux_exit(7)`->7). Audit-bearing (I-24); holotype + SMP
> gate on the death/ZOMBIE surface.
>
> A build-note the gate surfaced (NOT a git blocker): busybox `printf > file`
> yields an EMPTY file -- the printf builtin's musl stdio buffer is never flushed
> and NO write syscall is made (proven via a 3-level kernel write trace); `echo`
> (direct write()) works, and **git's own file I/O is direct write()**, so
> git-core is unaffected. The gate builds its test files with `echo`. Tracked as a
> separate userspace fidelity gap.

Everything here is built on **already-supported** primitives (O_CREAT|O_EXCL,
rename, O_APPEND, fork+exec, stat/readdir) and is almost certainly working today
-- but is **entirely UNTESTED by the current gate**. C1 is verify-then-fill.

- **Verify** (add gate coverage): `branch`, `checkout`/`switch`, `status`,
  `reset`, `diff` (piped), `log` (proven), **`merge`** (3-way; git writes
  conflict markers and spawns *nothing* -- the user edits the marked file and
  `git add`s, so no editor is needed for conflict resolution), **`rebase`**
  non-interactive (`--onto`/`--continue`; pauses on conflict to a temp file, same
  as merge), **`worktree`** (pointer FILES, no symlinks -> works), `stash`.
- **Fill the real gaps:**
  - **exit(N) boolean (#91)** -- fix so a container's specific exit status
    survives to the shell `$?`. Load-bearing for hooks + the shell-script tool
    paths; a correctness fix that also helps every scripted Linux workload.
  - **auto-gc** -- fires after commit/merge/rebase/fetch. Start with
    `gc.auto=0` (kills the self-fork) to get correctness first; then light it up
    (needs fork-self -> `git repack`/`prune`, `opendir`/`readdir` on one fanout
    dir, `gethostname` + `kill(pid,0)` liveness for the `gc.pid` lock -- confirm
    those two are translated). `repack`/`prune` use rename+unlink (no linkat),
    single-threaded (see 5.1).
  - FS-gap edge cases surfaced by gc/checkout (linkat/ftruncate) -- expect
    graceful fallbacks; fix any that are not.

**Exit criteria = the self-hosting bar:** in-guest, as the git identity, clone
thylacine, branch, commit, rebase (non-interactive), and push -- all green in a
gate. At this point Thylacine can host its own git-based development
non-interactively.

### Milestone C2 -- the interactive tier (nora + a real terminal)

The "complete practical" polish. Gated on a kernel chunk.

- **C2-kernel: terminal-control translation.** Translate `ioctl(TCGETS/TCSETS)` +
  termios + `TIOCGWINSZ` + `setsid`/`setpgid` under the phenotype, wired to the
  existing pts machinery (ptyfs / PTY-1/2, I-20) and the native `SYS_TTY_*`
  surface, so a phenotype process can be interactive: `isatty()` true, raw mode,
  window size, a controlling terminal. **This is the largest new kernel work in
  the plan -- and it is a general win**: it lights up *every* interactive Linux
  TUI under `viv` (an editor, `top`, `less`, a REPL), not just git.
- **C2-wiring: nora as the editor.** `core.editor=nora` + `sequence.editor=nora`
  -- **bare names, so git `execve`s nora directly with no `/bin/sh`** (git only
  invokes the shell when the editor string carries a metacharacter, space
  included). nora already fits the editor contract (takes a file-path arg, `:w`/
  `:q`, real exit code, tty:hup/quit handling; `usr/nora/src/main.rs:84`).
  - **mergetool/difftool** are different: `git mergetool`/`git difftool` are
    themselves POSIX **shell scripts** that `eval` `mergetool.nora.cmd` -- so this
    path hard-depends on a working `/bin/sh` at the compile-time `SHELL_PATH`
    (our container `/bin/sh` = busybox ash, which runs under the phenotype) AND on
    the exit(N) fix (the scripts branch on tool exit codes). nora gains a
    `--merge $LOCAL $REMOTE $BASE $MERGED` / `--diff $LOCAL $REMOTE` multi-file
    mode. (Simplest first step: no mergetool at all -- conflict markers + nora on
    the single merged file, which needs neither the shell path nor multi-file
    nora.)
  - **pager:** start `core.pager=cat` (no `less` to port); a real pager rides the
    C2-kernel terminal work later.

**Exit criteria:** `git commit` / `rebase -i` / `add -p` open nora on a real
terminal; interactive diff/merge via nora; the pager works.

### Parallel dependency -- A-3 (user-git)

git runs as SYSTEM until per-principal 9P ownership (A-3) lands, at which point a
real logged-in *user* can run git against their own files. A-3 does not block
B/C1/C2 (all provable as SYSTEM), but "seamless user git from a shell" -- the
original phrasing -- is only complete when A-3 + the ut cap-conferral land.

---

## 4. Definition of done

- **Self-hosting (B + C1):** a developer in Thylacine can `git clone` the
  thylacine repo over https, branch, edit, `commit -m`, `rebase --onto`, and
  `push` -- entirely non-interactively -- and it is gate-proven.
- **Complete practical (B + C1 + C2):** add interactive commit/rebase/add-p with
  nora, a pager, and interactive diff/merge.
- **Seamless user git (+ A-3):** all of the above as a real non-SYSTEM user.

---

## 5. Cross-cutting decisions

### 5.1 Threads: keep `NO_PTHREADS` (settled)

git needs pthreads only for *performance*, never correctness -- under
`NO_PTHREADS` the threaded paths (`index-pack` delta resolve, `pack-objects`
delta search, `grep`, `preload-index`) compile out, `online_cpus()` returns 1,
and git's async machinery falls back from a thread to `fork()` (which the target
supports). Because the phenotype *refuses* `CLONE_THREAD`, a pthreads-enabled git
would `pthread_create` in index-pack/pack-objects and fail at runtime;
`pack.threads=1` on a threaded build is fragile. So `NO_PTHREADS` is exactly
right. The **cost** is single-threaded `gc`/`repack`/`clone` -- correct but
several times slower on a large repo's delta search; acceptable for v1.0. **The
correct future speedup is CLONE_THREAD support in the kernel, not a threaded git
fighting a refusing phenotype.** (And, separately, libcurl must be
`--disable-threaded-resolver` -- the second, easy-to-miss thread layer.)

### 5.2 Decisions to ratify (D1-D4)

- **D1 -- TLS backend: OpenSSL vs mbedTLS.** Recommend **OpenSSL** (proven with
  musl, reuses toolchain, best cert compat); **mbedTLS** if binary size is a
  first-class concern (~10x smaller TLS). No functional difference for git https.
- **D2 -- build C2's terminal-control chunk as part of the git arc, or sequence
  it after B+C1?** It is the biggest new kernel work and a general interactive-TUI
  enabler. Recommend: **B + C1 first** (self-hosting, no kernel work beyond the
  small exit(N)/eventfd2 fixes), **then C2** as its own chunk.
- **D3 -- self-hosting-at-B+C1 as the headline milestone**, with C2 as polish.
  (Ratify the framing.)
- **D4 -- A-3 (user-git) sequencing.** It is orthogonal to B/C1/C2; when does it
  land relative to them? (git works as SYSTEM throughout regardless.)

### 5.3 Config posture (early bring-up)

`gc.auto=0` (then enable), `core.pager=cat`, `core.editor=nora` +
`sequence.editor=nora` (bare names), `core.symlinks=false`, `core.fsync=none`,
`pack.threads=1`, `core.createObject=rename`, `http.sslCAInfo=/etc/ssl/certs/ca-certificates.crt`,
`OPENSSL_armcap=0` (env). Light up mergetool/difftool + auto-gc once
process-spawn + exit(N) are solid.

---

## 6. Risks

- **eventfd2** in git's libcurl usage (likely dodged; verify at B3, small fix if
  not).
- **The C2 terminal-control chunk** is real kernel work touching the phenotype +
  pts + notes; scope it as its own audit-bearing arc.
- **exit(N) boolean** may bite the shell-script tool paths harder than expected.
- **Single-threaded gc/repack** perf on a full thylacine self-clone -- measure;
  mitigate with `gc.auto` tuning, not threads.
- **The A-3 wall** keeps git SYSTEM-only until built -- surfaced, not a blocker
  for the self-hosting milestone.

---

## 7. Heritage + sources

- Native floor: 9front **git9** (Ori Bernstein) -- self-hosts with no
  index/interactive-rebase/mergetool/auto-gc.
- Stock-git-over-translation: **gVisor/Sentry**, **WSL1** (pico processes; the
  stat-path + spawn-cost lesson), **illumos LX-zones**.
- Build reference: **Alpine `git` APKBUILD** (the canonical musl+curl recipe;
  keeps `git-remote-http[s]` in the base package), **git's Makefile**
  (`NO_CURL`/`CURLDIR`/`CURL_LDFLAGS`/`NO_EXPAT`), **stunnel/static-curl** (the
  sha-pinnable static libcurl input), **curl SSL-compared** (TLS backends).
- In-tree: `docs/VIVARIUM.md` (the phenotype + fidelity ladder), `tools/build.sh`
  (the git/curl staging), `kernel/vivarium.c` (the translation tables),
  `docs/phase7-status.md` (the curl-demo + git + /viv/bin rows).

---

## 8. Cross-links

- `docs/VIVARIUM.md` -- the phenotype substrate this rides; section 13 (the
  `/viv/bin` mount) is the deployment channel; the fidelity ladder (section 9)
  scopes what the phenotype supports.
- `docs/phase7-status.md` -- the /viv/bin arc row (milestone A, local git) and
  the curl-demo row (the TLS walls this reuses).
- A-3 -- per-principal 9P ownership (the user-git dependency), unbuilt at v1.0.
