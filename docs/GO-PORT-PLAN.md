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
- **Stage 4 — the Go *toolchain* runs on Thylacine.** Cross-build `cmd/compile`,
  `cmd/asm`, `cmd/link` (themselves Go programs) to **run on Thylacine** →
  `go build` *on the device*. This is the **v1 on-device-dev milestone** (write
  + compile Go on the Pi).
- **Stage 5 — self-host + (stretch) upstream.** The on-device Go toolchain
  rebuilds Go from source (the bootstrap-with-a-prior-Go chain; pin the bootstrap
  toolchain version). Optionally upstream `GOOS=thylacine` (Go accepts ports;
  `plan9` is upstream — the precedent + the heritage make this realistic).

Each stage is a clean, demoable checkpoint; Stages 0–3 are the runtime port,
Stage 4 is the payoff, Stage 5 is purity.

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
