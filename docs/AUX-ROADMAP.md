# Aux track roadmap

**As of 2026-08-16. Branch `aux-2`.** Read the branch off the worktree
(`git branch --show-current`), never off this line — on 2026-08-16 three
sources gave three answers (main's CLAUDE.md said `aux/userspace-apps`, this
header said `gfx-4 @ 45186b64`, the worktree was on `aux-2`), and `gfx-4` is a
separate live branch rather than a rename of anything. A branch name written
into a document is stale from the moment the track moves; the worktree cannot
be.

Supersedes the narrow `usr/apps/AUX-ROADMAP.md` (the old userspace-apps-only aux
agent). This track now owns the graphics arc, the Aurora environment, and the
VIVARIUM arc — **plus the kernel surfaces those arcs land on**, which is where
its recent work actually sits: measured over the 250 commits from `45186b64` to
`aux-2`, the touched tree is `kernel/test` (52 files), `docs/reference` (41),
`usr/ports` (28), `kernel/include` (27), `usr/lib` (19), `tools/interactive`
(13), `arch/arm64` (11). The notes / signals / job-control / PTY line
(aux#240..aux#255) is a kernel arc by any honest reading, and this track runs
its own full bar for it: the suite, the SMP gate, the pty spec set, and LS-CI.

The 2026-07-28 version of this header described the track as graphics-only and
went unrefreshed for three weeks while the work moved. Refresh this block
whenever the arc changes, not whenever it is convenient.

---

## Where the track stands

| Arc | State |
|---|---|
| **Graphics G-0..G-7** | **COMPLETE.** G-0..G-5 + PTY are already ancestors of `main`; G-6 (compositor) + G-7 (SDL/Quake) are on `gfx-4` awaiting the merge. |
| **Track B — Aurora config (cfg-1..cfg-5)** | **COMPLETE.** OSD + persistence + OSC session push + the apply-authority gate + runtime chords/gaps + baked font sizes, each audited. |
| **VIVARIUM (V-0..V-8) + DISTRO (D-1..D-5)** | **CLOSED (arc close).** Runs unmodified Linux musl-static + stock dynamic-PIE Alpine binaries under `viv`; TCP sockets, real signals, `/proc`+`/sys`, interactive ash on pts+console. **curl/git mission ACTIVE** (real 3rd-party programs end-to-end): the phenotype network reaches the internet; the time translators landed; **a REAL Linux curl (static-PIE 8.18.0) fetches a URL by name under viv (ROADMAP 9.2 criterion TICKED)** -- forced three socket rows (`getsockopt` SO_ERROR + `sendto`/`recvfrom` send/recv shapes) and produced the gap census (SIGILL feature-probing fatal, /dev/null redirects, eventfd2, pthread_create, timeouts -- each enqueued). **git arc OPEN** (operator "let us do git" 2026-08-25) with the reconnaissance-verified keystone: **#50, the path-mutation family** (design ratified, `VIVARIUM.md` section 6.24 — `SYS_OPEN_CREATE`=109 fulfilling the ARCH section 11.2 `create` row + the four phenotype rows `openat(O_CREAT)`/`mkdirat`/`unlinkat`/`renameat`; git cannot write a single file until it lands). Then: getdents64+fsync rows; build a static aarch64 NO_CURL git via the musl cross env (no prebuilt exists — milestone A = local init/add/commit/`clone file://` with `pack.threads=1`); milestone B = full https git (+libcurl+openssl, `OPENSSL_armcap=0`); then weigh the pthread arc for compute-SMP. |
| **Phenotype networking + threading (net-4d + CLONE_THREAD; the "make it literal" cluster)** | **N-1 + N-2 + N-3 LANDED (incl. the socktab-lock follow-on); N-4 DEFERRED to the Mycelium NOVEL (operator 2026-08-31); N-5 DONE + AUDIT-CLOSED (served readv; git v2 works, forced-v0 retired; the audit found + fixed a P0 -- readv AND writev copy_in the iovec ARRAY at an unvalidated iov_va -> unprivileged kernel-DoS, both twins guarded; SMP 40/40 PASS; PUSHED @537f8b7e); **N-6 DONE -- git push https:// works, MILESTONE B COMPLETE** (`docs/GIT-ON-THYLACINE.md` "Milestone B2"). **N-3 socktab-lock DONE `32eec7ca`** (N-3 sprang the DNS F2 trap -- `Proc.socktab` went multi-thread-reachable; the focused spinlock closed it, see below). The cluster of VIVARIUM gaps most networked/concurrent Linux binaries hit -- surfaced by curl (threaded resolver, DNS-by-name) and npxf (threads + AF_UNIX + non-blocking sockets). Sequenced N-1..N-6: **N-1 DONE** non-blocking sockets (`SOCK_NONBLOCK`/`SOCK_CLOEXEC` admitted; `CNONBLOCK` on the ctl fd, preserved across connect's swap) + **N-2 DONE** net-4d unconnected-UDP DNS-by-name (unconnected `sendto` + `recvmsg`(212) + 0->EAGAIN; **`git clone https://github.com/...` is now LITERAL, no `/etc/hosts` pin** -- proven by the git-https gate's `GITHTTPS-DNS` getent leg AND the pin-free clone) -> **N-3 DONE** phenotype threads (`CLONE_THREAD` -> a Thread in the caller's OWN Proc via `thread_create_forked(cur->proc,...)` -- NOT the guide's speculative `thread_create_forked_in_proc`, the existing core was already parameterized on proc; `exit`(93)->`SYS_THREAD_EXIT`, `gettid`(178), `futex`(98) WAIT/WAKE/REQUEUE onto torpor with REQUEUE emulated as `wake(val+val2)`; the pthread word `0x007D0F00`; proven by viv-pheno-probe L164-L169c spawn+run+SETTLS+gettid+ptid+join, discrimination-verified by a tls=0 sabotage -> `marker=L166`; `c507b787`+`f388e9fb`+`9227073d`. **socktab lock DONE `32eec7ca`** -- N-3 sprang the DNS chunk's F2 trap [[bug-n3-socktab-multithread-race]]: `Proc.socktab` was lock-free ONLY while `CLONE_THREAD` was refused; a multithreaded phenotype program with concurrent socket ops (curl's threaded resolver + connection socket; npxf) would have raced it. Closed by a leaf spinlock on `struct viv_socktab` -- SNAPSHOT reads (`viv_socktab_get`) + IDENTITY-GUARDED keyed writes (`set_*`/`record_remote`, keyed on a MONOTONIC per-claim `epoch` -- NOT `n`, which netd recycles as the lowest-free slot index: the holotype F1 P1), held only over pure array ops, never across I/O; `claim` gained a born-state; sigtab confirmed needs-no-lock. New regression `vivarium.socktab_keyed_write_identity` (same-`n` recycle, sabotage-discrimination-proven); suite 1470/1470; audit 0 P0 / 1 P1(F1 epoch) / 1 P2 / 1 P3, all fixed) -> **N-4** AF_UNIX **DEFERRED** (operator 2026-08-31 -> the Mycelium NOVEL, `docs/NOVEL.md` "Post-v1.0 candidates": no near-term driver -- npxf runs over AF_INET loopback, only the optional Wayland W4-5 needs AF_UNIX and it needs the whole AF_UNIX + SCM_RIGHTS + shmem cluster; design it native-first AS Mycelium, design session OWED) -> **N-5** protocol-v2 root-cause **DONE** (root cause: the phenotype never served `readv`(65) while its twin `writev`(66) was served -- git v2's stateless-connect path reads the helper response through readv -> ENOSYS -> silent abort; served readv mirroring `viv_writev`; forced `protocol.version=0` retired from BOTH gitconfigs; the git-net gate now clones on git's default v2; suite 1471/1471; **AUDIT (self-audit + Fable-5 holotype, CONVERGED on 1 P0, all else sound)**: viv_readv AND viv_writev copy_in the iovec ARRAY at an unvalidated iov_va -> a kernel-range array ptr extincts the kernel (unprivileged DoS; the uaccess fault-fixup covers ONLY the user half, exception.c:280); pre-existing in writev #150, PROPAGATED into readv, both twins fixed with one whole-span sys_validate_user_buf (viv_sock_recvmsg's F1 = the proven in-tree pattern); dirty close but a holotype-pre-blessed one-liner -> a self-audit of the fix replaced a re-round (getdents64 precedent); git-v2 clone E2E PASS (guard admits git's user-range iovs); SMP 40/40 PASS; PUSHED @537f8b7e; [[bug-viv-iovec-array-kernel-va-extinction]]) -> **N-6 push-over-https DONE**: git push https:// works under the phenotype to a real writable remote FIRST TRY -- push rides clone's transport (git-remote-https POST git-receive-pack via the helper pipe: readv [the N-5 prereq] + writev), NO new syscall (eventfd2 dodged), test-infra only (build.sh push leg + provisioning + tools/test-git-push.sh); operator-ratified external sandbox + a fine-grained PAT (revoked after); GITHTTPS-PUSH witness, token-leak 0. **MILESTONE B COMPLETE** (clone + fetch + push over https all green under the phenotype). |
| **Stream 0 — the workflow bar** | **ACTIVE, and it orders everything below it** (operator, 2026-08-31). Four end-to-end terminal workflows (W1 Go-over-https, W2 tarball-to-clang-TUI, W3 network 9P host mount, W4 vivarium graphics) plus six cross-cutting fixes. See Stream 0 immediately below. |
| **Halcyon G-8/G-9** | Not started. The graphics endgame. |
| **DOSBox-X (DX-0..DX-7) -- the DOS + Win9x emulation arc** | **DX-2c LANDED (2026-09-03): DX-2 COMPLETE -- first light AND run-a-program. DX-1 (@5af3e46d: curated C++ build LINKS -> 17.6 MB static AArch64 ET_EXEC, 0 PT_DYNAMIC, 311/311 TUs) + DX-2a (@c9c4cb40: EXECUTES, `dosbox-x -version`) + DX-2b (@3afc8975: render leg -- blue welcome screen + `Z:\>` on the scanout, 24 buckets; fixed SDL audio E_Exit [0004], SetWindowSize NULL-crash [tap-recreate hook], resize-war [0005]) + DX-2c (@e1f770a0: `mount c /home/michael` + run DX2C.COM off it -> `C:\OUT.TXT`=DX-2C-OK, read back through the shell). DX2C.COM = a 49-byte DOS `.COM` (`tools/dx2c-dosprog.py`, baked at the devramfs root, reached at `/bin/DX2C.COM` post-pivot via joey's /bin bind); the gate backgrounds DOSBox-X (`&`) so the shell stays free for the file readback (DOS output paints the pane, not serial). `ls-gfx-dosbox` now proves BOTH DX-2 halves in one leg (render screendump + OUT.TXT); no quake regression. DX-2 close DONE (operator-directed 2026-09-03): `THYLACINE_BAKE_DOSBOX` DEFAULT-ON -- the emulator + DX2C.COM ship in the default image (`=0` opts out; absent LLVM C++ fork skips gracefully). DX-3a LANDED (2026-09-03): the keyboard INPUT path proven end to end (QMP send-key -> virtio-keyboard -> tapestryd -> focused DOSBox surface -> SDL -> INT 21h AH=08h; witness DX3K.COM writes `C:\OUT.TXT`=KEY=a, gate `ls-gfx-dosbox-input.exp` + `tools/qmp-send-key.sh`). RESOLVED the foreground-exit open item: NOT an SDL-teardown hang -- a foreground dosbox that exits via autoexec `-c "exit"` returns the shell cleanly (first clean SDL-app exit to the shell on Thylacine); the DX-2c "wedge" was typing `exit` on SERIAL (dosbox reads PANE input, not serial). DX-3b LANDED (2026-09-03): file-based config/autoexec -- `dosbox-x -conf <file>` loads a dosbox-x.conf whose `[autoexec]` runs at startup (sample baked; gate `ls-gfx-dosbox-conf.exp`, verified `CONFIG: Loaded config file` + OUT.TXT with no -c flags). Sound = DONE by design (SDL dummy + full device emulation with discarded output is compat-correct). Larger real DOS program deferred to DX-5 (no assembler vendored; DX-5 owns the fetch). DX-3 substantially COMPLETE. DX-4 LANDED (2026-09-04, @e8ee10e6): the CAP_JIT dynarec -- `core=dynamic_rec` (DOSBox-X's PORTABLE `core_dynrec`/ARMV8LE, NOT dynamic_x86) wired to the I-42 dual-map code Burrow (patch 0006 + `usr/lib/thylajit/thyla_jit.h`). DOSBox-X already shipped the dual-map (`DYNCOREM_DUAL_RW_X` + `cache_rwtox`) + embeds no absolute in-cache addrs, so NO codegen change -- the port is the `__thylacine__` alloc arm (acquire CAP_JIT via the corvus jit clearance -> `SYS_JIT_CREATE`) + routing the `__builtin___clear_cache` publishes (incl. the cache_init/reset page-0 trampoline+stubs, a self-audit find the gate did not catch) through `SYS_ICACHE_SYNC` (EL0 dc/ic trap, SCTLR_EL1.UCI=0). Gate `ls-gfx-dosbox-dynarec.exp` (CAP_JIT acquired + DX-2C-OK under the dynrec; no page ever W+X). AUDIT-BEARING: focused audit CLOSED 0 P0 / 0 P1 / 1 P2 / 2 P3 all-fixed (Opus 4.8 tier -- Fable out of credits, operator-directed close on 4.8). Perf NUMBER deferred to DX-5 (DX2C.COM is 49 B, too small; dynrec faster by construction + proven active/correct). NEXT = DX-5 (a recognizable DOS game + Act-1 close + the perf number + reference/user-manual).** Scoped 2026-09-03 (`docs/DOSBOX.md`); variant = DOSBox-X (operator-decided) for Win9x guests + 3dfx Glide. Port via Pouch (C++/musl + the proven `SDL_thylacine`->Tapestry backend + libc++) = "TyrQuake in C++, at larger scale"; a flagship of the emulation story (beside VIVARIUM + the planned x86-xlat / Wine). Sound STUBBED (v1.0 non-goal). The dynarec on **CAP_JIT (I-42)** is CENTRAL (Win9x/Voodoo REQUIRE it) -- reviewed a CLEAN fit for the as-built dual-map surface, NO kernel change (same-thread emit/execute + software-SMC dodge the unbuilt JIT caveats; ORC `DualMapMemoryMapper` is the C++ template); `core=normal` is the DOS first-light floor. **Act 1 (DOS):** DX-1 vendor+C++ build (heaviest chunk -- big tree) -> DX-2 FIRST LIGHT (`Z:\` in a Tapestry pane + `ls-gfx-dosbox` gate) -> DX-3 sound/input -> DX-4 CAP_JIT dynarec (AUDIT-BEARING; prereq for Act 2) -> DX-5 a DOS game + close. **Act 2 (showcase):** DX-6 Win9x guest bring-up (Win98 desktop in a pane) -> DX-7 3dfx Voodoo (low-level via CAP_JIT; Glide passthrough GATED on the GL-accel arc being baked). **DX-8 LANDED (2026-09-05): defaults + presets + per-game configs + the build inputs** -- patch 0008 (a system base-layer config at `/lib/dosbox-x/dosbox-x.conf`, rendered from the configurator's `DOSBOX_CPU_PRESET`: autolock=true + core=dynamic_rec + cycles=fixed N), per-game `dosbox-x.conf` in both masters (`cd ~/duke3d; dosbox-x` is the launch), `build_tombraider_fixture` (archive.org `tomb3dem`, sha256-pinned, proven on thyla-pi), `[network.duke3d]`/`[network.tombraider]` + `CHUNK_DOSBOX`/`CHUNK_DUKE3D`/`CHUNK_TOMBRAIDER` with a pin-drift test + a lowering constraint; the Duke3D gate proves mouse-look BY DEFAULT (lateral-shift witness + no-input control). |
| **Notes / job control / PTY (the kernel line, aux#240..)** | **ACTIVE.** See Stream 4 below. |

---

## Stream 0 — the workflow bar (operator-set, 2026-08-31)

**The organizing principle above every other stream in this file.** Ratified by
the operator: *"someone sits with Utopia and wants to do something, finding
things that impede them along the way -- the more impediments we remove, the
more pleasant and complete will the system feel."*

Streams 1-4 order work by subsystem. This one orders it by **what a person at
the terminal is trying to finish**, and it wins ties: a small fix that unblocks a
workflow outranks a large chunk that advances a subsystem nobody is standing in
front of. Four workflows define the bar. Each is written as the literal command
sequence, so "done" is observable rather than argued.

The findings below were established 2026-08-31 by three parallel investigations
plus direct verification; every load-bearing claim carries its `file:line`.

### W1 — clone a Go repo over https, build it, run it

```
git clone https://github.com/<user>/<repo>.git && cd <repo> && go build && ./<repo>
```

**Works already:** on-device `go build` is a boot-fatal gate (`joey.c:7267`);
`/net` is mounted into the login namespace *before* the getty loop
(`joey.c:7865` vs `:11298`), so an interactively-spawned process inherits TCP
reach with no capability grant -- **networking is namespace-derived, not a cap.**

| Id | Blocker | Shape |
|---|---|---|
| **W1-a** | `GIT_EXEC_PATH` + `GIT_CONFIG_SYSTEM` are set only inside the three container bundles (`build.sh:1487,1656,1851`); nothing sets them for `/viv/bin/git` from a shell. Interactive git therefore cannot find `git-remote-https`, never reads `/viv/bin/gitconfig` (so no `templateDir`, no `sslCAInfo`, **and no `protocol.version=0`** -- which `build.sh:1529` records as required because v2 aborts silently under the phenotype), and cannot re-exec itself for `maintenance`. | **LANDED 2026-09-01**: login's `seed_session_env` seeds `GIT_EXEC_PATH=/viv/bin` + `GIT_CONFIG_SYSTEM=/viv/bin/gitconfig` beside PATH; the gitconfig gains `core.editor`/`sequence.editor = nora`. Also seeds `OPENSSL_armcap=0` (the helpers bundle OpenSSL; the SIGILL-probe wall). Witness `tools/interactive/git-shell.exp` (exec-path, config read, init+commit as the user, the dashed upload-pack from PATH, git-remote-https exec'd through GIT_EXEC_PATH -- the Design D 13.10.8 closure). Local clones need `/bin/sh`: **X-11**. |
| **W1-b** | `$PATH` omits `/viv/bin` (`usr/login/src/main.rs:952`, `joey.c:7682`) while ut's static list includes it (`eval/stmt.rs:590`) -- so `git` runs but `which git` fails. `which.rs`'s own header predicted this drift. | **LANDED** (both seeds already carried `/viv/bin` + `/viv/abin`; the drift was the git env, closed with W1-a). |
| **W1-c** | DNS by name. **DONE (N-2/net-4d).** musl's `getaddrinfo` now resolves over the phenotype unconnected-UDP path (`sendto` + `recvmsg`(212) + non-blocking); the `/etc/hosts` pin is retired. Proven by the git-https gate: the `GITHTTPS-DNS` getent leg (single-threaded getaddrinfo) AND the pin-free `git clone https://github.com/...`. | LANDED; pin dropped from `test-git-https.sh`. |

**Exit:** the command sequence above, from a fresh login, with no `/etc/hosts`
pin.

### W2 — fetch a C/C++ source tarball, unpack it, build it with clang, run the TUI

```
curl -O https://.../src.tar.gz && tar xzf src.tar.gz && cd src && clang++ ... -o app && ./app
```

**Works already:** `curl`, `wget`, `https`, `make` are all on the base image;
the clade sysroot is complete -- `libc.a libc++.a libc++abi.a libunwind.a
libclang_rt.builtins.a libSDL2.a` (`build/clade/stage/sysroot/lib/`); and the
clade clang is configured with `DEFAULT_SYSROOT=/clade/sysroot`,
`CLANG_DEFAULT_CXX_STDLIB=libc++`, `CLANG_DEFAULT_LINKER=lld`,
`CLANG_DEFAULT_RTLIB=compiler-rt`, `CLANG_DEFAULT_UNWINDLIB=libunwind`
(`build.sh:4713-4717`).

| Id | Blocker | Shape |
|---|---|---|
| **W2-a** | **CLOSED for busybox tar** (X-1 + X-2 + X-8). busybox `tar` -- uncompressed AND `.tar.gz` -- runs from a session shell anywhere in the namespace (`/viv/abin`, phenotype-by-location). Proven end to end: `tools/interactive/abin-tar.exp` 8/8. A NATIVE tar is still the longer-term answer (no busybox dependency); `gzip`/`unzip` as standalone commands are separate (busybox provides them, but a native set is cleaner). | LANDED via X-1/X-2/X-8 |
| **W2-b** | Despite the configured defaults above, `clang++ main.cpp -o app` does **not** work -- the operator needs a hand-written 13-line response file (`--ld-path`, `--sysroot`, `-nostdinc++`, `-isystem .../c++/v1`, `-nostdlib++`, `-lc++ -lc++abi -lunwind -lc -lm`) which *does* work. **Root cause not yet established.** Leading hypothesis: `aarch64-unknown-thylacine` has no ToolChain class in clang's driver, so the generic fallback emits neither the libc++ header search nor the default link line. | **investigate first.** Cheap fix: ship a `clang++.cfg` beside the binary (clang auto-loads it) carrying exactly the working flags. Proper fix: a Thylacine ToolChain in the clade LLVM patch set -- **that is an arc**, inserted below. |
| **W2-c** | An interactive TUI needs `isatty()`, which is `ioctl(TIOCGWINSZ)`. There is no ioctl row at all, so it is false on every fd. | **LANDED (console)** -- C2-k1b (`05e91a06`) wired the ioctl shell + serves TC*/TIOC* on a cons fd; `isatty()` is now true on the console. C2-k2 (`348e21b7`) added setsid/setpgid for job control. pts-fd ldisc reach = C2-k1c (deferred). |

**Exit:** the sequence above with a plain `clang++ main.cpp -o app`, and the
resulting TUI takes the terminal.

### W3 — mount a host directory over the network and copy both ways

```
mount tcp!10.0.2.100!5640 /n/host     # spelling per UTOPIA-SHELL-DESIGN.md:521 + 9front
cd /n/host && cp somefile ~/
```

**Zero kernel work is required.** Every mechanism exists and is proven: the
transport vtable is already an abstract byte pipe (`9p_transport.h:87-124`), and
`viv run` performs this exact dance today -- pipe pair, spawn the server on fds
0/1, close our ends, `t_attach_9p`, `t_mount` (`usr/viv/src/main.rs:499-534`),
which `docs/reference/145-vivarium.md:3049` names as Plan 9's own `mount(fd)`
idiom. **Swap the spawned server for a TCP relay and the workflow is done.**

The structural constraint that decides the shape: **mounts are per-Proc and
`RFNAMEG` is unimplemented** (`kernel/proc.c:1528` -- "the parent ALWAYS gets a
clone"), so a mount performed by a child dies with the child. `mount` therefore
**cannot be an external command** -- it must be a shell builtin, exactly as
`UTOPIA-SHELL-DESIGN.md:518` already states.

| Id | Piece | Shape |
|---|---|---|
| **W3-a** | a slirp `guestfwd` rule for the 9P port | ~5 lines; the mechanism is already parameterized (`run-vm.sh:183-199`). |
| **W3-b** | a native byte-relay daemon: dial with `TcpStream::connect`, poll `{fd 0, stream.ready_fd()}` (**never** the data fd -- it has no `QTPOLL` and reports always-ready, `dev9p_poll.c:284`), pump both directions. Parses nothing. | ~150 LOC; template is `coreutils/src/netpump.rs:80-126`. Name held for signoff -- `9pbridge` is plain, the thematic register would prefer something like `tether`. |
| **W3-c** | `mount` / `unmount` builtins in ut = **U-8**, designed and unbuilt (`phase7-status.md:255`; dependency U-6 landed). Should recognize a `tcp!` dialstring and spawn the bridge itself, and should join a relative path against cwd (`SYS_MOUNT` requires absolute). | ~150 LOC. Builtin table: `eval/builtin.rs:114`. |
| **W3-d** | *(perf, later)* msize is hard-capped at **4096** -- `SYS_ATTACH_DEFAULT_MSIZE` == `PIPE_BUF_SIZE`, and `CNBFRAME` is all-or-nothing, so a larger frame can never be sent. ~4 KiB per round trip. | audit-bearing (Pipe + SYS_ATTACH_9P rows). |

**The `/srv` route is closed and should stay closed:** posting requires
`PROC_FLAG_MAY_POST_SERVICE`, which nothing past login holds, and widening it
was explicitly rejected (`docs/reference/145-vivarium.md:3023-3120`) because one
shared boot registry lets any program squat a trusted name.

**Security posture:** default `MNOEXEC` on a network mount -- `dev9p` sets
`may_back_exec = true` (`dev9p.c:2252`), so without it a hostile server can back
executable mappings (I-12 PROVENANCE / I-36). Hostile `Rlerror` is already
bounded (I-14). The real residual gap is duplicate-reply / tag reuse, which
`ARCHITECTURE.md:3558` records as needing wire-level tag generations, a v1.x ABI
lift -- **acceptable for a trusted dev Mac, and it belongs in the manual page
rather than being discovered later.**

**On the server side:** the design is indifferent to what serves the directory,
so the operator's own `npxf` is a legitimate Mac-side server rather than a
kludge to replace. What this track owns is the guest half (W3-a..c).

### W4 — run a graphical Linux binary under vivarium

**Greenfield: `docs/VIVARIUM.md` contains no occurrence of graphics, GL,
Wayland, DRM, framebuffer, mesa, vulkan, tapestry, weave, warp, GPU or surface.**
No prior art, no TODO, no declined-with-a-reason. Inserted here as a new arc.

How a Pouch program reaches the screen today: stock SDL2 2.32.10 plus a
Thylacine video driver (`usr/ports/sdl2/thylacine/`, ~1360 lines) speaking the
tapestry protocol -- which is **open/read/write/close on a 9P tree plus exactly
one special syscall**. That syscall is `SYS_WEFT_MAP`, and the decisive fact is
that it takes an **fd** and returns a **VA** with **no capability gate**
(`kernel/syscall.c:6568-6602` -- verified: `handle_get` -> `KOBJ_SPOOR` ->
`dev9p_priv` -> `Tweft` -> `weft_map_claimed`, no `CAP_` check). The *producer*
side is gated on `CAP_HW_CREATE` (`:6404`), so only a trusted server can offer a
share; a client merely claims one. **`SYS_WEFT_MAP` is already `mmap`'s shape.**

| Id | Stage | New kernel primitive? |
|---|---|---|
| **W4-1** | translate `mmap(MAP_SHARED)` on a dev9p fd to `sys_weft_map_for_proc`. Domain: `prot == R\|W`, `flags == MAP_SHARED` exactly, `offset == 0`, length within the burrow. Add to `vivarium_mmap_arms_disjoint` (`vivarium.c:1309`). | **No.** One T2 row + shell, ~60 lines. |
| **W4-2** | a `viv` manifest annotation (`org.thylacine.display`) binding `/srv/tapestry` into the container, mirroring `org.thylacine.net` (`usr/viv/src/main.rs:309`). | No. ~30 lines. |
| **W4-3** | port the existing SDL2 Thylacine driver to the Linux ABI, shipped as `libSDL2.so` in the bundle. Two mechanical changes: `t_weft_map(fd,0)` -> `mmap(...MAP_SHARED,fd,0)`, and the two dirfd-relative opens -> absolute (phenotype `openat` takes `AT_FDCWD` only). | No -- but needs a Linux-musl cross lane. |
| **W4-4** | hardware GL via the warp winsys -- the **same** mmap arm on `bo/<id>/map`. Needs no `CAP_JIT` (shaders compile to GPU ISA), unlike guest-side llvmpipe. | No. |
| **W4-5** | *only if the target set demands it:* AF_UNIX (**N-4**) + fd passing + a client-allocatable shared-memory object, then a Wayland shim. | **Yes** -- the only stage needing real new machinery. |

**Rejected, with reasons that still hold:** DRM/KMS ioctl emulation
(`GPU-DESIGN.md:2696` -- a strictly larger surface than writing the winsys, and
GEM/PRIME is a second redundant capability system under ours); X11
(`VISION.md:509`); `/dev/fb0` (**SDL2 has no fbdev backend at all** -- it would
serve only Qt-linuxfb / SDL1.2 / DOSBox).

**Open risks to settle before W4-1 lands:** whether `SYS_BURROW_DETACH` is
correct on a `VMA_FLAG_SHARED_IN` VMA (the documented teardown is the weave
fid's clunk); session granularity -- a bind gives **one tapestryd conn per
container**, so container processes co-own surfaces and container death retires
them, which is coherent but must be *decided*, not discovered; and whether
`dlopen` works under vivarium (it would widen W4-3's reach via
`SDL_DYNAMIC_API`).

### Cross-cutting — one fix, several workflows

| Id | Item | Serves |
|---|---|---|
| **X-1** | **`S_IFMT` mask** before the `mode & ~0777` gate in `vivarium_mkdirat_decide:829` and `vivarium_openat_create_decide:790`. busybox passes the full mode (`S_IFDIR\|0755`, `S_IFREG\|0644`, established by disassembly); the gate was written to refuse setuid/sgid/sticky and catches `S_IFMT` as collateral. Linux defines those bits as ignored here, so masking is exact, and the deliberate `07000` refusal survives. | W2-a, every archive tool |
| **X-2** | restage the alpine bundle with `/bin` links **relative** (`-> busybox`, currently 80 of 80 are absolute `-> /bin/busybox`), then mount it `MPHENO_LINUX` like `/viv/bin` (`joey.c:7115`, ~25 lines) and add to both path lists. Absolute targets re-anchor at the caller's root (`stalk.c:383-403`, I-28 working as designed), which is exactly why they resolve inside a container and ghost outside one. | W2-a, and the operator's standing "invoke alpine bins directly" ask |
| **X-3** | raise `INITIAL_HEAP_SIZE` (`alloc.rs:77`) from 4 MiB to 64 MiB. **Costs zero committed pages**: `burrow_attach_lazy` charges nothing at attach (`syscall.c:5468`), the fault charges one page at a time (`fault.c:635`), and `HoleList::new` writes a single 16-byte header. Also fix the doc comment, which cites `BURROW_ATTACH_MAX` (256 MiB, the *eager* cap) where the real bound is `BURROW_RESERVE_MAX` (1 GiB). | nora, every native program |
| **X-4** | **make panics say something.** `lib.rs:3269` is `fn panic(_info) -> ! { t_exits(1) }` -- info discarded, silent exit 1. Every OOM and panic in native userspace is currently indistinguishable from "the program did nothing". | all of them |
| **X-5** | ut: stop the prompt render erasing a partial last row (`line_editor.rs:803` emits `\r` + `\x1b[K` first), which eats the last line of any output lacking a trailing newline. | all of them |
| **X-6** | ut: print on spawn failure. `eval/stmt.rs:1490` sets `$errstr`, sets `$status=127`, and returns `Ok` -- nothing prints on the `Ok` path, so an unknown command is silent. | all of them |
| **X-7** | **`mkdir -p` was broken essentially everywhere -- LANDED.** It walks the chain from `/` and tried to create every ancestor, swallowing only `Error::Exists`; an ancestor that already exists inside a directory the user cannot write answers **PermissionDenied** instead, because the kernel checks the parent's write bit before noticing the child is there. So `mkdir -p a/b` from `/tmp` died trying to create `/tmp`, and from a home died on `/home`. Now benign whenever the component *is* a directory afterwards, whatever the errno said. Found by X-1's own E2E, which could not build its fixture. | all of them |
| **X-8** | **`tar -z` (compressed) -- LANDED @`de6ca24e`.** busybox spawns `gzip` via `vfork()` proper (`clone(CLONE_VM\|CLONE_VFORK\|SIGCHLD, stack=0)`, disassembled), which the clone decide declined. Served as a fork (option B, operator-voted; `LINEAGE.md` 3.1). `.tar.gz` create+extract round-trips through gzip (`abin-tar` legs g/h). **The four "plumbing rows" I predicted (pipe2/dup3/wait4/ppoll) were NOT needed** -- ground truth (disassembly + the passing E2E) showed busybox passes the already-admitted arguments (flags=0/options=0/rusage=0); the census declines of those numbers came from `viv-pheno-probe`'s edge-case probing, not tar's pipeline. No speculative widening landed. | **W2-a COMPRESSED half closed** |
| **X-9** | *(fidelity, small)* the kernel answers **PermissionDenied** where Linux answers **EEXIST** when a create targets an existing entry inside a non-writable parent (Linux's `filename_create` returns EEXIST from the lookup before the permission check). X-7 makes userspace robust either way; whether to match Linux's ordering is a separate call, and it trades an information disclosure (existence, to a caller without write permission) that Linux accepts. | fidelity |
| **X-10** | **command resolution: native-first + `viv <cmd>` to force the phenotype variant (design RATIFIED, operator 2026-08-31).** `/viv/abin` now puts ~304 Alpine busybox applets on PATH, colliding with native coreutils (`ping`, `nslookup`, and any native we add). Model: PATH order is native-first (`/bin` before `/viv/abin`, already so) -- a bare name resolves native if one exists, else the phenotype applet transparently, and `/viv/abin` being `MPHENO_LINUX` means the resolving mount also picks the ABI. To FORCE the phenotype variant when a native exists: **`viv <cmd>`** -- extend the launcher (the Plan 9 `9 cmd` / `toolbox run` / `nix run` idiom), NOT a bang suffix (leading `!` is rc's NOT operator + bash history-expansion; trailing `!` risks the same and says nothing). `/viv/abin/<cmd>` stays the unambiguous escape hatch; `bind -b /viv/abin /bin` the persistent reorder. Impl: `viv` grows a "resolve `<cmd>` from the phenotype PATH and exec in the CURRENT namespace" mode beside its existing bundle mode. | all workflows; sequence after the networking/threads line |
| **X-11** | **no `/bin/sh` in the login namespace.** git hands any shell-shaped helper command to its compile-time `SHELL_PATH` `/bin/sh`: the `file://` transport (`git-upload-pack '<path>'`, one quoted string), `mergetool`/`difftool` (POSIX scripts), any `!`-alias or hook. Measured 2026-09-01 by the git-shell witness: `git clone file:///home/michael/gs` -> `fatal: cannot exec 'git-upload-pack '/home/michael/gs'': No such file or directory` (the witness uses `--no-local`, which execs upload-pack directly). `/viv/abin/sh` (busybox ash, phenotype-by-location) is on PATH but not at the absolute path. Candidate: a pool symlink `/bin/sh -> /viv/abin/sh` -- the absolute target re-anchors at the root and the walk crosses the pheno-mount, so `execve("/bin/sh")` lands LINUX under Design D exactly as `/viv/abin/sh` does; a native program's `system()`/`popen()` would get the same ash. A namespace-layout call (the operator's): a native `sh` is the other answer. | W1 (file:// clones, hooks), W2 (configure scripts, `make` recipes that assume `/bin/sh`), every ported program that shells out |

### Arcs inserted by this pass

1. **Clang Thylacine ToolChain** (W2-b) -- if the `clang++.cfg` workaround is
   not enough, teaching clang's driver about `aarch64-unknown-thylacine` is an
   LLVM-patch arc, not a fix.
2. **U-8 namespace builtins** (W3-c) -- designed in `UTOPIA-SHELL-DESIGN.md`,
   never built; W3 cannot exist without it.
3. **Vivarium graphics W4-1..W4-5** -- a new arc; nothing in scripture yet.
4. **Native archive tooling** (`tar`, `gzip`) -- if we choose not to depend on
   busybox for W2-a permanently.
5. **Allocator reclaim** -- `SYS_BURROW_DECOMMIT` = 84 exists, is complete and
   correct (the `MADV_DONTNEED` analog), is reachable as `t_burrow_decommit`,
   and has **zero callers**. `linked_list_allocator` has `extend` and no
   `shrink`, so a native program's peak stays resident and charged for its whole
   lifetime. Go reclaims (`joey.c:5820`); pouch does not; native Rust does not.

---

## Stream 1 — VIVARIUM (the active arc)

Phase 8's fourth pole: run unmodified Linux binaries. Design + build arc:
`docs/VIVARIUM.md` (all four decisions resolved — fork = **C** hybrid, build-now,
declare-not-infer branding, names adopted). Task **#62**.

**V-4a-0 + V-4a-0b LANDED** — the two kernel prerequisites the build surfaced.
V-4a was specced as pure userspace; ground-truthing the Tier-1 file set against the
tree before writing the crate found that **two of its entries had no native source
at all**:

- **`/proc/self/exe`** — `struct Proc` carried no executable identity whatsoever
  (the Image cache is qid-keyed, the text Burrow anonymous, `format_cmdline` a
  stub). Fixed by `Proc.exe_path` + `/proc/<pid>/exe` (§6.5), pinning the #66 `Path`
  the exec resolver already held.
- **`self` itself** — `srv_peer_info` reported `stripes` (an opaque tag with no
  userspace pid mapping) and **no pid**, so a 9P server could learn which
  *principal* was talking to it but never which *process*. Fixed by
  `srv_peer_info.pid` filling the reserved slot in place (§6.6).

Both were the pull-forward default, not scope creep: §6.2's rule is that the
diorama renders **only** from natively-reachable sources — that is what makes I-43
structural — so a missing source is a *kernel* gap by construction, never a licence
for the diorama to invent or accept an answer. §6.7 records the lesson and flags
`/proc/self/cwd` + `/proc/self/maps` (both V-4b) as the same shape.

**V-4a is DONE** — `usr/diorama` on the ptyfs skeleton, joey-spawned with
`MAY_POST_SERVICE`, selftest-before-serve, read-only, serving Tier-1 `/proc`. It
mounts at `/dio`; joey creates the mount point but deliberately does **not** mount
it, because `self` resolves to the connection's peer — i.e. the *mounter* — so a
shared mount would report joey to every reader. Each client mounts privately,
which is also how V-7 will set up a container.

**V-4b-1 and V-4b-2 are DONE**: `/self/cwd` and `/self/maps`, each with its kernel
source. Both confirmed §6.7's "budget these as kernel + userspace", but neither for
the predicted reason — `cwd` needed no new kernel *state* (the Territory has
carried `dot_path` since LS-4), and `maps` inherited its lock-order argument from
`devproc_mem_walk_cb`, which had already established and audited
`g_proc_table_lock → vma_lock`. The refined lesson (§6.7): **grep for an existing
accessor and an existing lock-order precedent before budgeting either.**

`maps` also forced the first real "which layer speaks Linux" decision, settled in
§6.8: the kernel emits a Thylacine-native table, the diorama translates. Anything
else is phenotype leaking into the kernel.

**V-4b-3 is DONE**: the numeric `/proc/<pid>/…` dirs, the root pid enumeration,
and `sys/kernel/{ostype,osrelease,version,hostname}`. **Pure userspace this time —
kernel byte-unchanged** — and the reason is worth carrying forward: `/self` was
*always* a per-pid render with the pid supplied by the connection's peer rather
than by the path, so the pid had been a parameter from the start and per-pid was a
generalization, not a new mechanism.

Two design findings landed with it:

- **§6.9 — the fourth source.** `sys/kernel/ostype` reformats nothing; the answer
  *is* the phenotype. That is not a §6.2 violation (a constant carries no
  information about the system, so there is nothing to leak), but the distinction
  had to be written down or it becomes the loophole every later file is argued
  through. The rule: *derived from kernel state needs a native source; a constant
  declaring which ABI you are looking at is the phenotype speaking about itself.*
- **§7.1 — the V-7 pid-visibility obligation.** The diorama's pid view matches
  native `/proc`'s exactly (all-pids, Plan 9 posture), so there is no new
  authority — but a contained Proc seeing every host pid is a leak, and that leak
  is in native `/proc` + `/ctl/procs` first. Scoping the diorama alone would be
  theatre. Owed at V-7, against the native surface.

**V-4b-4 is DONE** — the *shape* half (§6.11), which turned out bigger than the
one file that named it (task #66). §6.2 governs where a value comes from; V-4b-4 is
the second question: **in what shape does the consumer expect to read it?** Linux
serves `/proc/{self,<pid>}/{exe,cwd}` as symlinks, we serve regular files whose
contents are the path, and the consumer (`getMainExecutable`) calls `readlink`
specifically. Thylacine has no symlink surface to grow, so the translation went
into the **phenotype** — the pouch boundary-line — exactly as V-4b-3 predicted.

The finding that made it larger: the seam had parked `readlink` at the `ENOSYS`
sentinel, and on a symlink-free system that is the *wrong* answer rather than an
absent one. musl's `realpath()` is a pure userspace resolver (it does **not** use
`/proc/self/fd`, contrary to the note the LLVM fork's patch carries) that calls
`readlink()` per path prefix and treats anything but `EINVAL` as fatal — so
**`realpath()` was broken for every path on the system, for every ported program**.
The truthful `EINVAL` repairs it whole with no `realpath` patch. Revert-probed:
with the general arm back at `ENOSYS`, `realpath("/proc/./")` fails in-guest with
errno 38, while `realpath("/")` still passes (no components to walk).

It also surfaced a kernel gap and a family behind it (task #67): the `/proc` apex
answered `-1` from `stat_native`, i.e. `EIO`, so `stat("/proc")` failed — fixed
here (the apex is a real directory and now stats as one). The rest was left tracked
because each member needed a per-qid posture decision across a whole Dev.

The LLVM fork delta is now *available* to drop, **not dropped**: that needs an LLVM
rebuild + the Clade gates, which belong to the track owning that fork. Recording it
as available rather than done is the correction V-4b-3 made to V-4a, applied to
itself.

**V-4b-5 is DONE** — the synthetic-Dev stat family (§6.12; task #67), three gaps
closed together because they are one question asked of three Devs: *what does this
synthetic object claim to be?*

`/ctl` and `/env` had **no `stat_native` slot at all**, so `stat()` on the
directory, `fstat` on any fd beneath it, `lseek(SEEK_END)`, and — by the §6.11
mechanism — `realpath()` of anything underneath, all failed with `EIO`. Both now
answer, and they answer *differently about size*, which is the interesting part: a
`/ctl` file is generated at read time (so a size measured at `stat` is stale before
the read, and 0 is the honest answer — Linux's `/proc/meminfo` convention), while
an `/env` value is stored (so its size is real, and `SEEK_END` lands on it).

devproc's modes carried **no file-type bits**, so `S_ISDIR("/proc/<pid>")` was
false and every POSIX walker that classifies before descending stopped there.

And a fourth found while writing the prover's regression rather than while
designing the fix: `parse_decimal` accepted **leading zeros**, so one Proc answered
to unboundedly many names — which meant native `/proc` and the diorama disagreed
about which paths exist, and coherence between the two is the point of §6.2. Linux
rejects them for the same reason; `"0"` stays legal (kproc).

**Self-audit caught the one that mattered most, before it shipped.** Reporting a
size for `/env` entries has a consequence beyond `fstat`: `exec_resolve_from_
namespace` gates only on `dev->read` and a non-zero size, so a real size makes
`exec("/env/FOO")` reach the REVENANT Image cache — keyed on `(dc, devno,
qid_path, …)`. Every other Dev's qid namespace is global, so a static `devno == 0`
still leaves that pair unique; **devenv's is per-Proc** (ids restart at 1 in every
`Env`), so two Procs' unrelated variables both reported `(0, 1)` and the cache
would serve one Proc another's bytes — an I-1 leak out of the one device whose
premise is that a Proc sees only its own environment. Fixed with a per-`Env`
`devno` stamped onto the walked Spoor. The identity was equally wrong before, but
nothing had asked: **reporting a field that was never reported is a claim, and the
claim has to be true before the report is added.**

Revert-probed in three boots: all three kernel tests fail on pre-fix code at
exactly the reverted assertion; the in-guest prover reaches its `/ctl`, `/env` and
`S_ISDIR` legs before failing at the padded-pid one; and dropping the `devno` stamp
fails `devenv.stat_native_shapes` at "the walked Spoor carries it".

**V-4b-6 is DONE** — `/self/environ` (§6.13; task #65 part 1). A new kernel
source `/proc/<pid>/environ` renders the per-Proc `Env` as Linux's flat
`NAME=VALUE\0` block. Two things the §6.7 prediction ("a renderer over an existing
group") could not have known from outside:

* it had to be **offset-aware** rather than format-and-slice. An `Env` holds up to
  64 x 4096 bytes against devproc's 2 KiB buffer, and the failure mode of
  format-and-slice is *silently dropping environment variables* — which reads to
  the consumer as never-set, not as an error. One call clamps at 8 KiB (the copy
  runs IRQs-off); the file does not.
* it is the **first devproc info file with a real read gate** — owner-or-
  `CAP_HOSTOWNER`, because `/env` is self-only by construction so nothing else
  discloses a peer's environment, and environment variables carry secrets by
  convention. Same posture Linux takes.

**The self-audit find:** a gate that keys on the READER cuts both ways, and the
second way is a leak. The diorama is SYSTEM, so the kernel would let it read any
SYSTEM Proc's environ and hand those bytes to a client of any principal — who
natively would have been denied. `/srv` is the shared boot registry re-grafted
post-pivot, so a user session can mount the diorama: reachable, not theoretical.
Fixed by serving environ under `/self` ONLY (sound by construction: the target is
the connection's own peer). Replicating the owner check against `peer.principal_id`
was rejected — it makes a component whose design property is having no policy into
a policy point, for a file no v1.0 consumer reads. The generalized rule, recorded
for V-4c/V-7: *before proxying a file, ask not only "could the client read this
natively" but "could the client read this natively FOR THIS TARGET".*

Revert-probed in four boots: all four kernel-side claims at once (gate/clamp/skip/
windowing) — each test fails at exactly its reverted assertion; the gate alone
(which also fails BOTH sched-gate tests, proving the shared-predicate extraction is
live); the diorama trim alone (selftest FAIL → the server refuses to post, boot-
visible); and the cross-principal fix alone (same boot-fatal signal). An earlier
probe attempt landed on `devproc_kill_authorized` instead — the pattern matched an
identical line — which proved the I-26 kill gate covered but left mine unprobed
until re-run.

**V-4b is CLOSED.** Its last two files are dispositioned rather than built, each
with its evidence written down (§6.14, §6.10) so that neither is a silent
omission:

* **`auxv` — weighed, and deliberately not built.** Zero live readers in the
  tree: every consumer takes the stack path (`getauxval`, or a hand-walk of the
  `_start` frame), and the one file containing the literal string — SDL2's
  `SDL_cpuinfo.c` — is compiled out twice over on aarch64 (`!defined(__arm__)`,
  and `HAVE_GETAUXVAL 1`). But an in-tree grep is weak evidence about a *compat*
  surface, so the argument that carries is structural: **auxv on the stack is a
  prerequisite of V-7, not an optional extra** — a Linux ELF bootstraps out of
  `AT_PHDR`/`AT_ENTRY`, so `viv` cannot launch a foreign binary without building
  one, and `/proc/self/auxv` is the fallback for a thread that never received an
  entry frame (`dlopen`'d into a foreign host; a sanitizer off the main path).
  That is the named trigger. Recorded with it: if it is ever built it must be a
  *retained kernel copy* (Linux's `mm->saved_auxv`), never a reconstruction —
  `AT_RANDOM` and `AT_PHDR` are per-exec pointers into the process's own stack and
  image, so a recomputed answer is wrong rather than stale, and a consumer
  dereferences `AT_RANDOM`.
* **`fd` — blocked on #66c**, the #926 handle-table lifetime restructure: a
  kernel chunk, not a Vivarium one. The diorama must not route around it.

**V-4c is RESCOPED before any of it was written** (§6.15, scripture-first —
the design-conversation pattern, landed as docs with no code). Ground-truthing
Tier 3 found that **§6's `/dev` bullet contradicts §6.1**: the diorama is
read-only (`h_write` → `E_PERM`, unconditional), and `/dev/null` is *defined* by
accepting writes. Native devdev already implements `null`/`zero`/`full`/`random`/
`urandom` correctly — `/dev/full` even fails writes, the right shape — so routing
them through the diorama would take files that work today and break them. The
rule that generalizes, recorded beside §6.2's: **a re-presentation that loses a
capability the native tree already has is a downgrade wearing a compatibility
label.**

A container's `/dev` is composed by **bind**, in its own territory — the same
mechanism `viv` uses at V-7 and joey already uses at boot. The entries devdev
lacks land where the question already lives: `/dev/ptmx` is **already done** in
the phenotype (PTY-3's redirect, whose patch says in its own words that
"`/dev/ptmx` is a compat symlink Thylacine cannot provide"); `/dev/std{in,out,err}`
and `/dev/fd/N` are `dup(N)` at the boundary-line; `/dev/tty` is a bind of the
container's own pts, which `viv` knows and a server would have to guess.

So **V-4c = a minimal `/sys` + the per-container mount wiring** (promoted from
afterthought to the substantive half) **+ the two Tier-1 stragglers** `cpuinfo`
and `stat` **+ the arc's focused audit**, which owes a close on §6.13's
deputy-authority rule as well as §6.2's no-new-authority property and §6.12's
file-identity claim.

Two supporting findings, both from the same grep pass:

* `/sys` is thin — the entire tree holds exactly **one** `/sys` path (SDL2's
  cache-line-size read), and it is a *soft* read whose failure is benign. But it
  is a **second tree**, not a subdirectory: the diorama's existing `N_SYS` nodes
  are `/proc/sys/kernel/…`, a different thing sharing a name. One server exporting
  two trees is `Tattach` with a different `aname` (Stratum's `ds:<name>` is the
  in-tree precedent), and `h_attach` currently **ignores `aname`** and always
  lands on `N_ROOT` — so that dispatch is the real work, and the per-container
  mount wiring wants it anyway since V-7 attaches twice per container.
  > **Half of that was wrong, and V-4c-1 corrected it** (§6.16). `/sys` is
  > indeed a second tree — but the aname route is **closed**, not merely harder:
  > `devsrv_open_connect` attaches a 9P-mode service with a hardcoded *empty*
  > aname, and `SYS_ATTACH_9P_SRV` (which does carry one) is byte-mode-gated and
  > rejects a 9P-mode conn for a sound reason. Serving `/sys` that way would
  > need a **new kernel ABI**. The answer was the one §6.15 had already chosen
  > for `/dev` one paragraph earlier: **bind**. `SYS_MOUNT` takes any readable
  > Spoor, subdirectory included, so the diorama serves ONE tree whose root is
  > the *world* with `proc` and `sys` as siblings — **no kernel change at all**.
  > Recurring lesson: ground-truth the mechanism before the table entry.
* `cpuinfo` and `stat` are Tier 1 by §6.3 but have **no in-guest consumer today**
  (`config.guess` is a host script; SDL2's is the `__ANDROID__` branch), and both
  are only *partly* sourceable: `MIDR_EL1` is not EL0-readable at all, and
  `ctxt`/`intr`/`processes` have no native source. That makes them a per-*field*
  §6.7 question with a third answer the doc did not have: for a line-parsed file,
  omitting a line and fabricating one are different failures, and "report 0" is
  fabrication with a plausible face.

**V-4c-1 LANDED** — one server, two trees, by bind (§6.16). The diorama root is
now the synthetic *world*; today's content moved to `/proc/…` and `/sys` joined it
as a sibling, carrying `devices/system/cpu/{online,possible,present}` plus one
`cpuN` dir per CPU. Every byte is sourced from `/ctl/cpu`, whose `cpus:` header is
the *declared* set and whose `offline` row marker (prowl-5 F2) is exactly Linux's
present-vs-online distinction — a gift from devctl having had to make that same
distinction for prowl. The prover reads the cpulists AND **binds `/dio/sys` at a
second path**, reading identical bytes through the new name: the composition V-7
depends on, proven rather than assumed. Both new selftest legs are revert-probed
(each fails the boot when broken). 1215/1215 · boot OK · 0 EXT.

Two things the build settled that the scripture had left open:

* **A bound subtree is genuinely sealed.** The worry was `..` — the server still
  records `/sys`'s parent as the world root, so could a container climb out into
  `/proc`? No: `stalk` resolves `..` by *popping its own trail* and never sends a
  `Twalk("..")`, so `<mount>/..` lands on the mount point's parent in the
  **client's** namespace. Same property that contains `..` at `root_spoor` for
  I-28, doing double duty.
* **The per-field question now has a THIRD instance** with an identical shape.
  `cpuN/cache/index0/coherency_line_size` reads `CTR_EL0`, which is EL0-trapped
  exactly as `MIDR_EL1` is (`SCTLR_EL1.UCT` clear in `INIT_SCTLR_EL1_MMU_OFF`).
  So `cpuinfo`'s MIDR, `stat`'s ctxt/intr/processes, and the `cpuN` contents all
  await **one** decision — omit, or give the kernel a source — deliberately made
  once rather than piecemeal. That is **V-4c-2**, with the per-container mount
  wiring; **V-4c-3** is the arc's owed focused audit (a merge gate).

**V-4c-2 groundwork (researched; it mostly dissolves the fork).** Grepping for
the sources first turns "one decision" into four exposures and one real question:

| field | source status (verified) | cost |
|---|---|---|
| `stat: processes` | **exists** — `g_next_pid` (`proc.c:386`) is a monotonic atomic from 1, bumped per `proc_alloc`, so `−1` is exactly Linux's forks-since-boot | expose |
| `stat: intr` | **exists but PARTIAL** — `kobj_irq_total_fires()` (`irqfwd.h:114`) counts only *forwarded* userspace-driver IRQs; timer/UART/kernel-internal are not counted | expose + widen, or label honestly |
| `stat: ctxt` | **material exists** — prowl's per-thread `nsched` (`thread.h:460`); no global aggregate | aggregate |
| `cpuN/cache` | **exists in-kernel** — `CTR_EL0` read at `mmu.c:962`/`:982` for I-cache strides, simply unexposed | expose |
| `cpuinfo` MIDR | **none** — `grep MIDR` over `kernel/` + `arch/` returns nothing | new kernel read + surface |

`intr` is the trap, and it is a shape the original note did not anticipate: a
source *exists*, so the danger is no longer an invented zero but a **real number
that means something narrower than the field it fills** — fabrication with a
plausible face arriving by the back door. Either widen the counter or do not
call it `intr`.

**V-4c-2a DECIDED** (§6.17, scripture-first, no code). Two things the research
found that the table above had not:

* **`cpuinfo`'s `Features` line is already sourced** — `g_hw_features.linux_hwcap`
  carries the arm64 *uapi* bit numbers for the exec auxv, so the CF-4 AT_HWCAP
  chunk already paid for the field that capability-detecting consumers actually
  parse. Only the MIDR *identity* quartet is unsourced.
* **A sixth instance nobody had counted**: `stat`'s `cpu`/`cpuN` jiffies line. No
  EL0-vs-EL1 time accounting exists anywhere in the tree, and — unlike every other
  field — it **cannot be omitted**, because the columns are positional, so a
  missing middle column is a wrong answer rather than an absent one. All non-idle
  time is reported as `system` under a *stated premise* (the pattern `maps`
  already uses); utilization, which is what essentially every consumer computes,
  is exactly right either way.

The rule covering all seven: **give the kernel a source, per-CPU, in the kernel's
own shape — and omit only what has no truth to tell.** Per-CPU is not a detail: it
makes both new counters free (each CPU stores to a line it already owns), it is
how Linux accounts them, and it is the only form that stays correct on a
heterogeneous board — where `MIDR_EL1` genuinely differs per core, which is why
Linux prints a per-`processor` block at all.

The exposures land as **columns on `/ctl/cpu`** (whose row already *is* the
kernel's native per-CPU description — a `/proc`-shaped kernel file would be §6.8's
phenotype leaking inward) plus one global scalar on `/ctl/sched`. Appending is
safe: prowl's parser takes three tokens positionally and ignores the rest, and an
`offline` row stays two tokens and is still skipped — which is also right for the
diorama, since Linux lists only online CPUs as `cpuN`. So no new `/ctl` file, and
the `kernel-base` precedent (`CAP_HOSTOWNER`-gated after the #57a F1 KASLR-slide
leak) is not engaged; the added values are hardware description and event counts,
carrying no address or layout secret.

**V-4c-2b LANDED** — the kernel sources. Two per-CPU counters (`gic_dispatch` for
`intr`, the `sched()` switch chokepoint for `ctxt`), a per-CPU `hw_cpu_ident`
recorded at bring-up (MIDR + the CTR_EL0 line size), and the hwcap word — all
surfaced as `/ctl/cpu` columns plus one `/ctl/sched` scalar. 1216/1216.

**And a correction the build produced**: the table above named `g_next_pid − 1`
for `processes`. Wrong — **`proc_total_created()` already existed** (`proc.c:599`,
a dedicated `u64`, ~10 tests), and is strictly better than the derivation. The
fifth exposure dissolved to *zero kernel code*. That is §6.7's own lesson
recurring, and the instructive part is how it slipped past a pass explicitly doing
that research: the grep asked **where is this value produced**, found `g_next_pid`,
and stopped — it never asked **is this value already published**. Producer and
accessor are different searches, and finding the first is exactly what stops you
running the second.

**V-4c-2c LANDED** — the diorama half: `/proc/stat`, `/proc/cpuinfo`, and the
`cpuN/cache/index0/coherency_line_size` leaf that lifts V-4c-1's deliberately-
empty `cpuN` dir. The cpu qid gained a *kind* above the index, so `cpu_qid(n)`
stays bit-identical and the subtree is an extension, not a renumbering.

Two things the build settled. First, `walk_child` already had an
`is_cpu_node(dir)` arm that returned early — a second arm appended lower in the
same function would have been **unreachable**, and the cache subtree would have
silently not resolved. Second, that existing arm hardcoded `..` → `.../system/cpu`,
which was correct only while `cpuN` was a leaf dir; left alone, `cache/..` would
have skipped a level. Both are the same shape: *V-4c-1's code was right for
V-4c-1's tree, and extending the tree makes previously-correct shortcuts wrong.*

**V-4c-2 is COMPLETE. V-4c-3 — the arc's focused audit — is next, and is a merge
gate**: the two new counters sit on audit-trigger surfaces (the scheduler switch
chokepoint and GIC dispatch), V-4b-1..6 and V-4c all landed on self-audit only,
and devproc/devenv are ARCH §25.4 trigger surfaces.

**V-1b is merge-ordered, not blocked-forever**: it wants `kernel/exec.c` +
`kernel/syscall.c`, which CL-4 also touched. Land `clade-cl4-wip` → `main` → then
`gfx-4` → `main`, and V-1b is clear. See `docs/MERGE-gfx-4.md`.

Later: V-2 (the total-and-stateless translation table) · V-3 (the supervisor
channel — **spec-first**, `specs/phenotype.tla`, a new wait/wake on the death
lineage) · V-5 sockets → `/net` (gate: `curl` fetches a URL) · V-6 signals
(audit-bearing) · V-7 `viv` (gate: an Alpine shell) · V-8 audit on I-43.

## Stream 2 — Halcyon (G-8/G-9), the graphics endgame

The last stage of the arc and of Phase 10. Four parts to G-8:

1. **The native TTF rasterizer** (`no_std`, AA + hinting). The natural first
   sub-chunk: self-contained, testable, and it is simultaneously the user's
   "Apple-quality fonts" ask *and* the prerequisite for the Acme tag bar. Scripture
   already calls it "foundational, not a nicety" (`TAPESTRY.md §14`).
2. The transcript pane (Helix-modal, selection-first scrollback).
3. Inline graphical surfaces in the transcript.
4. `halcyon.rc` (the policy layer).

Then **G-9**: Aurora-terminals-as-panes, video player, image display, the Halcyon
audit + `docs/HALCYON.md`.

**Recorded direction — the Acme tag bar** (`TAPESTRY.md §14`, from the user's i3
find): render the pane `tag` as text in the Stacked/Tabbed strip (today glyph-free
colored segments, per D7). The strip becomes a thin **renderer-drawn title surface**
so the compositor never grows a glyph path; the richer end state is Acme's
**executable tag line** — the title bar as a live command surface.

## Stream 3 — polish + debt (small, satisfying, interleavable)

- **cfg-6 — the `letterbox`/zoom-policy OSD row.** A standing user ask; small
  (`Comp::letterbox()` exists, the Display section already shows it info-only —
  needs a config key + a live row like Mode).
- **#39 — the Aurora host-test harness** (the netd-style
  `cfg_attr(not(test), no_std)` refactor) so the dormant vt/render/osd/config
  regressions actually run in CI. Highest leverage of the debt items.
- **#57 — the live-display (cocoa) border under-paint + lingering dead pane.**
  User-observed, no headless repro (the #31 class) — needs eyes on a live window.
- #43 (synthetic key-release on focus change — stuck key) · #44 (4K weave cap +
  multi-point pixel asserts) · #32 (`ls /srv` → "I/O error"; devsrv has no
  `.readdir`) · #13 (per-pts ownership + 0600).

---

## Stream 4 — notes / job control / PTY (the kernel line)

> **P1 (round B on 437213c4/5336c894, Fable 5) -- FIXED @663d4b64, pushed
> @790f6671; SMP 40/40. The follow-up round found the fix INCOMPLETE (a P1):
> CNBFRAME is honored by devpipe_write ONLY, but SYS_ATTACH_9P accepts any
> writable Spoor as tx, so a NON-PIPE tx (a /srv byte-conn) re-opens the
> extinction. LATENT (callers pass pipes). OWED: gate tx/rx on dev==&devpipe
> (memory/audit_663d4b64_closed_list.md). CLOSED @c2dfbf0b: a SYSCALL pipe-only
> gate (sys_attach_9p_ends_are_pipes; NOT at the init -- that broke the Dev-
> generic mock tests). SMP 40/40; pushed @9f86e5e5; a fourth Fable round running:** the pipe (spoor) 9P transport BLOCKS inside `devpipe_write`
> while `client_send_flow` holds `c->lock` -- an unprivileged multi-threaded
> container can EXTINCT the box (a `#360` lock-across-sleep). `437213c4` made
> `SYS_ATTACH_9P` over a pipe pair the diorama transport = the Phase-5 spoor
> transport's FIRST production consumer under the shared `p9_client`, which was
> written for a NON-BLOCKING (EAGAIN) transport. Fix: make `spoor_transport_send`
> non-blocking (return `P9_TRANSPORT_EAGAIN` on a would-block) so the existing
> pump/park drops `c->lock`. `memory/bug_spoor_transport_lock_across_sleep_
> extinction.md`. AUDIT-BEARING + SMP gate + a follow-up round. **This outranks
> every item below it (stewardship: a soundness threat outranks chunk
> completion).**


The line the header names: the EL0-return tail's note dispatch, the STOP
class, the tty family, the pts job-control seam, and the tests that construct
their states. It runs the full bar (suite + SMP gate + pty specs + LS-CI).

**Landed (newest first, 2026-08-17 back to aux#240):**

- `277b02cc` -- the console TX ring pushes UNITS (item 4 below; ARCH 23.5.2
  UNIT ATOMICITY; closes #79) and `920bbfca` -- the d3a11c8e + 4df51c30 audit
  close (item 1 below; a fork() from inside a handler carries the handler
  snapshot). Bar over `277b02cc`: SMP 40/40, LS-CI 33 PASS + 2 SKIP; pushed.
- `7580c1f7` (+ the ccb597b8 audit close `56b5a412`) -- SIG_IGN discards a PENDING signal at the INSTALL (POSIX
  2.4.3 / Linux `flush_sigqueue_mask`): `notes_discard_name` (mask-blind,
  per-class latch drain, `kill` refused) called by the phenotype `rt_sigaction`
  shell after the store whenever the new disposition ignores; `notes_post`'s
  disposition read moved under `q->lock` so no stale ignored note survives --
  the EL0 tail's delivery-time SIG_IGN arm (the open item below, "reached by
  nothing") is now defense-in-depth by construction rather than an
  unconstructed mechanism. Found while designing it: the deferred discard was
  observably WRONG for `pending -> SIG_IGN -> handler -> unblock` (Linux fires
  nothing; the tail ran the handler). Unit `notes.discard_name_purges_pending`
  + viv-pheno-probe L205-L216 (in-guest, deterministic via the reader-less
  fd 0; L215 is the install-vs-delivery leg, RED before).
- `c62eb738` + `ccb597b8` -- pty-4's burned retry ROOT-CAUSED by the 11173762
  probe on its first miss and FIXED: both ldiscs zeroed the canonical assembly
  on every mode write (LS-8b F1 "TCSAFLUSH"), so type-ahead between a job's
  last output and ut's PROMPT-mode re-arm was cooked-echoed then dropped,
  partially (`sle` gone, `ep 30` ran). Now a write clearing ICANON DELIVERS the
  pending line (Plan 9 rawon / Linux n_tty); new `rx_drop_modeflush` counter;
  `cons.cook_mode_flip_delivers`, ptyfs selftest e1-e3, and a DETERMINISTIC
  type-ahead leg in pty-4 (3/3 red under the old posture).
- `93a91c6c` -- the `c8ab2744` audit close (Fable 5 round: 0 P0 / 1 P1 / 1 P2 /
  2 P3, ALL pre-existing three lines above the audited arm). F1 [P1]: both
  class scans (terminate + STOP) now gate every hit per note on
  `notes_proc_default_applies(p, name)` -- the terminate scan was phenotype-
  blind and any-index, so a Linux guest died of a CAUGHT `tty:hup`/`interrupt`
  queued behind a `SIG_DFL` candidate. F2 [P2]: a `SIG_DFL` `pipe` on
  PHENO_LINUX was consumed by NO arm (no native latch, #237) and sat as the
  dispatcher candidate for life; the phenotype branch now `exits()` on a
  `SIG_DFL` terminate-default candidate (`viv_signote_default_is_terminate`).
  F3: the consumer's dead drain call deleted; F4: three "never queued"
  contract sentences reworded. Unit `notes.class_scans_read_phenotype_sigtab`
  + L-6c legs J/K/L -- whose POSITIVE CONTROL (K) caught a second bug on its
  first boot: viv `fcntl(F_DUPFD)` answered EMFILE for a CLOSED fd, ash's
  `N>&M` probe aborted every `3>&1`, and J/L passed vacuously on an empty
  capture. Fixed (EBADF/EMFILE split; `vivarium.fcntl_dupfd_errnos`).
- `4525023a` -- the change-of-watch pair + Stop hook + launcher imported from
  main (aux's first self-compaction followed).
- `11173762` -- LS-CI failure-time state probe (`::lc_fail_probe`); pty-4 arms
  it so its next burned retry says INPUT vs OUTPUT vs lost-^Z from the log.
- `3a7f50f1` -- `mask note 'tty:*'` masked NOTHING (parser had no tty arm);
  prefix -> `NoteClass::Tty`; u-job-test 15c pins it + reads the kernel mask.
- `ffb8f0ab` -- `notes.masked_susp_stops_at_delivery` observes -> tears down
  -> asserts (an early-return assertion left ALIVE linked Procs and hung the
  next test's `wait_pid` loop).
- `c8ab2744` -- the deferred-^Z stop arm was reached by NOTHING (an
  unconstructed state); `/susp-mask-child` + jc-probe `maskstop` construct it;
  reading the arm found a P1 (decided class-filtered, consumed class-blind: a
  queued `child_exit` destroyed) -> `notes_stop_dequeue_locked`.
- aux#254 (sigtab UAF at exec), aux#253 (self-kill on a full queue), aux#251 +
  aux#252 (phenotype-blind catchability gate; the STOP class had no
  delivery-time reader), aux#247, aux#240 (`susp_stop_armed` freshness).

**Open, in order:**

1. ~~AUDIT ROUND OWED~~ RAN + CLOSED 2026-08-17 (Fable 5, 0/0/1/6): F1 -- a fork()
   from INSIDE a handler got no copy of the kernel-side handler snapshot (only
   the mask), so its rt_sigreturn was refused; the snapshot now crosses fork
   with the mask (probe L233-L236). Six P3s closed; enqueued from the
   observations: `Proc.socktab` not cloned at fork (LINEAGE, the fork half of
   the dup3 note), the handler mask discipline (sa_mask|sig never applied;
   sigreturn does not restore the mask), `pty.tla` CookSignal's echo arm vs the
   ldiscs. `memory/audit_d3a11c8e_closed_list.md`. (Formerly: on the fork/exec
   signal-state chunk (proc.c rfork +
   exec, vivarium.c helpers; the Notes + LINEAGE rows) and, lighter, on the F5
   ISIG-discard change (cons.c/ptyfs, LS-8 + ptyfs rows) -- ask. The
   `7580c1f7` round RAN and CLOSED 2026-08-17: Fable 5, 0/0/0/4, mechanism
   sound; `audit_7580c1f7_closed_list.md`. The `ccb597b8` round RAN and CLOSED 2026-08-17:
   Fable 5, 0/0/2/6, all on the new drop site's witness -- positive controls
   in both ldiscs, ptyfs's own `drop_modeflush`, the report line names the
   site, pty-4's leg gained an ARMED witness; `audit_ccb597b8_closed_list.md`.)
   Also owed at some point: an explicit flush verb (POSIX TCSAFLUSH /
   tcflush) -- pouch's TCSETS/SW/SF now all behave like TCSANOW.
2. ~~F5 vote~~ VOTED + LANDED (POSIX: an ISIG char discards the pending line
   in both ldiscs; PTY-DESIGN `e69e9baf` + the impl commit; the PTY-3 probe's
   old `xy\n` expectation went red on the first boot and was updated).
3. ~~exec resets SIG_IGN + mask~~ VOTED + LANDED with the fork half (task #127
   both halves; scripture `c484a7d1` + the impl commit): fork copies the sigtab
   + mask into the child, exec keeps SIG_IGN + mask on the phenotype.
4. ~~THE CONSOLE TX RING IS BYTE-ATOMIC, NOT MESSAGE-ATOMIC~~ **LANDED 2026-08-17**
   (ARCH 23.5.2 "UNIT ATOMICITY"; every producer pushes a UNIT under one lock
   hold -- the caller-stack `cons_diag_line`, the echo unit, the writer's staged
   chunk; closes #79; three tests + S1-S3; the LS-8 row addendum). Formerly:
   (handed to aux by
   main 2026-08-17; `memory/bug_console_tx_ring_byte_atomic.md`): the kernel's
   `cons_diag_puts` (the #126 non-blocking IRQ-safe emitter -- per BYTE under
   `g_cons_tx.lock`) and a userspace `SYS_PUTS`/`cons_output_write` (under the
   P1-F writer ROLE, but still per-byte into the same ring) interleave char by
   char (`ttaappeessttrryydd` on thyla-pi); any gate anchored on a console line
   printed near a kernel diagnostic burst can go falsely RED. Fix shape: a
   bounded per-MESSAGE push (`cons_tx_push_bulk` under ONE lock hold; the diag
   side drops the whole message when it does not fit -- the echo disposition,
   never spin; the role side pushes what fits under one hold and room-waits for
   the rest -- so a diag can land BETWEEN two of a writer's chunks but never
   inside one). Design point to settle first: whether the diag path can take the
   writer role like the banner does (#152 `cons_kernel_writer_begin`) -- it
   cannot (IRQ context; the role sleeps), so bulk-push is the honest floor and
   "anchor lines are short (< the ring's free space)" the residual rule.
   kernel/cons.c is the LS-8 audit row: audit round + SMP gate. Same family as
   the OPEN extinction-vs-peer tear (vault seam) and IPI_HALT.
5. **#237 stays open and is now sharper**: the phenotype answers SIG_DFL
   SIGPIPE for its own Procs; the NATIVE `pipe` note still carries no latch,
   so a native program that writes to a closed pipe with no handler and no fd
   reader keeps a stranded `pipe` note -- a Plan 9 ABI decision (signoff).
   Researched option set in `memory/design_237_pipe_note_default.md` (Plan 9
   kills; Rust-std/pouch mask; the Go port has no note handling at all):
   recommend terminate-for-real + libthyla-rs/Go-port startup masks.
6. **LANDED 2026-09-01 -- `Proc.socktab` COPIED at fork + ALIASED on dup/dup3/F_DUPFD (the vote below, option A; was: NOT cloned at fork)** (the d3a11c8e round, seen in passing;
   `memory/bug_socktab_not_cloned_at_fork.md`): a phenotype child forked with an
   open socket fd inherits the HANDLE but no `(proto,N)` row -- the fork half of
   LINEAGE.md:691's dup3 note; fork-per-connection servers are the L-6c
   population. Own chunk: `socktab_clone_into` next to the sigtab clone + a
   probe leg + the VIVARIUM row.
   **Re-derived 2026-08-17 (`memory/design_socktab_across_images.md`, VOTE
   OWED -- VIVARIUM 5.5.2 states "not rfork-inherited" as design):** a
   refcounted ENTRY cannot carry the per-table ctl->data handle swap, so it
   reproduces Linux no better than a copy; Plan 9 APE's own posture IS a
   per-process copy, and every fork shape that occurs (accept-then-fork,
   prefork accept) works under one. Recommend COPY at fork + lift dup3/F_DUPFD
   to the same alias rule + record the socket OBJECT as the faithful shape.
   Found alongside, same chunk (both verified in the tree): **(6b) exec leaves a
   STALE entry** -- `handle_close_on_exec` closes a close-on-exec socket handle
   without a socktab drop, and `fcntl(F_SETFD, FD_CLOEXEC)` is a served row
   (musl 1.2.5's `socket(SOCK_CLOEXEC)` fallback issues exactly it), so the
   exec'd image's next fd-creating call inherits (proto, N) -- the "dial verb
   to a stranger" class the V-5 header names as the sharpest this table can
   have (`memory/bug_socktab_stale_entry_at_exec.md`); a bug under every
   posture, no vote needed. **(6c) the SOCK_NONBLOCK refusal is defeated by
   musl** -- EINVAL is exactly musl's fallback trigger; it retries without the
   flag and IGNORES the failing `fcntl(F_SETFL, O_NONBLOCK)` (unserved), so the
   guest holds a BLOCKING socket it believes non-blocking, the very failure the
   refusal's comment claims to prevent (`memory/bug_sock_nonblock_refusal_
   defeated_by_musl.md`; a V-5 design call: a loud non-retried errno vs
   serving non-blocking /net I/O).
7. ~~Handler mask discipline on the phenotype~~ **LANDED 2026-08-17** (the
   handler-time mask is Linux's: mask|sa_mask|sig via `vivarium_handler_mask`,
   the phenotype's sigreturn restores `note_saved_mask`, the fork copy carries
   it; +2 unit tests, probe L237-L244, SM1-SM3; hash in the status row).
   Formerly (`memory/bug_handler_mask_
   discipline_phenotype.md`): sa_mask|sig is never applied while a handler
   runs (N-3's blanket guard stands in), and `notes_noted_restore` does not
   restore `note_mask` (a handler's rt_sigprocmask persists past sigreturn;
   Linux restores uc_sigmask). Permissive-direction divergences; one small
   chunk + a probe leg. Also `pty.tla` CookSignal echoes the signal char while
   neither ldisc does -- spec vs impl on one arm, decide at the next PTY touch.
   VOTE-FREE (POSIX fidelity, permissive direction) -- the next chunk while the
   #237 / socktab votes are pending.
8. **R5-F9 under the phenotype -- TO VERIFY** (`memory/bug_r5f9_longjmp_wedge_
   phenotype_exposure.md`): busybox ash's `raise_interrupt` longjmps OUT of the
   SIGINT handler when interrupts are enabled (dash lineage; it unmasks all
   signals first, so the mask is fine), and the kernel-side `in_handler` latch
   then never clears -- registered v1.x against pouch programs, but the
   phenotype population is every musl-static shell. One VM experiment settles
   it (^C at the prompt / inside `read` / inside `wait`, then ^C a job); if
   real, a P1 for interactive phenotype shells that needs an abandoned-frame
   rule or a per-thread stack of save blocks (design; vote).
   **Blocked first by item 9** (found while building the experiment): the
   interactive `viv run` never ran a container at all. **RAN 2026-08-18
   (3/3, after items 9 + the ^C mask): INCONCLUSIVE on the wedge, conclusive on
   item 11** -- after ash's prompt (blocked in read) a ^C produced NOTHING for
   10 s, twice; the next typed line was then discarded (`^C`, newline,
   reprompt AFTER the line): the SIGINT was delivered only when the read
   completed. Re-run once item 11 lands (the wedge witness needs a promptly
   delivered first ^C).
9. **An interactive `viv run` never ran the container -- LANDED (pending its
   bar) 2026-08-17** (`memory/bug_viv_interactive_container_no_stdio.md`):
   the console line `viv: spawn /bin/diorama` in every R5-F9 log is viv's
   ERROR path -- viv requested `MAY_POST_SERVICE` for its per-container diorama
   (which posted the fixed `/srv/viv-dio`) and nothing past login holds the
   bit (login confers CONSOLE_OWNER on ut; ut confers nothing), so
   `spawn_perm_grant_check` refused; every boot `viv` was joey-spawned WITH
   the bit, so no gate ran the interactive path. The V-7 commit body listed
   the seam; nobody enqueued it. Fix: the diorama channel is a PRIVATE PIPE
   PAIR + `SYS_ATTACH_9P` (Plan 9's mount(fd); the Phase-5 stub-driver
   transport's first production consumer) -- no name, no privilege, no
   collision: concurrent containers moved OUT -> IN (VIVARIUM 7.2.1), the V-8
   F3 attach gate became structural (the joey #101 leg -> the `viv-channel`
   leg, two spawns one variable apart; the V-7 leg runs TWO probe containers
   concurrently), joey's boot `viv run`s pass no perm bits (they now run the
   interactive path). Userspace-only (viv, diorama, joey, libt/libthyla-rs
   wrappers); kernel byte-unchanged. Rejected: widening `MAY_POST_SERVICE` to
   user commands (one shared boot registry -> name squatting). Reference:
   `docs/reference/145-vivarium.md` "The diorama channel is a private pipe
   pair". **Residual, OPEN (kernel, Pipe audit row):** `devpipe_write` posts a
   `pipe` note to the WRITING Proc on a dead reader, and the kernel 9P spoor
   transport writes in the syscalling Proc's context -- a container Proc that
   touches `/proc` after its diorama died (an orphan outliving its runner; a
   diorama crash) gets a SIGPIPE-shaped note where the /srv transport gave an
   error; fix = a `MSG_NOSIGNAL`-shaped kernel-internal transport write (Linux
   trans_fd writes from a workqueue). Also OPEN, seen once, unreproduced: two
   ^C at a `ptyhost`ed ut's IDLE prompt, then the next command echoed but not
   executed -- REPRODUCED, see item 10.
   **LANDED `437213c4`** (boot: 1413/1413, `V-7 viv-probe (containered, x2
   concurrent) PASS`, both `viv-channel` lines, V-1b/L-6c/D-5 PASS; LS-CI
   viv-run PASS attempt 1). **Follow-on, same watch (2026-08-18):** the first
   ^C at the interactive ash's prompt KILLED viv and its diorama (the pts's
   `interrupt` reaches the whole fg pgrp; a native Proc with no handler dies of
   it, LS-5), orphaning the shell into a terminal it then shared with the
   outer ut (`memory/bug_viv_dies_on_ctrlc.md`) -- viv masks `interrupt` (tty
   family unmasked so ^Z stops it with the container), the diorama masks both
   families; viv-run.exp gained the ^C leg (`uname -s | tr` -> LINUX after ^C).
10. **Two ^C at a `ptyhost`ed ut's IDLE prompt lose the next command line**
   (`memory/bug_hosted_ut_double_ctrlc_idle.md`; REPRODUCED 2026-08-18 by
   `scratchpad/r5f9/ctrlc-idle.exp`: outer-cc=1 inner-c=1 **inner-cc=0**
   recover=1 -- one ^C is fine, the console ut is fine, an extra Enter
   recovers). The render suggests the hosted ut's `interrupt` arrives LATE and
   its line-discard eats characters of the NEXT line. Aux's own line
   (notes/job control/PTY): reproduce, root-cause (ut's hosted-session poll set
   / notes-fd wake / ldisc post order), fix, adopt the scenario into LS-CI.
   CONFIRMED (read-only, 2026-08-18) the same mechanism as item 11: the hosted
   ut's fd 0 is a non-QTPOLL pts slave, `dev9p_poll.c:288` returns POSIX
   always-ready for it, so the hosted ut is functionally blocked in `read(fd0)`
   = a dev9p Tread at idle (its poll returns instantly). The pts ISIG posts
   `interrupt`; the dev9p read does not unwind (item 11) -> the ^C lands with
   the next typed line and the deferred `editor.reset()` eats it. Item 11's
   note-interruptible-wait fix (the #90 frame-atomic 9P reader) closes item 10;
   do NOT fix it separately. NB: option B (pts/cons-ldisc-only unwind) does NOT
   close this -- the wait to unwind is the dev9p CLIENT read of the pts slave.
11. **A CAUGHT note does not wake a thread blocked in a syscall wait -- VOTE
   OWED (kernel notes design)** (`memory/design_caught_notes_do_not_interrupt_
   waits.md`; measured 3/3 + read in code): `thread_die_pending` is the only
   sleep-unwind predicate and fires for group death and the UNCAUGHT terminate
   latches only, so a handler-having (or notes-fd) program blocked in read()
   gets its ^C only when the read completes -- after the user's next line,
   which the handler then discards. Plan 9 (Eintr) and Linux (EINTR /
   ERESTARTSYS + SA_RESTART) both interrupt the wait. Fork: (A) note-
   interruptible waits (extend the #811 predicate with "a deliverable note is
   pending"; native -EINTR [no `T_E_INTR` in errno.h -- an ERRORS.md ABI row,
   signoff]; phenotype restart per SA_RESTART; the 9P client's blocked read
   unwinds frame-atomically per #90) -- RECOMMENDED, audit-bearing (Notes +
   Pipe + 9P client rows + SMP gate); (B) a Dev-level unwind for pts/cons reads
   only; (C) v1.x. Retires the "late ^C eats the next line" family (items 8,
   10) in one move.
12. **On the KERNEL CONSOLE a container never receives ^C after 5336c894 -- LANDED a2870706 2026-08-20**
   (self-found 2026-08-18 before the 437213c4+5336c894 round; the fix + the
   discrimination-proven alpine-trap regression + the holotype round) (`memory/
   bug_console_ctrlc_swallowed_by_viv_mask.md`). The serial console has no job
   control: `env.job_control` is None there, the console's ^C is OWNER-routed
   (`proc_console_post_interrupt` -> the session `ut`), and `ut` FORWARDS the
   `interrupt` by pid to its foreground children (`drain_fg_wait_notes`,
   libutopia stmt.rs). `viv`'s new mask swallows that forward, so the container
   sees nothing (before 5336c894 the forward KILLED viv and orphaned the
   container -- destructively wrong; now silently wrong). The pgrp fan that makes
   the mask right exists only on a pts. Fix candidates: viv self-manages (opens
   its notes fd, parks on poll{notes} + WNOHANG reaps) and FORWARDS `interrupt`
   to the container entrypoint when the note did not come from the kernel's pgrp
   fan (docker's --sig-proxy) -- the mask then goes; or the console grows a
   fg-pgrp fan (kernel, LS-5's owner routing generalized). Regression: an LS-CI
   scenario on the CONSOLE (not ptyhost) with a `trap 'echo CAUGHT' INT` ash.

---

## Recommended sequencing

1. **V-4a** now — unblocked, specced, has a real consumer.
2. **The merge** when the main track is ready (`docs/MERGE-gfx-4.md`), which
   unlocks **V-1b**.
3. **cfg-6** as a warm-up whenever a short chunk fits.
4. **G-8's TTF rasterizer** as the next *big* pick after Vivarium's foundation is
   in — or ahead of it if the user prefers the visible win.

Vivarium and Halcyon are both large and both sit in the endgame beside `v1.0-rc.1`
(`ROADMAP §11.5` keeps the fallback: v1.0-rc ships without either if neither
converges). They can proceed in either order; Vivarium is the one currently moving.

---

## Coordination with the main track

**Merge round 1 is DONE** (2026-07-27): `gfx-4` merged into local `main` at
`15edb01e` + the `de451566` pouch O_APPEND restore. The pouch series collision the
handoff predicted was real and was resolved there. **Not pushed** — `origin/main`
is still `b0bf63f2`.

Two things carry forward, both in `docs/MERGE-gfx-4.md` (rewritten as a round-2
handoff):

- **Five `gfx-4` commits landed after the merge point** (`b7df5b21..5af01124`),
  including both VIVARIUM kernel prerequisites. Round 1's analysis said
  `struct Proc` would auto-merge because V-1a's `phenotype` fit the tail pad — that
  is no longer true: V-4a-0 grows it 352 -> 360. The size assert is the drift
  detector, so a bad merge fails the build loudly.
- **Clade CL-4 never landed.** The handoff advised `clade-cl4-wip` -> `main`
  first; in the event `gfx-4` went first, so CL-4's four commits still merge on top
  of the gfx-4 kernel changes. An inconvenience, not a defect — but it means
  `kernel/syscall.c` now has a three-way overlap (main-via-gfx-4, CL-4, and
  V-4a-0b's small `pid` out-param thread-through).

**Consequence for V-1b**: it is *still* best sequenced after CL-4 lands, for the
original reason — CL-4 touches `kernel/elf.c` + `kernel/syscall.c`, which is
exactly where V-1b's syscall-entry phenotype branch goes. V-4a (the diorama crate)
is pure userspace and has no such constraint, which is another reason to do it
first.

- The aux track has touched kernel files (cfg-3's `srv_peer` stamp, V-1a's
  `Proc.phenotype`, V-4a-0's `exe_path`, V-4a-0b's `srv_peer_info.pid`); it is not
  `usr/`-only. Check the main worktree's dirty state before editing shared kernel
  files.
