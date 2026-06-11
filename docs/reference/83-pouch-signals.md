# 83 — pouch signals: the POSIX signal layer over Thylacine notes (P6-pouch-signals sub-chunk 13b)

The userspace half of pouch's POSIX signal surface — the boundary-line patch `0007-pouch-signals` that retargets musl's `src/signal/` onto the kernel notes substrate landed at sub-chunk 13a (`SYS_NOTE_OPEN=44` / `SYS_NOTIFY=45` / `SYS_NOTED=46` / `SYS_POSTNOTE=47` / `SYS_NOTE_MASK=48`). pouch's signal surface — `sigaction` / `signal` / `raise` / `kill` / `pthread_sigmask` / `sigprocmask` — comes from this layer.

Per `POUCH-DESIGN.md §6.4` (the POSIX <-> notes mapping) + `ARCH §7.6.1-§7.6.8` (the kernel notes substrate; design landed at `237f096`, impl across 4 audit rounds at `7fdaf5a..c8bdae3`) + `NOVEL.md §3.1` ("Signals as a synthetic filesystem" totalization). Audit-trigger surface (`CLAUDE.md` / `ARCHITECTURE.md §25.4`): the pouch boundary-line is audited at chunk close (sub-chunk 13b); the kernel substrate has already been through 4 rounds (42 findings closed).

---

## Purpose

POSIX programs call `sigaction(SIGINT, &sa, NULL)`, `raise(SIGINT)`, `kill(pid, SIGTERM)`, `pthread_sigmask(SIG_BLOCK, &m, NULL)`, and expect them to behave per POSIX.1. pouch presents that surface from musl's portable upper half (signal.h, sigsetops, sigemptyset, etc. — unchanged) by replacing musl's LOWER half — the OS-boundary calls (`SYS_rt_sigaction`, `SYS_rt_sigprocmask`, `SYS_tkill`, `SYS_kill`, `SYS_rt_sigreturn`) — with calls onto Thylacine's kernel notes API.

The translation is intentionally partial. Plan 9 notes are string-named and causally-ordered; pouch maps the small set of POSIX signals real daemons need:

| POSIX signal | Thylacine note | Catchable | v1.0 default |
|---|---|---|---|
| `SIGINT` (2) | `interrupt` | yes | terminate (`exits("interrupt")`) |
| `SIGTERM` (15) | `interrupt` (shared with SIGINT — documented v1.0 limitation) | yes | terminate |
| `SIGKILL` (9) | `kill` (`sigaction(SIGKILL)` returns EINVAL per POSIX) | **no** — kernel-side N-4 enforced | terminate (kernel calls `exits("killed")`) |
| `SIGPIPE` (13) | `pipe` | yes (default-masked at startup; see below) | terminate (if mask cleared) |
| `SIGCHLD` (17) | `child_exit` | yes | ignore |
| every other signal | (unsupported) | — | `sigaction()` returns `EINVAL` |

The kernel's note ABI is **stronger** than POSIX signals — every posted note is consumed exactly once (N-2); deliveries from a single source are post-ordered (N-1); `kill` is non-catchable regardless of mask + handler + in_handler state (N-4). POSIX programs receive these stronger guarantees transparently.

---

## Files

The patch series entry is `usr/lib/pouch/patches/0007-pouch-signals.patch`. Nine files:

| musl file | What it does | pouch retarget |
|---|---|---|
| `arch/aarch64/bits/syscall.h.in` | The syscall-number table | Appends 5 Thylacine note extension numbers (44-48). musl's build-time sed pass auto-generates `SYS_note_open` / `SYS_notify` / `SYS_noted` / `SYS_postnote` / `SYS_note_mask` aliases. |
| `src/internal/_pouch_signal.h` | NEW | The pouch signal layer's pouch-private API: per-Proc sigaction table; __thread note-mask shadow; signum<->note translation helpers; bootstrap handler forward decl. |
| `src/signal/_pouch_signal.c` | NEW | The bootstrap async-handler implementation + the `.init_array` constructor that registers it via `SYS_NOTIFY`. |
| `src/signal/sigaction.c` | Full rewrite | Validates signum (refuses everything outside {SIGINT, SIGTERM, SIGPIPE, SIGCHLD}); records the user `struct sigaction` into `__pouch_sigtab[sig]`; adjusts the kernel `NOTE_BIT_PIPE` mask on SIGPIPE handler install/uninstall. Loses musl's SIGABRT/__abort_lock interlock (SIGABRT not in the v1.0 set). |
| `src/signal/raise.c` | Full rewrite | Translates `sig -> note name`, calls `syscall(SYS_postnote, 0, name, name_len)`. The `0` is the kernel's self-post sentinel (kernel/syscall.c `sys_postnote_handler` accepts `pid_raw == 0` as "post to my own Proc"). |
| `src/signal/kill.c` | Full rewrite | Same shape as `raise.c` but passes the caller-supplied pid. |
| `src/signal/block.c` | Full rewrite | `__block_all_sigs` / `__block_app_sigs` / `__restore_sigs` map onto `SYS_NOTE_MASK`. |
| `src/signal/aarch64/restore.s` | Rewrite | Replaces the legacy `mov x8,#139 // SYS_rt_sigreturn` stub with `SYS_NOTED(NCONT)`. The symbol is unreferenced at v1.0 (sigaction.c doesn't install a sa_restorer) — defense-in-depth in case of future indirect calls. |
| `src/thread/pthread_sigmask.c` | Full rewrite | Marshals POSIX sigset_t <-> NOTE_BIT_* via the `_pouch_signal` helpers; reads `__pouch_note_mask_shadow` for `SIG_BLOCK` / `SIG_UNBLOCK` read-modify-write paths. `sigprocmask.c` delegates here (UNCHANGED — already a thin wrapper around `pthread_sigmask`). |

Plus the kernel paired change: `sys_postnote_handler` in `kernel/syscall.c` extends the self-post fast-path gate from `target_pid == p->pid` to `target_pid == p->pid || pid_raw == 0`. The sentinel is documented in `kernel/include/thylacine/syscall.h`'s SYS_POSTNOTE docblock. POSIX `kill(0, sig)` semantics (send to every process in the calling process's group) reduce to "send to my own Proc" since Thylacine has no process groups at v1.0.

Files NOT modified despite referencing Linux signal syscall numbers (all hit the `0xFFFF` sentinel → -ENOSYS via the unchanged 0001 syscall seam):

- `src/signal/sigsetjmp_tail.c` — `siglongjmp` doesn't restore signal masks at v1.0.
- `src/signal/sigsuspend.c` — unsupported.
- `src/signal/sigaltstack.c` — unsupported (no alt-stacks at v1.0).
- `src/signal/sigpending.c` — unsupported.
- `src/signal/sigtimedwait.c` — unsupported.
- `src/signal/sigqueue.c` — unsupported (no siginfo_t).
- `src/signal/signal.c` — UNCHANGED (delegates to `__sigaction`).
- `src/signal/sigprocmask.c` — UNCHANGED (delegates to `pthread_sigmask`).

---

## The bootstrap handler — Plan-9-style ABI

`__pouch_note_handler(const char *name, unsigned int arg)` is registered once at process startup via `SYS_NOTIFY(handler_va)`. The kernel's EL0-return-tail dispatcher (in `arch/arm64/exception.c::exception_sync_lower_el`, called via `notes_deliver_at_el0_return`) calls this function at the eret edge of every syscall when a note is queued and the calling Thread has no `in_handler == true`. The kernel:

1. Pops the next deliverable note from the Proc's queue under `q->lock`.
2. Reserves 16 bytes at `(orig_sp - NOTE_NAME_MAX) & ~0xf` on the user stack via `uaccess_store_u8` per byte; pushes `struct Note.name[16]` (NUL-padded).
3. Saves the full user context (regs[0..30] + sp_el0 + elr + spsr) into the Thread's `note_saved_*` fields.
4. Rewrites `ctx->regs[0] = new_sp` (pointer to name on stack); `ctx->regs[1] = note arg`; `ctx->sp = new_sp`; `ctx->elr = handler_va`; spsr unchanged.
5. Sets `t->in_handler = true`.
6. Erets to userspace at `handler_va`.

`__pouch_note_handler` runs at EL0:

```c
hidden void __pouch_note_handler(const char *name, unsigned int arg) {
    int sig = -1;
    if (__pouch_note_name_eq(name, "interrupt")) {
        // SIGINT preferred over SIGTERM when both have handlers installed.
        if (__pouch_sigtab[SIGINT].sa_handler != SIG_DFL && != SIG_IGN)
            sig = SIGINT;
        else if (...) sig = SIGTERM;
        else sig = SIGINT;  // for default-action purposes
    } else if (...) ...

    void (*h)(int) = __pouch_sigtab[sig].sa_handler;
    if (h == SIG_DFL) {
        if (sig == SIGCHLD) __syscall(SYS_noted, NCONT);
        else                __syscall(SYS_noted, NDFLT);
    } else if (h == SIG_IGN) {
        __syscall(SYS_noted, NCONT);
    } else {
        h(sig);
        __syscall(SYS_noted, NCONT);
    }
}
```

`SYS_NOTED(NCONT=0)` restores the saved user context — `ctx->regs[*]`, `sp`, `elr`, `spsr` — from `t->note_saved_*` (per `notes_noted_restore` in `kernel/notes.c`); the original code resumes one instruction past the syscall that triggered delivery. `SYS_NOTED(NDFLT=1)` takes the note's default action — for v1.0's supported set, that's `exits(name)` (terminate the Proc with the note name as the exit string).

The handler's stack discipline: the kernel arranges `sp_el0` 16-aligned per AAPCS64; the 16 bytes of note name sit at `sp_el0` (x0 points there); the C handler's prologue can save callee-saved regs below `sp_el0 - 16` per normal AAPCS64 (the kernel didn't reserve a red zone — the handler is a fresh frame).

---

## The constructor — startup wiring

`__pouch_signal_init` is registered as an `__attribute__((constructor))` function. musl's CRT (`csu/__libc_start_main.c` → `__init_libc` → `libc_start_init` → iterates `.init_array`) runs it once at process startup on the main thread, BEFORE `main()`. It does two things:

```c
__pouch_note_mask_shadow = POUCH_NOTE_BIT_PIPE;          // local TLS shadow
__syscall(SYS_note_mask, POUCH_NOTE_BIT_PIPE, 0);        // kernel side
__syscall(SYS_notify, (long)__pouch_note_handler);       // register bootstrap
```

The SIGPIPE default-mask is the modern-daemon-friendly behavior: a write-to-closed-pipe returns `EPIPE` per POSIX, but no `SIGPIPE` is delivered to the bootstrap (the note stays queued but masked). A subsequent `sigaction(SIGPIPE, &sa, NULL)` with a non-default handler clears `NOTE_BIT_PIPE` (so the note is delivered + the handler fires).

Note that the constructor sets only the **main thread's** mask. Child threads spawned via `pthread_create` start with `note_mask = 0` (the kernel's `SYS_THREAD_SPAWN` does not inherit the parent's note_mask at v1.0 — documented limitation; v1.x extension). Programs that need POSIX-correct mask inheritance set the child's mask manually in their entry function via `pthread_sigmask`.

---

## State

### Per-Proc sigaction table

`__pouch_sigtab[_NSIG]` (`= [65]` on aarch64 musl) — the user's registered `struct sigaction` for each signum. Only slots {SIGINT, SIGTERM, SIGPIPE, SIGCHLD} are written; other slots remain zero-initialized. `sigaction(sig, NULL, &old)` reads the slot (returns the current handler/flags/mask). `sigaction(sig, &sa, NULL)` writes.

The table is per-Proc — POSIX semantics are explicit that `sigaction` is process-scoped (not thread-scoped). All threads in a Proc share this table.

### Per-Thread note-mask shadow

`__pouch_note_mask_shadow` is `__thread`-storage TLS, zero-initialized at thread startup. Pouch is the sole writer per thread (only sigaction-side SIGPIPE-mask adjustment and pthread_sigmask call `__syscall(SYS_note_mask, ...)`; no other code path touches the kernel mask). The shadow stays consistent with the kernel because every `SYS_NOTE_MASK` call updates both.

The shadow is per-Thread so `pthread_sigmask` (which is per-thread by POSIX) implements `SIG_BLOCK` and `SIG_UNBLOCK` via read-modify-write without racing across threads. A thread reads its own shadow, computes the new mask, writes via `SYS_NOTE_MASK` + updates the shadow.

---

## SIGINT ↔ SIGTERM aliasing (v1.0 limitation)

Both SIGINT and SIGTERM map to the kernel note `"interrupt"`. The bootstrap dispatcher recovers the signum by checking which user handler is installed:

```
if (SIGINT handler != SIG_DFL/IGN)         sig = SIGINT
else if (SIGTERM handler != SIG_DFL/IGN)   sig = SIGTERM
else                                       sig = SIGINT  (default-action only)
```

This makes a program that registers ONLY SIGINT see all "interrupt" notes routed to SIGINT's handler; same for SIGTERM-only. A program that registers BOTH sees its SIGINT handler invoked for every interrupt note. The user-visible POSIX limitation: `raise(SIGINT)` and `raise(SIGTERM)` are indistinguishable at the bootstrap.

The v1.x extension lifts this by adding a `term` note (separate kernel name); the bootstrap then dispatches each signum to its own note string. Out of scope at v1.0.

---

## Wire example — `/pouch-hello-signals`

Single-threaded; the proving binary exercises:

```c
sigaction(SIGINT, &handler, NULL);     // __pouch_sigtab[SIGINT] = handler
raise(SIGINT);                          // SYS_postnote(0, "interrupt", 9)
// kernel queues "interrupt" + on EL0-return-tail dispatches to
// __pouch_note_handler with x0=name VA, x1=0.
// __pouch_note_handler: SIGINT handler installed → calls handler(SIGINT).
// handler sets g_handler_count++; returns.
// __pouch_note_handler: SYS_noted(NCONT) → kernel restores saved user
//   context → raise() returns 0 with the side effect visible.
assert(g_handler_count == 1);

sigaction(SIGINT, SIG_IGN, NULL);
raise(SIGINT);                          // queued + delivered + bootstrap
// sees SIG_IGN → NCONT (no handler call); count stays at 1.
assert(g_handler_count == 1);

sigaction(SIGUSR1, &handler, NULL);     // EINVAL — unsupported v1.0 signum
```

Output (joey relays via the pipe-to-UART):

```
pouch-hello-signals: install handler
pouch-hello-signals: raise SIGINT
pouch-hello-signals: handler ran (count=1)
pouch-hello-signals: install SIG_IGN
pouch-hello-signals: raise SIGINT (ignored)
pouch-hello-signals: count unchanged (count=1)
pouch-hello-signals: unsupported sigaction returns EINVAL
pouch-hello-signals: exit 0
```

joey's `do_pouch_hello_smoke` content-checks the trailing `exit 0` and the non-zero status from any failed assertion is surfaced as a boot regression.

---

## Known caveats / footguns

- **SIGTERM aliased with SIGINT** (R1-F9). Two POSIX signals share one note; the bootstrap arbitrates by handler-presence. Programs that need to distinguish them require the v1.x `term` note. The user-facing limitation is documented in the v1.0 manual.
- **No mask inheritance across `pthread_create`**. The kernel's `SYS_THREAD_SPAWN` does not propagate `t->note_mask` from parent to child. Child threads start with mask=0; programs that need POSIX-correct inheritance manually set the mask in their entry function. v1.x extension: kernel propagates at spawn time.
- **`abort()` extincts the kernel at v1.0** (R1-F4). musl's `src/exit/abort.c` reaches `a_crash()` (a deliberate NULL deref) before its `_Exit(127)` tail. At v1.0 the kernel's `FAULT_UNHANDLED_USER` policy extincts on EL0 faults from any pouch program — so abort() manifests as kernel extinction rather than clean process termination. PRE-EXISTING limitation (not introduced by sub-chunk 13b); pre-13b pouch had the same path because `raise(SIGABRT)` hit the SYS_tkill 0xFFFF sentinel and abort() reached a_crash() regardless. v1.x extensions: (1) override pouch's abort.c to `_Exit(127)` directly, bypassing a_crash; (2) deliver SIGSEGV-shaped note instead of extincting on EL0 fault.
- **`siglongjmp` does not restore signal masks**. The `sigsetjmp_tail.c` path hits `SYS_rt_sigprocmask = 0xFFFF` → -ENOSYS. The `siglongjmp` itself still works (the non-signal-mask portion). v1.x extension via the same boundary line.
- **`alarm` / `sigsuspend` / `sigaltstack` / `sigpending` / `sigtimedwait` / `sigqueue` / `pthread_kill` all return -ENOSYS** (R1-F13). Modern daemons rarely use these; the v1.0 supported set is curated to "what stratumd + libsodium need." `pthread_kill(thread, sig)` issues SYS_tkill = 0xFFFF — -ENOSYS at runtime; raise() is the only per-Thread signal source at v1.0; cross-Thread targeting deferred to v1.x.
- **No real-time signals (SIGRTMIN..SIGRTMAX)**. `sigaction()` returns EINVAL.
- **No `siginfo_t`**. The kernel's `struct note_record` carries the analog (name + arg + sender_pid + timestamp_ns) for the fd-shaped path (`SYS_NOTE_OPEN`); the POSIX async handler shape is the legacy `void(int)`. Daemons that need rich info read the fd via the modern path.
- **Bootstrap handler stack discipline**: the kernel pushes 16 bytes for the note name at the new sp. The handler runs at EL0 with that 16-byte block at sp; the handler must not stomp it (AAPCS64 prologue saves below sp, never above). Confirmed by inspection.
- **SIGPIPE mask-adjust is per-Thread, not per-Proc** (R1-F-SELF-1). `__pouch_note_mask_shadow` is `__thread` TLS. A multi-thread Proc that calls `sigaction(SIGPIPE, &handler, NULL)` on thread A updates ONLY thread A's kernel `note_mask`. Other threads still have NOTE_BIT_PIPE set. POSIX requires the change to apply to every thread in the Proc. v1.0 limitation; v1.x extension: SYS_NOTE_MASK with a "Proc-wide" flag, or sigaction iterates threads.
- **SIG_IGN does not discard pending SIGPIPE notes** (R1-F3). POSIX 2017 §2.4 says SIG_IGN discards pending instances. Pouch sets BIT_PIPE in the kernel mask, which DEFERS delivery (queued, not discarded); a subsequent sigaction(SIGPIPE, &real_handler, NULL) clears the mask and the queued notes deliver retroactively. POUCH-DESIGN §6.4 [RESOLVED 6.4] embraced the masked-by-default behavior; the SIG_IGN-discard divergence is a v1.0 limitation. v1.x extension: drain pending notes on the SIG_IGN/SIG_DFL transition.
- **Multi-thread Proc + SIG_DFL non-SIGCHLD note: NDFLT group-terminates** (superseded: #809 `SYS_EXIT_GROUP` + the RW-8 R5-F1 fix). The 13a-era kernel gate that refused NDFLT in multi-thread Procs is RETIRED — NDFLT now cascades via `proc_group_terminate` (the #811 wake-total primitive), so SIG_DFL for a fatal signal terminates the whole Proc, matching POSIX. The pouch bootstrap's NCONT fallback remains only as the error-path safety net.
- **pthread_sigmask sigset_t round-trip is lossy + spurious-bit-additive** (R1-F6). The translation drops unsupported signums. Round-trip via `pthread_sigmask` adds a SIGTERM partner whenever SIGINT is in the mask (because both map to BIT_INTERRUPT). v1.x extension: parallel sigset_t shadow per Thread for byte-identical round-trip.
- **`raise(SIGKILL)` in a multi-thread Proc group-terminates** (superseded: #809). The 13b-era kill-vs-multi-thread refusal (R1-F9's `-1`→EIO) is RETIRED — `kill` cascades via `proc_group_terminate` uniformly. The errno-precision note below still applies to genuine failures.
- **`kill()`'s precise errno (ESRCH / EPERM) is collapsed to EIO** (R1-F-SELF-6). pouch's `syscall(SYS_postnote, ...)` returns the Thylacine -1; `syscall_ret.c` maps to EIO. v1.x: kernel returns -errno (-ESRCH/-EPERM) instead of flat -1.
- **`raise()` does not coalesce** (R1-F14). Two calls of `raise(SIGINT)` enqueue two "interrupt" entries; the bootstrap dispatches the handler twice. POSIX is loosely specified here (without `sigqueue`, signals are not formally queued); pouch's v1.0 behavior is "every raise delivers." Acceptable.
- **`sa_handler` write is not atomic; bootstrap reads non-atomically** (R1-F5). The bootstrap reads `__pouch_sigtab[sig].sa_handler` (offset 0; naturally aligned 8 bytes on aarch64; single-word loads/stores are atomic at the platform level). The struct-copy `__pouch_sigtab[sig] = *sa;` is multi-word but ONLY sa_handler is read by the bootstrap, so torn-read for sa_handler is impossible on aarch64. Future enhancement that reads sa_mask or sa_flags in the bootstrap would need `__atomic_store_n` (v1.x).
- **Kernel note-name set is duplicated between kernel and pouch** (R1-F15). The literals "interrupt", "kill", "pipe", "child_exit" are defined in `kernel/notes.c::g_known_notes` AND `src/signal/_pouch_signal.c::__pouch_sig_to_note`. v1.x technical-debt cleanup: factor into a shared header `<thylacine/notes_abi.h>` both kernel and pouch include.
- **Constructor ordering**: SYS_NOTE_MASK runs BEFORE SYS_NOTIFY in `__pouch_signal_init` (R1-F12). The order ensures the BIT_PIPE mask is in place before the handler_va is set, so a hypothetical pre-main note delivery (impossible at v1.0; no concurrent posters exist at startup) wouldn't run the bootstrap against an unset sigtab. The invariant is documented as a code comment.

---

## Static asserts + ABI pinning

- `_NSIG = 65` (aarch64 musl default). `__pouch_sigtab` size = `65 * sizeof(struct sigaction)` ≈ 9 KB statically allocated in `.bss`.
- `POUCH_NOTE_NAME_MAX = 16` matches kernel `NOTE_NAME_MAX` in `<thylacine/notes.h>`.
- `POUCH_NOTE_BIT_*` constants pin the wire bit positions to the kernel (`<thylacine/notes.h>` `NOTE_BIT_*`).
- `POUCH_SYS_NOTED_NCONT = 0` / `POUCH_SYS_NOTED_NDFLT = 1` match the kernel `SYS_NOTED` arg ABI (see `kernel/syscall.c::sys_noted_handler`).

---

## Naming rationale

`pouch` (the libc) was the named home for POSIX over Thylacine (POUCH-DESIGN.md §16). The signal layer doesn't introduce new themed names — it uses POSIX's `sigaction` / `raise` / `kill` / `pthread_sigmask` directly, since those are the POSIX surface programs expect. The kernel substrate uses the Plan 9 heritage term `notes` (per `kernel/include/thylacine/notes.h`).

The boundary-line file convention (`_pouch_signal.h`, `_pouch_signal.c`) mirrors `_pouch_socket.h`/`_pouch_socket.c` from sub-chunk 12 — the `_pouch_*` prefix marks pouch-private files inside the musl tree, not part of musl's upstream surface.

---

## Cross-references

- `docs/POUCH-DESIGN.md §6.4` — the binding signals-over-notes design.
- `docs/ARCHITECTURE.md §7.6.1-§7.6.8` — the kernel notes substrate (canonical invariants).
- `docs/NOVEL.md §3.1` — "Signals as a synthetic filesystem" (the fd-first novel angle).
- `kernel/include/thylacine/notes.h` — the kernel API (N-1..N-5 invariants).
- `kernel/include/thylacine/syscall.h` SYS_POSTNOTE / SYS_NOTIFY / SYS_NOTED / SYS_NOTE_MASK docblocks — wire ABI.
- `docs/reference/78-pouch.md` — pouch overview (the broader pouch architecture).
- `docs/reference/82-pouch-pthread.md` — sub-chunk 9b parallel (pthread layer).
- `memory/audit_p6_pouch_signals_13a_closed_list.md` — the kernel substrate's 4-round audit closed list (42 findings).
