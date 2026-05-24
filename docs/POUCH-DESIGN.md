# POUCH-DESIGN.md — the pouch POSIX environment + cross-compilation

**Status: BINDING (converged 2026-05-22). Design-session complete — first cut + two redline rounds. Cross-referenced from VISION / ARCHITECTURE / ROADMAP.**

This document is the binding design for execution **Phase 6: Pouch** — the POSIX environment and the Thylacine cross-compilation toolchain. It is drafted in the CORVUS-DESIGN.md mold.

The design session: round 1 (first cut) carried 20 open questions; round 2 resolved all 20 to the drafted leans (user signoff) and added §6.6 (the namespace / synthetic-filesystem model); round 3 confirmed §6.6. Every decision is recorded in §18. The inline **[OPEN Q]** / **[RESOLVED]** markers in §§4–12 are retained for traceability; all are resolved per §18.

---

## 1. Mission & convictions

Thylacine's practicality — its claim to be "a real OS, not a toy" (VISION §1) — stands or falls on the reuse of existing software. An OS that can run only programs written specifically for it is a research artifact. An OS that can host the large body of well-written POSIX C software — daemons, libraries, tools — is a system people can actually use. **stratumd is the proximate proving binary; the durable deliverable is the cross-compilation path itself.**

This phase builds that path: a Thylacine-native C library that presents a POSIX surface (**pouch**), a cross-compilation toolchain and sysroot, and the runtime layer that translates POSIX abstractions into Thylacine primitives.

### 1.1 Design philosophy (binding for every decision below)

Per user direction (2026-05-22), four principles govern this phase, in tension and resolved case-by-case:

1. **Plan 9 heritage is the starting point, not a cage.** Where a Plan 9 idea is correct, keep it. Where it is dogma that costs real benefit, modernize.
2. **Modernize where it brings real benefit.** A futex-backed thread model is not Plan 9, but real C software needs it; we implement it on Thylacine's terms.
3. **Do not become a BSD/Linux clone.** POSIX is a surface pouch *presents*, not a kernel we *emulate*. The kernel stays Thylacine-native.
4. **Do not be a slave to practicality.** We are creating something new. Where POSIX demands something genuinely at odds with Thylacine's design (e.g. `fork()`), we may decline, document the decline, and let unsuitable software be unsuitable.

The resolution pattern: **pouch carries the burden of translation so that application source stays portable, but the kernel carries none of it.** POSIX lives entirely in userspace; the kernel never learns what POSIX is.

### 1.2 What pouch is — and is not

- pouch **is** a Thylacine-native libc that *also* speaks POSIX. It is a first-class system component, not a grudging compat ghetto. (Plan 9's APE — the ANSI/POSIX Environment — was second-class; pouch is not.)
- pouch **is not** a Linux emulation layer. It contains no Linux syscall numbers, no Linux kernel ABI assumptions below its own boundary line.
- pouch **is not** glibc. It is lean, honest about its boundaries, and returns a documented error for every POSIX surface it does not implement — never a silently-wrong result.

---

## 2. Scope

### 2.1 In scope

- The **pouch** libc: a musl-derived C library whose portable upper half is musl's, and whose OS-boundary lower half is Thylacine-native.
- The **cross-compilation toolchain**: a clang-based `--target` + sysroot that builds C programs for Thylacine, usable from CMake and from plain Makefiles.
- The **sysroot**: `build/sysroot/{include,lib,bin}` — pouch headers, `libc.a`, CRT objects.
- The **POSIX runtime layer**: pouch's lower-half implementations that translate POSIX file I/O, sockets, `poll`, signals, and threads into Thylacine primitives.
- The **kernel-side primitives** pouch's lower half requires that do not yet exist (see §12): auxv population, the wait-on-address primitive, and a small set of additions.
- **Proving artifacts**: a static "hello" C program, the libsodium library cross-compiled, and real **stratumd** built + booted.

### 2.2 Explicitly out of scope (deferred)

- **The Utopia userland** (coreutils, bash, rc) — that is the renamed Phase 7 (formerly Phase 6), built *on* pouch.
- **Unmodified Linux binary execution** — running Linux ELFs as-is is a later phase (the "Linux compat + network" phase). pouch hosts *recompiled* C source, not Linux binaries.
- **Dynamic linking / shared objects** — v1.0 is static-only. pouch ships `libc.a`; `.so` support is deferred.
- **`AF_INET` / TCP sockets** — deferred to the network phase. pouch returns `EAFNOSUPPORT` for `AF_INET`.
- **The exotic POSIX surface** — real-time signals, `fork()` (see §8.3), file-backed `mmap`, locale beyond `C`, `epoll`/`io_uring`/`inotify`. Each returns a documented error.

### 2.3 The completeness bar

pouch ships the POSIX.1 subset that the **proving set** (stratumd + libsodium + the test programs) requires, **designed to extend cleanly**. "Modern daemon-shaped POSIX," not "every POSIX.1 corner." §8 enumerates the target precisely.

---

## 3. Architecture overview — the boundary line

pouch is structured as two halves divided by one boundary:

```
   ┌─────────────────────────────────────────────┐
   │  pouch UPPER HALF  (musl, near-unmodified)   │
   │  string.h stdio.h stdlib.h math.h ...        │   portable C — pure computation
   │  printf, malloc, qsort, strtol, ...          │   no OS knowledge
   ├─────────────────────────────────────────────┤  ◄── THE BOUNDARY LINE
   │  pouch LOWER HALF  (Thylacine-native)        │
   │  the POSIX runtime layer:                    │   translates POSIX → Thylacine
   │  open/read/write, socket→/srv, poll→t_poll,  │
   │  sigaction→notes, pthread→Thylacine threads  │
   ├─────────────────────────────────────────────┤
   │  Thylacine syscalls  (SYS_* — native)        │   the kernel, unchanged in character
   └─────────────────────────────────────────────┘
```

musl is already structured around exactly this seam: `src/internal/syscall.h` is musl's chokepoint, and musl's OS-specific subsystems (sockets, threads, signals) are isolable modules. pouch keeps musl's upper half — which is excellent, lean, portable C — and **replaces musl's lower half** with Thylacine-native code.

The boundary line is the load-bearing architectural commitment: **above it, portable musl; below it, Thylacine. Nothing Linux-shaped crosses into the kernel.**

---

## 4. The pouch libc — vendoring & the patch series  *(Decision 2)*

**Recommendation**: vendor a pinned musl release into the tree (`third_party/musl/` — **[RESOLVED 4.3]** path), with the Thylacine delta as a **patch series concentrated at the boundary line**. pouch is openly a *musl derivative*, not musl.

Rationale:

- The changes are deep but *localized*. We retarget `arch/aarch64/bits/syscall.h` and replace the socket / thread / signal / poll subsystems. We do **not** touch musl's string/stdio/stdlib/math. A patch series concentrated below the boundary line is tractable; a whole-tree fork is not, because it forfeits upstream musl's security fixes.
- The patch series is itself scripture-tracked. Upstream musl security releases → rebase the series. The series' *size* is the honest measure of our divergence.
- Pinning a release (musl 1.2.5 at time of writing) keeps the bringup deterministic.

**[OPEN Q 4.1]** musl vs. building pouch's portable half from a different source. musl is the right call (lean, static-friendly, clean seam), but worth a sentence of confirmation.

**[OPEN Q 4.2]** Vendoring mechanism: in-tree copy + patch series, or a git submodule + patch series. In-tree copy is simpler for a hermetic build; submodule tracks upstream more visibly. Lean: in-tree copy.

**[RESOLVED 4.3]** (2026-05-22, sub-chunk 2 `pouch-musl-vendor`) The vendored-tree path is **`third_party/musl/`**, not `usr/lib/pouch/musl/`. `third_party/` is the conventional, instantly-recognizable home for pristine vendored upstream code under its own license, and it keeps a clean boundary — `usr/lib/pouch/` holds *pouch's own* code (the patch series, and later the lower half), `third_party/` holds the unmodified upstream input. The patch series lives at `usr/lib/pouch/patches/`. See `third_party/README.md` + `docs/reference/78-pouch.md`.

---

## 5. The syscall ABI — Thylacine-native, no Linux numbers  *(Decision 3)*

**Recommendation**: pouch's lower half is **C code that calls Thylacine syscalls**, exactly as any Thylacine-native program does. There is **no "POSIX syscall" concept in the kernel** and **no Linux syscall number anywhere**.

Concretely:

- musl's `arch/aarch64/bits/syscall.h` is regenerated so musl's internal `SYS_*` constants are *Thylacine* numbers. Where a POSIX call maps 1:1 onto a Thylacine syscall (`write(2)` → `SYS_WRITE`), pouch's wrapper is a thin `svc 0` with `x8` carrying the Thylacine number.
- Where a POSIX call does **not** map 1:1 — `socket()`, `pthread_create()`, `poll()` — pouch's lower half implements it **in C** as a *sequence* of Thylacine syscalls. These are ordinary library functions, not kernel syscalls.
- Where pouch needs a primitive Thylacine genuinely lacks (the wait-on-address primitive — §7), we add it as a **native Thylacine syscall, designed on Thylacine's terms** — Thylacine-shaped arguments, Thylacine error convention, a Thylacine name. It is not "the Linux futex syscall ported"; it is a Thylacine primitive that happens to serve pouch's mutex implementation.

This is the most Plan-9-faithful answer available: Plan 9's APE was likewise "C atop Plan 9 syscalls," never a foreign-kernel emulation. It also makes the **ROADMAP §3.6** principle ("the kernel is never modified to accommodate POSIX") *structurally true*, not merely aspirational — see invariant P-1 in §11.

**[OPEN Q 5.1]** musl assumes the Linux error-return convention (negative errno in the syscall return). Thylacine's convention is `-1`-on-error for most syscalls, with no errno channel. pouch's lower half must synthesize `errno`. Decision: pouch's syscall wrappers map Thylacine's `-1` returns to POSIX `errno` values **in the lower half**, per-call. This means every pouch lower-half wrapper carries a small Thylacine-rc → errno mapping. Acceptable; flag for confirmation. A richer Thylacine error channel is a possible future kernel change but **out of scope** here.

---

## 6. The POSIX runtime layer — the mappings  *(Decision 5)*

This is pouch's lower half: how each POSIX abstraction becomes a Thylacine primitive.

### 6.1 Files & the namespace

POSIX `open / read / write / pread / pwrite / lseek / fstat / fsync / ftruncate / fcntl / unlink / mkdir / rmdir / rename` map onto Thylacine's Spoor + 9P-client + handle-table machinery, which already exists. A POSIX `int fd` **is** a Thylacine `hidx_t` handle index — the identity is exact, no descriptor table indirection needed. `open(path, ...)` resolves through the calling Proc's Territory (the namespace) via the walk machinery (`SYS_WALK_OPEN` and its successors).

**[OPEN Q 6.1]** `open()` of a *device* path (`/dev/...`) for stratumd's block device. Two options: (a) pouch's `open` resolves `/dev/...` through a `/dev`-style Thylacine namespace populated by the boot path; (b) stratumd receives the block device as an inherited handle (`SYS_SPAWN_WITH_FDS`-style) and its block layer has a thin Thylacine arm that uses the inherited fd instead of `open()`. Option (a) keeps stratumd's source more portable; option (b) is more capability-pure (Thylacine hands out the device explicitly, no ambient `/dev`). This interacts with §10 (cross-OS source story). **Lean: (b)** — capability-explicit is more Thylacine, and stratumd already has a per-OS block-layer seam; but (a) is friendlier to future ported software. Needs a decision.

### 6.2 Sockets → /srv

POSIX `AF_UNIX` `SOCK_STREAM` sockets map onto Thylacine's **`/srv`** mechanism (the `devsrv` registry + `SrvConn` per-connection transport).

**Revised 2026-05-23 at sub-chunk 12 impl time:** the original design statement that `/srv` "was built for exactly this shape" was *almost* correct — it's true for the 9P-shaped servers `/srv` was designed for (corvus, future stratumd), but the kernel SrvConn shipped at P5-corvus-srv as a **9P channel**: the client-side `read/write` uses `srvconn_client_read/write` (Tread/Twrite frames via the kernel-owned `p9_client`), and the server endpoint Spoor reads raw 9P T-frame bytes from c2s. A POSIX userspace server reading `read(conn, buf, 5)` to receive `"PING\n"` would see ~13 bytes of Twrite frame header + payload instead. That asymmetry serves a 9P responder (corvus parses the T-frames) but breaks raw AF_UNIX byte streams.

Sub-chunk 12 resolves this by adding a **byte transport mode** to the SrvConn (kernel change, audit-bearing):

- `enum srv_mode { SRV_MODE_9P, SRV_MODE_BYTE }` on the SrvService (set at `srv_reserve`; immutable through LIVE).
- New syscall `SYS_POST_SERVICE_BYTE = 43` — the byte-mode variant of `SYS_POST_SERVICE = 26`. Legacy callers (corvus, stratumd) keep `SYS_POST_SERVICE` (default 9P mode); pouch's `bind` calls `SYS_POST_SERVICE_BYTE`.
- The SrvConn carries `byte_mode` propagated from the service at mint (`srv_conn_open_for_proc` reads `service->mode` under the registry lock).
- `SYS_srv_connect` against a byte-mode service skips the 9P handshake and refuses a non-empty path; `sys_read/write_for_proc`'s KOBJ_SRV arm dispatches on `cn->byte_mode` (byte → `srvconn_client_send/recv`, raw c2s/s2c; 9P → `srvconn_client_read/write`, framed).

The pouch-side translation table now reads:

- `socket(AF_UNIX, SOCK_STREAM, 0)` → allocates a pouch-side userspace slot (a tagged fd with `POUCH_SOCK_TAG` = 0x40000000); no kernel call yet.
- `bind(fd, path) + listen(fd, backlog)` → `SYS_POST_SERVICE_BYTE` for the service name derived from `path`; listen is a no-op pouch-side validation.
- `accept(fd)` → `SYS_srv_accept` → a `SrvConn` server-endpoint Spoor handle (NOT pouch-tagged — server-side I/O bypasses the dispatch shim).
- `connect(fd, path)` → `SYS_srv_connect(name, name_len, NULL, 0)` — byte mode skips the kernel 9P handshake.
- `send / recv` → `write / read` on the connection handle (tagged-fd dispatch shim translates client-side, raw fast path server-side).
- `getsockopt(SO_PEERCRED)` → `SYS_srv_peer`. **Strictly better than SO_PEERCRED**: the peer identity (`stripes`, `caps`, `console`, `alive`) is kernel-stamped and unforgeable. Marshaled into `struct ucred` for POSIX compat at v1.0; the native richer surface is reserved for Thylacine-native code. CLIENT-SIDE `SO_PEERCRED` returns `ENOTSOCK` at v1.0 (the kernel `SYS_SRV_PEER` gate is server-only — a kernel extension for client-side peer query is a v1.x item).

**The path↔service mapping**: a Unix-socket path *is* a filesystem path, and a `/srv` service *is* reachable in the filesystem at `/srv/<name>`. The clean mapping: an `AF_UNIX` socket bound at path `/srv/<name>` *is* the `/srv` service `<name>`. Unix sockets are not bolted on as a separate namespace — **they are the `/srv` filesystem**, which is the Plan-9-native answer. stratumd's listen paths are configuration; we point them at `/srv/...`. stratumd remains a 9P-mode service (`SYS_POST_SERVICE` = 26) — its users speak 9P over the SrvConn. Pouch sockets are a separate concern, riding the byte-mode path.

**[OPEN Q 6.2]** `AF_UNIX` paths *not* under `/srv/` — unsupported (return an error at `bind`), or backed by a second mechanism. Lean: unsupported at this phase; document it; revisit if a ported app needs filesystem-scattered sockets.

**[OPEN Q 6.3]** `SOCK_DGRAM` (datagram Unix sockets) — stratumd does not use them. Defer; `socket()` returns `EPROTONOSUPPORT`.

### 6.3 poll / select → t_poll

POSIX `poll(2)` over fds maps onto Thylacine's `SYS_POLL` — which **already exists** (P5-poll landed) and polls over handle indices. Because a POSIX fd *is* a Thylacine handle index (§6.1), pouch's `poll()` is a near-direct passthrough plus the `struct pollfd` ↔ Thylacine-poll-record marshaling. `select()` is implemented in pouch's C upper-ish layer on top of `poll()`. `ppoll` / `pselect` map similarly with the signal-mask handled per §6.4.

### 6.4 Signals → notes

POSIX signals map onto Thylacine **notes** (Plan 9 heritage; kernel substrate designed in ARCH §7.6.1-§7.6.8 + landed at sub-chunk 13a `pouch-signals-design`). Notes are a different model — string-named, causally-ordered, no fixed signal numbers, and **fd-shaped first** (the documented modern path is to read notes from a `SYS_NOTE_OPEN`'d fd in a poll-driven event loop; the async handler is the legacy opt-in for libcs that insist on the POSIX model). The mapping is *partial by design*:

- pouch implements the **small signal surface real daemons need**: `SIGINT`, `SIGTERM`, `SIGPIPE`, `SIGCHLD`, plus `sigaction` / `sigprocmask` / `pthread_sigmask` for those.
- Each supported signal is a reserved note string; `sigaction` registers a pouch-side handler that the kernel's async-handler delivery path (`SYS_NOTIFY` → EL0-return-tail invoke) calls into. The kernel's fd-shaped path is parallel: `SYS_NOTE_OPEN` returns a fd that delivers `struct note_record` (32-byte fixed format) on `read`. Pouch exposes a thin wrapper (`note_open()`) so daemons can choose the modern path; the existing musl `sigaction` API also works for code that wants it.
- `SIGPIPE`: a write to a closed `/srv` peer returns `EPIPE`; the kernel synthesizes the `pipe` note delivery, but pouch **defaults `SIGPIPE` to masked** at startup ([RESOLVED 6.4]) — POSIX default-ignore-friendly modern practice. The `write` returns `EPIPE` and that's it. Daemons can opt in to handler delivery via `sigaction(SIGPIPE, …)` (clears the mask bit).
- **Deferred**: real-time signals, `SIGSEGV`/`SIGBUS` userspace handlers (Thylacine faults are extinction-shaped), `SIGHUP`/`SIGALRM`/`SIGUSR1`/`SIGUSR2`/`SIGSTOP`/`SIGCONT` (each requires a separate kernel hook — `alarm` needs `SYS_ALARM`, `hangup` needs cons close path, `stop`/`cont` need scheduler integration). The full 31-signal surface stays incomplete. Unsupported signals: `sigaction` returns `EINVAL` with a documented list.

**[RESOLVED 6.5]** (2026-05-24). Notes' causal-ordering guarantee (I-19, refined to N-1..N-5 in ARCH §7.6.7) is *stronger* than POSIX signal ordering — every posted note is consumed exactly once across the handler and fd-read paths, and notes from a single posting source are delivered in post order. POSIX programs receive the stronger guarantee transparently; signal-heavy ports gain reliability they couldn't get on Linux's non-RT signal model.

### 6.5 Block I/O

stratumd owns its pool's block device and does `pread/pwrite/fsync` on it. Per **[OPEN Q 6.1]**, the device is reached either through a `/dev` namespace or as an inherited handle. The I/O itself — `pread/pwrite` at offset, `fsync` — maps onto Thylacine read/write/sync on the device handle. Thylacine's Phase-4 userspace virtio-blk driver work is the substrate. The Linux block `ioctl`s (`BLKGETSIZE64`, `BLKDISCARD`) that stratumd uses are best-effort: pouch returns `ENOTTY` and stratumd's existing graceful-degradation path (loop-file fallback) handles it.

### 6.6 The namespace — synthetic filesystems  *(design-session round 2 — new)*

§6.1–6.5 covered the **call-based** POSIX surface (`socket()`, `pthread_create()`, `poll()` — no path). This section covers the **path-based** surface: the paths a POSIX program `open()`s and expects to exist and behave (`/dev/urandom`, `/proc/self/maps`, `/dev/null`, `/etc/...`).

**The synthetic filesystem *is* the translation layer — there is no second mechanism.** In Thylacine a path resolves through the calling Proc's Territory to a Spoor backed by a Dev; the Dev's `read`/`write` *is* the translation. `open("/dev/urandom") + read()` walks to a CSPRNG-backed Dev whose `read` calls the kernel CSPRNG — the very primitive the call-based path also reaches. Because Thylacine's primitives are already filesystem-shaped (`/srv`, `/cap`, `/ctl`, devproc), POSIX's *entire* path-based surface maps directly onto the existing Dev model. pouch's `open`/`read`/`write` stay near-pass-through; the translation lives in the **Devs**, not in pouch. This is "the filesystem is the OS" (VISION) delivering its dividend — for everything path-shaped, pouch is a thin POSIX *spelling* over Thylacine's device model.

**The interplay runs both directions:**
- A POSIX program **consumes** a Thylacine synthetic filesystem — `open()` on a Unix-expected path resolves to a Thylacine Dev.
- A POSIX program **provides** one — pouch's `AF_UNIX`→`/srv` mapping (§6.2) lets a POSIX program *be* a Thylacine file server. **stratumd is exactly this**: a portable POSIX C daemon that, on Thylacine, serves a 9P filesystem on `/srv` with no Thylacine-specific server code. pouch's socket layer is what makes that automatic.

**The one exception:** the thread + `torpor` layer (§7) is *not* filesystem-shaped — it is the single part of pouch's lower half that is a genuine call translation, not a synthetic-FS translation. Files, sockets, `poll`, and signals-as-notes all fit the synthetic-FS framing; threads do not.

**Who serves the synthetic filesystems**: Thylacine Devs (kernel — e.g. a tiny `devnull`) or userspace 9P servers (Plan-9-native — e.g. devproc's successor). pouch does not *serve* them; pouch *establishes the convention* (which Unix paths exist, in what shape) and *consumes* them. Trivial nodes may be small kernel Devs; rich ones (`/proc`) are servers.

**Scope for this phase**: the proving set needs only a *minimal* namespace —
- `/srv` — already exists (`devsrv`).
- stratumd's block device — an inherited handle (§6.1 resolution), so possibly no path at all.
- CSPRNG for libsodium — modern libsodium prefers the `getrandom` *syscall*; a Thylacine `SYS_GETRANDOM` call satisfies it directly, no `/dev/urandom` path needed.
- A handful of trivial `/dev` nodes (`null`, `zero`, `full`).

This phase therefore **establishes the synthetic-FS-as-translation pattern and implements that minimal set**. The fuller Unix-shaped namespace — `/proc` in depth, `/dev/pts` + `/dev/ptmx` (PTYs; `pty.tla` is gate-tied spec #9, already planned), `/sys`, `/dev/console` — is shared with, and mostly belongs to, the renamed Phase 7 (Utopia), where bash and coreutils make it load-bearing.

**[RESOLVED 6.6]** (2026-05-22) Minimal-namespace scope confirmed: the proving set's namespace is `/srv` (exists) + the inherited block handle + the `getrandom` syscall + trivial `/dev` nodes. `null`/`zero`/`full` are tiny kernel Devs; the richer Unix namespace (`/proc` in depth, `/dev/pts`, `/sys`) is deferred to the Utopia phase.

---

## 7. The thread model  *(Decision 4 — the hardest)*

**Recommendation**: POSIX threads **are** Thylacine Threads within one Proc — a model the kernel already has (`struct Thread`, per-Proc `threads` list, `thread_count`). A Proc is already a shared address space (one Territory, one page table). POSIX `pthread_create` therefore creates a new Thylacine Thread in the caller's Proc; it does **not** create a Proc.

The synchronization primitives are the real design content:

- **mutex / condvar / rwlock / once / barrier** are implemented in **pouch's C layer**, on top of one kernel primitive: a **wait-on-address** operation — "atomically: check that a userspace word still holds an expected value, and if so sleep; separately, wake N threads waiting on a word." This is the futex mechanism. It is *modernization* — Plan 9 had no such thing, its thread library was largely cooperative — but real C software (stratumd's 4 thread-spawn sites, libsodium) requires preemptive threads with cheap uncontended locks, and the uncontended-fast-path/contended-syscall shape *requires* wait-on-address.
- Thylacine **already plans this**: `futex.tla` is gate-tied spec #7. The wait-on-address syscall is therefore not new scope invented here — it is a Phase-5/6 kernel primitive this phase consumes. It is implemented Thylacine-native: Thylacine argument shape, Thylacine error convention, and a **Thylacine name** (see §16) — not "the Linux futex."
- **TLS**: aarch64 thread-local storage is entirely userspace via `TPIDR_EL0` (the research confirmed: musl sets it with one `MSR` instruction). The kernel's only obligations are (a) EL0 may write `TPIDR_EL0` — architecturally always true — and (b) `TPIDR_EL0` is saved/restored across context switch — already required for any Thread. **No kernel TLS work.**
- **Thread-local `errno`**: a TLS variable in pouch — free once TLS works.

**Deferred from the pthread surface**: robust mutexes, `pthread_cancel` / cancellation points, priority inheritance, `pthread_atfork`. Implement the **core** stratumd + libsodium need — `create / join / detach / mutex / cond / rwlock / once / key (TLS) / sigmask` — and shape the surface so the rest extends. The deferred calls return a documented error.

**[RESOLVED 7.1]** The wait-on-address primitive's exact contract — sub-chunk 8 landed: `SYS_TORPOR_WAIT(addr_va, expected, timeout_us)` takes a **relative microsecond timeout**, converted internally to an absolute `timer_now_ns()`-timebase deadline that composes with `tsleep`. `timeout_us < 0` is "block indefinitely"; `timeout_us == 0` is a probe that returns `ETIMEDOUT` immediately; `> 0` is a finite microsecond timeout up to `TORPOR_MAX_TIMEOUT_US` (1 hour). Userspace passing absolute deadlines (pthread_cond_timedwait's `abstime`) converts to a relative microsecond timeout at the libc layer. See `docs/reference/80-torpor.md`.

**[RESOLVED 7.2]** No `specs/futex.tla` is written. CLAUDE.md "Spec-to-code suspended" was broadened on 2026-05-23 to suspend the spec-first DESIGN discipline itself (not only the per-chunk clean-cfg gate); the no-lost-wakeup invariant is validated by **prose reasoning** in `kernel/torpor.c` + `kernel/include/thylacine/torpor.h` + the audit round + the 7 runtime tests. The buggy cfgs of EXISTING specs remain pre-commit gates for impl changes that touch those mechanisms; new sub-chunks don't add to the inventory.

**[OPEN Q 7.3]** `pthread_create` stack allocation: pouch allocates the new Thread's stack (anonymous memory) and passes it to the Thylacine thread-spawn syscall. Confirm Thylacine's thread-spawn surface accepts a caller-provided stack + entry, or note the kernel gap. — sub-chunk 9 (`pouch-threads`).

---

## 8. libc completeness target  *(Decision 6)*

### 8.1 Supported

- **ANSI C, in full** — `string.h`, `stdio.h`, `stdlib.h`, `math.h`, `ctype.h`, `time.h` (the computational parts). Near-free from musl's upper half.
- **File I/O** — the surface in §6.1.
- **The POSIX runtime layer** — §6 in full (the supported subset of sockets / poll / signals; §7 threads).
- **Memory** — the `malloc` family via musl's `mallocng`, resting on the native `burrow_attach` / `burrow_detach` syscalls (ARCHITECTURE.md §6.5, Tier 1). [RESOLVED 8.1: the lower half retargets musl's `__mmap` / `__munmap` onto the burrow-attach pair; `brk` / `madvise` / `mprotect` / `mremap` are tolerated no-ops — `mallocng` needs none of them.]
- **`clock_gettime`, `nanosleep`** — map onto Thylacine timer syscalls.
- **`mlock` / `munlock`** — stratumd pins its passphrase; maps onto `SYS_MLOCKALL` / a future per-range lock.

### 8.2 Unsupported — documented errors, never silent-wrong

`AF_INET`, dynamic linking, file-backed `mmap` (refused by design — ARCHITECTURE.md §6.5), real-time signals, `epoll`/`io_uring`/`inotify`/`signalfd`/`eventfd`/`timerfd`, locale beyond `C`, `fork()` (§8.3), `exec` of a Linux binary, System V IPC. Each: a documented `errno` (`ENOSYS` / `EAFNOSUPPORT` / `EINVAL`) and a manual-page note.

**Documented v1.0 limitation — stack guard pages on pthread stacks** (P6-pouch-threads-b audit F2). pouch's `__mmap` (0003-pouch-mman boundary-line patch) ignores `prot` and always returns RW; `__mprotect` returns `-ENOSYS`. musl's pthread_create allocates `[map, map+size)` with PROT_NONE then mprotects the writable portion — both calls return RW, so the nominal "guard" region at the stack base is also RW. Stack overflow corrupts guard bytes silently; only an overflow past the entire stack region hits an unmapped page and faults. Stratum-class workloads (no deep recursion) are bounded by this. **The real fix needs a new kernel syscall to flip VMA permissions (PROT_NONE-capable) — deferred to v1.x**; at v1.0, the limitation is documented in `docs/reference/82-pouch-pthread.md` "Known caveats / footguns" and callers can extend their stacks via `pthread_attr_setstacksize` for headroom.

### 8.3 `fork()` — a deliberate decline

Plan 9 never had `fork()` — it has `rfork`, selective resource sharing, paired with `exec`. Thylacine inherits this (`rfork`). POSIX `fork()` — duplicate the entire address space, COW everything, one return becomes two — is at genuine odds with the model.

**Recommendation: pouch does not provide `fork()`.** It provides **`posix_spawn`** (the modern, fork-free process-creation API — itself POSIX, and what well-written modern daemons already use) mapped onto Thylacine `rfork`-spawn. Software that hard-requires `fork()` is, by this phase's lights, software written against an older model; it is not our target, and forcing `fork()` onto Thylacine would be exactly the "slave to practicality" the philosophy rejects. This is a place we *decline* and document.

stratumd uses pthreads + (per the research) no `clone3`/`unshare`/`fork` — so this decline costs the proving binary nothing.

**[OPEN Q 8.2]** Confirm the `fork()` decline. The alternative — a limited `fork()` (Plan-9-APE-style: fork-then-immediately-exec only) — is possible but adds surface. Lean: decline, ship `posix_spawn`.

---

## 9. The cross-compilation toolchain & sysroot  *(Decision 8)*

- **Toolchain**: clang-based — the kernel build already uses clang; clang's `--target=` + `--sysroot=` is a single-binary cross-*compiler* with no separate GCC build. **Linking is a separate step**: for the unknown `thylacine` OS on a macOS host, clang's *link driver* mis-selects the host Darwin toolchain and emits Mach-O linker arguments — so the pouch link step invokes the ELF linker `ld.lld` directly. A `pouch-clang` wrapper pins `--target` + `--sysroot` for compilation; a `pouch-ld` wrapper drives `ld.lld` with the pouch ELF link line. Plain-Makefile / autotools / CMake projects use the pair without fighting the clang link driver. (Surfaced by `pouch-hello-smoke`; deep-dive in `docs/reference/78-pouch.md`.)
- **Triple**: **[OPEN Q 9.1]** — `aarch64-thylacine` (clean; OS-named, libc implied) vs. `aarch64-thylacine-pouch` (libc-explicit, conventional 4-tuple) vs. `aarch64-unknown-thylacine`. Lean: `aarch64-thylacine`.
- **Sysroot**: `build/sysroot/{include,lib,bin}` — pouch headers, `libc.a`, the CRT objects (`crt1.o`/`crti.o`/`crtn.o`), and the **compiler runtime** `libclang_rt.builtins.a` (the compiler-rt builtins — a complete C toolchain needs them; e.g. `vfprintf` formats `long double` = aarch64 `binary128`, whose soft-float requires `__eqtf2`/`__extenddftf2`/… builtins). `tools/build.sh sysroot` (today a placeholder) becomes the real builder.
- **Integration**: a CMake toolchain file (`cmake/Toolchain-aarch64-pouch.cmake` — the name `Toolchain-aarch64-thylacine.cmake` is already the kernel toolchain, so the pouch file is role-named `-pouch`, parallel to the existing `-userspace`) + the `pouch-clang` wrapper. Stratum builds with CMake; the wrapper covers everything else.

---

## 10. The cross-OS source story  *(Decision 7)*

The goal — your stated goal — is **one source tree, three targets**: Linux, macOS, Thylacine, with Thylacine the *primary* target for Stratum and Thylacine-first software.

The model:

- **Most code is pure POSIX and needs zero Thylacine awareness.** pouch translates; the source does not know it is on Thylacine.
- **Where POSIX itself is insufficient or divergent**, a program carries a thin per-OS boundary file, selected at build time. stratumd *already* does this — `peer_creds.c` has a Linux arm (`SO_PEERCRED`) and a macOS arm (`getpeereid`). Thylacine becomes a third arm (`t_srv_peer`). This is the *only* shape of Thylacine-specific code a well-structured port needs.
- **pouch exposes both surfaces.** A program may be pure-POSIX (maximally portable) or may `#include <thylacine/...>` and use native extensions (better integration, Thylacine-only). The program chooses per-call. stratumd stays ~99% portable POSIX with a small native arm.

The discipline: **minimize the Thylacine-specific surface in any ported program.** If pouch translates a POSIX surface well, no per-OS arm is needed. Per-OS arms are reserved for genuine boundary divergences (peer creds, block-device specifics). The measure of a good pouch is how few per-OS arms a port needs.

**[OPEN Q 10.1]** Whether Thylacine-first software (Stratum, future Thylacine-native daemons) should be *encouraged* to use pouch's native extensions freely, or held to portable-POSIX-plus-thin-arms even when Thylacine is the primary target. The user's framing ("Stratum is native to Thylacine ... but want to target Linux/Mac as well") suggests: portable POSIX core + thin per-OS arms, so the Linux/Mac targets stay cheap. Confirm.

---

## 11. Invariants  *(Decision 9)*

This phase introduces these invariants. They are **P-numbered**, canonical in this section, and cross-referenced from `ARCHITECTURE.md §28` — mirroring the corvus invariants (C-1..C-23, canonical in CORVUS-DESIGN.md, cross-ref'd from §28). [RESOLVED 11.1.]

- **P-1 — No foreign syscall numbers in the kernel.** The kernel syscall table contains only Thylacine-native `SYS_*` entries. No Linux/POSIX syscall number exists anywhere in `kernel/`. Enforced by code review + audit; makes the ROADMAP §3.6 principle structurally true.
- **P-2 — pouch is the sole POSIX path.** Every POSIX surface reaches the kernel through pouch's lower half. The kernel makes, and contains, zero POSIX accommodations.
- **P-3 — No silently-wrong POSIX.** Every POSIX surface pouch exposes either (a) maps to a defined Thylacine behavior, or (b) returns a documented `errno`. There is no POSIX call that compiles, links, runs, and does the wrong thing.
- **P-4 — The boundary line holds.** pouch's upper half (vendored musl) contains no Thylacine-specific code; pouch's lower half contains all of it. The patch series touches only the lower half + the syscall seam.

---

## 12. Kernel-side work

A deliberately *bounded* list — most of pouch is userspace. The kernel needs:

1. **auxv population in `exec_setup`** — argc / argv / envp / auxv on the initial user stack, with at minimum `AT_PAGESZ`, `AT_RANDOM` (16 bytes), `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`, `AT_NULL`. (The research pinned the minimum.) This is owed to the future `exec` syscall regardless.
2. **The wait-on-address primitive** — the futex-equivalent syscall, spec'd by `futex.tla` (#7). Already on the spec roadmap.
3. **The anonymous-memory syscalls** — `burrow_attach(length) → vaddr` / `burrow_detach(vaddr, length)` (ARCHITECTURE.md §6.5 Tier 1, §11.2). [RESOLVED 12.1: the gap is real — the Burrow + VMA + demand-paging machinery exists and is audited (`burrow.tla` / I-7) but had no userspace entry point. The two syscalls are thin skins over the existing `burrow_create_anon → burrow_map → burrow_unref` discipline; `brk` is declined (the model drops it). Implemented at sub-chunk 7 (`pouch-mem`); the `0003-pouch-mman` patch retargets musl's `mman/` lower half onto them.]
4. **`set_tid_address` semantics** — return the tid; the clear-on-exit futex semantics matter only once threads use robust lists (deferred). Small.
5. **notes** — pouch's signal layer needs the kernel notes substrate. [RESOLVED 12.2] (2026-05-24): confirmed absent at sub-chunk 12 close; designed in ARCH §7.6.1-§7.6.8 as `pouch-signals-design` (the fd-first inversion + the async-handler compat path); implementation lands at sub-chunk 13a (`pouch-signals-impl`); pouch boundary-line patch + proving binary at sub-chunk 13b.
6. **Thread-spawn surface** — confirm Thylacine's thread-creation syscall accepts a caller-provided stack + entry (§7 [OPEN Q 7.3]).

Everything else pouch needs already exists: Spoors + 9P client (files), `/srv` (sockets), `SYS_POLL` (poll), the handle table (fds), `TPIDR_EL0` save/restore (TLS).

---

## 13. Exit criteria

- [ ] `aarch64-thylacine` cross-toolchain is complete — compiler, libc, CRT objects, **and the compiler-rt builtins** — and links via `ld.lld`; `tools/build.sh sysroot` produces a populated sysroot.
- [ ] A static "hello" C program, compiled with the cross-toolchain against pouch, runs in Thylacine — prints, exits 0, leaves no leak.
- [ ] `printf`-shaped hello (buffered stdio path) also works.
- [ ] A multithreaded test program — N threads, a shared mutex-protected counter, join — runs correctly under default + TSan.
- [ ] An `AF_UNIX` `SOCK_STREAM` echo client/server pair (pure POSIX source) round-trips over pouch → `/srv`.
- [x] **libsodium** cross-compiles against pouch and its self-test passes. *(sub-chunk 14, 2026-05-24 — libsodium 1.0.20 vendored byte-pristine at `third_party/libsodium/`; `tools/build.sh::build_libsodium` archives 97 .c files into `build/sysroot/lib/libsodium.a` (520780 B); `/pouch-hello-sodium` runs 5 primitives — `sodium_init`, SHA-256("abc") FIPS 180-4 KAT, BLAKE2b round-trip, xchacha20-poly1305-IETF AEAD round-trip, ed25519 sign + verify + reject-tampered. Detail: `docs/reference/84-pouch-libsodium.md`.)*
- [ ] **stratumd** compiles against the sysroot (with a Thylacine `peer_creds` arm) and links.
- [ ] stratumd **boots** in Thylacine, binds its `/srv` FS socket, and serves 9P — the Phase-5 stub is retired.
- [ ] joey mounts `/sysroot` from real stratumd; the ramfs→Stratum pivot completes.
- [ ] No P0/P1 audit findings on pouch's lower half or the kernel additions.
- [ ] The patch series against vendored musl is documented + reproducible.

The last two stratumd criteria **re-open and satisfy the bulk of Phase 5's own exit criteria** — this phase is what unblocks Phase 5 close.

---

## 14. Sub-chunk decomposition

Each sub-chunk lands independently with the two-commit pattern; audit-bearing ones get a focused round.

| # | Chunk | Scope | Audit-bearing |
|---|---|---|---|
| 1 | **pouch-toolchain** | Triple, sysroot layout, clang toolchain file + `pouch-clang` wrapper, `tools/build.sh sysroot` real | no |
| 2 | **pouch-musl-vendor** | Vendor pinned musl; the boundary-line patch series scaffold; build the portable upper half | no |
| 3 | **pouch-kernel-auxv** | `exec_setup` auxv population; `set_tid_address` | **yes** (exec / boot) |
| 4 | **pouch-syscall-seam** | Retarget `bits/syscall.h`; pouch's 1:1 syscall wrappers; the rc→errno mapping | **yes** (syscall surface) |
| 5 | **pouch-hello-smoke** | Static hello + buffered-stdio hello build + run; the first milestone | no |
| 6 | **pouch-compiler-rt** | Vendor + build the compiler-rt builtins for `aarch64-thylacine` (the compiler runtime a complete toolchain needs — `binary128` soft-float et al.); the `pouch-ld` link-driver wrapper; the real `printf` hello | no |
| 7 | **pouch-mem** | Allocator backend (anonymous-memory call); confirm/fill the kernel gap | **yes** (mm) |
| 8 | **pouch-wait-addr** | The wait-on-address kernel primitive `torpor`. No `specs/futex.tla` — CLAUDE.md "Spec-to-code suspended" was broadened 2026-05-23 to drop spec-first design itself; the no-lost-wakeup invariant is validated by prose reasoning + audit + runtime tests. | **yes** (scheduler / wait-wake) |
| 9 | **pouch-threads** | pthread create/join/detach + mutex/cond/rwlock/once + TLS errno | **yes** (concurrency) |
| 10 | **pouch-poll** | `poll`/`select`/`ppoll`/`pselect` retargeted onto `SYS_POLL` (kernel poll primitive audited in P5-poll-a/b) — **LANDED 2026-05-23** | no |
| 11 | **pouch-devnodes** | The minimal synthetic-FS namespace (§6.6): trivial `/dev` nodes (`null`/`zero`/`full`) as tiny kernel Devs; the `getrandom`-syscall path libsodium needs — **LANDED 2026-05-23** (devfull Dev + /pouch-hello-getrandom proving binary; path-based `open("/dev/null")` access deferred to a future multi-component-walk sub-chunk) | no |
| 12 | **pouch-sockets** | `AF_UNIX` `SOCK_STREAM` → `/srv`; `SO_PEERCRED` → `t_srv_peer`. Discovery at impl time: /srv shipped as a 9P-shaped channel — raw byte streams need a NEW kernel `SrvService.mode` (SRV_MODE_9P / SRV_MODE_BYTE) + `SYS_POST_SERVICE_BYTE` syscall + KOBJ_SRV read/write mode dispatch. corvus + stratumd stay on 9P mode; pouch sockets use byte mode. **LANDED 2026-05-23** | **yes** (capability surface + new kernel transport mode) |
| 13 | **pouch-signals** | The supported signal subset → notes. Splits into **13-design** (scripture: ARCH §7.6.1-§7.6.8 — the fd-first kernel notes substrate as a NOVEL.md §3.1 totalization), **13a-impl** (kernel substrate: `kernel/notes.c` + `kernel/devnotes.c` + the syscall surface + synthetic posters), **13b-pouch** (boundary-line patch `0007-pouch-signals.patch` + `/pouch-hello-signals` proving binary). | **yes** (13a kernel substrate; 13b pouch side) |
| 14 | **pouch-libsodium** | Cross-compile libsodium; self-test — **LANDED 2026-05-24** | no |
| 15 | **pouch-stratumd-build** | Build stratumd against the sysroot; Thylacine `peer_creds` arm — **LANDED 2026-05-24** (stratumd 860536 B static ET_EXEC at `build/pouch/progs/stratumd`; `tools/build.sh::build_stratumd` drives Stratum's CMake; toolchain adds `__thylacine__` + `_GNU_SOURCE` + routes link through `pouch-ld`; Stratum-side coordination on branch `thylacine-pouch-arm` in `~/projects/stratum/v2` — CMakeLists.txt Thylacine detection + peer_creds.c `__thylacine__` arm) | no (Stratum-side) |
| 16 | **pouch-stratumd-boot** | joey spawns real stratumd; `/sysroot` mount; ramfs pivot; retire the stub. **Sub-chunked 16a/16b/16c** (in-session 2026-05-24, user signoff). **16a LANDED 2026-05-24**: binary-load probe in joey (`t_spawn` + `t_wait_pid`; verifies libc init + argv parsing + clean exit); not audit-bearing. 16b (pool gen + block device + mount) and 16c (9P client + `/sysroot` mount + chroot + stub retire) remain audit-bearing. | **yes** (16b + 16c — boot ordering) |

`pouch-compiler-rt` was inserted as sub-chunk 6 — `pouch-hello-smoke` surfaced that the sysroot had no compiler runtime (POUCH-DESIGN.md §9 originally enumerated only headers + libc + CRT) and that clang cannot drive the ELF link on macOS. It executes next and is a hard prerequisite for the `pouch-libsodium` + `pouch-stratumd-build` sub-chunks. The `pouch-wait-addr` + `pouch-threads` sub-chunks are the critical path and the highest risk; `pouch-threads` may split. **`pouch-wait-addr` landed at 2026-05-23 without a TLA+ spec** — CLAUDE.md "Spec-to-code suspended" was broadened the same day to drop spec-first design itself; sub-chunk 8 is the worked example (the no-lost-wakeup invariant validated by reasoning in `kernel/torpor.c` + the audit + 7 runtime tests).

---

## 15. Risk register

- **R1 — the thread model is genuinely hard.** Mutex/condvar correctness under contention, TLS init ordering, the wait-on-address race surface. Mitigation: spec-first (`futex.tla`), TSan from the start, the chunk may split.
- **R2 — musl's lower half resists clean replacement.** musl's internals may be more entangled with Linux than the clean-seam model assumes. Mitigation: chunk 2 is exploratory; if the seam is dirtier than hoped, the patch series grows and we re-assess vendoring (§4).
- **R3 — Stratum-side coordination.** The `pouch-libsodium` + `pouch-stratumd-build` sub-chunks need Stratum changes (the `peer_creds` Thylacine arm, possibly the block-layer arm). User-owned; sequencing risk if Stratum and Thylacine drift.
- **R4 — scope creep into the renamed Phase 7 (Utopia).** A working pouch *invites* "let's port bash too." Discipline: this phase's proving set is hello + libsodium + stratumd. Utopia stays the next phase.
- **R5 — the `mmap`/anonymous-memory gap is larger than estimated.** If Thylacine's Burrow surface needs real extension for a malloc backend, the `pouch-mem` sub-chunk grows. Mitigation: [OPEN Q 12.1] resolved early.

---

## 16. Naming  *(Decision 10)*

**`pouch`** — the libc. **APPROVED (2026-05-22).** The marsupium — the pouch — is where the joey develops, protected, until it can survive in the open. Foreign POSIX code "runs in the pouch": pouch's translation shelters it from the fact that the kernel beneath is not the one it was written for. It is iconic to marsupials, short, friendly, and — unlike Plan 9's second-class APE — connotes something central and nurturing, not grudging. The design doc is `POUCH-DESIGN.md`; the libc lives at a `pouch`-named path.

**`torpor`** — the wait-on-address primitive. **APPROVED (2026-05-22).** A thread going dormant on an address until poked is a deep, specific dormancy; `torpor` is the marsupial deep-sleep state. A futex wait is precisely a torpor: a thread enters torpor on an address; another thread rouses it. `SYS_TORPOR` / `torpor_wait` / `torpor_wake`. (`torpor` was previously floated for the `_hang` WFI loop and held; if `_hang` later becomes torpor-themed too, the two are distinct mechanisms and share the family without collision — resolved acceptable, [OPEN Q 16.1] closed.)

**The triple / sysroot** — `aarch64-thylacine`; the sysroot is just "the sysroot." No thematic name forced.

---

## 17. Phase numbering & scripture relationship

**Proposal**: the current Phase 5 (9P client + Stratum integration) is **suspended**, substantially complete — the audit close just landed; the stub-bringup arc proved the syscall plumb. Its remaining items (real stratumd swap-in, P5-hostowner-c, P5-login) were always Pouch-dependent; they resume after this phase.

This phase becomes **Phase 6 — Pouch**. The old Phase 6 ("Syscall surface + musl + Utopia") loses its musl portion to this phase and is renamed **Phase 7 — Syscall surface completion + Utopia**. Old 7/8/9 shift to 8/9/10.

The ROADMAP §2.1 mapping table already flags the section headers as "stale by one phase." The renumber pass that lands alongside this phase's scripture is a chance to fix that drift in one motion. **[OPEN Q 17.1]** — the user may prefer a different scheme (e.g. "Phase 5.5", or absorbing Pouch as Phase 5's own final macro-chunk). The numbering is cosmetic; the *execution order* — Pouch before Phase 5 close — is the substantive call, already made.

Scripture cross-updates landing alongside this doc:
- `ROADMAP.md` — §2 phase diagram + §2.1 mapping table insert Phase 6 (Pouch); a new Pouch phase section; §3.6 ("compat built on top, not baked in") names pouch + invariant P-1; the old Phase 6 title drops "musl". **The full in-body phase-number renumber across ROADMAP/ARCH/VISION is a deferred doc-hygiene pass** — the ROADMAP §2.1 caveat already establishes this precedent (it has tracked a stale-by-one drift since Phase 3 was inserted); §2.1 is the authoritative phase registry.
- `ARCHITECTURE.md` — §28 gains a cross-reference note to the P-1..P-4 invariants (mirroring the C-1..C-23 corvus treatment); §25 gains an audit-trigger row for pouch's lower half + the kernel additions; §16 + §23 (the existing POSIX-compat sections) gain a banner naming POUCH-DESIGN.md as the binding design.
- `VISION.md` — §12 (Relationship to Linux and POSIX) names pouch as the Tier-1 realization + cross-refs here.
- `CLAUDE.md` — the gate-tied spec table already lists `futex.tla` (#7), `notes.tla` (#8), `poll.tla` (#6), `pty.tla` (#9); a future update notes their Pouch-consumer relationship + that Stratum is Thylacine-native-primary.

---

## 18. Decisions — resolved (design-session, 2026-05-22)

All twenty round-1 open questions resolved to the drafted leans (user signoff, 2026-05-22). Three (7.3, 12.1, 12.2) resolve to "this approach is settled; verify the kernel-side detail in the named implementation chunk." The round-2 item (6.6) is the only entry still pending confirmation.

| ID | Question | Resolved |
|---|---|---|
| 4.1 | musl as pouch's portable-half source | ✅ yes — musl |
| 4.2 | Vendoring: in-tree copy vs. submodule | ✅ in-tree copy + patch series |
| 5.1 | Thylacine-rc → POSIX-errno mapping in pouch's lower half | ✅ yes, per-call |
| 6.1 | Block device: `/dev` namespace vs. inherited handle | ✅ inherited handle (capability-pure) |
| 6.2 | `AF_UNIX` paths outside `/srv/` | ✅ unsupported this phase |
| 6.3 | `SOCK_DGRAM` | ✅ deferred |
| 6.4 | `SIGPIPE` default-ignore vs. synthesized delivery | ✅ default-mask + `EPIPE` (kernel still posts the note; pouch's startup mask suppresses delivery; opt-in via sigaction) |
| 6.5 | Note causal ordering vs. POSIX signal ordering | ✅ stronger — N-1..N-5 in ARCH §7.6.7 |
| 6.6 | minimal-namespace scope + Devs-vs-servers split for trivial nodes | ✅ confirmed (round 3) — minimal namespace this phase; trivial nodes as kernel Devs; richer namespace → Utopia |
| 7.1 | wait-on-address: absolute deadline / relative / both | ✅ absolute deadline (reuse `tsleep`) |
| 7.2 | wait-on-address spec is `futex.tla` | ✅ yes |
| 7.3 | Thylacine thread-spawn accepts caller stack+entry | ✅ approach settled; verify kernel detail in the `pouch-threads` sub-chunk |
| 8.1 | allocator backend: POSIX `mmap` shape vs. leaner Thylacine call | ✅ leaner internal call |
| 8.2 | `fork()` decline confirmed | ✅ decline; ship `posix_spawn` |
| 9.1 | Triple name | ✅ `aarch64-thylacine` |
| 10.1 | Thylacine-first software: native extensions vs. portable+arms | ✅ portable + thin per-OS arms |
| 11.1 | Invariant numbers in ARCH §28 | ✅ P-numbered; canonical in §11; cross-ref'd from ARCH §28 (mirrors the corvus C-invariant treatment) |
| 12.1 | anonymous-memory kernel gap size | ✅ approach settled; size it in the `pouch-mem` sub-chunk |
| 12.2 | `notes` implementation status | ✅ confirmed absent 2026-05-24; design landed (ARCH §7.6.1-§7.6.8 + NOVEL.md §3.1); fd-first novel inversion approved by user; impl owed to sub-chunk 13a |
| 16.1 | `torpor` for both `_hang` and the futex — muddle? | ✅ distinct mechanisms; acceptable |
| 17.1 | Phase numbering scheme | ✅ Pouch = Phase 6; renumber the tail |

Names locked: **`pouch`** (the libc), **`torpor`** (the wait-on-address primitive). See §16.

---

*Design-session complete (3 rounds, 2026-05-22). Binding. The ROADMAP / ARCHITECTURE / VISION cross-updates (§17) land alongside this doc; implementation then begins at sub-chunk 1 (`pouch-toolchain`).*
