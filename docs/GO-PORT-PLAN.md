# Porting Go to Thylacine — the design + a cookbook for the main agent

**Status: adopted (2026-06-19) as a binding design reference via the aux-merge
intermezzo. Scope: a post-REVENANT (#231) Phase-8 arc — Go cross-compiled
binaries exceed the 1 MiB `SYS_SPAWN_BLOB_MAX` slurp cap, so REVENANT's
file-backed exec is a prerequisite; the one kernel ask (a lazy / demand-paged
`BURROW_ATTACH` flag for Go's `sysReserve`) co-scopes with REVENANT. ADDITIVE to
the #67 / RW-13-D3 on-system toolchain decision (clang/lld/make/git via Pouch) —
Go is a second toolchain, not a replacement.**

Aux-authored design legwork (2026-06-17), researched **read-only against the
latest `main`** (net-3b era) via git — NOT a merge/rebase (aux is pushed + has
diverged from main on `usr/nora`; a sync wasn't needed for this and would force
a nora reconciliation — flagged separately). The kernel-touching pieces are
main-track; this is the *thinking work* so the main agent can write the literal
cookbook and execute it. Cross-check every Go-internals claim against the real
Go source + the upstream "Porting Go" guide when cooking — Go internals drift
between releases; pin a Go version first (see §7).

---

## 1. Thesis: Thylacine is the most Go-portable target imaginable

A Go port to a new OS is "implement a new **`GOOS`**" — the runtime's OS layer +
the syscall stubs — with `GOARCH` already done. For Thylacine, **`GOARCH=arm64`
is mature and free** (Google's production ARM64 backend), so the entire effort is
the `GOOS`.

And the `GOOS` almost writes itself, because **Thylacine is Plan 9-shaped at
every level Go's `plan9` port already targets**:

| Go needs… | Plan 9 / `plan9` GOOS | Thylacine | Verdict |
|---|---|---|---|
| process creation w/o Unix `fork` | `rfork` | `SYS_SPAWN_FULL_ARGV` | borrow plan9 model |
| signals | **notes** | **notes** (`SYS_POSTNOTE`) | same concept, same name |
| networking | **`/net`** (clone/ctl/data) | **`/net`** (clone/ctl/data, netd) | near-drop-in |
| everything-is-a-file / namespaces | 9P + per-proc ns | 9P + per-Proc Territory | same |
| process control | `/proc` | `/proc` | same |

So the **net package, the signal layer, and `os/exec` lean directly on
`plan9`-Go**, which speaks `/net/tcp/clone`+`ctl`+`data`+`connect`/`announce`
already (confirmed: `docs/reference/121-netd.md`, `NET-DESIGN.md` §2/§3 — the
tree is literally the Plan 9 shape, smoltcp behind it).

But Thylacine is a **hybrid**: it *also* exposes Linux-flavored primitives that
are *cleaner* than Plan 9's for the runtime core:

| Go runtime core | Linux `GOOS` | Thylacine | Use… |
|---|---|---|---|
| scheduler sync (mutex/note/sema) | `futex(2)` | **`SYS_TORPOR_WAIT/WAKE`** (a real futex) | **linux model** |
| OS thread (M) creation | `clone(2)` | **`SYS_THREAD_SPAWN`** (entry,sp,arg,tls,ptid) | **linux model** |
| heap memory | `mmap`/`munmap` | **`SYS_BURROW_ATTACH/DETACH`** + `burrow_map` | **linux model** (with one caveat, §4.1) |

**So the plan is a deliberate hybrid GOOS:** structure + net + signals + exec
from `plan9`; scheduler-sync + thread + memory primitives from `linux`. Call it
`GOOS=thylacine`. This is *far* more tractable than LLVM/Rust precisely because
it's bounded, well-referenced (two existing GOOS ports to crib from), and needs
**no JIT** (Go is AOT — the W^X/`CAP_JIT` story is irrelevant here).

> **Language features are version-bound, NOT port-bound — read this before
> worrying about "does plan9 have generics?"** It does, and so will `thylacine`.
> Generics (1.18), range-over-func, the current stdlib, and every compiler/
> language nicety live in `cmd/compile` + the frontend + `types2`, which are
> entirely **GOOS-independent**. "Crib the `plan9` GOOS" means crib the *OS-layer
> structure* (net/signals/exec patterns) — it does **not** mean inherit an old
> Go. **Fork the LATEST stable Go** and write `GOOS=thylacine` against it; the
> language + stdlib come from that pin, so `thylacine/arm64` has generics for
> free, exactly as mainline `plan9/arm64` does on current Go (plan9 rides the
> current release). The only things mainline `plan9` actually *lacks* are
> tooling/runtime, never language: **cgo** (we're `CGO_ENABLED=0` anyway),
> **`-race`** (a host-side dev tool — cross-compile + race-test on `linux/arm64`,
> deploy to Thylacine), and the simpler **netpoller** (§4.3, already staged).
> None touch the language surface.

---

## 2. The GOOS surface — the concrete file list

A new `GOOS=thylacine` is (mirroring the `plan9` + `linux` file sets; names
indicative — verify against the pinned Go tree):

**`src/runtime/` (the bulk):**
- `os_thylacine.go` — `osinit`, `mpreinit`, `minit`/`unminit`, `newosproc`
  (→ `SYS_THREAD_SPAWN`), `exitThread`, `getproccount`, `goenvs`, semaphore
  glue, `usleep`, `osyield`.
- `sys_thylacine_arm64.s` — the raw `svc` syscall stubs (one `svc #0` shim per
  `SYS_*`, args in x0..x5, number in x8 or the Thylacine ABI register — verify
  the kernel's SVC ABI in `arch/arm64/exception.c`), plus `clone`/`tstart`
  thread-entry trampoline, `sigtramp`, `nanotime`/`walltime` fast paths.
- `defs_thylacine_arm64.go` — constants: the `SYS_*` numbers (from
  `kernel/include/thylacine/syscall.h`), note/signal constants, timespec layout
  (`struct t_timespec { i64 tv_sec; i64 tv_nsec }`, 16 B — confirmed), the
  `t_stat` layout, errno values (`kernel/include/thylacine/errno.h`,
  POSIX-aligned).
- `lock_futex.go` reuse — Thylacine has a real futex (torpor), so use the
  **`futex`-based** `runtime.lock2`/`unlock2`/`note*` (NOT plan9's `lock_sema`).
  `futexsleep(addr,val,ns)` → `SYS_TORPOR_WAIT(addr, val, timeout_us)`;
  `futexwakeup(addr,cnt)` → `SYS_TORPOR_WAKE(addr, cnt)`. **This is the single
  cleanest mapping in the whole port** — torpor's `(addr, expected, timeout_us)`
  / `(addr, count)` *is* `FUTEX_WAIT`/`FUTEX_WAKE`.
- `signal_thylacine.go` + `signal_unix.go`-analog — note delivery → Go's
  `sighandler`. Model on `signal_plan9.go` (notes), see §4.2.
- `mem_thylacine.go` — `sysAlloc`/`sysFree`/`sysReserve`/`sysMap`/`sysUsed`/
  `sysUnused`/`sysFault` over `SYS_BURROW_ATTACH/DETACH`. See §4.1 (the one hard
  part).
- `netpoll_*.go` — start with the **plan9-style "no real poller" / blocking
  model** (§4.3); a `netpoll_thylacine.go` over `SYS_POLL` or `SYS_LOOM` is the
  later optimization.
- `nanotime`/`walltime` → `SYS_CLOCK_GETTIME(MONOTONIC/REALTIME)`.

**`src/syscall/` + `src/internal/runtime/syscall`:** the `Syscall`/`RawSyscall`
shims + `zsyscall_thylacine_arm64.go` + `zerrors_*`; the file ops
(`open`/`read`/`write`/`close`/`pread`/`seek`/`fstat`) map to the Thylacine fs
syscalls (`SYS_WALK_OPEN`, `SYS_READ`, `SYS_WRITE`, `SYS_FSTAT`=50,
`SYS_LSEEK`=51, `SYS_FSYNC`, `SYS_READDIR`, `SYS_CLOSE`).

**`src/os/` + `src/net/`:** `os/exec` forkExec → `SYS_SPAWN_FULL_ARGV` (no fork
— the plan9 `exec` path is the template, §4.4); `net` → adapt the `plan9` net
package over `/net` (§4.3). `internal/poll` sits on the netpoll choice.

**Build plumbing:** `go/build/syslist.go` (+ `goos`/`goarch` tables), the
`cmd/dist` bootstrap tables, `internal/syscall/unix` exclusions, the
`//go:build thylacine` tags throughout. `CGO_ENABLED=0` always (no on-device C
toolchain; pure-Go — §6).

---

## 3. The syscall mapping table (the heart)

Confirmed `SYS_*` numbers from `main:kernel/include/thylacine/syscall.h`:

| Go runtime / syscall hook | Thylacine syscall | # | Notes |
|---|---|---|---|
| `futexsleep` (lock2/notesleep) | `SYS_TORPOR_WAIT(addr, expected, timeout_us)` | 39 | direct futex; ns→µs |
| `futexwakeup` | `SYS_TORPOR_WAKE(addr, count)` | 40 | direct futex |
| `newosproc` (M create) | `SYS_THREAD_SPAWN(entry, sp, arg, tls, ptid)` | 41 | shares pgtable+handles; sets TPIDR_EL0=tls |
| `exitThread` (M exit) | `SYS_THREAD_EXIT` | 42 | clear-child-tid wake via the tid addr |
| `exit` (whole proc) | `SYS_EXIT_GROUP(status)` | 60 | cascades all Ms (the death-wake) |
| `sysAlloc`/`sysReserve`/`sysMap` | `SYS_BURROW_ATTACH(length)` + `burrow_map` | 37 | **eager pages today — §4.1** |
| `sysFree`/`munmap` | `SYS_BURROW_DETACH(vaddr, length)` | 38 | |
| `nanotime`/`walltime` | `SYS_CLOCK_GETTIME(clk_id, ts)` | 75 | MONOTONIC=1 / REALTIME=0; 16B timespec |
| signals (`signalM`, sigsend) | `SYS_POSTNOTE` | 47 | notes; per-Thread targeting — §4.2 |
| set clear-child-tid | `SYS_SET_TID_ADDRESS(tidptr)` | 36 | for M join/exit wakeups |
| `osyield` | `SYS_YIELD` (verify it exists) or torpor spin | — | check the enum; else short torpor |
| netpoll | `SYS_POLL(fds, nfds, timeout_ms)` | 29 | level-triggered; or `SYS_LOOM` (66/68) — §4.3 |
| `os/exec` forkExec | `SYS_SPAWN_FULL_ARGV` | 49 | no fork — §4.4 |
| `wait4` (Wait) | `SYS_WAIT_PID(want_pid, flags, status)` | 22 | by-pid + WNOHANG exist |
| file open/read/write/… | `SYS_WALK_OPEN`/`READ`/`WRITE`/`FSTAT`(50)/`LSEEK`(51)/`CLOSE` | … | the `internal/poll.FD` layer |
| `getrandom` (runtime seeds) | `SYS_GETRANDOM` | — | the kernel CSPRNG (Lazarus W3) |

The SVC calling convention (which register carries the syscall number, how
errors are returned — a negative errno in x0, the `[-4095,-1]` POSIX-aligned
convention per `docs/ERRORS.md`) must be read off `arch/arm64/exception.c` and
pinned in `sys_thylacine_arm64.s`. Errno values are POSIX-aligned
(`errno.h` `_Static_assert`s), so `zerrors_thylacine` ≈ the linux table.

---

## 4. The hard parts (precise, with code pointers)

### 4.1 Memory: `sysReserve` vs eager pages — the one real allocator friction

**Finding (`main:docs/reference/30/31-burrow*` / `kernel/burrow.c`):**
`burrow_create_anon(size)` **eagerly allocates all backing pages at create
time** (v1.0), though **PTEs are already lazy/demand-paged** (the TTBR0 tree
starts empty, grows on fault; `burrow_unmap` tears PTEs down).

Go's allocator wants **cheap address-space reservation**: `sysReserve(n)`
reserves (no physical commit) arena chunks (64 MiB on 64-bit), then `sysMap`
commits sub-ranges as the heap grows. Eager commit breaks this — Go would commit
64 MiB per arena chunk on first touch.

**Two paths (the main agent picks):**
1. **Bring-up fallback (no kernel change):** implement `sysReserve` == commit
   (`SYS_BURROW_ATTACH`). Works for hello-world + small-heap programs; wastes up
   to a 64 MiB arena-chunk granule. Possibly tune `heapArenaBytes`/the arena
   hint down for the port to shrink the waste. Good enough to prove the runtime.
2. **The proper fix (small, well-motivated kernel ask):** a **demand-paged anon
   burrow** — make the *page* allocation lazy on fault, exactly as the *PTE*
   path already is. The fault handler (`arch/arm64/fault.c::userland_demand_page`,
   already under `vma_lock`) already demand-installs PTEs; the change is to
   demand-*allocate* the page there instead of pre-allocating at create. This is
   the natural `mmap(MAP_ANONYMOUS)` semantics, benefits *every* large anon
   mapping (not just Go), and composes with the #65 page-cap (charge on fault).
   Frame it as a `BURROW_ATTACH` flag (`LAZY`) or a new lazy-anon default. **This
   is the recommended real answer** and the single most valuable kernel
   enabler for Go (and for any large-heap runtime).

Stack growth (Go's copy-stacks) is pure userspace + `mmap` for new segments →
`burrow` → same story; small stacks make the eager-commit fallback fine there.

**STATUS — path 2 LANDED (the overcommit model, #318–321; ARCH §6.5 + doc 127).**
Stage 1 shipped path 1 (the bring-up fallback: `sysReserve` == eager commit, with
`heapAddrBits` constrained to 40 + 4 MiB arenas, the ios/arm64 config, to keep the
page-summary reserve under the 256-MiB eager cap). #318–321 built the proper
answer as a **dedicated `SYS_BURROW_ATTACH_LAZY` (83)** (chosen over a flag on
`SYS_BURROW_ATTACH` for blast-radius discipline) + `SYS_BURROW_DECOMMIT` (84). At
#321 `mem_thylacine.go` reserves via the lazy attach (`sysReserve`/`sysAlloc`) and
decommits via `sysUnused`/`sysFault`, and **`malloc.go` reverted to stock
`heapAddrBits = 48` + 64 MiB arenas** — booting that go-hello (the ~512-MiB
page-summary reserve commits nothing until touched) is the proof the model needs
no per-program tuning. The same lazy path backs libthyla-rs's native heap +
pouch's `mmap`, so every native + ported program gets overcommit.

### 4.2 Signals: notes → `sighandler`, and **no async preemption** (like plan9)

Thylacine **notes** are the Plan 9 notes model (same name, same concept) —
`SYS_POSTNOTE` posts, delivery is at the **EL0-return tail** (zero-lock), and
`kill` is non-catchable (I-19/I-24). Map them to Go's signal layer the way
`signal_plan9.go` does (note string ↔ Go's signal disposition).

**The load-bearing limitation — async preemption.** Go ≥1.14 preempts a
goroutine stuck in a tight loop by sending it a **signal** that interrupts at an
arbitrary PC (`SIGURG`/`doSigPreempt`). Thylacine notes deliver **only at the
EL0-return checkpoint** — they cannot interrupt a thread spinning in EL0 user
code. So **`GOOS=thylacine` has no signal-based async preemption — exactly like
`plan9`.** Consequence: rely on Go's **cooperative + loop-back-edge safe-point
preemption** (the compiler inserts preemption checks at function entry + loop
back-edges since 1.14), which covers the vast majority of code. Set
`asyncPreemptStack`/the preempt path to the plan9 posture (`preemptMSupported =
false` analog). A goroutine in a tight no-safepoint loop won't preempt — a known,
documented plan9-class limitation, acceptable for v1.

> **Open question for the main agent (verify in `kernel/notes.c` +
> `arch/arm64/exception.c`):** can a note (a) target a *specific* Thread of a
> multi-thread Proc, and (b) run a Go-provided handler on a Go-controlled
> alternate stack (gsignal) at the EL0-return tail with the interrupted register
> state available? Go's sighandler needs the trap context. If notes don't expose
> the interrupted `ucontext`, the synchronous fault notes (`snare:segv` etc.)
> still map to Go's panic-on-fault path, but the *delivery contract* must be
> nailed down. A future "directed safe-point note" or timer-IPI preemption would
> restore async preemption — a NOVEL-adjacent later enhancement, not a v1 blocker.

### 4.3 Net + netpoll: adapt `plan9`-Go over `/net`

**The gift:** `/net` is the Plan 9 tree (`/net/tcp/clone` → read conn number →
write `connect host!port` to `/net/tcp/N/ctl` → read/write `/net/tcp/N/data`;
`announce` to listen; `tcp/`,`udp/`,`icmp/` dirs; a `cs`/dns story per
NET-DESIGN). **Go's `plan9` net package already speaks exactly this.** So
`net_thylacine` ≈ `net_plan9` with the dial/listen paths retargeted to
Thylacine's `/net` (check `cs`/name-resolution: does netd serve `/net/cs` +
`/net/dns`, or is resolution done differently? — verify against `usr/netd/
src/server.rs` and NET-DESIGN §3).

**netpoll staging:**
1. **Blocking model first (plan9 posture):** no real poller — each blocking
   read/write on a `/net/N/data` fid blocks its M; the scheduler spins up more
   Ms (`netpollinited=false` path). Simple, correct, proven by plan9. **Start
   here** — it gets net working with zero new kernel surface.
2. **A real netpoller later (optimization):** `netpoll_thylacine.go` over
   `SYS_POLL` (level-triggered poll(2) — a poll-based netpoller, like the older
   simple ports) or, more ambitiously, over **`SYS_LOOM`** (the io_uring-shaped
   async ring — Go doesn't use io_uring upstream, so this is novel work, but
   Loom is the higher-performance substrate). Defer until net works.

### 4.4 `os/exec`: no fork → `SYS_SPAWN_FULL_ARGV`

Go's `syscall.forkExec`/`os/exec` assume `fork`+`exec`. Thylacine has **no
fork** — `SYS_SPAWN_FULL_ARGV` is the combined spawn (name, argv up to
`SYS_SPAWN_ARGV_MAX`=16 / `SYS_SPAWN_ARGV_DATA_MAX`=4096, fd list up to
`SYS_SPAWN_MAX_FDS`=16, caps). This is the **plan9 model** (plan9 has no Unix
fork either — its `syscall.forkExec` is already a non-fork spawn). So crib the
plan9 `exec.go`/`forkExec` shape and retarget to `SYS_SPAWN_FULL_ARGV`.
Limitations to document: argv/fd caps (16/16); the Go child-setup-in-fork dance
(chdir, dup2, setpgid) collapses into the spawn's fd-list + the kernel's
semantics — map what's expressible, error the rest. `os/exec` with `CGO_ENABLED=0`
+ these caps covers the common case (run a tool, pipe it).

### 4.5 TLS / `g`

arm64 Go keeps the current-`g` pointer in TLS (`TPIDR_EL0`). `SYS_THREAD_SPAWN`
takes a `tls_va` and (per the pthread substrate) sets `TPIDR_EL0` for the new
Thread. Go manages its *own* M threads (not pthreads), so it *owns* `TPIDR_EL0`
on its Ms — no clash with the pouch-pthread TLS (different Procs/threads).
Confirm the kernel sets `TPIDR_EL0 = tls` on `SYS_THREAD_SPAWN`
(`kernel/thread.c`, the trampoline) so Go's `load_g` works.

---

## 5. Bootstrap & build strategy (the Canadian cross)

Self-hosting is a staged bootstrap; you never build Go *on* Thylacine first.

- **Stage 0 — the GOOS compiles, cross from a host.** Fork the (pinned) Go tree,
  add `GOOS=thylacine` (all of §2). Build a toolchain on Linux/macOS that *runs
  on the host* but **targets** Thylacine: `GOOS=thylacine GOARCH=arm64 go build`.
  Go cross-compiles trivially once the GOOS compiles — this is the big unlock.
- **Stage 1 — hello world runs on Thylacine.** Cross-build a `print("hi")`
  binary, run it in-VM. Proves: ELF loads, `osinit`/`schedinit`/`mallocinit`,
  one goroutine, `write` to fd 1, `exit_group`. (Expect to fight the SVC ABI +
  `sysReserve` here first.)
- **Stage 2 — goroutines + channels + GC.** A program that spawns goroutines,
  uses channels, allocates + triggers GC. Proves: `SYS_THREAD_SPAWN` (Ms),
  `TORPOR_WAIT/WAKE` (scheduler sync), the GC + memory layer, cooperative
  preemption.
- **Stage 3 — fs + os/exec + net.** Read/write files; `os/exec` a coreutil; dial
  a TCP socket over `/net` (the plan9-net adaptation). Proves the syscall package
  + the blocking netpoll + spawn.
- **Stage 4 — LOCAL on-device build (no net).** The `/env` device (closes G15)
  + bake the trimmed GOROOT into the pool → `go build` a local `.go` file *on the
  device* produces and runs a Thylacine ELF. This is the **headline milestone**:
  the "write a Go program in Nora → build → run" loop, minus modules.
- **Stage 5 — MODULES over the net.** `net/http` (a stdlib port) + `go mod` /
  `go get` over the fast `/net` (TLS + DNS, already shipped) → download + build a
  real dependency. The module proxy protocol is plain HTTPS GETs.
- **Stage 6 — BY DEFAULT + Nora integration.** Ship the toolchain in the default
  image; the `ut`/Nora UX; polish → the full end-to-end **write-in-Nora, `go mod`,
  build, run** experience, out of the box.
- **Stage 7 — self-host + (stretch) upstream.** The on-device Go toolchain
  rebuilds Go from source (bootstrap-with-a-prior-Go; pin the bootstrap version).
  Optionally upstream `GOOS=thylacine` (`plan9` is upstream — precedent + heritage
  make it realistic).
- **Stage 8 — Nora as a Go IDE + the cross-boundary debugger.** The headline: a
  fully-fledged Go IDE shipped by default — gopls (editing intelligence) + a
  capability-scoped, namespace-native, **visual debugger** in the Kaua TUI, with
  unified **user->kernel stacks**, variable/frame inspection, the namespace/
  resource + scheduler superpowers, and an interactive run-pane. Rides a new
  kernel `/proc` debug-fs (arm64 HW breakpoints, no software `BRK` — preserves
  W^X + the REVENANT Image cache) + a `dlv` `proc_thylacine` backend, driven over
  DAP/LSP. **Ratified + charter'd 2026-06-23: `docs/GO-IDE-DESIGN.md`** (NOVEL
  #13). Built AFTER the toolchain (4-6).
- **Stage 9 (deferred moonshot) — OS-native time-travel.** Deterministic
  record-replay (we own the scheduler + syscalls + CSPRNG + clock) -> a true
  reverse-step in the Go debugger. A research-grade arc of its own; deferred per
  the 2026-06-23 vote, earned after Stage 8 is solid. GO-IDE-DESIGN section 9.

Each stage is a clean, demoable checkpoint; Stages 0-3 are the runtime port,
Stages 4-6 are the on-device toolchain (the payoff), Stage 7 is toolchain purity,
Stage 8 is the IDE + cross-boundary debugger (the headline), Stage 9 is the
time-travel moonshot.

#### 5.0.1 The on-device-toolchain design (locked 2026-06-23, user-voted)

**Why Go is the *ideal first* on-device toolchain (the aux + main investigation).**
Go is **self-contained**: `cmd/compile` / `cmd/asm` / `cmd/link` are all *pure Go*
(no LLVM backend, no C-toolchain dependency with `CGO_ENABLED=0`), and the Go
*runtime* is ALREADY ported and proven in-VM (Stages 1-3). So the on-device
toolchain is "ship the prebuilt toolchain binaries + close env + bake GOROOT" —
**not** "port a compiler backend." Against the alternatives this is night-and-day:
Rust needs LLVM; a C toolchain needs clang/lld (#67). Go is the tractable path to a
self-hosting Thylacine, by a wide margin — which is why it is the first toolchain.
What's already in place makes Stage 4 mostly assembly, not invention: `os/exec`
(3b), file I/O (3a), TLS (net-7c), the overcommit/lazy-mem model (compiles are
RAM-hungry), REVENANT large-binary exec (`compile`/`link` are 10-20 MB each),
`getCPUCount` (parallel builds), graceful file-lock degradation (`filelock_other`),
and the linker already emits Linux-shaped static ELF (Hlinux, set at Stage 0a).

**Decision 1 — environment variables (G15): the Plan 9 `/env` device.** The
toolchain is profoundly env-driven (`$GOROOT`/`$GOPATH`/`$GOCACHE`/`$HOME`/
`$TMPDIR`/`go env`). Go-on-plan9 reads its environment from **`/env/*` files**
(`runtime/env_plan9.go::goenvs`), NOT a Unix `envp` array — so `GOOS=thylacine`
mirrors plan9: a per-Proc `/env` device, and the Go port reuses the plan9 `goenvs`
path almost verbatim. **No spawn-ABI change.** `/env` is a new kernel Dev (Dev
semantics + rfork share/copy inheritance + a boot mount + the I-1 per-Proc-
isolation posture), so **Stage 4 opens with a focused `/env` design pass +
audit** (scripture before code). Rejected: wiring the reserved `_pad_envp` slot in
`SYS_SPAWN_FULL_ARGV` (an ABI change; less Plan-9-idiomatic; needs a non-plan9 Go
env path).

**SCRIPTURE LANDED (Stage 4a, 2026-06-23): ARCH §9.7** (the per-Proc Env group +
the `/env` `devenv` Dev) + the §25.4 audit-trigger row + the CLAUDE.md mirror. As
committed: a ref-counted per-Proc `Env` group (`kernel/env.{c,h}`,
`struct Proc.env`), deep-**copied** on spawn (`env_clone_into` in `rfork_internal`,
the Plan 9 default-copy-on-`rfork` like `territory_clone`; the reserved
`RFENVG`=0x100 stays deferred), surfaced by `devenv` (dc='E', a synthetic dir Dev
mounted by the `/srv`-idiom; per-Proc *content* behind a global mount = I-1). Two
research findings folded in: **(1)** entries enumerate as **standard 9P2000.L
`Treaddir` dirents** (the tree-wide format), NOT Plan 9's legacy stat-format
directory read — so the fork gets a thin `runtime/env_thylacine.go` (`goenvs`
reads `/env` via readdir; structurally the plan9 `goenvs`, dirent-parse adapted),
"almost verbatim" holding for the *shape*; **(2)** inheritance rides the kernel
clone (a `go build` sub-process inherits `$GOROOT` etc. exactly as it inherits the
namespace) — the `os/exec` explicit-`Cmd.Env` *override* is a Go-side seam, never a
kernel ABI change. Composes I-1 + I-32; no new §28 invariant; prose-validated (the
`env->lock` is the standard per-Proc-group pattern, Territory/allowance). The impl
+ the go-fork `goenvs` + the boot env-var setup + the focused audit are Stage 4a's
kernel / go / bootenv / audit sub-chunks.

**4a-AUDIT CLOSED (2026-06-23): 0 P0 / 0 P1 / 0 P2 / 3 P3 — CLEAN.** Two concurrent
independent prosecutors converged: an Opus-4.8-max `holotype-reviewer`
(MODEL(start)==MODEL(end), no Fable fallback) found F1 (`devenv.perm_enforced`
implicit zero-init → explicit `false` + comment) + F2 (`test_env_bounds` proved the
at-cap write's return not its storage → read-back added); a concurrent self-audit
found F3 (`devenv_create` did not reject a non-root/value-file parent → one-line
`qid.path == ENV_QID_ROOT` guard + regression). All 3 P3 fixed; no soundness hole.
The lifetime/refcount balance (rfork-rollback + later-failure + `proc_free`), the
`env->lock` peer-thread discipline, the I-1 isolation (a `/env/NAME` Spoor's id
resolves against the *caller's* env), the I-32 bounds, and the net-3d monotonic-id
all traced sound. `1004/1004`, boot OK, `go-env: STAGE 4a OK`, 0 EXTINCTION; SMP gate
(default+UBSan × smp4/smp8). Reference: `docs/reference/128-devenv.md`. Close list:
`memory/audit_env_closed_list.md`. Stage 4a (the `/env` device) is COMPLETE; the
remaining Stage-4 work is the GOROOT bake (Decision 2 below) + the post-pivot `/env`
re-graft seam.

**4b LANDED (2026-06-23): the on-device toolchain is BUILDABLE + BAKED. The headline
de-risk is done — the whole Go toolchain cross-compiles for `GOOS=thylacine`.** Three
landings, all verified host-side (no boot needed for the bake itself):
- **The toolchain cross-builds** (go-thylacine fork `6e2a5c6`). `go build cmd` for
  `GOOS=thylacine GOARCH=arm64` was 8 small GOOS-surface fixes short: a getrandom
  `rand_thylacine.go` (`SYS_GETRANDOM`), an honest no-op `signal_thylacine.go` (notes
  aren't yet delivered to os/signal), a `root_thylacine.go` (CA bundle reader), a
  `syscall.Exec` ENOSYS stub (spawn-only OS; cmd/link's execArchive never runs on a
  pure-Go internal link), and 4 `notunix`/`other` build-tag additions (telemetry mmap,
  cmd/go mmap, testenv, the 3 `signalsToIgnore`). Result: `go` 17.7M / `compile`
  26.2M / `link` 7.9M / `asm` (+pack/buildid/cover/vet), all thylacine/arm64 static
  ELF; a realistic fmt/os/strings/sort program cross-builds to 2.08 MB. Deferred (NOT
  toolchain-required, NOT a Stage-4 dep): `os/user` + `net/internal/socktest` (a stub
  lands when a real consumer needs it, Stage 5/6).
- **`stratum-fs put`** (Stratum `thylacine-pouch-arm` `8c75623`): a single-session
  recursive host-dir -> pool copy, descending with a per-level dir fid (each op walks
  <=1 component -> never hits the 16-component Twalk cap, never re-attaches). A
  per-file CLI loop over ~3600 files was infeasible. Host-tested: a deep tree copies
  in 43 ms, exec bits preserved.
- **4b-1 re-graft** (thylacine `aa66f65`): post-pivot `/env` re-graft in joey (mirror
  `/dev`); verified in-VM (`Go-4b post-pivot /env re-graft OK`, go-env Stage 4a
  unregressed, boot OK, 0 EXTINCTION).
- **4b-2 GOROOT bake** (thylacine `07be1a6`): `build_go_goroot()` cross-builds the
  toolchain (stripped) + stages the trimmed stdlib src (109 MB / 3620 files); the pool
  grows 64->256 MiB and `stratum-fs put`s it at `/goroot`. Gated on
  `THYLACINE_BAKE_GOROOT=1` (default off -> the dev loop is byte-identical). Host-
  verified: a baked pool resolves `/goroot/bin/go` (0755, 12.4 MB), all 7
  `pkg/tool/thylacine_arm64` tools, `src/fmt/print.go`, VERSION=go1.25.3.

**4c (NEXT) — the on-device `go build` proof.** A baked 256 MB pool with `/goroot` is
ready. The remaining piece is a joey probe that sets `$GOROOT`/`$GOCACHE`/`$GOPATH`/
`$HOME`/`$TMPDIR` on its post-pivot `/env`, spawns `/goroot/bin/go build` on a tiny
in-guest `.go`, runs the output, and reaps. **THE ONE DESIGN DECISION: the emulated
in-guest compile speed.** A trivial program still compiles its stdlib deps from source
with the 18 MB `compile` under TCG -> likely minutes, which blows the 90 s boot-test
timeout. So 4c needs one of: (a) **pre-warm the GOCACHE on the host + bake it into the
pool** (link-only on-device -> fast; the right "by default" answer, but the cache must
be action-ID-portable to the on-device toolchain), (b) an **HVF / interactive proof**
(measure the real time off the gated boot path), or (c) a **raised bake-boot timeout**.
Pick (a) if portable; else (b) for the headline + (a) as Stage-6 polish.

**Decision 2 — GOROOT shipping: baked by default (~150 MB trimmed).** Bake the
toolchain binaries (~60 MB) + the stdlib *source* (~90 MB after stripping
`_test.go`/`testdata`/`src/cmd`) into the default pool image, growing `pool.img`
from 64 MB to ~256 MB. Truly offline + by-default — Thylacine ships a complete Go
toolchain out of the box, no "install the compiler first" asterisk. Rejected: a
fetch-once installable `go` package (not literally by-default) and binaries-baked-
stdlib-downloaded (first build needs net). The on-device build cache + module cache
live on the writable post-pivot Stratum FS.

### 5.1 Status (as built, 2026-06-23)

The Go fork lives at `~/projects/go-thylacine` (a sibling tree, like
`~/projects/stratum`); its commits stay LOCAL. The Thylacine-repo side is the
`build.sh` cross-compile + FS bake (`build_go_probes` → `$GOFORK/bin/go build`,
baked into the ramfs by `build_ramfs`) plus the `go-hello` boot probe in
`usr/joey/joey.c`.

- **Stage 0 — DONE** (fork `b2e204b` + `6d6829b`). 0a registered `GOOS=thylacine`
  in the toolchain tables; 0b ported the runtime OS layer (6 `runtime/*_thylacine*`
  files + build-tag edits), so `GOOS=thylacine GOARCH=arm64 CGO_ENABLED=0 go build`
  compiles the runtime and a `println` hello LINKS to a 1.5 MiB Thylacine ELF.
  Thylacine is a **hybrid GOOS**: non-unix like plan9 (no POSIX signals → no
  `signal_unix.go`, so no sigctxt/sigtramp at Stage 0/1) but futex-based like linux
  (`lock_futex.go` over `SYS_TORPOR_WAIT/WAKE`). The three mechanism divergences:
  thread_spawn is entry-point (a `thread_entry` asm trampoline + custom `newosproc`),
  futex is split into two syscalls, and mmap is `SYS_BURROW_ATTACH` (eager-commit,
  kernel-chosen address, no MAP_FIXED).

- **Stage 1 — DONE** (fork `0266e9f` + Thylacine `usr/go-hello/` + `build.sh` +
  `joey.c`). A `println` hello RUNS in-VM to clean exit: `go-hello: sum(1..100) =
  5050`, reaped status 0, boot OK, 0 EXTINCTION. The unstripped 1.5 MiB binary execs
  via the REVENANT file-backed path (the >1-MiB exec REVENANT enabled). go-hello is
  spawned with `/dev/cons` wired to fd 0/1/2 (a Go program gets stdio from its
  spawner; native libthyla-rs binaries instead print via the fd-less `SYS_PUTS`).
  The **eager-commit memory model** was the real fight (as predicted): Go's 64-bit
  page allocator reserves the page-summary arrays for the whole 48-bit heap (~512
  MiB), and `sysReserve ≡ commit` blew past `BURROW_ATTACH_MAX` (256 MiB). Fix:
  `thylacine/arm64` uses the same constrained heap config as `ios/arm64`
  (`heapAddrBits=40`, 4 MiB arenas) — the summary shrinks to ~2 MiB and the
  eager-commit physical ceiling keeps live mappings near the 4 GiB
  `EXEC_USER_BURROW_BASE`, far below 2^40.

  **The proper memory model — LANDED (#318–321; user-directed 2026-06-23, "implement
  properly preemptively"; Option 2 = the overcommit contract, chosen for
  future-proofing across toolchains/pouch).** The overcommit model (a dedicated
  `SYS_BURROW_ATTACH_LAZY` (83) demand-zero reserve + commit-on-fault +
  charge-on-fault + `SYS_BURROW_DECOMMIT` (84) backing `sysUnused`) + the I-32
  VMA-count fourth axis bounding the free reservation (ARCH §6.5 "The overcommit
  model" + §28 I-32 + the §25.4 audit row; kernel mechanism + audit at #319/#320;
  doc 127). This is the **Linux overcommit contract** the whole native ecosystem
  (musl/glibc/jemalloc/LLVM/Go-stock) assumes — Option 1 (charge-at-reserve) only
  worked for programs I could hand-tune (Go via heapAddrBits=40), so it does not
  generalize to arbitrary pouch/LLVM binaries. The contract reaches every program via
  the two malloc substrates (libthyla-rs `sysAlloc` + the pouch boundary-line `mmap`,
  retargeted `__NR_mmap` 37→83) + the Go fork's `mem_thylacine.go` — all reserve
  lazily. **At #321 `malloc.go` reverted to stock `heapAddrBits=48`** (the 512-MiB
  page-summary reserve commits nothing until touched) — booting that go-hello is the
  proof the overcommit model needs no per-program tuning (verified: `go-hello: sum =
  5050`, 993/993, boot OK). One carve-out: a Loom registered buffer needs eager
  contiguous backing, so native code allocates it via `libthyla_rs::loom::RegisteredBuffer`
  (eager `SYS_BURROW_ATTACH`), not the lazy heap (doc 107/127). The eager
  `SYS_BURROW_ATTACH` (37) thus survives for the kernel-internal copy-target callers +
  the native registered-buffer helper. **The Stage-1 `heapAddrBits=40` hack above is
  RETIRED by this.**

- **Stage 2 — DONE** (fork `be82db1` [the #321 overcommit rewire, the Stage-2
  prerequisite] + Thylacine `usr/go-goroutines/` + the `build.sh` + `joey.c`
  probe wiring). The runtime port's real test: a concurrent Go program RUNS the
  three subsystems Stage 1 never touched, on the proper overcommit model, FIRST
  TRY. `usr/go-goroutines` does `GOMAXPROCS(4)` + 8 worker goroutines that
  ping-pong an UNBUFFERED channel fan-in, join via `sync.WaitGroup`, churn
  `make([]byte, ...)` allocations, increment a cross-M `sync/atomic` counter, and
  a concurrent goroutine hammers `runtime.GC()` — so it drives **multiple Ms**
  (SYS_THREAD_SPAWN under load via `thread_entry`, placed across the SMP CPUs),
  **scheduler-sync on torpor** (M park/wake + SYS_TORPOR_WAIT/WAKE), and the
  **GC + the overcommit memory layer** (STW under cooperative-only preemption +
  `sysUnused` → SYS_BURROW_DECOMMIT). In-VM (`-smp 4`):

  ```
  go-goroutines: workers = 8  GOMAXPROCS = 4
  go-goroutines: allocOps = 16000  want = 16000
  go-goroutines: NumGC = 4  HeapAlloc = 79208  bytes
  go-goroutines: fan-in total = 1056640
  go-goroutines: STAGE 2 OK (goroutines + channels + GC)
  ```

  reaped status 0, 993/993, boot OK, 0 EXTINCTION. The `fan-in total = 1056640`
  is *arithmetically exact* (sum of id*2000 = 56000, plus 8*125080 = 1000640),
  so the workers did not merely run — every byte was computed correctly across
  all 8 goroutines and 4 Ps under concurrent GC + decommit, proving the memory
  model holds under churn. `sync` + `sync/atomic` compiled for `thylacine` with
  NO build-tag work (they are GOOS-independent). The design avoids a tight
  no-safepoint loop (every worker iteration allocates → a cooperative
  safepoint), so STW always converges under `preemptMSupported = false` (the
  documented plan9-class no-async-preempt limitation). SMP-gated (default+UBSan ×
  smp4/smp8 N=10): go-goroutines runs under real parallelism on every boot.

  **Stage-2 finding (deferred, not a blocker): `getCPUCount()` returns 1**, so
  the default GOMAXPROCS is 1; the probe forces parallelism with an explicit
  `runtime.GOMAXPROCS(4)`. A real CPU-count source (a `/proc` or `/ctl` file, or
  a syscall) does not exist in the kernel yet — wiring it would let GOMAXPROCS
  default correctly. Genuinely separable from the Stage-2 proof (which the
  explicit GOMAXPROCS gives identically); a follow-up, owed at Stage 3 with the
  syscall/fs package.

- **Stage 3a (fs) — DONE** (fork `f6015e9` + Thylacine `usr/go-fs/` + the
  `build.sh` + `joey.c` probe wiring). The `syscall` and `os` packages (plus
  `internal/poll`, `time`, `internal/filepathlite`) are ported to
  `GOOS=thylacine`, so a Go program does real **file I/O via the `os`
  package**. The shape is the hybrid the plan calls for: **Plan-9 structure**
  (Open and Create are distinct, files reached by path through the per-Proc
  namespace, no fork, `fd2path`, `netpoll_stub`) + **Linux conventions**
  (numeric `Errno` — the kernel returns `-errno` — not Plan 9's error strings;
  a flat fixed `Stat_t` filled from the 80-byte `t_stat`, not a marshaled 9P
  `Dir`). The new-GOOS build-tag wrinkle: thylacine, like plan9, is non-Unix,
  so the os files written as "non-plan9 == unix" (`rawconn.go`,
  `types_unix.go`) had to exclude it; `removeall_noat.go` + `root_noopenat.go`
  include it; `error_errno.go` supplies the `Errno` error type for free.
  `os.Create` opens the parent **O_PATH** to get the born-R|W create base
  `SYS_WALK_CREATE` requires; `os.Remove` tries unlink then falls back to rmdir
  for a directory (the lone in-VM bug, root-caused at step 8 of 8). The
  syscall primitives are **raw SVC** (no `entersyscall`) at 3a — file blocking
  is bounded and sysmon is the backstop; the `entersyscall`-wrapped blocking
  path lands at **3b** (below). `usr/go-fs` runs POST-pivot (devramfs is
  read-only) against the writable Stratum FS:

  ```
  go-fs: wrote+read 25 bytes; stat size=25; seek+readdir+rename+remove OK
  go-fs: STAGE 3a OK (fs file I/O: create/write/read/stat/seek/readdir/rename/remove)
  ```

  reaped status 0, 993/993, boot OK, 0 EXTINCTION; Stage 1/2 unregressed.
  The **getCPUCount seam is also closed** (the Stage-2 deferred finding):
  `getCPUCount` now reads `/ctl/sched` (which emits `cpus: N`) via an
  allocation-free raw `SYS_OPEN`+read at osinit, fail-soft to 1 on any error;
  `go-fs` prints `NumCPU=4` under `-smp 4` (was stuck at 1), so GOMAXPROCS
  defaults to the real CPU count.

- **Stage 3b (os/exec) — DONE** (fork `a40796a` + Thylacine `usr/go-exec/` +
  the `build.sh` + `joey.c` probe wiring). The `os/exec` package end-to-end:
  a Go program **spawns** a child via `SYS_SPAWN_FULL_ARGV` (`os.StartProcess`
  -> `syscall.startProcess`, marshalling name + argv + fd_list into a 96-byte
  `struct sys_spawn_args` — the Go mirror matches the kernel layout
  byte-for-byte, every u64 on an 8-boundary, verified against the kernel
  `_Static_assert` offset pins; identity/allowance blocks left zero = inherit),
  **captures** the child's stdout through an `os.Pipe`, and **reaps** it via
  `SYS_WAIT_PID` (`os.Process.Wait` -> by-pid `waitProcess`, status read back).
  The load-bearing piece is the **`entersyscall`-wrapped blocking-syscall
  path** (the riskiest asm): a goroutine blocked in a raw `SYS_WAIT_PID` /
  pipe `Read` while the GC does stop-the-world would HANG (the un-preemptible
  SVC is not a safepoint), so `Syscall`/`Syscall6` now bracket the SVC with
  `BL runtime.entersyscall<ABIInternal>` / `exitsyscall` — the Darwin/BSD
  arm64 model (a `$0` NOSPLIT frame is fine; the arm64 assembler auto-saves LR
  for a non-leaf, so the `BL` doesn't clobber the return address), keeping the
  Linux-shaped `CMN $4095` errno decode. `RawSyscall`/`RawSyscall6` stay raw.
  File I/O now correctly rides the wrapped path too (a 9P read can block), and
  go-fs re-ran clean through it. `os/exec` needed the new-GOOS plumbing again:
  `exec_unix.go`'s `!plan9 && !windows` wrongly matched thylacine, so add
  `&& !thylacine` + provide `exec_thylacine.go` (`skipStdinCopyError`) +
  `lp_thylacine.go` (`LookPath`; `findExecutable` checks exists-and-not-a-dir,
  NOT the POSIX 0111 bit — thylacine gates exec on namespace X-search +
  `OEXEC`) + `path/filepath/path_thylacine.go`. `ProcAttr.Dir` is fail-closed
  `ENOSYS` (no spawn-time chdir); `Env` is silently dropped (G15). `usr/go-exec`
  runs POST-pivot (it execs the `/bin` coreutils, reachable via the `/bin`
  bind):

  ```
  go-exec: captured "go exec stage 3b" via os/exec + pipe; /bin/false exit code 1
  go-exec: STAGE 3b OK (os/exec: spawn /bin coreutil + capture stdout + wait + exit-status)
  ```

  reaped status 0, 993/993, boot OK, 0 EXTINCTION; Stage 1/2/3a unregressed.

- **Stage 3c (net) — DONE** (2026-06-23; fork `6320e2f`, thylacine `68d785b`).
  A Go program drives the full plan9-shaped net stack over netd's `/net`,
  end to end. The `net` package is ported by mirroring the 14 `*_plan9.go`
  files to `*_thylacine.go` — netd's `/net` IS a Plan 9 network filesystem
  (clone/ctl/data/connect/announce/listen + cs/dns), so the plan9 package maps
  on almost verbatim. The dial/listen path is
  `queryCS1(/net/cs) -> open clone -> read N -> connect`/`announce -> data`,
  exactly what netd serves; every BSD-sockets net file is `unix`-tagged (which
  excludes thylacine, like plan9), so no `_posix`/`_unix` file is touched. Four
  targeted edits to the otherwise-verbatim copies: `syscall.EPLAN9` ->
  `ENOSYS`; `syscall.Dup(fd, -1)` -> one-arg `Dup`; ipsock `fixErr`'s
  `ErrorString` assertion -> `Errno`; and ipsock `probe()` returns a fixed
  IPv4-enabled result (netd exposes no `/net/iproute`; v1.0 is IPv4-only). The
  blocking accept-open + data `Read` ride the Stage-3b `entersyscall` path;
  `netpoll_stub.go` already carries the thylacine tag (Stage 0b).

  One **netd gap** surfaced + was fixed (the kernel stayed byte-unchanged):
  `ctl_announce` never recorded `slots[n].local`, so `/net/tcp/N/local` was
  empty for an announced-but-unconnected listener -> Go's `listenPlan9 ->
  readPlan9Addr` read it and got `io.EOF` -> `net.Listen` failed. The native
  libthyla-rs client never read `local` after announce, so the gap was latent
  until the Go port exercised the full Plan 9 listen sequence. Fix: record the
  bound listen endpoint on a successful announce (correct Plan 9 `local`
  semantics for every `/net` client). Root-caused by ground truth — an
  instrumented probe proved cs and the clone read both worked, isolating the
  EOF to the local read. `usr/go-net` (a self-contained listen+accept+dial+echo
  round-trip on the resident lo):

  ```
  go-net: listening on 127.0.0.1:9099
  go-net: dialed 127.0.0.1:9099 -> 127.0.0.1:9099 (local 127.0.0.1:49237)
  go-net: round-trip OK ("go net stage 3c over /net")
  go-net: STAGE 3c OK
  ```

  All 5 go stages reaped status 0; 993/993, boot OK, 0 EXTINCTION, login
  clean. **STAGE 3 COMPLETE.** Next: the on-device toolchain (Stage 4 — the
  `/env` device + GOROOT bake + local `go build`).

---

## 6. Native, not Pouch — and `CGO_ENABLED=0`

Go is a **native** Thylacine citizen, **not a Pouch port**: its runtime emits
raw `svc` syscalls from `sys_thylacine_arm64.s` against the `SYS_*` ABI
directly — it links **no musl, no libt** (Go reimplements its own stubs per
GOOS, as it does everywhere). This is the "foreign-but-syscall-native" third
category (cf. the native-vs-ported scripture, ARCH §3.5): foreign code that
expects raw syscalls + its own runtime, so it goes native without being authored
here. **Keep `CGO_ENABLED=0`** (no on-device C toolchain; pure-Go avoids the cgo
C-compiler dependency, and is the static-binary norm anyway). The one cost: cgo-
only stdlib paths (e.g. some `net` DNS resolvers, `os/user`) must use their
pure-Go fallbacks — which the `/net`/`cs` model + `GODEBUG=netdns=go` already
favor.

---

## 7. What the main agent must verify/decide before cooking

1. **Pin a Go version** (a recent stable; the GOOS surface + bootstrap chain are
   version-specific). Fork it; do the port against that pin.
2. **The SVC ABI** (`arch/arm64/exception.c`): syscall-number register, arg
   registers, error-return convention → `sys_thylacine_arm64.s`.
3. **The `sysReserve` decision** (§4.1): eager-commit bring-up fallback vs the
   demand-paged-anon-burrow kernel ask. **Recommend the kernel ask** — it's
   small (the PTE path is already lazy) and the highest-leverage enabler.
4. **The notes→sighandler contract** (§4.2): per-Thread targeting + the trap
   context at the EL0-return tail. Confirms whether Go's signal-based fault
   handling (panic-on-segv) maps cleanly; async preemption is *out* regardless
   (cooperative only, like plan9).
5. **`/net` name resolution** (§4.3): does netd serve `/net/cs` + `/net/dns`, or
   is it `GODEBUG=netdns=go`-only? Sets how close `net_plan9` drops in.
6. **`osyield`/`SYS_YIELD`** existence (check the enum) and the
   `getproccount`/CPU-count source (`/proc` or a sysconf-analog).
7. **TPIDR_EL0 on `SYS_THREAD_SPAWN`** (§4.5).

---

## 8. Bottom line for the strategy

Go-on-Thylacine is **bounded, well-referenced, and JIT-free** — the opposite of
the LLVM/Rust beast. The system's Plan 9 heritage makes the `plan9` GOOS a
*template, not just a reference*: net, signals, and exec come along for the ride,
while torpor/thread_spawn/burrow give the runtime core a clean Linux-style
substrate. The single real friction is `sysReserve` (one small, well-motivated
kernel enhancement). The payoff — Stage 4, `go build` on a Raspberry Pi 400/500
running Thylacine — is reachable as a focused multi-chunk arc, and it's the
v1-class "you can build on this OS" milestone we want, *without* touching LLVM,
Rust-self-hosting, or the W^X/JIT wall.

This is the thinking work. The cookbook = pin Go, stand up Stage 0, and walk
the stages, resolving §7 against the live kernel as you go.
