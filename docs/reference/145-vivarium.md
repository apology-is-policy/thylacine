# 145 — Vivarium: the phenotype and the syscall-entry branch

**Status**: as-built at **V-8, the arc close**. The declaration
(`SPAWN_PHENO_LINUX`), the syscall-entry branch, and the Tier-1/Tier-2 dispatch
shells — including `mmap`/`munmap` (V-2d), the socket family (V-5) and signals
through a running handler (V-6) — are live and boot-gated. **V-3's supervisor is
deferred, and not merely unbuilt**: its sketched destination cannot serve the
forwarded set at all (§7 below, and `VIVARIUM.md` §4.1), and V-5 then measured
that sockets need no supervisor either, so the fork travels to the next chunk
that needs a destination it cannot synthesise — process creation, task #93.

What is NOT built is stated in `VIVARIUM.md` §9, which is the scope contract:
the OUT list (`epoll`, `ptrace`, `io_uring`, process creation, …) answers a
clean `ENOSYS`, and the DEGRADED table names every place a call is served but
differs from Linux observably. Read §9 before concluding this chapter's silence
means a call works.

Design: `docs/VIVARIUM.md` (§4 the hybrid split, §5 the mechanism, §12.1 the
declaration rules, §8 invariant **I-43**, §9 the fidelity ladder). Invariant:
`ARCHITECTURE.md §28 I-43`. Audit surface: `ARCHITECTURE.md §25.4` (the V-1b
row) — the focused round is **V-8**, closed below.

V-2 built the translation tables and deliberately left them uncalled, because
`Proc.phenotype` could not yet be set to anything but 0 and a branch on a
provably-zero field is dead code. V-7 landed the container object that declares
it. This chapter is what those two chunks joined into.

---

## 1. What a phenotype is

A per-Proc `u8` (`Proc.phenotype`, offset 347) with two values:

| value | meaning |
|---|---|
| `PHENO_NATIVE` (0) | the default; syscall numbers are Thylacine's |
| `PHENO_LINUX` (1) | syscall numbers and argument structures are Linux aarch64's |

It is set **once**, in the spawn thunk, before `exec_setup` and before the child
reaches EL0 — the same set-once-before-EL0 window the identity and allowance
overrides use, and race-free for the same reason (the child has no peer thread
yet, so a plain store has no concurrent reader). It is inherited by `rfork`
like the other per-Proc properties, which is §12.1 rule 2: within a
declared-Linux vivarium, every exec is Linux.

There is **no syscall that changes a running Proc's phenotype.** That is not an
omission — a Proc that could re-decode its own ABI at runtime would be a strange
and useless attack surface.

## 2. I-43, and why it holds by construction

> A phenotype confers ABI **shape**, never **authority**.

The code is arranged so this is structural rather than a property someone has to
keep re-verifying:

- **A Tier-1 row is a renumber performed in place.** `viv_linux_dispatch`
  rewrites `ctx->regs[8]` and the argument registers and returns `true`, and
  `syscall_dispatch` then **falls through into the ordinary native `switch`**.
  The call lands on the very same `sys_*_handler` a native caller reaches, with
  the same capability gate, the same `stalk` resolution, the same `perm_check`,
  the same resource charge. There is no second implementation in which a gate
  could be missing.
- **A Tier-2 row calls the same `sys_*_for_proc` core.** `sys_fstat_for_proc`
  was *extracted* from `sys_fstat_handler` at V-1b precisely so the phenotyped
  path could share it rather than reimplement the kind gate and the ref
  discipline. The phenotype-specific work is confined to pure argument reshaping
  and struct conversion in `kernel/vivarium.c`.

## 3. Why the declaration is ungated

Every `SPAWN_PERM_*` bit is gate-checked, because each confers a **role**.
`SPAWN_PHENO_LINUX` is not gated, and the reason is worth stating because the
asymmetry looks wrong at a glance.

Every Linux number the table translates also exists as a live native number:

| Linux | native at the same number |
|---|---|
| 56 `openat` | `SYS_READDIR` |
| 57 `close` | `SYS_RENAME` |
| 62 `lseek` | `SYS_BOOT_COMPLETE` |
| 63 `read` | `SYS_CONSOLE_RELINQUISH` |
| 64 `write` | `SYS_CONSOLE_OPEN` |
| 79 `newfstatat` | `SYS_CLOCK_SETTIME` |
| 94 `exit_group` | `SYS_TTY_SIGNAL` |

So a Proc wrongly branded Linux really does mis-decode its own calls — and that
is exactly as far as it goes. The mis-decoded call still passes every gate the
native caller would have faced, on the mis-brander's own Proc, with its own
authority. It breaks itself and reaches nothing new. A parent that wanted that
outcome could equally have spawned a binary of its own authorship that made the
same wrong calls natively.

The collision table is also why §12.1 rule 3 — *outside a vivarium the phenotype
is always native* — is load-bearing rather than tidy, and why the ELF byte may
corroborate but never decide.

**When adding a table row, re-check this argument against the new number** —
and one more: the branch copies all six argument registers verbatim, so a
Tier-1 row is sound only while its native target reads **no more arguments than
the Linux call supplies**. That is true of all five current rows
(read/write/close/lseek/exit_group), and it is the kind of thing a new row
breaks silently, because the extra register would carry whatever the Linux
caller happened to leave there.

**The re-check, performed for V-2d's rows.** `mmap` 222, `munmap` 215 and
`mprotect` 226 collide with *no* native number — all three are currently
unassigned (above the native ceiling; see below), so natively
they reach the dispatcher's `default:` and answer `-1`. The argument still holds,
but by a different clause than the collision table's: a mis-declared Proc issuing
222 now receives an anonymous mapping where it would have received `-1`, and that
is not new authority because **both targets are ungated syscalls that operate
only on the caller's own address space** — `SYS_BURROW_ATTACH_LAZY` (83) and
`SYS_BURROW_DETACH` (38) require no capability and the Proc could call either
directly. `mprotect` dispatches nothing at all. Arity: `mmap` supplies six
arguments and the shell reads exactly `args[0..5]`; `munmap` supplies two and the
shell reads `args[0..1]`.

**Design D (2026-09-01; VIVARIUM section 13.10; scripture `56085f83`).** The
declaration's *timing* changed after this section was written, and the argument above
survives unchanged: a phenotype is now the ABI of the LOADED IMAGE, decided at **every
image load** -- every spawn variant AND `execve` -- by one function
(`phenotype_decide`, `proc.h`): `PHENO_LINUX` iff the exec resolution crossed an
`MPHENO_LINUX` mount OR the resolving Territory declares Linux (`Territory.root_pheno`,
set on the child's clone by `SPAWN_PHENO_LINUX` before EL0, copied by
`territory_clone`); else native. `rfork` preserves it (no new image); `execve`
re-decides it and stores the field ONLY in `proc_exec_replace`'s infallible commit
region, RELEASE-ordered before the phenotype-conditional signal reset, which branches on
the decided value rather than the field (the three-legged ordering hazard the design
review found, 13.10.4). Where this section or others in this file say the phenotype is
"set once in the spawn thunk" or "inherited" across an exec, read: decided at every
image load. The ONE place the field touches authority is the fork cap-inheritance
policy (`rfork_forked_with_caps` under `PHENO_LINUX`); an `execve` that flips the
phenotype changes that policy for the image's subsequent forks -- ABI shape, I-2-bounded
(review F5), recorded in `ARCHITECTURE.md` section 28 I-43.

## 4. The dispatch path

```
syscall_dispatch(ctx)
  └─ phenotype == PHENO_LINUX?
       ├─ no  → the native switch, byte-unchanged
       └─ yes → viv_linux_dispatch(ctx, p)
                  ├─ VIV_TRANSLATED → rewrite ctx; return true → NATIVE switch
                  ├─ VIV_TIER2      → viv_tier2(p, nr, args); return false
                  ├─ VIV_FORWARD    → -ENOSYS (V-3 replaces this arm); false
                  └─ VIV_ENOSYS     → -ENOSYS; false
```

`VIV_FORWARD` and `VIV_ENOSYS` are kept as separate case arms even though they
produce the same wire answer today. §4's Option C routes a non-translatable call
to a userspace supervisor, which is V-3; until it exists, the honest answer for
both is `-ENOSYS` (§9's ladder: *ENOSYS is a supported outcome; a lie is not*).
Keeping the arms distinct makes V-3's change one line.

### The Tier-2 shells

The impure shells, one per translator, all in `kernel/syscall.c` (the pure
halves stay in `kernel/vivarium.c` so they remain unit-testable with no kernel
plumbing). The file translators the V-2b/c arc built first:

- **`openat`** — `vivarium_openat_decide` first, then `viv_measure_user_path`,
  then `vivarium_openat_build`, then `sys_open_handler`. **Decide before
  measure** is deliberate: measuring is a faultable user read, and a call
  destined for the supervisor should not take that fault inside the kernel fast
  path on a buffer the supervisor would have validated itself.
- **`fstat`** — `sys_fstat_for_proc` into a kernel `t_stat`, then
  `vivarium_stat_to_linux`, then a 128-byte copy-out.
- **`newfstatat`** — openat's front half joined to fstat's back half:
  `vivarium_fstatat_decide`, measure, copy the path into kernel scratch,
  `sys_stat_for_proc`, convert, copy out. (There is no
  `vivarium_fstatat_build`, and its absence is structural — see §6.20 of the
  design doc.)

The time translators (the phenotype-network → curl/git arc: a libc bounds every
timeout with these, and busybox `date` reads 1970 without them):

- **`clock_gettime`** (113) — `vivarium_clock_gettime_map` (pure clk_id map)
  then the native `sys_clock_gettime_handler`, which does the validated timespec
  writeback. The Linux `struct timespec` is byte-identical to `t_timespec`, so
  the map is the whole translation. It is T2 rather than a renumber because the
  clk_id domain is **not total** — Linux ids `CLOCK_PROCESS/THREAD_CPUTIME` (2/3)
  have no Thylacine clock and answer a served `-EINVAL` (Linux's own answer),
  while `MONOTONIC_RAW`/`_COARSE`/`REALTIME_COARSE`/`BOOTTIME` each map onto the
  two clocks Thylacine has, per-id justified in the map. The lseek precedent:
  a coincident-enumeration renumber drops to T2 the moment the domains diverge.
  Consequence worth recording for a future port author: musl's ISO-C `clock()`
  is built on `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`, so it returns
  `(clock_t)-1` (its documented error path) here rather than a CPU-time value.
  The target binaries (curl/git/busybox) use only REALTIME/MONOTONIC and are
  fully served; a per-process CPU clock is a v1.x capability, not a gap in
  fidelity — `-EINVAL` is exactly Linux's answer for a clock it cannot serve.
- **`gettimeofday`** (169) — no native counterpart, and a MICROsecond `timeval`
  where the native clock speaks nanoseconds, so the shell
  (`viv_gettimeofday_write`) reads the realtime clock and writes the converted
  struct itself, mirroring `sys_clock_gettime_handler`'s uaccess discipline
  (4-byte-aligned target, one `uaccess_store_u32` per word, any fault → EFAULT
  with nothing further touched). `gettimeofday(NULL, tz)` matches Linux —
  writes only `tz`, returns 0 — and a non-NULL `tz` is zero-filled, not rejected.

(The socket, signal, `clone`, `wait4`, and startup-batch families are also T2
shells; they are documented in their own arc sections, not re-listed here.)

`viv_measure_user_path` is bounded by `SYS_OPEN_PATH_MAX` and validates each
byte's VA before loading it, because the length is unknown up front and the
usual validate-then-copy prologue does not apply. The `newfstatat` shell's
re-read rejects an embedded NUL exactly as `sys_stat_handler` does: the re-read
is a *second* user-memory read, so a peer thread may have rewritten the buffer,
and honouring `sys_stat_for_proc`'s NUL-free contract at both callers keeps the
two identical.

That measure-then-copy split means the length is a **hint**, not a contract: a
peer thread can change the bytes in between, and the kernel then resolves
whatever is there at the length it measured. This is sound but worth stating
plainly — the resolved path is still fully validated and fully gated, so the
worst outcome is that a Proc racing itself opens a different file than it
meant, on its own buffer, with its own authority. (Linux copies once and has
the same self-inflicted property; we take one extra read to learn a length its
ABI hands us for free.)

## 5. Declaring a phenotype

`sys_spawn_args.pheno_flags` (offset 92) carries `SPAWN_PHENO_LINUX`. The field
consumed the former `_pad_allow` must-be-0 forward-compat slot exactly as that
slot's contract prescribed, so a zero-filled request — every pre-V-1b caller —
still means "inherit", byte-identically. Unknown bits are rejected (-1), the
`_pad_envp` rationale.

Only `SYS_SPAWN_FULL_ARGV` carries it. The register-argument spawn variants
cannot declare and always spawn native, which is §12.1 rule 3 realised in the
ABI's shape rather than in a check.

The v1.0 producer is `viv`: a bundle manifest's
`annotations["org.thylacine.phenotype"] == "linux"` sets the bit on the
container's **entrypoint** spawn only. The per-container diorama is spawned
native — it is a Thylacine server that happens to serve a Linux-shaped world.

## 6. The brand hint is advisory, and now has a caller

`elf_brand_hint` (V-1a) is consulted in exactly one place: `exec_setup_from_spoor`,
**after** `elf_load` has already failed with `HAS_INTERP` / `HAS_DYNAMIC`. If the
hint says the binary looks like a dynamic Linux one, a diagnostic explains the
rejection. It can never change an outcome — only explain one — which is §12.1
rule 4 ("a diagnostic and a clean failure, not a silent mis-decode") with the
fail-safe direction preserved.

### The memory rows (V-2d)

- **`mmap` (222) → `SYS_BURROW_ATTACH_LAZY`** over a stated domain: any `addr`
  (a hint Linux licenses the kernel to ignore), any prot within
  `PROT_READ|PROT_WRITE`, exactly `MAP_PRIVATE|MAP_ANONYMOUS`, `fd == -1`,
  `offset == 0`. `len` is judged in the *shell*, not the pure decide, so Linux's
  own errors survive: `EINVAL` for 0, `ENOMEM` for anything the target refuses.
  Translating that refusal matters — Thylacine signals failure with a bare `-1`,
  which a Linux libc would read as `-EPERM`.
- **`munmap` (215) → `sys_munmap_range_for_proc`** (#199, D-3c; was the
  exact-match `SYS_BURROW_DETACH` subset). The range form detaches every VMA
  wholly inside `[addr, addr+len)` — each one WHOLE, never partial — succeeds
  on an empty range (the Linux no-op), and refuses ATOMICALLY (nothing
  detached) on a boundary straddle or a CODE region. Needed because D-3b's
  MAP_FIXED split turns one library map into 2-3 VMAs and musl's
  `unmap_library` munmaps the whole span in one call (its error path and
  dlclose). The per-VMA accounting body is the factored `detach_one_locked`
  the native exact syscall also uses; the native `SYS_BURROW_DETACH` ABI keeps
  exact-match. Refused shapes decline `ENOSYS` (claiming success on a partial
  overlap would leave a mapping the guest believes gone). Tests:
  `burrow.munmap_range_{tiled,partial_refused,empty_ok}`; the straddle refusal
  revert-probed (S3: only its own leg reddens, 1382/1383).
- **`mprotect` (226) → `ENOSYS`**, recorded rather than left to the default.

**The protection degradation.** Thylacine anonymous memory is always RW/XN and
there is no prot-mutation syscall (an I-12 choice), so `PROT_NONE` yields a
*writable* mapping. That is admitted deliberately: `PROT_NONE` is the dominant
anonymous shape in musl (thread guard pages, mallocng meta areas), so declining
it means malloc never initialises. musl itself sanctions the outcome —
`mallocng/malloc.c:92` reads `if (mprotect(...) && errno != ENOSYS) return 0;`,
anticipating a system without `mprotect` and proceeding on the assumption that
the mapping is usable. Published in `VIVARIUM.md` §9's DEGRADED tier, and pinned
by prover leg L23 so that implementing real `PROT_NONE` later fails the gate and
forces the ladder entry to be updated rather than going stale.

`PROT_EXEC` is refused, not degraded — I-42/`CAP_JIT` territory, and W^X (I-12)
forbids the RW-and-X region the naive translation would produce. The admission is
an allow-list of two bits rather than "everything but `PROT_EXEC`", because
aarch64 musl also defines `PROT_BTI`/`PROT_MTE` and generic musl
`PROT_GROWSDOWN`/`GROWSUP`; a deny-list would have admitted all four silently.

### The signal pure layer (V-6a)

V-6a lands the *decode*, not yet the shells. Three pure functions, unit-tested,
with no caller — deliberately, and for the same reason V-2's tables landed before
V-1b gave them one (`VIVARIUM.md` §6.19/§6.20).

`viv_signal_note(signum)` is the map from a Linux signal onto the Plan 9 note
that carries it. Every row lands on a note that already exists, which is what
makes Tier 0 a decode rather than new machinery — and it is why the *default*
dispositions are already correct with no code: `interrupt` already
default-terminates (LS-5), `kill` is already non-catchable (I-19 N-4), the
`tty:*` family already carries PTY-1 semantics. The unmapped set is asserted just
as explicitly as the mapped one (`SIGALRM` has no timer note, `SIGUSR1/2` no
general-purpose note, the realtime range needs queued `siginfo`), because those
are decisions with reasons, not gaps.

`vivarium_sigaction_decide` carries the argument domain. The one worth knowing:
**installing a real handler requires `SA_RESTORER`**, because the guest's own
trampoline is how the handler returns. Measured, musl always supplies one — it
compiles with `-D_XOPEN_SOURCE=700`, which exposes `SA_RESTORER` in
`arch/aarch64/bits/signal.h`, so `sigaction.c` fills `ksa.restorer` with
`__restore_rt` (`mov x8,#139; svc 0`). Thylacine will not synthesise a
substitute: the only alternative is a vDSO sigreturn trampoline, and the vDSO
page is deliberately RO+XN (I-12/I-13). `SIG_DFL` and `SIG_IGN` need no
trampoline and are admitted without the flag, which matters — `signal(SIGPIPE,
SIG_IGN)` is the commonest signal call in real programs and it works here with no
handler machinery at all.

`viv_sigset_to_notemask` folds a Linux `sigset_t` onto the per-`Thread`
`note_mask` that already exists for exactly this purpose. Two behaviours are
load-bearing rather than incidental: **`SIGKILL` is dropped, never translated**
(POSIX says unmaskable, I-19 N-4 says the `kill` note bypasses the mask — the two
agree, so there is nothing to translate), and **unmapped signals are dropped
rather than declining the whole call**. Together those are what make musl's
`__block_all_sigs` — which sets *every* bit — translatable at all.

**What is NOT in the table yet, and why.** `rt_sigaction` (134),
`rt_sigprocmask` (135), `kill` (129), `tkill` (130) and `tgkill` (131) have their
translators but no shells, so they are *absent* from the reject table rather than
listed as `VIV_TIER2`. A `VIV_TIER2` row whose shell is missing would be a table
declaring a capability the code does not have — `viv_tier2`'s default arm calls
exactly that a "table/shell disagreement" and fails closed. The rows land with
the shells. Six siblings ARE live now, as explicit `ENOSYS` rows, each with its
own reason rather than a blanket "not yet": `sigaltstack`, `rt_sigsuspend`,
`rt_sigpending`, `rt_sigtimedwait`, `rt_sigqueueinfo`, `restart_syscall`.
(V-6b promoted the first two of those five; `kill`/`tkill`/`tgkill` still wait
for their shells, and they are also the only signal rows that are an *authority*
question rather than a disposition one — they name another Proc, so they must
reuse an existing cross-Proc gate verbatim, never invent a third.)

### Dispositions, and the two things building them corrected (V-6b)

V-6b makes `rt_sigaction` and `rt_sigprocmask` real. `rt_sigprocmask` maps onto
the per-`Thread` `note_mask`; `rt_sigaction` records `SIG_DFL`/`SIG_IGN` in a
lazily-allocated per-`Proc` `struct viv_sigtab` (`Proc.sigtab`, reset **in
place** at exec and freed only at `proc_free` — so the pointer is stable for the
life of the Proc, which is what makes the lock-free cross-Proc readers below
safe (#254) — **copied into an `rfork`/`clone` child and reset-caught-only at exec since the fork/exec POSIX rule of 2026-08-17** (it used to be neither inherited nor SIG_IGN-preserving; see "Signal state across fork and exec" below), and an ignored
signal's note is then **discarded at generation** inside `notes_post`, returning
success because Linux's `kill()` to a process ignoring the signal succeeds. One
stated divergence from Linux, POSIX-permitted (the 7580c1f7 round, F3): the
generation-time drop is MASK-BLIND -- a `SIG_IGN` signal that is currently
BLOCKED is discarded too, where Linux `sig_ignored()` queues it ("blocked signals
are never ignored, since the handler may change by the time it is unblocked")
and discards at dequeue. POSIX 2.4.1 leaves it unspecified "whether the signal is
discarded immediately upon generation or remains pending", so both are
conformant; the observable difference is the ordering `block; SIG_IGN; raise;
handler; unblock` -- Linux fires the handler, Thylacine fires nothing. Chosen
for the slot/latch reasons below and recorded rather than matched.

Post-time and not delivery-time is the load-bearing choice. An ignored note that
reached the queue would occupy one of 16 slots, would arm the LS-5c terminate
latch (an ignoring Proc has no handler and is not self-managing, so it passes
every arm gate), and would leave blocked threads unwinding `*_INTR` until the
EL0-return tail got round to dropping it. Never posting touches none of that.

**And the other half of POSIX 2.4.3 is done at the INSTALL (2026-08-17).**
Generation-time covers a note that arrives after `SIG_IGN`; a note that arrived
*before* -- blocked, or in the window -- was left for the EL0 tail's
delivery-time discard arm, which is correct for `SIG_IGN` alone and visibly
wrong the moment a guest re-installs a handler before unblocking (Linux delivers
nothing: the pending instance died at the `SIG_IGN`; the deferred discard ran the
handler for it). So `rt_sigaction` now stores the disposition and then calls
`notes_discard_name(p, name)` whenever the new disposition IGNORES (`SIG_IGN`, or
`SIG_DFL` for a default-ignore signal -- Linux `do_sigaction`'s
`flush_sigqueue_mask`): every queued note of that name is removed under
`q->lock`, mask-blind, each removal draining the class latch as a dequeue does;
`kill` is never removed. `notes_post`'s disposition read moved UNDER `q->lock` so
the two are one step: a poster that read `SIG_DFL` enqueued in a lock hold the
discard follows; one that takes the lock after the discard reads `SIG_IGN` and
drops. No ordering leaves a stale ignored note, so the tail's `SIG_IGN` arm is
now defense-in-depth (kept: its absence would hand such a note to the
`SIG_DFL`-terminate arm). Pinned by `notes.discard_name_purges_pending` (the
primitive: mask-blind, per-class latch drain, order preserved, `kill` refused, a
purged full ring is really empty) and by viv-pheno-probe L205-L216 (the shell,
in-guest: pending -> `SIG_IGN` -> unblock survives with nothing fired and no
stale note at the head; pending -> `SIG_IGN` -> handler -> unblock fires NOTHING
-- the leg that separates install-time from delivery-time; each round ends with
a fresh SIGPIPE delivered exactly once).

**Signal state across fork and exec (POSIX; operator-voted 2026-08-17).**
Two halves of one recorded decision (task #127 named both when clone became a
table row): (1) `rfork`/`clone` COPIES the parent's `viv_sigtab` -- every
disposition, caught and ignored -- and the calling Thread's `note_mask` into
the child (POSIX `fork(2)`); before, `child->sigtab` stayed NULL (all-`SIG_DFL`)
and the child thread's mask was 0, so `trap '' PIPE; cmd | head` handed `cmd` a
`SIG_DFL` SIGPIPE. (2) `execve` resets CAUGHT rows to `SIG_DFL` and KEEPS
`SIG_IGN` rows and the `note_mask` (POSIX `execve(2)`: ignored stays ignored,
the mask is inherited); before, `proc_exec_drop_image_state` zeroed both, and
its comment "Zeroing is exact POSIX" was true of caught handlers only -- so
`nohup cmd`, a non-interactive `cmd &` (SIGINT/SIGQUIT ignored in the child by
the shell before exec) and `trap '' INT; exec prog` all lost their immunity at
the exec. Native Procs keep the Plan 9 rule (ARCH §7.6: the sigtab, the mask,
`handler_va` and `in_handler` all clear at exec; nothing crosses rfork). The
phenotype branch is decided in `proc.c` on `p->phenotype`, and the table's
lifetime rule is unchanged (immortal per Proc; the child gets its OWN table --
`viv_sigtab_clone_into` -- so the cross-Proc lock-free readers still never see
a freed table). Pending notes are never inherited (POSIX: the child's pending
set is empty; a fresh Proc has a fresh queue). Two more facts cross a fork with
the mask (the d3a11c8e round): the HANDLER-EXECUTION SNAPSHOT -- this design keeps
the interrupted user context kernel-side (the sigframe is written for reading;
`rt_sigreturn` restores from the per-Thread save block), so a `fork()` issued
from inside a handler copies `in_handler` + the save block onto the child thread,
and BOTH processes return from the handler to the interruption point (before
this the child's return was refused and it ran on past the svc); and the
COARSENESS of the mask crosses with it -- `NOTE_BIT_TTY` is one family bit (see
the mask section below: blocking SIGWINCH really does block SIGHUP), so a parent
that blocks SIGWINCH hands its forked and exec'd children a blocked
SIGHUP/SIGTSTP/SIGCONT/SIGQUIT too, where Linux inherits only SIGWINCH.

**The handler-time mask (aux item 7, 2026-08-17).** While a phenotype handler
runs, `note_mask` is Linux's `signal_delivered` value -- the pre-handler mask |
`sa_mask` | the delivered signal (omitted under `SA_NODEFER`), computed by the
pure `vivarium_handler_mask` through the same coarse translation as
`rt_sigprocmask` (a tty-family `sa_mask` entry blocks the family; SIGKILL in
`sa_mask` is dropped) -- and the phenotype's `rt_sigreturn` restores the
PRE-handler mask from `Thread.note_saved_mask`, which the Linux delivery path
writes beside the register block (`notes_deliver_linux_locked`) and which the
fork copy above carries with the snapshot. The frame's `uc_sigmask` still
carries the pre-handler mask and is written for reading: a handler that edits
it changes nothing (Linux would honour the edit -- a conservative-direction
divergence of this frame design). Consequences, each a probe leg (L237-L244):
a handler's own `rt_sigprocmask` does not outlive the handler; a read inside
the handler shows mask|sa_mask|sig; an `execve` from inside a handler hands the
image mask|sa_mask|sig plus whatever the handler blocked (Linux keeps the
current blocked set across exec); a `fork()` from inside a handler gives the
child the handler-time mask AND a sigreturn that restores the saved one.
Delivery is unchanged: the `in_handler` guard still holds every note for the
handler's duration (VIVARIUM 6.22's stated imprecision), so the mask cannot
admit a nested delivery. Native `noted` keeps the as-built rule: a mask changed
inside a handler stays changed (`notes.phenotype_sigreturn_restores_mask` has
that as its control).

(V-6b's "a real handler still declines" paragraph stood here; V-6c landed the
Tier-1 frame and a real handler installs and RUNS — see "V-6c" below.)

Two corrections came out of building this, both from measuring rather than
reasoning:

**The `k_sigaction` layout is fixed by the arch, not chosen per call.** V-6a
shipped runtime helpers returning 24 or 32 bytes depending on whether the caller
set `SA_RESTORER`. Reading musl showed the member is gated on a *compile-time*
`#ifdef`, aarch64 has no `ksigaction.h` override, and `sigaction.c` sets the flag
unconditionally — and Linux copies `sizeof(struct sigaction)` regardless of
flags, so musl working at all proves the kernel expects 32. It is now a constant.

**SIGTERM lost its note.** V-6a mapped SIGINT and SIGTERM both onto `interrupt`
and called it "a stated imprecision". Dispositions showed it is not imprecise but
*unrepresentable*: a note carries no signal identity, so `sigaction(SIGINT,
SIG_IGN)` with SIGTERM at `SIG_DFL` has no correct answer — honour it and SIGTERM
goes silent, refuse it and a Proc that asked to ignore Ctrl-C dies on Ctrl-C. And
that is the call a shell makes. So `interrupt` belongs to SIGINT alone (it *is*
the Ctrl-C note) and SIGTERM declines until it has one of its own. Nothing
regressed: no v1.0 path posts SIGTERM, so the entry was reachable only through
the mask conversion. `viv_signal_owns_note_exclusively` now enforces the property
by *scanning* the map, so a future edit that re-shares a note narrows the domain
automatically instead of silently making one of the two signals wrong. The
`terminate` note SIGTERM wants is an I-19 supported-set addition needing signoff
(task #95), load-bearing when `kill` lands.

**Two permanent declines worth naming.** `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE`
take `SIG_DFL` and nothing else: measured against `notes.c`, the `snare:*` family
is not in `g_known_notes` and `proc_fault_terminate` calls `exits()` directly, so
a fault never reaches a queue — there is nothing to catch or ignore, and
terminate is what already happens. And `SIGCHLD` + `SIG_IGN` declines because on
Linux that is **auto-reap**, not "ignore"; Thylacine reaps only through
`wait_pid`, so honouring the surface meaning would leave a guest with zombies it
believes are gone.

**The mask reports back honestly.** `viv_notemask_to_sigset` names *every* signal
a set bit actually blocks. `NOTE_BIT_TTY` is one bit for five signals, so
blocking `SIGWINCH` really does block `SIGHUP` — and a guest reading its mask
back is told so, rather than shown the tidy answer it asked for while the system
does something wider. Over-blocking defers a signal; it does not lose one. The
in-guest leg L26 asserts the divergence so it cannot go stale silently.

## 7. Known limits at V-2d

- **`FORWARD` = `ENOSYS`, and V-3 is deferred rather than pending.** Measured at
  V-3's entry: no syscall lets one Proc mutate another's address space, handle
  table, or process tree, so a peer supervisor Proc could serve essentially
  nothing of `ARCH §11.5`'s top-50. The peers avoid this because a compat
  supervisor must either *be* the guest (Starnix's restricted mode, same thread)
  or *own* it (Plan 9 `linuxemu` over `/proc`; gVisor's Sentry). `VIVARIUM.md`
  §4.1 carries the three live candidates; V-5 decides.
- **The calculus this inverts.** §6.19's "declining is always safe (the
  supervisor is strictly more capable)" was true with a live supervisor and is
  false now — a declined call is a guest-visible failure. A T2 translator must
  admit everything it can prove exactly equivalent, not the easy minimum.
- **The exit status is boolean.** `exit_group(N)` reports 1 for any nonzero N —
  a Thylacine-wide v1.0 property (`sys_exits_handler` /
  `sys_exit_group_handler` collapse to `exits("fail")`), not a Vivarium one, but
  Vivarium gives it its first Linux consumer: a shell reading `$?` inside a
  container sees 0-or-1. Task **#91**.
- **A `PHENO_LINUX` Proc has no native ABI left**, including the runtime's own
  exit. `_start` ends in `mov x8, #0` (`T_SYS_EXITS`), and Linux 0 is
  `io_setup` — not a row — so a native-runtime binary declared Linux would park
  in `_start`'s defensive loop rather than exit. This is correct behaviour, not
  a gap: a Linux binary is a Linux binary all the way down. It does mean a
  *prover* for this surface must speak raw `svc` and terminate by hand.
- **`/proc/self` in a container reports the runner** (task #90) — orthogonal to
  the phenotype, but a Linux guest is exactly the reader that will notice.

## 8. Coverage

| what | where |
|---|---|
| the pure table + all three translators, by domain | `vivarium.*` kernel tests (V-2b/V-2c) |
| the `pheno_flags` ABI gate (accept 0, accept the bit, reject unknown) | `sys_spawn_full_argv.validate_req_pheno_flags` |
| **the discriminator** — a native Proc leaves a Linux number untranslated | joey's V-1b leg A: `viv-pheno-probe native` (asserts `brk` → -1, not -ENOSYS) |
| **the chain** — manifest → viv → declaration → branch → translated call | joey's V-1b leg B: `viv run /vivarium/pheno`, whose entrypoint speaks only raw Linux numbers |
| the mmap argument domain, each admission and decline by name | `vivarium.mmap_domain` |
| **the memory round trip** — map, write through it, read it back, unmap | leg B's L16–L23 |
| **the time family** — realtime seed, monotonic advance, `timeval` µs conversion, the EFAULT + EINVAL paths | leg B's L249–L253 |

The two layers are not redundant, and V-2d's revert probes show exactly why.
Reverting the `mmap` table row *or* widening the prot allow-list to admit
`PROT_EXEC` fails precisely two unit tests and no others. But breaking only the
*shell* — leaving the table intact — keeps the unit suite at a full **1239/1239
PASS** while the in-guest leg fails at `L16`. The pure tests prove the
*decision*; the guest legs prove the *plumbing*; neither can see the other's
bugs.

Leg B's prover (`usr/viv-pheno-probe`) reports through a **file**, not its exit
status: the status is boolean (§7), so per-leg codes would all arrive as 1 and
name the wrong leg. It writes its verdict into the bundle's `pheno-scratch`
through Linux `write(64)`, and joey reads it back **from outside the
container** — which simultaneously reports the result and proves the write moved
real bytes across a territory boundary. joey stamps the file with a sentinel
before the run, so a stale marker from a previous boot cannot pass the gate.

The legs, in order: `brk`→ENOSYS (the discriminator), `munmap`→ENOSYS (a
FORWARD row), `openat`+`read` (the ELF magic of its own binary), `lseek`,
`fstat`, `newfstatat` cross-checked against `fstat` on `(st_dev, st_ino)`,
`newfstatat`+`AT_SYMLINK_NOFOLLOW` → still rejected (the deliberate `lstat`
refusal, asserted so a future "optimisation" cannot quietly delete it),
`close`, `write`, `close`, and `exit_group` — the last of which is its own
assertion, since an untranslated 94 would reach `SYS_TTY_SIGNAL` and joey's
by-pid wait would never return.

The time-family legs (L249–L253) are each deterministic fail-without-fix,
because before the translators these numbers FORWARD → `-ENOSYS` and the `== 0`
guard is false: **L249** `clock_gettime(REALTIME)` writes a wall clock past
`1_700_000_000` (a 1970 epoch, the no-write failure mode, is far below);
**L250** `clock_gettime(MONOTONIC)` is sub-second-bounded and never runs backward
across two reads; **L251** `gettimeofday` writes a `timeval` whose `tv_usec` is
`< 1e6` (the µs conversion's signature — a shell that wrote nanoseconds would
overflow it); **L252** both calls answer `EFAULT` on an unmapped pointer; **L253**
a CPU-time clk_id answers the served `EINVAL`. The discrimination was measured:
reverting the two table rows fails the boot at exactly `marker=L249`.

Iterating on this prover needs `THYLACINE_MKFS_PRESERVE=0`: the entrypoint lives
in the **pool** (the bundle rootfs), and a preserved pool skips populate, so the
container keeps running the previous binary while joey and viv — which live in
the cpio — update normally. That asymmetry produces two identical-looking boots
and is worth remembering.

### The Tier-1 frame -- a Linux handler that runs (V-6c)

V-6c lifts the one line V-6b left: a real handler installs, and it runs.

**What the guest gets.** Delivery writes `siginfo_t` + `ucontext_t` (4688 bytes)
to the guest stack, with a 16-byte `{fp, lr}` frame record above it so a
backtrace from inside a handler walks into the interrupted code, and enters the
handler with the aarch64 contract: `x0` signum, `x1` &siginfo, `x2` &ucontext,
`x29` &frame_record, `x30` the guest's own `SA_RESTORER` trampoline, `sp` the
frame, `pc` the handler. `rt_sigreturn` is intercepted in `viv_linux_dispatch`
and routed to `SYS_NOTED(NCONT)`.

**The restore does NOT read the frame** -- it reads the per-`Thread` snapshot
the delivery path saved. That is the design decision from VIVARIUM.md 6.22, and
its value is a deleted hazard rather than a smaller diff: no field of the user
frame ever reaches `pstate`, `pc` or `sp`, so the "reject any frame that would
elevate" obligation has no validator to get wrong. The named cost is that
writing to `uc_mcontext` is inert.

**Two things measuring corrected, again.**

`sizeof(struct sigcontext)` is **4384** and `__reserved` starts at **288**, not
280. musl declares the tail as `long double __reserved[256]`, which on aarch64
is 16 bytes per element AND 16-aligned. Compiling that declaration with the host
`cc` on an arm64 Mac answers 2328, because macOS `long double` is 8 bytes -- so
the first layout probe was wrong in a way that would have put every mcontext
field the guest reads 8 bytes out of place. The numbers were re-taken under
`--target=aarch64-linux-gnu` and are now `_Static_assert`ed.

The kernel writes the first **600** bytes plus an 8-byte `_aarch64_ctx`
terminator, and leaves the rest of `__reserved` alone. That is not a shortcut:
the region is the guest's own stack below its own sp, so nothing crosses a
boundary, and an EMPTY record chain is the truthful report of what the GUEST
may edit: since task #96 the kernel saves Q0-Q31 + FPSR/FPCR itself at delivery
(`fp_save_area` into `note_saved_fp`) and restores that copy at `rt_sigreturn`,
so a frame-side FPSIMD record would be a copy the guest could edit to no effect
-- a record that claims an authority it does not have.

**Three deliberate delivery behaviours beyond "call the handler".** A `SIG_IGN`
disposition drops a note that was already queued when the disposition changed
(defense-in-depth since 2026-08-17: `rt_sigaction`'s install-time
`notes_discard_name` plus `notes_post`'s under-lock disposition read leave no
ordering in which such a note reaches the tail -- see "the other half of POSIX
2.4.3" above -- and the arm stays because its absence would hand a stale note
to the terminate arm), and a `SIG_DFL` whose Linux default is *ignore*
(SIGCHLD/SIGWINCH/SIGCONT) is dropped rather than left in the ring -- a Linux
guest has no notes fd, so nothing would ever consume it and the queue would
fill; this cause is live (a `child_exit` lands under `SIG_DFL` constantly).
`SA_RESETHAND` is honoured: the disposition returns to `SIG_DFL` before
the handler is entered. And a `SIG_DFL` whose Linux default is *terminate*
(SIGPIPE / SIGINT / SIGHUP / SIGQUIT -- `viv_signote_default_is_terminate`)
`exits()` the Proc **from the phenotype branch itself**, on the candidate, with
the note's canonical name, instead of falling through to the native uncaught
arm. That arm scans for the *native* terminate latch. When this branch was
written the native `pipe` note had none -- so before the c8ab2744 round a
`SIG_DFL` SIGPIPE was consumed by **no** arm: the
terminate scan skipped it, the stop consumer skipped it, and "leave it queued
for the fd reader" stranded it at the head of a queue no fd will ever read.
It became the dispatcher candidate for the life of the guest; every later
caught signal was blocked behind it, every later default-ignore note was never
dropped, and (F1 below) every later caught terminate-class note killed the
guest. Linux's answer -- SIG_DFL SIGPIPE terminates -- is not in doubt, so the
phenotype answers it for its own Procs. (#237 has since given the native
`pipe` note its own terminate latch, but the phenotype keeps answering
SIG_DFL SIGPIPE from this branch -- exactly as it does for SIGINT / SIGHUP /
SIGQUIT, whose native latches it likewise does not defer to -- because it must
exits() on the candidate, with the Linux-canonical name and disposition.) The phenotype `wait4` folds a
note-death into an EXITED status (#91), so `exits("pipe")` reads exactly as
`exits("interrupt")` already did. Only the STOP default (`tty:susp`) still
falls through, to the native branch's stop consumer. In-guest: the L-6c gate's
`L6C-J` (`yes | head -n 1`, both ends writing into one fd-3 capture -- head its
one line, the writer its stderr -- so the capture is EXACTLY `y`: an exec'd
all-`SIG_DFL` binary is killed before its write returns; pre-fix it appended
`yes: write error: Broken pipe` and lived), `L6C-K` (the positive control: a
subshell writer that runs `trap "" PIPE` in its own process -- a fork does not
inherit the sigtab and an exec resets it -- so the write RETURNS EPIPE and the
builtin `echo` reports `write error` after the line, proving the capture can
see a message when there is one) and `L6C-L` (K with the trap removed, one
variable away: the capture is exactly head's line again). The script logs the
raw K capture as `L6C-K-RAW:` for the errno text; that line is diagnostics. K
earned its keep on its first boot: the capture itself was broken (the `fcntl`
errno defect below), J and L passed vacuously on an empty capture, and K alone
went red.

**The class scans read the sigtab per note (the c8ab2744 round, F1).** The
fall-through above (a `SIG_DFL` `tty:susp`, masked then unmasked) enters the
native `handler_va == 0` branch, whose TERMINATE scan
(`notes_terminate_pending_name_locked`) used to gate on `handler_va` alone and
return the first latch-class name at ANY index. `handler_va` is always 0 for a
Linux guest, so a CAUGHT `tty:hup` or `interrupt` queued behind the candidate
was returned and the tail `exits()`ed a guest with its SIGHUP/SIGINT handler
installed. Both class scans -- the terminate scan and the STOP index scan behind
`notes_stop_dequeue_locked` -- now gate every hit on
`notes_proc_default_applies(p, name)` (a native handler, a sigtab handler, or a
sigtab `SIG_IGN` for THAT name each veto it); the fixed-name
`notes_proc_default_applies(p, NOTE_NAME_TTY_SUSP)` gate that used to sit
outside the stop scan is subsumed. Native Procs are byte-identical (the
predicate is unconditionally true with no handler and no phenotype, and the
arm is only reached with `handler_va == 0`). Regression:
`notes.class_scans_read_phenotype_sigtab` -- positive control (phenotype,
all-`SIG_DFL`, `[child_exit, tty:hup]` names the hup at index 1), then
`[tty:susp, interrupt+handler]` (terminate scan NULL, the stop consumer takes
the susp and the interrupt survives), the control's queue with SIGHUP caught
(NULL), an interrupt queued under `SIG_DFL` and then `SIG_IGN`ed (NULL), the
stop consumer with a SIGTSTP handler (declines; takes it once the row is
`SIG_DFL` again), and the native control with the same table (names the
interrupt -- the phenotype is the gate, not the table).

**Proving it in-guest needed the one signal a v1.0 guest can raise.** `kill`
and `tkill` are not table rows, so a Linux guest can signal neither another
Proc nor itself through the obvious route -- and `clone` IS a row (LINEAGE
L-3d, the fork shape only: `vivarium_clone_decide` admits no CLONE_THREAD
word), so it still cannot spawn a thread
to race its own disposition table either. **What makes the lock-free
`viv_sigtab` sound is stated below rather than by that narrowness** -- this
sentence used to claim "the only cross-thread reader is `notes_post`'s `SIG_IGN`
hook, and it touches one naturally-aligned `u64`", and both halves were wrong
(main#243, aux#254 -- found independently on the two tracks in the same week).
The intra-Proc narrowness bounds only the *writers* (a v1.0 Linux guest has no
peer thread; rt_sigaction, SA_RESETHAND and exec's reset all run on a thread of
THIS Proc). The **readers are a set, each taking an arbitrary `Proc`**, and it
grows for the most ordinary reason there is -- a new disposition predicate needs
the dispositions: `notes_post`'s `SIG_IGN` hook (`notes.c`, one `u64`);
`notes_proc_has_live_handler` (`notes.c`), which copies the **whole 32-byte**
`viv_ksigaction` out (`*out = *a`) and is reached from
`notes_arm_intr_terminate_locked` and from `notes_proc_default_applies`; and
`notes_proc_default_applies` itself (the ignored gate), which the pgrp fans and
the `^Z` fan (`proc_tty_susp_would_stop_locked`) call on any group member.
The soundness argument is therefore not "nobody races it" but two facts: (1)
the table is **never freed while reachable** -- reset in place at exec
(`viv_sigtab_reset` / `viv_sigtab_reset_caught`), freed only at `proc_free`,
one immortal object per Proc (a lock would protect only the readers who
remember to take it; the third reader above was written as a bare atomic load
by the author holding the finding); and (2) **every field is written at 8-byte
granularity** (a struct assignment -> `stp` of X registers), so no reader can
observe a value that was never stored -- the byte loop this replaced was
measured to compile to halfword stores. Entry-to-entry consistency is explicitly
NOT promised: a reader may see a mix of pre- and post-reset entries, which is
the latitude POSIX gives a `sigaction` racing a signal already in flight.

> This claim has now been wrong twice in the same place. `docs/VIVARIUM.md`
> records the first: V-6c asserted byte-sized entries could not tear, true at
> V-6b and false once entries widened to 32 bytes (task #97) -- corrected there
> and not here. Whoever narrows this argument again should check the reader set
> and the store width, in that order.

What remains is
self-infliction: the bundle declares `org.thylacine.sigpipe-selftest`, `viv`
hands the entrypoint fd 0 as the write end of a reader-less pipe, and the
guest's own `write()` makes the kernel post `pipe`. Synchronous, no second Proc
in the timing.

The gate also pins that masking **defers** rather than loses: with SIGPIPE
blocked the handler must not run, and must run at the `rt_sigprocmask` that
unblocks it.

Revert-probed three ways, each failing at its own layer: removing the
`rt_sigreturn` interception kills the guest outright (its restorer's `svc`
returns `-ENOSYS` into a `brk`); removing the delivery arm fails exactly the
handler-ran leg; zeroing the frame's saved `pc` fails the pure unit test.

---

## Sockets -- the fd that changes what it means (V-5a)

`docs/VIVARIUM.md` section 5.5 is the design; this is what landed.

A Linux socket is one fd. A `/net` connection is three files (`ctl`, `data`, and
the metadata leaves) under a directory that netd mints on demand. Reconciling
them is the whole of V-5a, and the shape follows from five properties of netd
that were **measured, not assumed** -- each of them is a place the obvious
design would have been wrong:

| measured | consequence |
|---|---|
| opening `clone` mints a connection and rebinds that fid onto its `ctl`; the fid holds the connection's **only** reference (`slot_ref` 0->1) | the socket fd must survive until something else binds the connection -- dropping it early *frees* the connection |
| opening a TCP `data` file **defers** its `Rlopen` until ESTABLISHED (#257) | `data` cannot be pre-opened at `socket()`; it opens only after the dial |
| **any** fid on the connection holds a reference (`fid_set`/`fid_clunk`) | once `data` is open, `ctl` is disposable |
| reading a `ctl` fid yields N in decimal (`file_content`: `FK_CTL => push_dec(n)`) | the connection number is recoverable from the fd -- while the fd is still `ctl` |
| `read`/`write`/`close` are **T1 renumber rows** | if the socket fd *is* the `data` fd, the entire data path needs no translation at all |

So the socket fd **changes what it denotes at `connect`**: `ctl` from `socket()`
onward, `data` afterwards. That is what keeps the hot path free, and it is why
`handle_replace` exists -- `handle_dup` allocates a *new* slot, and
close-then-alloc cannot reserve the index against a peer thread.

### What the kernel has to remember, and why it is not more

`Proc.socktab` holds `(proto, N, state)` per socket, in the `Proc.sigtab` shape:
lazily allocated, CAS-installed, freed at `proc_free`, not `rfork`-inherited,
bounded at `VIV_SOCK_MAX`.

It is the *minimum*, and the two ways to avoid it were both examined:

- **`N`** is re-readable from `ctl` -- but only while the fd still is `ctl`,
  which stops being true at exactly the moment the rest of the socket's life
  begins.
- **`proto`** is knowable only at `socket()`. Recovering it later means decoding
  netd's qid layout (`CONN_FLAG | proto<<32 | N<<8 | filekind`), and that is
  **refused**: `/net` is a mount point and need not be netd. Decoding a foreign
  server's qid as netd's is precisely the silent mistranslation the
  argument-domain rule exists to forbid, so `proto` is remembered instead.
- **`Spoor.path`** would carry the name, and is forbidden by **I-33**, which
  makes path retention explicitly non-load-bearing.

### Why this is in the kernel, and why I-43 is structural here

Every `/net` operation goes through `sys_open_kpath_for_proc` -- the same
resolution core `SYS_OPEN` uses, **extracted rather than duplicated** so a
socket open passes through the same `stalk`, the same per-component
`perm_check`, and the same omode-derived rights as any other open. A translated
socket call therefore reaches exactly what the guest could reach by opening
`/net` by hand; a container whose territory has no `/net` gets a walk failure,
not a bypass.

That is also the finding that dissolved V-3 (section 4.1.1): the multi-step
orchestration sockets need is an argument against a *ring*, not against the
kernel doing the work with the caller's own authority.

### The argument domain

`AF_INET` only. `SOCK_STREAM` -> `tcp`, `SOCK_DGRAM` -> `udp`. `AF_INET6` is
`EAFNOSUPPORT` and `SOCK_SEQPACKET`/`SOCK_RAW` are `EPROTONOSUPPORT` -- distinct
errnos because a Linux program acts on them differently, and collapsing them to
`EINVAL` would make a guest retry an address that can never work.

`SOCK_NONBLOCK` and `SOCK_CLOEXEC` in the type word are **refused, not masked
off**. A guest that asks for a non-blocking socket and silently receives a
blocking one blocks where it expected `EAGAIN`.

Landed as honest declines with their reasons in the table: `setsockopt`
(`/net` exposes no option surface, and answering "success" to a `TCP_NODELAY`
nothing honours is the silent lie this tier exists to prevent), `socketpair`,
`sendmsg`/`recvmsg` (scatter-gather plus `SCM_RIGHTS`, which is I-4's domain).
`bind`/`listen`/`accept` are V-5b; `shutdown`/`getsockname`/`getpeername`/
`sendto`/`recvfrom` are V-5a's remainder.

`getsockopt` left the blanket refusal in the curl-demo chunk and became a
Tier-2 row serving **exactly one point: `(SOL_SOCKET, SO_ERROR)`**
(`vivarium_getsockopt_decide` + `viv_sock_getsockopt`). The forcing case:
curl's `verifyconnect` -- run by every libcurl consumer after every connect --
reads `SO_ERROR` and treats a *getsockopt failure* as a *connect failure*, so
the refused row turned a measured-successful connect (61 ms through slirp)
into `(7) Could not connect`. Serving it is honest because `SO_ERROR` is a
READ of pending-error state, not a tuning knob, and a phenotype socket carries
no SYNCHRONOUSLY-pending error: `SOCK_NONBLOCK` is refused at `socket()` and
`F_SETFL` is not served, so every guest socket op completes synchronously and
its failure was already that op's own return value -- the constant 0 the shell
writes is the true answer for every synchronously-delivered error, which is
the connect-verification purpose the row serves (see the async-latch
degradation below for the one state where it is stale). The revisit is pinned
in the decide's header: a future NONBLOCK row must make `FRESH` carry the
in-flight connect outcome. Every other `(level, optname)` still declines to `ENOSYS` through
the same T2 "declined these arguments" path, so guest fallbacks behave
byte-identically to the blanket era. One deliberate delta: `getsockopt` on a
non-socket fd is now `ENOTSOCK` (the Linux answer) rather than `ENOSYS`.
Covered by `vivarium.getsockopt_domain` (the pure domain + the 32-bit
narrowing + fail-closed) and end-to-end by `tools/phenonet/curl-demo.exp`
(fails without the row -- measured three times -- and passes with it).
The shell validates BOTH the `optval` and `optlen` spans with
`sys_validate_user_buf` before any access -- the byte-wise uaccess helpers
assume a validated user VA, and the fault fixup only engages below
`UACCESS_USER_VA_TOP`, so an unchecked kernel VA would silently write kernel
memory or extinct the box (holotype F1, fixed before merge; regression
`vivarium.getsockopt_shell_guards_uaccess` drives the real shell and proves a
kernel-range address on either span is rejected `EFAULT` before any access).

**Two documented degradations** (both narrower than the old blanket ENOSYS,
neither a silent lie):
- *The async-latch gap (F2, operator-ratified narrow).* The 0 answer is true
  for every synchronously-delivered error -- the connect-verification purpose
  the row serves -- but netd also latches errors ASYNCHRONOUSLY (a
  connected-UDP/ICMP local send failure sets `slot.err` and surfaces POLLERR
  via `check_ready`), and the shell does not consult that latch, so a guest
  that observes POLLERR then reads `SO_ERROR` gets 0. Latent at v1.0 (no
  shipping guest reaches it -- UDP DNS is not live). The honest fix is the
  **netd-errno arc**: a netd protocol path exposing the per-connection errno
  (the latch is a `&'static str` today, not an errno) + a blocking kernel
  read-and-clear in the shell + the race-safe fd lifecycle + a fresh audit.
  Tracked, not built.
- *ENOTSOCK vs EBADF (F4).* `viv_socktab_find` returns NULL both for a
  valid-but-non-socket fd (correct: `ENOTSOCK`) and for a closed/never-open fd
  (Linux: `EBADF`); the shell answers `ENOTSOCK` for both. Strictly better
  than the old blanket `ENOSYS`; the residual drift on the bad-fd axis is
  benign (no distinguisher exists at this layer without a handle-table
  lookup).
- *F_DUPFD / dup of a socket loses the socktab entry (R2-F4).* `fcntl(sock,
  F_DUPFD, n)` mints a second fd on the data Spoor with no socktab entry (the
  OMIT-the-entry shape `dup3` already declines), so `send`/`recv` on the
  alias is `ENOTSOCK` where Linux serves, while `write`/`read` on it work.
  No soundness impact (no stale entry, no authority gain, I-43 holds); the
  honest fix is socktab-entry duplication on dup, its own chunk (tracked).

Coverage gap on the success path: the optval WRITEBACK value has no direct
kernel-test witness (the harness cannot stage a live EL0 mapping for the
uaccess; the E2E covers it -- verifyconnect reads a 0 and the fetch
completes). A poisoned-buffer probe leg would need a net-granted probe bundle
= a network-dependent boot gate -- declined, tracked.

`sendto`/`recvfrom` became Tier-2 in the same chunk, because **aarch64 has no
plain `send`/`recv` syscall**: musl's `send()` IS `sendto(fd, buf, len,
flags, NULL, 0)` and `recv()` IS `recvfrom(..., NULL, NULL)`, so any Linux
binary that moves socket data through `send()`/`recv()` -- curl does; busybox
wget happens to use `write()`, which is why it never hit this -- died
`ENOSYS` mid-connection (`curl: (55) Send failure`). The served shape is
exactly the connected-socket `send()`/`recv()`: socktab state `CONNECTED`,
NULL address, flags 0 (send also admits `MSG_NOSIGNAL` as a *truthful no-op*
-- the socket data path is a 9P Spoor write and the pipe EPIPE-note machinery
never runs there, so no SIGPIPE exists to suppress). After the screening
(`vivarium_sendto_decide` / `vivarium_recvfrom_decide`, pure) the shells
delegate the data movement to the NATIVE `sys_write_handler` /
`sys_read_handler` -- the same staging tiers, weft fast-path, short-op
semantics, and #844 fd lifecycle a T1-renumbered `write()`/`read()` on the
same fd gets. Declines, each to `ENOSYS` (census-visible unbuilt), each honest: the
with-address datagram shape (a per-datagram destination has no /net verb
yet), `MSG_PEEK` (no non-consuming 9P read), `MSG_DONTWAIT` (blocking-only
sockets), `MSG_WAITALL` (changes the return contract), a non-NULL `recvfrom`
source address (peer-address state the socktab does not carry), AND an
UNCONNECTED socket. That last is the R2-F1 correction: unconnected send/recv
is genuinely unbuilt (no per-datagram dial; the bound-UDP-server `recv`
idiom Linux serves has no path here), so it declines to `ENOSYS` -- which
`viv_report_unserved` surfaces on the mission's work-list -- rather than a
fabricated `ENOTCONN` (Linux answers `EPIPE`/`EDESTADDRREQ` on the send side,
none of which we serve, so answering an errno would both mismatch Linux and
hide the gap).
Error-value fidelity on a dead connection is the data path's (an errno from
the 9P write, not Linux's `EPIPE`) -- honest in kind, imperfect in value;
noted rather than papered over. Covered by `vivarium.sendrecv_domain` and
end-to-end by the curl demo (the fetch is a send + recv on the wire).

### The close hook -- the sharpest bug this chunk could have had

`close()` stays a T1 row falling through to the native handler, so fd teardown
keeps one implementation. The socktab entry is dropped by a **hook** in
`viv_linux_dispatch`, before the translation runs.

Without it, `close()` frees the fd *index* while the entry survives; the next
fd-creating syscall gets that index back; and a later `connect()` finds a stale
entry and writes a dial verb **to a stranger's connection**. The hook is
unconditional for a phenotyped Proc and `viv_socktab_drop` is a no-op for an fd
with no entry, so an ordinary file's close costs one NULL test.

### Coverage

Six pure unit tests (`vivarium.socket_domain` / `sockaddr` / `net_cmd` /
`conn_n` / `socktab` / `socktab_close_hook`) plus `handles.replace`, and
fourteen in-guest legs (L39-L52) against the **live** netd through the
container's own territory -- the bundle declares `org.thylacine.net: granted`.

UDP is the deterministic choice for the live legs: netd's `udp_connect` binds a
local port and records the remote with **no handshake**, so the whole path is
proven without a peer or a live network. The identity change is observed
directly -- `fstat` before and after `connect` returns different `st_ino`,
because `ctl` and `data` are different qids.

Revert-probed five ways, each failing at its own layer and nowhere else:
letting `handle_replace` inherit the outgoing slot's rights fails the I-6
assertion; removing its outgoing-kind gate fails the I-5 one; leaving a stale
socktab entry fails the close-hook regression; skipping the connect swap (while
still reporting success) fails in-guest **L46** and only L46; removing the close
hook fails in-guest **L50**.

### The exec sweep -- close-on-exec bypasses the close hook (6b)

The close **hook** above rides `viv_linux_dispatch`, so it only fires for a
`close()` that reaches the syscall dispatch. `execve` closes its close-on-exec
fds a different way: `handle_close_on_exec` walks the cloexec bitmap and calls
the `handle_close` **primitive** directly, which never enters the dispatch and so
never runs the hook. Left alone, every `socket(); fcntl(F_SETFD, FD_CLOEXEC);
execve()` -- the shape real musl's `socket(SOCK_CLOEXEC)` falls back to -- strands
a `(proto, N)` entry on the freed fd for the new image's first `socket()`/`open()`
to collide with, and `connect()` then dials a dead `data` while `poll` opens a
stale `ready`.

`sys_execve_core` closes the gap with `viv_socktab_drop_cloexec(p)`, run after
the commit (`proc_exec_replace`) and **before** `handle_close_on_exec`, so it
reads each entry's cloexec bit while it is still set. It drops only the entries
whose fd is close-on-exec (`handle_get_cloexec == 1`); a plain socket's entry and
a pre-existing stale entry at a closed fd (`== -1`) both survive. It takes no
lock: `execve` is single-threaded there (`proc_exec_replace` extincts if a live
peer exists), the same window that lets `handle_close_on_exec` walk the handle
table unlocked.

`viv_socktab_drop_cloexec` clears **by slot** (it holds the entry pointer) rather
than via `viv_socktab_drop`'s re-find-by-fd; the two are equivalent here because
the cloexec bit lives on the handle, not the entry, so two entries that ever
share an fd agree on it. Regression: `vivarium.exec_drops_cloexec_sockets` drives
the sweep on a fresh phenotype Proc with three entries (cloexec -> dropped, plain
-> survives, closed-fd stale -> survives), each leg failing under a distinct wrong
predicate.

**Design D audit close (F2, 2026-09-01) narrowed the sweep's unique coverage
without retiring it.** Two things changed. `viv_socktab_claim` now **replaces**
any row already keyed on its fd (the fd table is the truth: the caller was just
handed that number by `handle_alloc`, so an existing row is stale by definition),
which on its own closes the *socket-recycle* instance of the misroute -- a
`socket()` on the freed number evicts the stale row instead of being shadowed by
it. And `proc_exec_drop_image_state`'s NATIVE arm calls the new
`viv_socktab_reset` (every slot cleared in place under the lock; the object and
the monotonic epoch kept), because Design D made a native image with a Linux
socktab constructible -- a Linux->native `execve` ran only this cloexec sweep,
native `close()` never drops a row, and the next native->Linux `execve` inherited
rows keyed on numbers the new image was handed afresh. What the sweep still
uniquely prevents is the number recycled to a **non-socket**: an `open()` claims
nothing, and every socket arm (`connect`/`sendto`/`getsockname`/...) looks the
table up by number, ENOTSOCK on a miss, so a `connect()` on that file fd would
find the stale row and dial its dead connection. The ground-truth regression
`vivarium.exec_sweep_prevents_fd_reuse_misroute` was re-aimed accordingly: its
original control ("without the sweep, a fresh UDP socket on the reused number
reads the stale TCP row") went red at the close for the *new* reason -- the bug
arm now reads UDP because replace-on-claim evicted the row -- the #240 shape, a
new guard hollowing an old test of the same negative. Its bug arm now recycles the
number as a plain handle first (stale TCP row found: the sweep's job) and claims a
UDP socket on it second (UDP read in BOTH arms: the replace-on-claim witness).
`vivarium.socktab_reset` covers the reset and the replace directly.

**Still owed** (each its own chunk, all "the socktab across an image or fd
transition"): the fork clone (an inherited socktab must be copied, not shared),
`dup`/`close_range` (the FORWARD obligation the T2 header already tracks), and
`fcntl(F_DUPFD)`'s entry-less alias.

---

## The server path -- a connection accepted, from one thread (V-5b)

`bind`/`listen`/`accept`/`accept4` join the T2 family. The scripture is
VIVARIUM.md section 5.5.3; what follows is what the code does and what the
gates actually prove.

### Three verbs, three different shapes

`bind` writes nothing. netd has no bind ctl verb, so the request is recorded in
the socktab (`bound_addr`/`bound_port`, repacked into the existing 16 bytes) and
`listen` spends it. `listen` writes `announce` to the fd -- which is still `ctl`
and stays `ctl`, because that is what `accept` re-walks from. `accept` walks
`listen` -> reads the accepted connection number -> opens its `data`, and
returns *that* fd; there is no swap, because the fd it needs is the one the open
already produced.

The one place a `bound` flag might seem wanted is deliberately absent: an
unbound socket and one bound to `0.0.0.0:0` are indistinguishable in every path
that reads the fields, so the flag would be state no reader could branch on.
The header says so, and says to add it with the reader that first needs it.

### The wildcard is not cosmetic

`vivarium_net_cmd_announce` renders `0.0.0.0` as `announce *!port` and a
concrete address as `announce a.b.c.d!port`. netd migrates an explicitly-
announced `127.x` listener onto its loopback stack while a `*` listener stays on
the NIC, so collapsing the two would move the server to a different interface --
which is also why the in-guest gate binds `127.0.0.1` explicitly rather than
`INADDR_ANY`.

### One thread, both ends

The gate drives a server AND a client from the same single-threaded process,
because a `PHENO_LINUX` Proc can neither `clone` nor `fork`. That works because
TCP establishes in netd's stack rather than in `accept()`: the client's
`connect()` completes the handshake against the announced listener, and the
server's `accept()` finds the connection already waiting. The bytes then cross
in both directions over untranslated T1 `read`/`write`, which is the point of
the whole design -- once a socket is connected there is no socket code on the
hot path.

A second client is then connected and accepted, which is the claim
`listen() == 0` only gestures at: that asserts socktab state, the second accept
asserts netd actually re-armed the listening socket.

### Coverage

Six new pure unit tests (`vivarium.socktab_bind_fields` / `listen_decide` /
`announce_cmd` / `parse_ipport` / `sockaddr_build` / `sockaddr_parse_any`) and
forty-four in-guest legs (L53-L96).

Revert-probed four ways, each failing at its own layer:

| Sabotage | Fails at |
|---|---|
| The constrained-bind decline removed from `connect` | in-guest **L68** |
| `accept` does not claim its socktab entry | in-guest **L83** |
| `listen`'s bound-port requirement removed | unit `1261/1262`, boot-fatal |
| The peer-address fill removed from `accept` | in-guest **L74** |

Two of those are worth naming, because writing the probe is what produced them.

**L83 exists because the sabotage would otherwise have been invisible.** An
`accept` that forgot to claim a socktab entry still returns a working fd:
`read`/`write` on it are untranslated T1 rows on a real Spoor, indifferent to
whether the socket table knows about it. Nothing else in the leg set could tell.
L83 asserts `listen(accepted_fd) == EINVAL` -- *connected*, not *not a socket* --
and paired with L96's `ENOTSOCK` after close it brackets the entry's whole life.

**L74 was satisfiable by a kernel that never wrote it.** `addrlen` is
value-result, and the probe originally seeded it with 16 -- the answer -- so
"it equals 16" proved nothing. It is now seeded `0xFFFF`, a capacity large
enough for the full copy and a value only the kernel can turn into 16. Probe 4
fails there; before the change it would have passed.

---

## Readiness -- the fd that gets polled is not the fd that gets read (V-5c-1)

`ppoll` is the poll family on aarch64: the generic ABI dropped plain `poll(2)`
and `select(2)`, so musl's `poll()` and `select()` both arrive at 73 and 72.
V-5c-1 lands `ppoll`; `pselect6`'s three-`fd_set` reshape is V-5c-2.

### The array needs nothing; the fd needs everything

`<thylacine/poll.h>` is deliberately Linux-shaped -- 8 bytes, `fd` at 0, `events`
at 4, `revents` at 6, and the same event values. So `viv_ppoll` copies the array
in and out unchanged, and the entire translation is the **fd**.

A `/net` socket cannot be polled on its own fd. That fd names
`/net/<proto>/N/data`, an ordinary dev9p file, and dev9p reports an ordinary file
as always-ready -- correct for a file, useless for a socket. netd publishes
readiness on a sibling, `/net/<proto>/N/ready`, whose qid carries `QTPOLL`, and
`dev9p.poll` probes exactly that bit. So the translator opens the sibling, polls
*that*, and restores the caller's own fd number before returning. Polling the
socket's own fd returns "ready" instantly and defeats every wait -- the same bug
the pouch boundary-line hit at net-6b-3.

The `ready` fd is opened **per call**. Caching it (pouch's choice) would place a
handle the guest never asked for into the guest's own fd-number space, where the
guest can close it and where it breaks POSIX's lowest-available-fd guarantee. In
pouch that hazard is absent because there the ready fd *is* a guest fd its own
libc opened. Here the guest cannot see it, so it must not outlive the call -- and
it is unobservable for exactly the reason the socktab needs no lock (a
`PHENO_LINUX` Proc is single-threaded). Both properties end together at task #93.

### The measurement that changed the design

The first version of these legs asserted a zero-timeout `POLLOUT` on a freshly
accepted socket. It failed -- and finding out why is the substance of this
sub-chunk.

netd's readiness probe is **asynchronous**. `dev9p.poll` *submits* a probe and
answers from a cache; a freshly-opened `ready` fd has no cached value, so the
first poll of it returns not-ready no matter what the socket is doing. Combined
with the per-call open, *every* poll is a first poll. A strict zero-timeout scan
would therefore report "nothing ready" for a plainly writable socket, and a
caller looping on timeout 0 would never make progress at all.

So a caller-supplied timeout of **0** gets a 10 ms budget when a socket is
actually in the array (`VIV_PPOLL_PROBE_MS`). That changes the latency, never the
answer: what comes back is netd's real verdict rather than an approximation of
it, and a probe that misses even that budget yields not-ready, which the caller
retries. A caller's own timeout is never touched. This is a mitigation, not a
closure -- task #98 holds the two real fixes (a poll core that holds Spoors
rather than fd indices, so the ready fd can be cached outside the guest's fd
space; or a synchronous readiness query), both on the audited net-6b surface.

**The same measurement condemned a leg that was passing.** L107 asserts that an
announced listener with no call pending does *not* report `POLLIN`. With a zero
timeout it passed -- because the probe had not answered yet, not because the
listener was quiet. It tested nothing. It now uses a real timeout and is
explicitly documented as meaningful only paired with L110, which shows the same
fd *does* report `POLLIN` once a call arrives. The same pairing rule governs the
connected-socket legs: `POLLOUT` is asserted **first**, so that the `POLLIN`
timeout below it is a real silence rather than an unanswered question.

### The netd half -- #220, finally live

POSIX defines `POLLIN` on a listener as "a connection is pending -- `accept` will
not block". netd computed readiness from `can_recv()`, which is false for a
listening socket in every state, so `poll(listener, POLLIN)` deferred forever
while a real client sat connected. A server that polls before accepting -- the
entire point of poll -- could never learn it had a caller. This was a documented
seam from net-6b-4 and became reachable the moment a Linux guest could poll at
all.

`slot_poll_readable` now reports an announced slot via `accept_ready`, the *same*
predicate `poll_accepts` uses to decide a deferred accept may complete, so a
poller and an accepter cannot disagree about whether a call has arrived. The
window is not narrow: netd swaps a listener only when some fid is already blocked
in `open(listen)`, so a server that polls first leaves the established call
sitting in the listener's socket until it chooses to accept.

### The collision re-check, which finally has work to do

Every earlier V-table row is above the native ceiling, so ARCH section 25.4's
mandated collision re-check was discharged by construction. These two are not:
**72 is `SYS_GETPID` and 73 is `SYS_GETUID`.**

> **The ceiling is a symbol, not a number, and this is why.** It was written out
> as a literal in four separate places — twice in `vivarium.h`, twice here — and
> when `SYS_EXECVE` (101) and `SYS_RFORK` (102) landed at LINEAGE L-2a/L-3b, all
> four went stale together; one of them still carried the pre-#97 "256,
> sparsely". None of it was load-bearing at the time, because every affected row
> sits far above either value. But a claim that nothing checks is a claim that
> will eventually be wrong when it matters. L-3d replaced the literals with
> `VIV_NATIVE_CEILING`, pinned it to `SYS_RFORK` with a `_Static_assert` in
> `vivarium.c` (the one file that can see both headers), and made every row that
> leans on the ceiling assert against the symbol — so a future bump stops the
> build instead of quietly voiding an argument. It cannot catch a *new* higher
> number on its own (C has no max-over-an-enum), which is why bumping the
> constant is stated as part of adding a syscall.

The argument still holds, for a different reason. A `PHENO_LINUX` Proc cannot
reach a native number *at all* -- every number it issues goes through
`vivarium_translate`, and an unclassified one lands on FORWARD, which with V-3
deferred is ENOSYS. It never had getpid or getuid to lose. In the other
direction, a native program mis-declared `PHENO_LINUX` issuing native 72 now
dispatches the pselect6 translator over getpid's absent (hence garbage)
arguments: it reads user memory the caller owns, bounds-checked, and polls fds
the caller already holds, so the worst outcome is EFAULT or a block, never
authority. A mis-declared Proc is comprehensively broken either way; I-43 governs
what it can *reach*.

A future row below 100 owes the same paragraph. `vivarium.rejects_are_deliberate`
asserts both collisions by name, so the fact is stated rather than discovered.

### Coverage

Two new pure unit tests (`vivarium.timespec_to_ms` / `vivarium.ppoll_decide`) and
twenty-eight in-guest legs (L97-L124). Suite 1262 -> 1264.

Revert-probed four ways, each failing at a different layer and a different file:

| Sabotage | Fails at |
|---|---|
| The ready-file translation removed (poll the socket fd itself) | in-guest **L107** |
| The #220 listener fix removed from netd | in-guest **L110** |
| The zero-timeout probe budget removed | in-guest **L113** |
| The sigmask decline removed | unit `1263/1264`, boot-fatal |

Probe 1 fails *earlier* than designed and that is worth noting: the very first
socket poll catches it, because an untranslated socket fd is an ordinary
always-ready file and the leg expecting silence gets noise. Probe 4 fails at the
unit layer before the in-guest leg can run -- the cheaper test catching it first,
the same ordering V-5b saw.

## V-5c-2 -- pselect6, the fd_set reshape

`pselect6` is the one T2 row whose translation is a genuine change of
*representation*: three 1024-bit bitmaps in, one pollfd array out, three bitmaps
back. Everything else in the tier is a renumber, a flag map, or a struct copy.

The socket->`ready` fd swap is not reimplemented. `viv_poll_translated` was
factored out of V-5c-1's `viv_ppoll` and both rows call it, which forced one
real change: the helper now RESTORES the caller's fd numbers rather than merely
closing what it opened. For `ppoll` that is tidiness (the write-back touches only
`revents`); for `pselect6` it is load-bearing, because `pfds[i].fd` is the BIT
INDEX to set in the caller's `fd_set`, so a left-behind readiness handle would
report the wrong fd as ready.

### The prior art was wrong four times

pouch's userspace `select()` (`0005-pouch-poll.patch`) performs this identical
translation over native `SYS_POLL`. Measuring it before writing this was worth
more than the writing -- every defect is a decision point here, and three of the
four are directly asserted by the new unit tests (task #99):

| pouch defect | What this does instead | Pinned by |
|---|---|---|
| **F-a** rejects any fd >= 64 with `EBADF` -- the bound copied from `PROC_HANDLE_MAX` when it was 64, left behind when `ffcc64b7` split it from `POLL_MAX_NFDS` (256 vs 64) | The ceiling is on the COUNT of contributing fds; fd 200 is an ordinary handle | `vivarium.fdset_to_pollfds`, "fd 200 converts" |
| **F-b** maps `exceptfds` to `POLLPRI`, which native poll cannot report, so a pure-`exceptfds` wait blocks forever | A SET bit DECLINES with `ENOSYS`; NULL or all-zero passes | `vivarium.fdset_to_pollfds`, "a SET exceptfds bit is refused" |
| **F-c** forwards `POLLHUP` into the write set, commented "(Linux semantics)" | `POLLIN_SET = POLLIN\|POLLHUP\|POLLERR`, `POLLOUT_SET = POLLOUT\|POLLERR` | `vivarium.pollfds_to_fdset`, "NOT writable" |
| **F-d** returns a count of fds | A count of BITS -- an fd ready both ways counts twice | `vivarium.pollfds_to_fdset`, "counts TWICE" |

F-a is the one with a real user (anything holding more than 64 open files) and
the one whose lesson generalises: the constant was COPIED rather than NAMED, so
when the kernel moved it the copy silently became a bug. `vivarium_pselect6_decide`
references `PROC_HANDLE_MAX` by name for exactly that reason, and the same stale
conflation was found and fixed in `poll.h`'s own comment while passing through.

### The sleep that both forms needed

`select(0, NULL, NULL, NULL, &tv)` is the classic portable sleep. V-5c-1 declined
`ppoll`'s twin on the grounds that "there is no native sleep syscall to route it
to" -- true, and one layer too high: there is no sleep *syscall*, but there has
always been a sleep *primitive*. `sys_poll_sleep_for` is poll's own slow path
with the fd array removed -- a private Rendez nothing can signal, a cond that is
never true, and the same deadline arithmetic -- so it ends on its deadline or on
a death-interrupt (#811), holding no lock, handle, or ref across the wait.

`sys_poll_for_proc`'s `nfds == 0` rejection is deliberately NOT relaxed: it is a
native ABI a native caller may rely on. This is a separate entry point.

### Coverage

Four new pure unit tests (`vivarium.pselect6_decide` / `fdset_bytes` /
`fdset_to_pollfds` / `pollfds_to_fdset`), one measured kernel test
(`poll.sleep_for_waits`), and ten in-guest legs (L125-L134). Suite 1264 -> 1269.

`poll.sleep_for_waits` reads the clock ON PURPOSE. The in-guest leg can only
assert that the call returns 0, which a kernel that never waited would also
satisfy -- the phenotype has no `clock_gettime` row, so the guest cannot tell the
difference. The unit test is the half that proves the sleep is a sleep.

Revert-probed five ways, each failing at its own assertion:

| Sabotage | Fails at |
|---|---|
| `POLLHUP` added to `POLLOUT_SET` | unit `vivarium.pollfds_to_fdset`, "NOT writable" |
| The write-side bit stops incrementing (counts fds) | unit `vivarium.pollfds_to_fdset`, "counts TWICE" |
| pouch's F-a reproduced (cap on fd VALUE) | unit `vivarium.fdset_to_pollfds`, "fd 200 converts" |
| `sys_poll_sleep_for` returns without waiting | unit `poll.sleep_for_waits`, "actually waited" |
| The table row reverted to `VIV_FORWARD` | in-guest **L125** -- unit suite fully GREEN |

The last probe is the one worth keeping. With the table assertion neutralised so
the probe could reach the guest, the unit suite passed **1269/1269** while the
in-guest leg failed: the two layers genuinely see different bugs, which is the
whole reason both exist. The unit tests prove the DECISION; the guest legs prove
the PLUMBING; neither is a substitute for the other.

## V-5d -- the focused audit (the self-audit half)

The formal prosecutor round is recorded in `memory/audit_vivarium_closed_list.md`.
This section is the concurrent self-audit's own findings, which were fixed in the
V-5d commit.

### SA-1 [P2] -- the close hook's completeness was an unstated precondition

The socktab keys on the fd NUMBER, so an entry has to be dropped whenever that
number is freed. Exactly one place does it: the hook in `viv_linux_dispatch`,
which fires on `VIV_LINUX_CLOSE` and nothing else. The hook's comment explains
why the drop is needed and never states the fact that makes it SUFFICIENT --
that `close` is the only fd-freeing row.

It is: `dup` (23), `dup3` (24) and `close_range` (436) were absent from the
table entirely, so they FORWARD to ENOSYS. But absent is a weak defence, and
this file's own standard says so -- "a number never considered and one
considered and rejected are different facts". Each of the three is a near-trivial
renumber (`dup3` -> `SYS_DUP` is nearly one), so adding one as an ordinary T1 row
is an easy and invisible mistake, and it would reintroduce precisely the bug the
hook exists to prevent: a freed fd number whose `(proto, N)` entry survives to be
handed to the next fd-creating call, so a later `connect()` writes a dial verb to
a stranger's connection.

FIX: the three numbers are now named in the enum and listed in `g_viv_rejects`
with the coupling stated, and `vivarium.fd_freeing_rows` asserts none of them is
served. The assertion message names the remedy ("extend the socktab close hook
before serving it") rather than the symptom, because the failure will be read by
someone who is mid-way through adding a row and does not yet know why they
cannot.

### SA-2 [P2] -- the fd restore was load-bearing and untested

`viv_poll_translated` swaps each `/net` socket's fd for a freshly-opened
readiness fd, and restores the caller's own numbers before returning. That
restore is cosmetic for `ppoll` (only `revents` is written back) and LOAD-BEARING
for `pselect6`, where `pfds[i].fd` is the BIT INDEX to set in the caller's
`fd_set` -- an unrestored array reports the readiness handle's number as the
ready fd, a number the guest never opened. V-5c-2's commit message called this
the chunk's headline correction.

Nothing tested it. Every `pselect6` leg used the report file -- an ordinary file,
never translated, so `opened[i]` stays -1 and the fd is never rewritten. `L103`,
whose comment claims to test exactly this ("the kernel polled a different handle
underneath"), polls a regular file too, so it is satisfiable with the restore
deleted. **Removing the restore left the entire gate green.**

FIX: `L135`-`L141` run `pselect6` over a real connected UDP socket. `L138` is the
restore proof (the returned bit must be the socket's); `L139`-`L140` are the
overwrite proof (a bit set going in for an fd that does NOT become ready must
come home clear -- which `L129` could not give, since it asserted a set that was
already zero on entry).

### SA-3 [P3] -- the two write-back paths disagreed above nfds

Linux copies `FDS_BYTES(n)` bytes back out of a buffer it zeroed, so a bit the
caller set ABOVE `nfds` -- in range of the COPY though out of range of the SCAN --
comes home clear. The `count > 0` path already matched (the reverse map zeroes
before it sets). The `count == 0` path returned early and wrote nothing, so such
a bit survived. A well-formed caller never sets one; the two paths disagreeing is
the part worth fixing.

FIX: `count == 0` zeroes the buffers and falls through to the shared write-back.
Pinned by `L142`-`L143`.

### SA-4 [P3] -- a short ctl-verb write read as success

`connect` and `listen` both write a verb to the connection's `ctl` file and
tested only `w < 0`. A ctl verb is all-or-nothing -- netd parses the whole buffer
or rejects it -- so a SHORT write would mean a TRUNCATED command was accepted,
which is a different event from a slow one and must not read as success. It is
unreachable today (a verb is at most 48 bytes, far under any negotiated msize),
and the check costs nothing, so both sites now compare against the full length.

## V-5d -- the formal round (Fable 5, max effort)

`MODEL(start) == MODEL(end) == Fable 5` -- no fallback, so the round carried full
lineage independence from the Opus implementation agent.

**0 P0 / 1 P1 / 1 P2 / 4 P3. NOT dirty** -- the P1 is a local restructure inside
one helper, the P2 a four-arm unwind; neither touches a wait/wake protocol or
lifts a lock-order rule, so no round-2 is owed. Every fix is revert-probed.

### F1 [P1] -- a negative fd made a blocking `ppoll` return instantly

Linux and the native poll disagree about a negative fd, totally. poll(2): *"If
`fd` is negative, then the corresponding `events` field is ignored and the
`revents` field returns zero"* -- the entry is INERT and contributes nothing to
the count. That is how every fixed-array event loop disables a slot without
compacting. Thylacine's poll says the opposite and documents it (`poll.h`:
"negative => POLLNVAL"): `poll_scan_one` returns 1 for such an entry.

`viv_poll_translated` skipped them (`if (kfds[i].fd < 0) continue;  // caller-
disabled entry`) and let them reach the native poll unchanged. So ANY disabled
slot made `ready_count > 0` on the first scan, the native fast path fired, and a
`ppoll` asked to block forever **returned at once** with `POLLNVAL` on exactly
the slots the caller had switched off -- a hard spin at 100% CPU, plus a
`revents` a robust event loop reads as "this fd died, tear it down". It also
defeated the #98 probe budget: the fast path fires before `VIV_PPOLL_PROBE_MS`
can be spent, so a socket beside a disabled slot reports not-ready forever.

Subtracting them from the result afterwards would have fixed the count and the
`revents` and **not the blocking**, so they must not reach the native poll at
all. FIX: compact them out before polling and route the all-disabled case to
`sys_poll_sleep_for` -- the same primitive `nfds == 0` already uses, which makes
the two cases one shape. `orig[]` is the discriminator that makes it exact:
`orig[i] < 0` is the CALLER's disable, `orig[i] >= 0` with `kfds[i].fd < 0` is
OURS (a readiness file that would not open), and that one is still owed its
`POLLNVAL`.

The comment `// caller-disabled entry` shows the case was noticed. What was
never asked is what the native poll *does* with it -- the round's own summary of
the pattern: **the guard that exists is what stops you asking whether it is the
right guard.**

### F2 [P2] -- a failed `accept` kept the connection

By the peer-address write-back the accept has fully committed: `dfd` is open,
the socktab entry is claimed, and netd's connection `M` is live and held by
`dfd` alone. All four `uaccess` failure arms did a bare `return -EFAULT`,
handing the guest three resources and telling it nothing -- the fd number was
the return value it just lost. Per call: one handle (of `PROC_HANDLE_MAX`), one
socktab entry (of `VIV_SOCK_MAX`), and one **netd slot** (of `MAX_SLOTS`, shared
across every `/net` client on the box), reclaimable only by Proc death. Linux
unwinds here -- `__sys_accept4`'s `move_addr_to_user` failure goes to `out_fd:
fput(newfile); put_unused_fd(newfd)`.

Not new authority (the ceiling is the documented #65-class bound and I-43
holds), but the guest cannot clean up after itself, which the native path
allows. FIX: all four arms `goto fault_unwind`, which drops the socktab entry
BEFORE closing the fd -- closing first would leave an entry naming a number the
next fd-creating call can be handed, which is the stale-entry bug the close hook
exists to prevent, reintroduced from the other end.

The sibling `viv_sock_connect` gets the identical situation right, and every
*earlier* arm in `accept` unwinds correctly. Only the tail stopped.

### F3 [P3] -- `nfds` was tested as 64-bit where Linux passes an `int`

`vivarium_pselect6_decide` took `s64` and tested `nfds < 0`. A caller leaving x0
merely zero-extended (`0x00000000FFFFFFFF` for `int n = -1`) yields a POSITIVE
4294967295, which was then **clamped to `PROC_HANDLE_MAX` and served** where
Linux answers `EINVAL` -- working on one toolchain and not another. This is
exactly what `vivarium_openat_decide`'s `(s32)(u32)dirfd` exists to prevent, and
its comment says so. FIX: the same truncation, inside the pure decide so a unit
test can see it. The existing `L132` leg passes a *sign*-extended -1 and cannot
distinguish; the new unit leg passes the zero-extended form.

### F4 [P3] -- the mis-declared-phenotype claim was incomplete

The number-collision argument for rows 72/73 said the worst outcome of a
mis-declared Proc issuing native `getpid` is "EFAULT or a block". `pselect6`
also *writes* up to three 32-byte `fd_set` results to caller-register-controlled
addresses. The load-bearing half is correct -- bounds-checked, confined to the
caller's own address space, confers nothing, so **I-43 holds** -- but the
enumeration was wrong, and this paragraph is the I-43 discharge the next reader
leans on. Corrected. (The prior round's lesson: *a claim in a comment is exactly
as unverified as one in chat.*)

### F5 [P3] -- the kind gate ran after the operation it protects

`viv_sock_connect` cast `dh.obj` to `struct Spoor *` and `spoor_ref`'d it without
testing `dh.kind`. `handle_replace`'s Spoor-only gate would catch a non-Spoor --
four lines later, after the ref had already been taken at an offset that is only
a Spoor's by assumption. Unreachable today, and now checked in the order the
header claims.

### F6 [P3] -- a transient shortage answers `EBADF`

The readiness-open failure arm maps a dead connection, a vanished `/net`, and a
TRANSIENT resource shortage onto one `POLLNVAL`, which `pselect6` escalates to a
whole-call `EBADF`. Linux never turns a resource shortage into `EBADF` on
select. Left as-is deliberately and documented: the fix is to stop consuming
guest fd numbers at all, which is the same change #98 needs, and splitting the
arm now would encode the fd-space design it should replace.

### The revert probes

| Sabotage | Fails at |
|---|---|
| The `kfds[i].fd = orig[i]` restore removed | in-guest **L138** -- unit suite fully GREEN (1270/1270) |
| The `count == 0` early return restored | in-guest **L143** -- unit suite fully GREEN (1270/1270) |
| `dup` added as an ordinary T1 row | unit `vivarium.fd_freeing_rows`, 1269/1270 |
| F1: the caller-disabled pass-through restored | in-guest **L144** -- unit suite fully GREEN |
| F2: the leaking bare `return -EFAULT` restored | in-guest **L154** -- unit suite fully GREEN |

Four of the five land in the guest with the unit suite untouched. That is the
L125 lesson recurring, and sharper here: these
are not merely properties the unit layer happens not to cover, they are
properties it CANNOT cover, because the object under test (`viv_poll_translated`)
is a static function that needs a live Proc, a live handle table and a live
`/net` mount. The guest legs are the only layer that can see them.

**The F2 leg failed on its first run against CORRECT code**, and the reason is
worth keeping: it measured a run of fds *before* the client socket existed and
compared it against a run taken *after*, so the two differed by one fd for a
reason that had nothing to do with a leak. The fix was to the measurement, not
the kernel -- take both runs with the same sockets held. A regression that fails
for the wrong reason is worth exactly as little as one that passes for the wrong
reason.

## V-8 F3 [P2] -- the fixed diorama name was first-come-first-served

> **SUPERSEDED 2026-08-17** -- the per-container diorama no longer posts any
> name: its channel is a private pipe pair handed at spawn (see "The diorama
> channel is a private pipe pair" at the end of this document). The gate, the
> `#101` joey leg and the "concurrent containers are still unsupported" note
> below describe the state between V-8 and that change; kept as the record of
> why the fixed name existed and what it cost.

`/srv/viv-dio` is a FIXED name, and deliberately so: the boot `SrvRegistry`
never frees a dead entry (task #33), so a per-container unique name would burn
a registry slot on every `viv run` forever, where a fixed one rebinds a single
tombstone across sequential runs. `post_srv_diorama`'s comment already promised
the consequence -- "a CONCURRENT second container collides here and the runner
fails closed". It did not. The runner polled the name and took the first
success, so a second `viv run` from the same shell mounted the FIRST
container's diorama and enumerated its processes. It failed OPEN.

### Why the check is in the server, not the runner

The finding proposed a per-runner name; that conflicts with the decision above.
The obvious alternative -- have the runner verify it got its own -- turns out
to be impossible with the identity available at that moment, and each way of
trying is instructive:

* `SYS_SRV_PEER` is **server-side only**. `sys_srv_peer_for_proc` refuses a
  client endpoint outright ("a client-side query would mis-report the caller's
  OWN identity"), and refuses this handle a second time over: a 9P-mode connect
  yields a dev9p root, and `devsrv_conn_of` returns NULL for anything that is
  not a devsrv connection Spoor.
* The registry records `poster_pid`, kernel-stamped -- but nothing outside a
  kernel test ever reads it. Exposing it is new ABI.
* Membership cannot help either. It is ppid-descent from the container
  ENTRYPOINT, which does not exist yet when the runner holds the connection, so
  every diorama's member set is equally empty. The selftest vector asserting
  exactly that ("vivarium pre-entrypoint not empty") sits directly above the
  gate's own vectors.

The server has what the runner lacks: `Conn::peer()` already queries the
kernel-stamped peer per use, in the direction the kernel supports. So the gate
lives in `h_attach` -- and gating ATTACH rather than each op is what makes the
cross-mount impossible rather than merely detectable, since every fid descends
from the attach root. A refused attach fails the opener's `SYS_OPEN`, which is
the fail-closed the comment promised.

`viv_attach_allowed(runner, peer_alive, peer_pid)` is a pure decision so its
truth table runs in the boot-fatal selftest. `runner == 0` -- the shared boot
diorama -- is no gate at all, and does not even pay for the `peer()` syscall.

### Concurrent containers are still unsupported

This makes the failure honest, not the limit go away. The runner now stops
polling the moment its own diorama exits (a dead diorama can never post) and
says so: another `viv run` holds the name, which is a known limit rather than a
fault in the bundle. Lifting it is the #33 registry-lifecycle work.

### The probes, and why two were needed

Boot OK proves the ALLOW branch -- a real `viv run` still mounts its own
diorama -- but it does **not** prove the gate is wired: an `if (false)` gate
would pass it identically. So the deny branch needs its own proof, and joey is
the only place a non-runner can reach a vivarium diorama (inside the container
`/srv` is not bound, so `viv-probe` cannot attempt it). joey spawns one whose
declared runner is a pid it cannot be, proves the service actually POSTED via
an O_PATH walk -- devsrv's *open* is the connect, so O_PATH is not itself
gated, and without this precondition a diorama that never came up would satisfy
the assertion just as well -- then attempts the open it has no right to.

Two reverts, two different failure points, neither visible to the other:

| sabotage | selftest | joey leg | unit suite |
|---|---|---|---|
| gate removed from `h_attach` | PASS | **FAIL** -- foreign runner mounted it | 1272/1272 PASS |
| `viv_attach_allowed` always true | **FAIL** | (never reached) | -- |

The first reproduces the original defect exactly. That the unit suite reads a
full 1272/1272 through it is the same blindness #100 recorded: pure tests prove
the decision, guest legs prove the plumbing.

## V-8 F4-F7 [P3] -- four small things, two of them only comments

The round's remaining findings. None was a live break; all four were verified
against the code before being acted on, and two of them turned out to be
sharper than the report.

### V-8 F4 -- the sentinel is also an index

`VIV_SIGNOTE_NONE` is 0 and means "no note in this tree carries that signal".
It is also a valid subscript into `viv_sigtab.act[]`. The three accessors gated
on `(u32)note >= VIV_SIGNOTE_COUNT`, which admits it, so a lookup for an
unknown note read `act[0]`.

Nothing reached it, but the reason spanned two files:
`vivarium_sigaction_decide` (vivarium.c) refuses a signum whose note is NONE,
so nothing could WRITE slot 0, so the READS in notes.c saw a zeroed slot and
answered SIG_DFL. The reads are the fragile end -- `notes_proc_has_live_handler`,
`notes_post`'s SIG_IGN hook and the delivery path all pass
`viv_signote_from_note_name(name)` straight in, and that decode answers NONE for
any name not in its table. Add a name to `g_known_notes` without adding its
decode row -- task #95's `terminate` is the one already queued -- and every
disposition lookup for it silently consults slot 0.

`viv_signote_indexable` now excludes the sentinel in all three, so the answer is
local: an unknown note has no slot.

The guard is COMPLETE rather than merely applied, and that is worth checking
rather than assuming -- the recurring failure in this arc is that a fix present
on site N stops you asking about site N+1. Grepping `act[` across the kernel
returns exactly the three accessor bodies in production; nothing indexes the
array directly, so guarding the accessors guards every path.

**The fix is behaviourally inert today**, which is exactly what makes its test
worth describing. Through the API slot 0 is unwritable, and a zeroed slot reads
"no handler / not ignored" either way -- so a test that only called the
accessors would pass identically with the guard reverted. The regression writes
`tab.act[0]` by hand and then asks the question the guard actually answers:
when slot 0 holds something, does a NONE lookup find it? Reverting the guard
fails at that assertion (1271/1272).

One thing the report did not raise and the code turned out to handle: the two
decode tables are DIFFERENT SIZES. `viv_signal_note` has 13 rows,
`viv_signote_from_note_name` has 9 -- the `snare:*` family (SEGV/BUS/ILL/FPE) is
writable but has no name row, so a disposition stored at `act[SNARE_SEGV]` would
be looked up at `act[0]`. That asymmetry is closed independently: a snare signal
admits only SIG_DFL (`handler != VIV_SIG_DFL && !viv_signote_is_deliverable(...)`
forwards), and `proc_fault_terminate` never calls `notes_post` at all, so there
is no queue entry to look up. Sound, but it is a second cross-file argument, and
the guard makes it stop mattering.

### V-8 F5 -- the justification named the wrong memory class

`notes_deliver_linux_locked` copies the signal frame out with `q->lock` held,
and the comment justified it by the target being ANONYMOUS memory: the guest's
own stack, "anon by construction (`exec_map_user_stack`)".

The conclusion held -- the clause about no writable mapping being file-backed
does rule out the only blocking arm -- but the named reason is the case a
phenotyped guest is LEAST likely to be in. V-2d routes
`mmap(PROT_READ|PROT_WRITE, ANONYMOUS)` to `sys_burrow_attach_lazy_for_proc`, so
a guest running on a makecontext/coroutine stack (the obvious use of anonymous
mmap) is on `BURROW_TYPE_ANON_LAZY` and faults in the lazy arm, not the eager
one the comment cites.

That arm is safe for a different reason, which is now what the comment says: it
has NO SLOW PATH. Its fill is `alloc_pages` + install entirely under the
already-held `vma_lock` -- no backing read, no pin, nothing death-interruptible.
The load-bearing property is which fault ARM runs, not which memory it is, and
stating it that way is what makes a future arm with a blocking fill (anon COW,
pageout) get checked against this site.

### V-8 F6 -- whence compared at 64 bits where Linux passes 32

`lseek`'s T1 row renumbers onto `sys_lseek_handler` with the argument words
copied verbatim, and that handler compared the full 64-bit x2 against
`T_SEEK_SET/CUR/END`. Linux's own `SYSCALL_DEFINE3(lseek, ..., unsigned int,
whence)` narrows by construction, so a guest leaving rubbish above bit 31 got
EINVAL for a seek Linux performs.

Two things settled where the fix belongs. First, the same handler ALREADY
narrows `fd` -- `(hidx_t)hraw`, and `hidx_t` is `int` -- and fd is the far more
dangerous argument, so the strict compare on an enum selector was an artifact of
the raw argument arriving as `u64`, not a designed strictness. Second, the
comment above the check already appealed to "POSIX's answer for an unrecognized
whence", and POSIX's whence is an `int`; narrowing makes the code agree with its
own stated contract rather than weakening an audited surface to serve a compat
row. `libthyla-rs` had already declared the parameter `u32`.

The probe is a PAIR, in joey's `probe100` block. A single leg would pass under a
handler that stopped range-checking whence altogether; the second says the low
half is still read:

| leg | whence | expect |
|---|---|---|
| `hiwhence` | `(1L<<32) \| T_SEEK_SET` | `0` -- high half ignored |
| `hibad` | `(1L<<32) \| 99` | `-22` -- low half still checked |

Reverting the narrowing gives the sharpest split this arc has recorded: the
unit suite reads a clean **1272/1272 PASS** while the in-guest leg fails at
exactly `hiwhence=-22`. The kernel tests cannot see it at all -- the handler is
`static` and takes a user VA, so only a real EL0 caller can pose the question,
and `libt`'s `t_lseek` taking a `long` is what lets a native probe pose it.

### V-8 F7 -- dispositions do not cross a fork

`rfork_internal` copies `child->phenotype` and reasons at length about a Linux
process forking, but `child->sigtab` stays NULL -- all-SIG_DFL. A forked Linux
child would silently lose every handler and every SIG_IGN its parent installed;
POSIX fork(2) inherits both, and execve(2) is the different rule again (reset
caught, preserve ignored), so process creation needs two behaviours here rather
than one.

(This paragraph said "no clone/fork/execve number is a table row, so a
PHENO_LINUX Proc cannot create another Proc at all" and pointed at task #93.
Both landed: `clone` (fork shape, L-3d) and `execve` (L-6a) are rows, and the
copy exists -- `viv_sigtab_clone_into` at fork, `viv_sigtab_reset_caught` at
exec, the POSIX rule voted 2026-08-17; `vivarium.sigtab_fork_exec_rule` is the
test.) The rest of this note is kept as the record of the reasoning that
opened the gap. Pinned in a comment beside the line whose
own reasoning opens the gap -- the same single-threadedness notes.c leans on for
its sigtab tearing argument.

---

## The V-8 close -- what was audited, what it cost, and where the tests are blind

**Scope, chosen by measuring rather than by assuming.** Two rounds already
existed: V-4c-3 covered the arc through V-4c-2, and V-5d covered the socket
family. Everything between and after was unaudited -- and V-6 (signals) is
marked "kernel and audit-bearing" in `VIVARIUM.md`'s own track note and had
never received a round. So V-8's scope is `3434d6c0` V-1b + `3ab62f07` V-2d +
`ab70b6c8`/`c514bc5f`/`bee70983` V-6a/b/c + `e7327f51` V-7, plus the #96 FP
change landed alongside it. A close that had said "the arc has been audited"
without asking which chunks would have missed all of V-6.

**0 P0 / 1 P1 / 2 P2 / 4 P3, `MODEL(end) == Fable 5`, and NOT a dirty close.**

| # | sev | what | closed |
|---|---|---|---|
| F1 | P1 | every T1 byte-I/O target returned a flat `-1`, which stock musl reads as `EPERM` | `285acd2c` (+ `d87c2be2`, #100) |
| F2 | P2 | a phenotyped handler lives in the sigtab, so the terminate-latch exemption never saw it -- a frame-push failure then spun the guest at 100% forever | `ff79386b` |
| F3 | P2 | the fixed diorama name was first-come-first-served; a second `viv run` mounted the first's | `27a11e2c` (#101) |
| F4-F7 | P3 | sentinel-as-index; a fault-arm justification naming the wrong memory class; `whence` compared at 64 bits; sigtab not inherited across a fork | `b3648a2b` (#102) |

Landed ahead of the round from its own disposition pass: **#96** (note delivery
did not save FP/SIMD -- silent corruption of an interrupted computation,
PRE-EXISTING on the native path and made reachable by ordinary compiled C once
the phenotype landed) and **#94**, both `a39d2c53`. A gate failure that surfaced
during the round -- `EXTINCTION: thread_free of RUNNING thread` -- was
root-caused to a PRE-EXISTING race in the kernel test suite, not V-8, and fixed
at `8c1c080e` (#103/#104).

### The headline: a correction on one tier is what stops you asking about the other

F1 is the arc's cleanest instance. The T2 shells map their errors to
`-(s64)T_E_*` correctly, and the `mmap` shell *names the hazard in a comment* --
"Thylacine signals failure with a bare -1, and a Linux libc reads -1 as -EPERM."
The knowledge was present, written down, and applied to Tier 2. **Tier 1 was
never asked the question.** Same family as V-4c-3's "finding one release site is
what stops you looking for the other writer" and V-5d's "the guard that exists
stops you asking whether it is the right guard".

Underneath it was something sharper and not vivarium-specific: `T_E_PIPE` had
been defined and ABI-pinned since the errno scripture landed and **nothing in
the tree ever emitted it**. `devpipe_write`'s EOF arm returned `-1` beneath a
comment asserting a musl wrapper translated it -- no such wrapper exists -- so
every write to a closed pipe reported `EIO` **tree-wide**, not merely under a
phenotype. A registered code with zero emitters is a contract stated and not
kept, and it is worth grepping for elsewhere.

### Where the tests are blind, stated once because the arc measured it four times

Every finding that could be revert-probed was, and the probes divide sharply by
layer. The pattern is durable enough to design future coverage around:

| layer | proves | blind to |
|---|---|---|
| kernel unit tests (`vivarium.*`) | the DECISION -- a pure table or predicate returns the right answer | anything reached only through a user VA or a real fd |
| in-guest joey/probe legs | the PLUMBING -- the decision is actually wired to the call | a decision that is wrong in a way the probe's inputs never pose |
| the diorama selftest | userspace predicates, boot-fatally | the server's own dispatch |

F6 is the extreme case and the one to remember: reverting the `whence`
narrowing leaves the unit suite at a full **1272/1272 PASS** while the in-guest
leg fails at exactly `hiwhence=-22`. The handler is `static` and takes a user
VA, so no kernel test can pose the question at all. F3's is the mirror image --
removing the gate from `h_attach` leaves both the selftest and the unit suite
green while the joey leg catches it.

Two shapes of test were needed that a green run does not suggest. A
**behaviourally-inert** fix (F4 -- the sentinel guard changes nothing today,
because nothing writes `act[0]`) needs a test that MANUFACTURES the state by
hand, or it passes with the guard reverted. And a leg that can be satisfied by
OVER-fixing (F6's `hiwhence` alone would pass under a handler that stopped
checking `whence` entirely) needs its pair.

### What the round confirmed, so it is not re-derived

The **I-43 argument-arity property** holds for all five T1 rows, checked against
handler signatures rather than dispatch sites. `kill`/`tkill`/`tgkill` are in
neither table, so **I-26's two-axis gate is untouched by this arc**. The
phenotype has three writers and three readers and nothing in `exec.c`/`elf.c`
touches it, so §12.1 rule 4 holds by construction. All 15 T2 rows have live
shells and the default arm fails closed. `q->lock` is never acquired under
`p->vma_lock`. The V-6 sigframe is bounds-tight, underflow-free, zeroed before
fill (no I-13 disclosure), read-only in effect, and **SIGKILL is uncatchable BY
CONSTRUCTION** rather than by a special case. V-2d's prot check is a true
allow-list, so `PROT_EXEC`/BTI/MTE/GROWSDOWN all decline and I-12 is not
weakened. `viv` holds no capability beyond its invoker's.

Carried forward from the round's own honesty: it executed nothing -- every claim
came from reading -- `usr/viv/src/json.rs` was not read line by line, and F3's
timing argument was reasoned from relative work rather than measured.

### Gates at the close

Suite **1272/1272**, boot OK, 0 EXTINCTION, with the two-vantage phenotype gate
and `probe100`'s byte-I/O legs boot-fatal on every build. The FULL SMP gate
**40/40 PASS, 0 corruption / 0 timing / 0 external-kill** (default+UBSan x
smp4/smp8, N=10). LS-CI **32/32**, 0 retries, 0 kills, coverage verified by set
comparison rather than by count. No spec gate: nothing in this arc is modelled
in `specs/` -- `specs/phenotype.tla` is reserved for whichever chunk builds a
forward destination, which on present evidence is process creation (#93).

---

## The `clone` row (LINEAGE L-3d)

The row that gives a Linux guest a second process, landed as part of the LINEAGE
arc rather than the VIVARIUM one — the address-space machinery is L-1..L-3c's,
and this is the last piece: the translation.

### The mapping is a constant; the decide function decides the domain

```
clone(CLONE_VM|CLONE_VFORK|SIGCHLD, stack, ptid, tls, ctid)
    ->  SYS_RFORK(RFPROC|RFMEM, stack, 0)
```

`vivarium_clone_decide(flags, stack)` is pure and takes exactly two words. Every
other question this row could have had — where the child's stack pointer must
sit, whether it overlaps the caller's, whether the flags word is well-formed —
is `SYS_RFORK`'s own gate, and the shell reaches it through `sys_rfork_core`,
extracted here so the native handler and the phenotype share one copy. That is
the V-8 `sys_fstat_for_proc` discipline applied to a call that returns twice.

### Only two argument words are read, and that is a correctness requirement

arm64 selects `CONFIG_CLONE_BACKWARDS`, so the register order is

```
x0 flags   x1 stack   x2 parent_tid   x3 tls   x4 child_tid
```

— `tls` *before* `child_tid`, not the x86-64 order. musl's own
`src/thread/aarch64/clone.s` states it in a comment at the top of the file,
which is where it was read from.

But the order is the smaller trap. **On the call that matters, x2/x3/x4 hold
garbage.** `posix_spawn` invokes `__clone(child, stack, flags, arg)` with four
arguments (`posix_spawn.c:198`), and `clone.s` then executes

```
mov x2,x4      mov x3,x5      mov x4,x6
```

moving three registers the caller never set. Linux tolerates it because
`CLONE_PARENT_SETTID`, `CLONE_SETTLS` and `CLONE_CHILD_SETTID` are all clear, so
its kernel never reads them.

A translator that reached for `args[3]` as the child's TLS would therefore hand
the child an uninitialised register as its `TPIDR_EL0` — and the child would
fault or corrupt at its first thread-local access, at a site with no visible
connection to the clone. So the shell reads only `args[0]` and `args[1]` and
passes a literal `0` for `child_tls`, which is `SYS_RFORK`'s inherit sentinel and
what a vfork child needs anyway. The domain's exclusion of `CLONE_SETTLS` is what
makes that correct rather than merely lucky.

This is the *inverse* of the arity property stated above for T1 rows: there the
risk is a native target reading more argument words than the Linux call supplies;
here the words are supplied and meaningless. Both reduce to one rule — **read a
register only when the call's own contract says it holds something.**

### The flags word is compared at 64 bits, unlike every other decide here

`vivarium_mmap_decide` and `vivarium_openat_decide` narrow to `u32`, and that is
correct for them: their Linux parameters *are* `int`, so the narrowing is the
ABI. `clone`'s `flags` is an `unsigned long`. Narrowing there would be an
assumption about Linux's own source rather than about its ABI, and that source is
not in this tree — so it cannot be checked, and it must not be asserted from
memory.

Under that uncertainty the stricter reading is the right one, because this
tier's own rule is that *declining is always safe* while admitting a bit nobody
has reasoned about is precisely the failure it exists to prevent. It also costs
nothing: musl's `clone.s` emits `uxtw x0,w2` (confirmed in the built object as
`ubfx x0, x2, #0, #32`), so the high half is always zero from the real consumer.
The only caller this turns away is a hand-rolled one setting an unconsidered bit
— exactly who should be turned away.

The first draft narrowed, by copying the mmap decide's shape without
re-deriving its justification; the self-audit caught it. **A shape copied from a
sibling carries that sibling's justification, which may not survive the copy.**

### `CLONE_VM` without `CLONE_VFORK` is refused

This is the one place LINEAGE L-3c-2's reasoning does not carry over, and the
inversion is worth stating because the two chunks reach opposite conclusions from
the same shape.

L-3c-2 keyed the vfork suspend on `RFMEM` rather than on a flag of its own,
arguing that the fail-safe direction is one-sided: an unwanted suspend blocks
visibly and terminates, while an unwanted concurrency is corruption three layers
from its cause. That holds for a **native** caller, who reaches `SYS_RFORK`
through a Thylacine ABI whose only shape is the vfork one.

It does not hold here. A stock Linux binary that sets `CLONE_VM` and clears
`CLONE_VFORK` has said, in the only vocabulary it has, *do not suspend me* — and
serving it with a suspend converts a working program into a deadlock the moment
its child neither execs nor exits promptly. That is not conservative; it is a
hang with our name on it.

So the domain is an exact equality and the caller gets an honest decline. The
genuinely concurrent shape keeps the target it already has: `CLONE_THREAD` onto
`SYS_THREAD_SPAWN`, whenever that row is written.

A zero `stack` is Linux's `vfork()` proper ("share the parent's stack"), and it
is **served as a fork** (option B, operator-voted 2026-08-31). Plan 9 has no
two-Procs-one-stack shape — `rfork` always gives the child its own stack — and
`SYS_RFORK`'s RFMEM `child_sp` rule is that invariant, so rather than weaken it
(option A, rejected as anti-lineage) the null-stack vfork maps to a private
copy-on-write child. POSIX makes anything but `_exit`/`exec` after `vfork`
undefined, so a copy is conformant, and the result is `share_mem=false` — the
same translation `clone(SIGCHLD, 0)` produces, reached by a second flags word. A
**non-zero** `stack` stays a true RFMEM vfork (`posix_spawn`'s shape). Full
rationale: `docs/LINEAGE.md` §3.1 + the "A zero stack" paragraph. The concrete
driver was busybox's `tar`/`gzip` pipeline, which issues `vfork()` proper and
had no fallback for the pre-B `ENOSYS`.

The exit signal is the **low byte**, not a flag, and only `SIGCHLD` is admitted:
`exits()` posts `child_exit` unconditionally (I-19), so any other request —
including `0`, "no signal", which is what a detached child asks for — would get a
note it did not ask for.

### It is a `VIV_TIER2` row, not an interception

`rt_sigreturn` is handled ahead of the table because it *rewrites the frame*
instead of returning a value, so the dispatcher's `regs[0] = viv_tier2(...)`
store would destroy the x0 it had just restored. Clone is not in that class: it
returns its result the normal way, into the **parent's** frame. The child's
`regs[0]` was set to 0 in its own *copy* of the frame by `fork_frame_init`,
before the shell returns, and the child is a different Thread on a different
stack that never comes back through the dispatcher.

So `viv_tier2` gains a `ctx` parameter — used by this one case, ignored by the
rest — rather than the row gaining a second dispatcher.

### The gate was not the one this chunk was given

`LINEAGE.md` §7 said *a stock `posix_spawn` binary runs in a vivarium*. Reading
what `posix_spawn` actually drives shows the child needs `execve` (221) and the
parent usually `wait4` (260), neither of which is a table row — and neither of
which is a dependency of *the clone translation*. They are dependencies of
`posix_spawn`, which is L-6's deliverable, and each is a real translator with its
own reasoning (execve must walk a `char *[]` and repack it into `SYS_EXECVE`'s
concatenated blob). Pulling both here would have made L-3d into L-6 rather than
completing L-3d.

So the gate became: **a clone whose child runs, writes into the shared address
space, and exits, with the parent suspended until it does.** This is the same
correction L-3b's gate needed, for the same reason — a gate written before the
work was measured named a consumer instead of the property.

### Coverage, and a false green worth remembering

`vivarium.clone_domain` asserts every decline by name and by reason (three
independent classes of widening, so a mask test would admit all of them at once),
plus a `VIV_TIER2` pin so a future edit cannot demote the row to T1 and copy all
six argument words verbatim into `SYS_RFORK`.

The in-guest legs L155–L163 go through `__viv_clone`, a transliteration of musl's
`clone.s` — necessary because the child returns on a *different stack* while
holding the parent's `x29`/`x30`, so no `asm!` wrapper can be safe. It diverges
from musl in exactly one way: it loads **recognisable poison** into x2/x3/x4
rather than leaving them uninitialised. "Uninitialised" is the real hazard but is
not a value a test can assert against; poisoning makes it deterministic, so a
translator that ever reads x3 produces a child whose `TPIDR_EL0` is `0xBAD3` and
L162 says so precisely.

Two revert probes, the layers blind in both directions: gutting the pure domain
check fails the unit suite at exactly its own assertion (1286/1287) and never
reaches the guest; breaking only the shell leaves the unit suite at a **full
1287/1287 PASS** while the guest fails at exactly `marker=L162`.

**L161 is deliberately NOT claimed as independently revert-probed.** A third
probe disabling the vfork park does fail — but at `/fork-probe` leg I, the
*native* leg, which runs earlier in the boot, so the container never starts and
L161 is never reached. The park is therefore proven load-bearing on exactly the
path this row uses (the shell reaches `rfork_internal` through the same
`sys_rfork_core` the native handler does), and what L161 adds is that the
property survives *the translation* — not an independent proof of the park
itself. Isolating it would need a probe that disables the park only for a
phenotyped caller, which would be testing a configuration the system never runs.

> **The first two runs of that second probe were a FALSE GREEN.**
> `viv-pheno-probe`'s *containered* copy lives in the **pool** (`/vivarium`,
> baked by `populate_stratum_pool`), which `THYLACINE_MKFS_PRESERVE=1` skips —
> so both the production run and the revert probe executed a binary that
> predated the new legs entirely, while `build/ramfs-src/viv-pheno-probe`
> disassembled correctly and the boot reported `phenotype ... PASS`. The
> ramfs copy is fresh; only the container's is stale, which is what makes this
> hard to see. Any chunk adding viv / diorama / alpine-bundle legs must take one
> `PRESERVE=0` build before trusting any result (task #126).
>
> **L-6a hit this again, and the failure mode was subtler the second time.** The
> chunk rewrote L156 from a decline into a real fork; with `PRESERVE=1` the
> guest ran the *old* binary, which still asserted the decline — so the boot
> reported `marker=L156 status=1`, a plausible-looking failure of the new work
> rather than an obviously stale artifact. A stale binary does not always read
> as "nothing changed"; when the *policy* flipped, it reads as "the change is
> broken".

---

## The `execve` row + the `clone` fork shape (LINEAGE L-6a)

`execve` is `VIV_LINUX_EXECVE = 221` -> `VIV_TIER2`, and it arrives together
with the *other half* of the clone row, because a shell needs both or neither.

### The clone row's stated reason had expired

L-3d refused `clone(SIGCHLD, 0)` — a plain `fork()` — and said why: the child
would need a private copy-on-write address space, which did not exist. L-4 and
L-5 built one. The refusal kept passing anyway, because **a decline is also what
a domain that simply never widened produces**, so nothing in the tree could tell
"still correct" from "now stale". The leg's own comment named the chunks that
would discharge it; it had to be looked for rather than waited for.

So `vivarium_clone_decide` gains a second exact word, `VIV_CLONE_FLAGS_FORK`,
and a `bool *share_mem_out` telling the shell which of `RFPROC` /
`RFPROC|RFMEM` to pass. Two exact words rather than a relaxed mask, because the
shapes differ in more than a bit:

| | vfork shape | fork shape |
|---|---|---|
| flags | `CLONE_VM\|CLONE_VFORK\|SIGCHLD` | `SIGCHLD` |
| `stack == 0` | **refused** — two Procs pushing on one stack | **normal** — means INHERIT |
| address space | shared, parent suspends | private, copy-on-write |

The `stack` rule *inverts*, and that is the kernel's contract rather than a
choice made here: `SYS_RFORK` refuses a zero `child_sp` under RFMEM and treats
it as INHERIT under RFPROC alone. A single shared rule would have to be wrong
for one of them.

### execve translates the argument shape

Linux passes `char *const argv[]`; `SYS_EXECVE` takes one concatenated blob.
The walk has to build that blob in *kernel* memory, so there is no user VA to
hand the native handler — which is why `sys_execve_core` was extracted. See
`147-execve.md` "Two front ends, one core" for the split, the double free it
briefly introduced, and the envp measurement.

`envp` DECLINED when non-empty at L-6a. It was not a stub: `exec_build_init_stack`
wrote a lone NULL for envp in both frame shapes, so **no process on this system
had ever had a POSIX environment on its stack** — `/env` was the only channel and
only the Go fork read it. An empty envp was served exactly; a non-empty one was
refused, which made the decline a detector for whether the arc gate needed the
`/env` -> envp projection. **The detector fired at #151 and #140 built the
projection**, so the decline is gone: envp rides the same `viv_pack_strv` walk as
argv, and over-length now answers `T_E_2BIG`.

### Coverage, and the two probes that map the blindness

L156/L156b are the fork (a real one, returning twice, plus the exactness of the
fork word). L164–L169 are execve, and they are all **failing** shapes on
purpose: a successful execve replaces the probe, which could then never report.
That is not a coverage hole — a *failing* execve exercises the entire argv walk,
and the errno discriminates. Reaching the resolve at all (ENOENT) means the walk
measured every string, built the blob, and passed the core's NUL-count-vs-argc
self-check; a builder bug answers EINVAL from that check instead. So ENOENT is a
positive statement about the blob, not merely "it failed".

Two revert probes, and they are complementary rather than redundant:

- Dropping the **fork admission** fails `vivarium.clone_domain` at its own
  assertion. The decision is pure, so the unit test sees it.
- Disabling the **envp gate** leaves the unit suite at a full **1300/1300 PASS**
  and fails only in-guest, at exactly `marker=L166`. The decision lives in the
  shell, and no pure test can reach it.

Together they say precisely which layer sees what — the same split the arc has
now measured five times.

---

## The `wait4` row (LINEAGE L-6b)

`wait4` is the row that lets a guest **reap** what L-6a let it create, and it is
the last one `/bin/sh` needs. It builds no machinery: `wait_pid_for` (PTY-1e) is
already a POSIX `waitpid`, with Linux's own pid selectors, its non-blocking flag
and its stop/continue reports. What it builds is a **map**.

### The option words look interchangeable and are not

Measured from `third_party/musl/include/sys/wait.h`:

| flag | Linux | Thylacine |
|---|---|---|
| `WNOHANG` | 1 | 1 |
| `WUNTRACED` / `WSTOPPED` | 2 | 2 |
| `WCONTINUED` | 8 | 4 |
| `WEXITED` (waitid's) | 4 | — |

The first two agree **by coincidence**, which is the trap: two thirds of the
word passing through unchanged is exactly what makes a passthrough look correct.
The third disagrees, and the value it vacates is **occupied** — Linux's
`WEXITED` is numerically Thylacine's `WAIT_CONTINUED`.

So a passthrough is wrong in both directions at once. A guest asking for
`WCONTINUED` sets a bit the native handler rejects as unknown, so its wait fails
outright. A guest passing `WEXITED` — waitid's flag, but defined in the same
header a guest includes — is silently opted into continue-reports *and* into the
packed status encoding, with no error anywhere. Neither outcome is a decline;
both are answers that look plausible. That is the whole reason this is a
translator rather than a renumber, and why the admitted set is an allow-list
rather than a mask-and-proceed.

### The status encoding is already Linux's, but applied conditionally

PTY-1e built `WAIT_STATUS_*` as "the Linux wait(2) layout so the Pouch
boundary-line maps 1:1", and it checks out against musl's accessors —
`WEXITSTATUS`, `WIFEXITED`, `WIFCONTINUED` and even the awkward `WIFSTOPPED`
all read `WAIT_STATUS_STOPPED` (0x147f) correctly, recovering signal 20.

The catch is that `wait_pid_for` applies that encoding **only** when a PTY-1e
flag was passed, returning the RAW exit status otherwise so every pre-PTY caller
is unaffected. Linux always wants packed. So the translator packs exactly when
the kernel did not — and it cannot work that out by looking at the answer,
because a raw exit status of 5247 and a packed `WAIT_STATUS_STOPPED` are both
`0x147f`. It has to know what it **asked** for, so `kernel_packs` is derived
from the native flag word one line after that word is built, and before the call.

### The pure layer hands back a description, not a flag word

The obvious shape would be to return the native `WAIT_*` word directly. That
would drag `proc.h` into `vivarium.c` — the same import the `clone` row already
refused for `RFPROC`/`RFMEM`. The pure layer says what was asked in Linux's
terms; the shell, the one place that sees both ABIs, translates.

The split also puts the risk in the right half. The dangerous direction is Linux
bit 4 quietly becoming `WAIT_CONTINUED`, and that is decided in the pure layer,
by an allow-list a unit test pins with no kernel plumbing at all. What remains
for the shell is `.continued -> WAIT_CONTINUED`: one assignment, sitting directly
above the `kernel_packs` derivation it has to agree with.

### `-1` becomes `ECHILD`, and it covers two conditions

`wait_pid_for` answers `-1` both for "no matching child" and for a #811
death-interrupted sleep. Mapping both to `ECHILD` is exact rather than lossy:
the death path returns through the sync-from-EL0 tail, where
`el0_return_die_check` is **noreturn** on the die branch, so a group-terminating
Thread never carries a value back to EL0. There is no observer that could tell
them apart.

`T_E_CHILD` (10) was appended to the registry under signoff for this line. A
near-miss will not do — ECHILD is the *termination condition of every reap
loop*, and a bare `-1` reaches a Linux libc as `EPERM`, which is the #100 class
of wrong answer: an errno that means something else entirely rather than
something vaguer.

`rusage` declines when non-NULL. Filling it would mean inventing figures the
kernel does not collect per child; zeroing it would be a stored lie about a
child that used no CPU. musl's `waitpid` and `wait` pass a literal 0, so only a
deliberate `wait4(..., &ru)` is turned away.

### Coverage, and what the reap finally makes assertable

L-6a's fork leg carried a stated gap: with a private address space the child had
no channel back, so "the child RAN" could not be asserted. L170 asserts it now —
a by-pid blocking reap returning that pid means the child ran the frame L-3b
copied for it, to completion. L170c adds **COW privacy**: the child writes a
witness before exiting, the reap orders that write before the parent's read, and
the parent's copy must be untouched.

L173 is the packing proof, and it needs a child that exits **non-zero**: 0 packs
to 0, so a zero-exit leg cannot distinguish packed from raw. Raw 1 fails
`WIFEXITED` outright — `(1 & 0x7f) != 0` reads as "killed by signal 1" — while
packed 1 is 0x100. (`WEXITSTATUS` is 1 rather than a richer code because the
exit status is boolean at v1.0; `sys_exits_handler` collapses every non-zero to
"fail", task #91.)

Two revert probes, opposite directions:

- Dropping the **allow-list** fails `vivarium.wait4_domain` at its own
  assertion, and the guest is never reached. The decision is pure.
- Removing the **conditional pack** leaves the unit suite at a full
  **1301/1301 PASS** — through a bug every Linux guest would see — and fails
  only in-guest, at exactly `marker=L173`. The decision lives in the shell.

**Not proven in-guest, and named rather than left silent**: the `WNOHANG`
"alive but nothing to report" return of 0. It needs a child reliably
alive-and-not-yet-exited at a chosen instant, which needs a synchronisation
channel this phenotype does not have — `pipe2` is not a row, and a private
address space rules out shared memory. Timing a loop would be a flake in a
boot-fatal probe. L-6c's shell exercises it naturally.

## The startup batch (#150, LINEAGE L-6c)

The set a real Linux binary issues between `_start` and its first useful
instruction. Every row here was **measured, not guessed**: it is exactly the
census `viv_report_unserved` printed the moment #149's loader fix let Alpine's
busybox execute, minus the two numbers that are declined on purpose.

| Linux nr | call | tier | why |
|---|---|---|---|
| 172 | `getpid` | **T1** | 0 args, pid return, no error path |
| 66 | `writev` | T2 | the arity rule's sharpest case (below) |
| 17 | `getcwd` | T2 | the return is off by one, the error is ERANGE |
| 173 | `getppid` | T2 | no native twin exists |
| 174 | `getuid` | T2 | the sentinel mapping |
| 176 | `getgid` | T2 | the sentinel mapping |
| 160 | `uname` | T2 | a fabrication, not a translation |
| 96 | `set_tid_address` | T2 | an errno translation, only |
| 146 | `setuid` | T2 | EPERM, except the no-op |
| 144 | `setgid` | T2 | EPERM, except the no-op |
| 25 | `fcntl` | **ENOSYS** | needs close-on-exec (task #151) |

Unserved numbers went **13 -> 3**: `fcntl`, plus `brk` (214) and `mprotect`
(226), which were already declined by policy and remain so.

### `writev` — why registers lining up is not arguments meaning the same thing

`writev(fd, iov, iovcnt)` and `SYS_WRITE(fd, buf, len)` take three arguments
each, in the same registers, with the same first one. A renumber compiles, runs,
and is catastrophically wrong: argument 1 is a **pointer to an array of
pointers**, not a buffer, and argument 2 is an **entry count**, not a byte
length. The kernel would write `iovcnt` bytes *of the iovec array itself* — the
guest's own pointers — to the fd.

This is the clearest instance of the rule §4 states, and the test asserts it by
name so a future "simplification" into the renumber table fails rather than
passes.

The shell **loops the existing byte-I/O core** rather than growing a vectored
one. Each entry goes through `sys_write_handler`, which is the whole audited
staging path (the weft fast-path, the CF-3 two-tier bounce, the `SYS_RW_MAX`
clamp, the #100 errno translation), so the translator adds a decode and nothing
else.

Three properties are worth stating because each was a decision:

- **Two passes, for Linux's *error* semantics rather than for memory safety.**
  Linux validates the whole array up front (`import_iovec`) and answers
  EINVAL/EFAULT having written *nothing*. A single-pass loop that validated
  entry *k* just before writing it would leave entries 0..*k*-1 already written
  when it found a bad one. Safety does not depend on the up-front pass — pass 2
  re-validates every buffer through `sys_write_handler`'s own checks — so the
  re-read between passes is benign: a peer thread rewriting the array yields
  values that are themselves validated before use, and the outcome degrades to
  a short write, which is a legal `writev` result.
- **O(1) storage, deliberately.** `UIO_MAXIOV` is 1024 and an iovec is 16 bytes,
  so buffering the array would want 16 KiB — the entire kernel stack.
- **`iovcnt == 0` is in domain.** Linux resolves the descriptor *before* it looks
  at the array, so `writev(badfd, x, 0)` is EBADF and not 0. The shell issues a
  zero-length write through the same core rather than short-circuiting.

The SSIZE_MAX accumulator tests **room** (`add > max - total`) rather than the
sum, because the naive `total + add > max` wraps and passes; the test pins that
directly.

### The identity mapping is a decision, not a passthrough

Thylacine's TCB identity is `PRINCIPAL_SYSTEM == 0xFFFFFFFE`, and the
container's shell runs as exactly that. Passed through raw, a Linux guest reads
`(uid_t)-2` — which in Linux practice is the historic "nobody" value, i.e. the
number meaning the *least* privileged identity. So the raw pass-through is not
neutral: it **inverts the fact being asked about**, telling a Proc that holds the
system identity that it is nobody. It maps to 0.

Two properties make that safe rather than lucky:

- **It cannot collide.** `PRINCIPAL_INVALID` and `GID_INVALID` are both 0, so no
  real principal or group is ever 0 to begin with; every other value passes
  through unchanged. `PRINCIPAL_NONE` (the genuinely unauthenticated identity) is
  deliberately *not* folded — it really is nobody.
- **It confers nothing.** Every authority decision reads the real `principal_id`
  through `perm_check` or a `CAP_*` gate. A container shell that believes it is
  root will *attempt* privileged operations and be refused at the real gates
  exactly as before. The mapping changes what a guest is **told**, never what it
  may **do** (I-22).

`setuid(getuid())` — the idempotent call every "drop to my own uid" path issues —
succeeds; everything else is EPERM, which is both true here and what Linux tells
an unprivileged process. The comparison is made in the **guest's** number space,
because that is the only value the guest has ever been shown, and a raw
comparison would refuse the call for a `PRINCIPAL_SYSTEM` Proc — the case that
needs it most.

### `uname` — what it claims, and why

A fabrication, so the question is not *how* to translate but *what* to assert. A
wrong answer here is the mistranslation the argument-domain rule exists to
prevent: a guest that believes it is on a kernel it is not will take a code path
we cannot serve.

- `sysname` = **"Linux"**. The truthful answer *within* the phenotype — the ABI
  the guest sees IS Linux's. "Thylacine" would send every `uname -s` check down
  an unknown-OS path with no Thylacine support behind it.
- `release` = **"4.4.0"**. No number is honest, because there is no Linux whose
  syscall surface matches ours. The choice is which direction to be wrong in, and
  **low is safer** — a guest that assumes little uses the oldest paths, which are
  the ones translated here. 4.4 is *the newest kernel that promises nothing we
  lack*: below `statx` (4.11, which we FORWARD), `io_uring` (5.1), `clone3`
  (5.3), `openat2` (5.6), `faccessat2` (5.8) and `close_range` (5.9). It also
  clears glibc's 3.2 floor, under which a glibc binary aborts with "FATAL: kernel
  too old" before `main()`.
- `version` = **"#1 Thylacine VIVARIUM"**, on purpose. Programs essentially never
  parse the build banner, so it is where `uname -a` can tell the truth without
  any version check tripping over it — §9's DEGRADED tier applied *inside a
  single struct*: compatible where a field is load-bearing, truthful where it is
  observable.
- `machine` = "aarch64" (simply true), `nodename` = "thylacine" (no hostname
  concept exists), `domainname` = "(none)" (Linux's own default).

The 390-byte struct is zeroed wholesale before filling, which is an **I-13
obligation** and not tidiness: all 390 bytes are copied to EL0, so every byte
past each terminator must be a defined 0 rather than whatever the kernel stack
held. The test pre-poisons the struct with `0xAA` first — over a zeroed stack the
assertion would pass whether or not the fill zeroed anything.

### The four rows below the native ceiling

`getcwd` (17), `fcntl` (25), `writev` (66) and `set_tid_address` (96) sit below
`VIV_NATIVE_CEILING`, so each owes the per-number collision paragraph the
`pselect6`/`ppoll` block mandates. The first half is shared — a PHENO_LINUX Proc
cannot reach a native number at all, so it never had the native call to lose. The
second half, what a *mis-declared native* Proc now reaches, is per number and is
written out at the enum in `vivarium.h`. In every case the worst outcome is
EFAULT, EBADF, or a write of the caller's own bytes to the caller's own fd. Never
authority.

### `fcntl` — ENOSYS by measurement, and the scripture fact it voided

Instrumenting the row and running the gate showed Alpine busybox's `/bin/sh`
issues `fcntl` exactly twice at startup, and both are the same family:

```
cmd 0x2   = F_SETFD,          arg 1  = FD_CLOEXEC
cmd 0x406 = F_DUPFD_CLOEXEC,  arg 10 = ash's savefd(), moving the script fd above 10
```

> **SUPERSEDED BY #151** (next section): close-on-exec now exists and both cmds
> are served. The paragraph below is left as written because it records what
> #150 MEASURED and why it declined; only its present tense is stale.

Neither could be served truthfully at #150, because **Thylacine had no
close-on-exec at all** — verified on both halves: no CLOEXEC bit exists in `handle.h` or
`handle.c`, and `proc_exec_replace` did not touch the handle table, so exec
preserved every fd. `F_SETFD(FD_CLOEXEC)` could only be answered by silently
succeeding, after which the guest execs with the fd still open having been told
otherwise. `F_DUPFD_CLOEXEC` is that lie plus a real dup, and it cannot even be a
renumber onto `SYS_DUP` — *that* call's second argument is a rights mask, not a
minimum fd, so the arity rule refuses it for the same reason it refuses `writev`.

That is a **kernel feature, not a translation** (task #151), and it is what
blocked the L-6c gate until #151 built it.

The measurement also **voided a stated scripture fact**. VIVARIUM.md's
ignorable-flags table admits `O_CLOEXEC`, and its justification was never
"harmless" — it was the stronger and correct claim that *there is nothing to opt
out of*, because the only way to start a program was `SYS_SPAWN_*`, which endows
an **explicit** fd list. LINEAGE voided that one commit at a time: L-2a's
`execve` replaces the image in a live Proc and leaves the table untouched, and
L-3c-1 gave `rfork` a **copy** of the parent's table. Together they are POSIX
fork+exec, under which every fd survives exec. `vivarium.h:878` even says
"CLOEXEC needs an exec that preserves fds" — and that exec now exists.

Neither commit had any reason to look at an openat flag table. **A round is
scoped to one commit, so a premise a *later* commit voids is invisible to it**;
the defence is re-checking a load-bearing fact when the thing it rests on moves,
not a better review of either commit.

### Coverage

Four tests, one per **obligation** rather than one per syscall:
`vivarium.startup_batch_rows` (the table classification and *why* each row sits
where it does), `vivarium.writev_domain` (the argument domain and the overflow
rule), `vivarium.uname_fill` (the fabricated content plus the I-13 zero-fill) and
`vivarium.identity_map` (the sentinel mapping and the setid no-op).

Revert-probed **four ways in one build**, and the discrimination is the point:
writev-as-T1, a naive wrapping overflow test, a dropped uname zero-fill and an
identity uid map each failed at their **own** named assertion (1306/1310, no
cross-talk).

One more blindness worth recording, because it cut both ways in the same
session. The unit suite is boot-fatal, so when the temporary fcntl
instrumentation contradicted `startup_batch_rows`, the suite failed and **the
container never ran** — the guest could not have reported anything. Yet the guest
is the *only* thing that knows which `fcntl` cmds busybox actually issues. Both
legs, or neither proves anything.

---

## Close-on-exec (#151, LINEAGE L-6c)

The `fcntl` section above closed with "that is a kernel feature, not a
translation, and it is what now blocks the L-6c gate." #151 built the feature and
then served the row, in that order. The order is the whole content of the chunk:
serving `F_SETFD(FD_CLOEXEC)` by storing nothing would have passed the gate and
left a known lie in the tree.

### Where the flag lives, and why not in `struct Handle`

`struct HandleTable` gained `u64 cloexec[HANDLE_CLOEXEC_WORDS]` — a bitmap
parallel to the slot array. Both halves of that are deliberate.

**Parallel**, because POSIX close-on-exec is a property of the **descriptor**,
not of the open file description behind it. `dup(fd)` yields a second descriptor
onto the same description with the flag *clear*, and `F_SETFD` on either does not
touch the other. A bit stored in `struct Handle` would be shared by exactly the
two things POSIX requires to differ. Linux keeps `close_on_exec` as a bitmap
beside `fd[]` in `struct fdtable` for this reason; the shape is not a
coincidence.

**Not a field**, also because `struct Handle` has no slack: `8 + 4 + 4 + 8` is
exactly the 24 its `_Static_assert` pins. A `u32` there grows it to 32, taking the
table from 6152 to 8200 bytes — across the 2-page boundary into 3, a 50%
per-Proc increase to carry one bit per slot. The bitmap costs 32 bytes total.

### Established, never inherited

The bit is written in `handle_install_locked`, the single point every fd-creating
path in the kernel goes through (`handle_alloc` and both dup forms). Writing it
there is what makes a **reused index** unable to carry the previous occupant's
flag — the reused-identity class this tree keeps meeting, most recently L1f's F1
(a reused inode serving a prior occupant's cached page) and net-3d's slot
re-mint. It is also *mandatory* rather than defensive: it is how
`handle_dup_posix` delivers `F_DUPFD_CLOEXEC`'s requested value.

`handle_close` clears the bit as well, and **the revert probes measured that the
two overlap completely**: removing either one alone leaves the whole suite green,
and only removing *both* trips `handles.cloexec_lifecycle`. So the
no-inheritance property is doubly held and no test can attribute it to a site.

That is worth stating rather than leaving as an implication that both are
load-bearing. The close-side clear is **hygiene** — it keeps "bit set ⇒ slot
live" true, an invariant nothing currently reads — and is the one that could be
dropped; the install-side write cannot be, for the reason above. A redundancy
noticed only when a sabotage refuses to fire is a redundancy that would otherwise
have been re-derived wrongly by whoever touched it next.

Clearing at install also gives POSIX `dup` its required semantics for free — the
new descriptor is not close-on-exec — without `handle_dup` having to say so.

The four lifecycle sites, stated as a set because a missing one is silent:

| site | behaviour | why |
|---|---|---|
| `handle_install_locked` | clears (or sets, per the caller's argument) | establish-never-inherit; the single chokepoint |
| `handle_close` | clears | the flag dies with the descriptor |
| `handle_table_copy_into` (fork) | **copies** | POSIX: fork preserves. A shell sets it once and forks per command |
| `handle_replace` | **leaves alone** | the fd number persists, so the fd's flag persists — a `SOCK_CLOEXEC` socket stays close-on-exec across `connect` |

### The sweep runs after the commit point

`handle_close_on_exec` is called from `sys_execve_core` immediately after
`proc_exec_replace`, and both halves of that placement are forced:

- The closes may **sleep** (a Spoor's Dev close hook sends a 9P `Tclunk`), so
  this cannot run under any lock. The `spoor_clunk` directly above it already
  establishes that sleeping is legal at this point.
- A **failed exec must leave the process unchanged**, so nothing that closes the
  caller's fds may run before the last thing that can fail. Linux places
  `do_close_on_exec()` after its own point of no return for the same reason.

It is before the trapframe rewrite, so no instruction of the new image can
observe an fd that was supposed to be gone.

The sweep snapshots and clears the bitmap in one lock hold, then closes outside
it. The snapshot cannot go stale in the only caller that exists — `execve`
refuses unless this is the Proc's sole live thread — but the clear-first form
does not *depend* on that, which is why it is written that way.

**Native behaviour is byte-unchanged**, and that falls out rather than being
arranged: nothing outside the phenotype can set the flag, so a native Proc's
bitmap is empty and the sweep closes nothing.

### The `fcntl` row, and the served set

`fcntl` is a **multiplexer** — the shape Thylacine's native ABI refuses and a
Linux phenotype has to speak, which is why it lives in the vivarium and has no
native counterpart.

| cmd | served | note |
|---|---|---|
| `F_GETFD` / `F_SETFD` | yes | `F_SETFD` **masks** `arg & FD_CLOEXEC` rather than validating — Linux ignores every other bit, and being *stricter* than Linux for an input a guest may legally send is its own mistranslation |
| `F_DUPFD` / `F_DUPFD_CLOEXEC` | yes | `handle_dup_posix` — rights verbatim, lowest free slot ≥ `arg`; closed fd → `EBADF`, table full → `EMFILE`, `arg` ≥ the table → `EINVAL` |
| everything else | `ENOSYS` | not Linux's `EINVAL`: that claims the cmd is not a valid fcntl operation, which for `F_GETFL` or `F_SETLK` is false. `ENOSYS` says the surface is absent, which is true |

`F_GETFD` and `F_DUPFD` join the two measured cmds because each is the exact
inverse or sibling of one of them, differing by a line; serving one of a pair and
declining the other is an arbitrary edge for a guest to discover at runtime.

**The minimum-fd argument is why `handle_dup_posix` exists.** A shell's
`savefd()` does `F_DUPFD_CLOEXEC(fd, 10)` precisely to move its bookkeeping fd
out of the low range a user redirection could collide with. Returning the first
free slot regardless would hand back fd 3 and break the guarantee the call was
made to obtain — silently, and only under a redirection.

**The errnos are load-bearing too, and were wrong until the c8ab2744 close.**
`handle_dup_posix` folds "no such fd" and "table full" into one -1, and the arm
answered `EMFILE` for both — on the argument that a guest which just used the
fd knows it exists. That is backwards for the one shell that matters: busybox
ash's `redirect()` probes the TARGET fd of every `N>&M` with
`fcntl(N, F_DUPFD, 10)` precisely to learn whether N is open (`EBADF` — not
open, nothing to save; anything else — "strange", and the whole command is
aborted with `fcntl(N,F_DUPFD,10): <strerror>`). fd 3 is not open in a script
shell, so every `3>&1` died — measured on the L-6c gate the moment a leg used
one: the command substitution around it yielded "" and the two legs asserting an
EMPTY stderr capture (`L6C-J`, `L6C-L`) passed *vacuously*; only the positive
control (`L6C-K`) said no. The arm now re-checks liveness after a failed dup
(the same lookup the `F_GETFD` arm uses): closed fd → `EBADF`; the residual
(table full, a non-dup-able kind, a rights failure) → `EMFILE`. POSIX says
exactly that. Regression `vivarium.fcntl_dupfd_errnos` (through
`viv_fcntl_for_test`, the real T2 arm on a fresh Proc): control (a live fd dups
at ≥ 10), a closed fd → `EBADF` for both spellings, a live fd with a FULL table
→ `EMFILE` (proved full two ways first — so the two errnos are distinct and a
fix that always said `EBADF` fails there), a minimum at the table size →
`EINVAL`.

### `O_CLOEXEC` is now honoured for real

`vivarium_openat_decide` gained a third output, `cloexec_out`, and the shell
applies it after `sys_open_handler` succeeds. It is a separate output rather than
a bit folded into the omode because it is **not part of the open at all** — it
names the resulting descriptor.

It is set on every translated path including `O_PATH`: Linux honours `O_CLOEXEC`
on an `O_PATH` open (one of the three flags `O_PATH` does not ignore), and an
`O_PATH` open produces a descriptor like any other.

### Coverage

Five kernel tests, one per obligation: `handles.cloexec_lifecycle` (set/get, the
free-slot refusal, and the reused-index property), `handles.cloexec_exec_sweep`
(exactly the flagged slots close, the *survivors* are checked by name, and a
second sweep is a no-op), `handles.cloexec_fork_preserves` (fork carries it, then
the child's exec consumes it — the pair together), `handles.dup_posix` (the
minimum, the verbatim rights, and both flag polarities) and
`vivarium.fcntl_domain` (the classifier, with the two measured cmds written as
their raw wire values `0x2` and `0x406` so a mistyped constant cannot agree with
itself).

**Plus one in-guest leg, and it is the one that matters most**: L177–L179 in
`viv-pheno-probe`, which forks, dups three descriptors to *fixed* numbers (20
plain, 21 `F_DUPFD_CLOEXEC`, 22 plain), and re-execs the probe in a third mode
that reports whether 21 died and 22 lived. Fixed numbers because the re-execed
image has no memory of what the pre-exec one chose — which makes `F_DUPFD`'s
minimum load-bearing in the leg as well as in the shell it was built for. It
reports through fd 20, i.e. through the mechanism under test in the direction
that must *not* fire.

**The kernel tests are blind to the wiring, and the probes measured exactly how
blind.** Deleting `handle_close_on_exec(p)` from `sys_execve_core` leaves the
unit suite at a full **1315/1315** while the in-guest leg fails at precisely
`marker=L180`. The other five sabotages (writev-style: install-does-not-clear,
sweep-closes-everything, fork-drops-the-flag, dup-ignores-the-minimum,
`F_SETFD`-rejects-stray-bits) go the other way — four fail at their own named
assertions in one build, and the fifth is the redundancy finding above.

### What the gate did next, and the blocker it revealed

Serving `fcntl` moved the gate from *"busybox speaks"* to **"busybox runs a
script"** — `L6C-A-shell-runs` now fires and the shell executes `/gate/run.sh`.
It then fails at `L6C-B-external-exec`, spawning an external command.

Measured rather than inferred: instrumenting `viv_execve`'s envp arm shows the
guest passing `envc=2` with `env0='SHLVL=1'`. **Busybox ash synthesizes `SHLVL`
and `PWD` itself**, so its envp is non-empty even starting from an empty
environment — there is no container configuration that avoids the arm. The
blocker is therefore **task #140** (no process has a POSIX environment on its
stack), and it wants a change to `exec_build_init_stack`, not to the phenotype.

The joey gate message was updated to name it. A `KNOWN-BLOCKED` that outlives its
blocker is just a disabled test, and one that names the *wrong* blocker is worse
— it sends the next reader at the thing that was already fixed.

---

## `pipe2` (#155, LINEAGE L-6c)

A shell cannot build a pipeline without it, and on aarch64 there is no second way
to ask. Linux's generic syscall table carries no legacy `pipe` — the arch header
defines only `__NR_pipe2 59`, where x86_64 carries both 22 and 293 — so musl's
`pipe()` compiles to *this* number with flags 0. The architecture had to be
checked rather than inherited from another arch's table.

### The domain was measured, not derived from what Linux permits

Linux's `pipe2` accepts `O_CLOEXEC`, `O_NONBLOCK` and `O_DIRECT`. The question a
translator has to answer is narrower: which of those does *this guest* send, and
which can the native mechanism reproduce **exactly**?

Six call sites in the gate's own busybox reach nr 59, and the binary answers both
halves at once:

| site | shape | flags |
|---|---|---|
| ×4 | musl `pipe()` — `mov x1, #0` | `0` |
| ×2 | musl `pipe2()` — `mov w1, #0x80000` | `O_CLOEXEC` |

So `{0, O_CLOEXEC}` is not a conservative subset of the domain. It **is** the
domain, and both members are exactly reproducible: `0` is `SYS_PIPE` unchanged,
and `O_CLOEXEC` is the descriptor flag #151 built — applied to *both* ends after
creation, since pipe2's flag is not per-end and flagging only the read end would
still satisfy any check that looked at one fd.

### An allow-list, for V-2d's reason

The gate is written as *nothing outside the admitted set*, not as a list of
rejects. `mmap` recorded why at V-2d: aarch64 defines flags a deny-list admits by
having forgotten them. Here the point is concrete — the unit test's bare `1u` case
is a bit no pipe2 flag uses, and a deny-list serves it silently.

`O_NONBLOCK` and `O_DIRECT` are excluded because devpipe has no non-blocking read
and no packet framing — not because they are unusual. Admitting either would hand
a guest a pipe and tell it something false about it.

### Declining `O_CLOEXEC` would also have worked

Worth recording, because the reasoning inverted once while checking. musl's
`pipe2` carries its own ENOSYS fallback — `pipe()` then `fcntl(F_SETFD)` — and
since #151 made `fcntl` a served row, that fallback now runs *correctly* rather
than silently dropping the flag as it would have one chunk earlier.

So declining is not unsafe. It is merely worse: three syscalls instead of one, and
it leans on a compat shim belonging to one libc. A statically-linked Go binary
calling `pipe2` directly has no such fallback. Serving what can be served exactly
is the answer that does not depend on who is asking.

### The shell's order, and the one step that cannot be skipped

Decide → range-check → create → copy out → *clean up on failure*.

Both refusals precede the allocation deliberately: a call that was never going to
land its result should not cost a pipe ring and two descriptors first.

The cleanup is the step that looks redundant and is not. `sys_validate_user_buf`
proves only that the VA lies in the uaccess band; the page can still be absent,
read-only, or unmapped by a peer thread between the check and the store. Linux has
the identical window — `do_pipe2` creates, copies, and closes both fds on failure
— and without it an EFAULT leaves two live descriptors whose numbers the guest was
never told, which is an unreachable leak for the life of the Proc.

### The collision paragraph

59 is below the native ceiling, so it owes one. The native occupant is
`SYS_WSTAT(fd, valid, mode, uid, gid, size)`, and a native program mis-declared as
`PHENO_LINUX` is refused twice over:

- `valid` is a bitmask of `T_WSTAT_{MODE,UID,GID,SIZE}` == `0xF`, so every legal
  wstat mask lands in `[1,15]`, and the domain admits only `{0, 0x80000}`. No
  wstat a native caller can legally make gets past the decide.
- Were a garbage `valid` of 0 to slip through, `args[0]` — the fd index wstat put
  there — becomes the VA the pair is written to. That is page zero, so the
  copy-out faults, both fds are closed, and the answer is EFAULT.

Its own address space, its own two descriptors, never authority.

### Coverage, and what each layer cannot see

`vivarium.pipe2_domain` proves the *decision*; in-guest legs L187–L192 prove the
*shell* — the `int[2]` reaching guest memory, a byte actually round-tripping
through the pair, `O_CLOEXEC` on both ends read back via `F_GETFD`, the
`O_NONBLOCK` decline, EFAULT on an unmapped-but-in-band VA, and the leak
assertion that last one sets up: 200 failing calls burn 400 descriptors against a
256-slot table, so a shell that does not clean up cannot then make one more pipe.

**Two revert probes, blind to each other in opposite directions.** Removing the
cleanup leaves the unit suite at a full **1319/1319** and fails only in-guest at
`L192`. Turning the allow-list into a deny-list fails `vivarium.pipe2_domain` at
**1318/1319** on the bare `1u` case, and the guest legs do not notice — L190 tests
`O_NONBLOCK`, which a deny-list still catches.

### What the gate did next, and why the next blocker was already known

The log is unambiguous about how far it moved: **`pipe-in` is printed**. The
pipeline's left side runs and its bytes reach the pipe. What fails is the right
side, and busybox names the call itself:

```
/gate/run.sh: line 6: dup2(4,1): Function not implemented
```

On aarch64 `dup2` compiles to `dup3`, which is a deliberate `FORWARD`: it *frees*
the fd it overwrites, and `kernel/vivarium.c`'s fd-freeing block carries a
standing instruction that the socktab close hook be extended in the same commit
that serves it. pipe2 needed no such extension because it is fd-**creating** — it
is in `openat`'s class — and that distinction is what the two adjacent tests now
state together.

That is task **#157**, and it was measured *before* this row was written: the four
`dup3` call sites and their flags were read off the same binary in the same pass,
precisely so that fixing site N could not stop the question being asked about site
N+1.

## dup3 (#157, LINEAGE L-6c) -- and the arc gate goes green

The last blocker, and the one #155 had already measured. On aarch64 there is no
`dup2` syscall number at all, so musl's `dup2.c` compiles `dup2(old,new)` into
`__syscall(SYS_dup3, old, new, 0)`; a shell's redirection plumbing has no other
route to a pipeline.

**The measured domain.** Four call sites reach nr 24 in the gate's own busybox:

| site | how | flags |
|---|---|---|
| `0x4d3ca0` | musl `dup2()` | `mov x2, #0` -> 0 |
| `0x4d3d0c` | musl `__dup3()` | `sxtw x2, w2` -> the caller's |
| `0x4db7d8` | busybox-internal | `mov x2, #0` -> 0 |
| `0x4db8cc` | busybox-internal | `mov x2, #0` -> 0 |

**The domain is COMPLETE, not a subset, and that decides the errno.** Linux's own
`ksys_dup3` refuses everything outside `{0, O_CLOEXEC}` with `EINVAL`. So unlike
`pipe2` -- whose excluded flags (`O_DIRECT`, `O_NONBLOCK`) are ones Linux serves
and devpipe cannot represent -- this allow-list is *equal* to Linux's, and a
refused flags word is us reproducing Linux rather than declining to serve it.
The shell therefore answers **EINVAL, not the ENOSYS decline**: claiming the
surface is absent would be false, and V-2d's `munmap` note drew the same line for
`len == 0` and a misaligned address.

**Two facts read out of musl's `dup2.c`, both load-bearing.** `old == new` never
arrives here from `dup2` -- musl short-circuits it through `fcntl(old, F_GETFD)`
-- which is the only thing that shows #151's `fcntl` row is load-bearing for
`dup2(x,x)`. And musl retries on `-EBUSY` in a bare `while` loop on both paths,
so this row must never produce that errno; a guest would spin forever.

**`handle_dup_to`, a genuinely new primitive.** The three neighbours each fix a
different one of the two axes -- where the source comes from, and whether the
destination may be occupied:

| primitive | source | destination |
|---|---|---|
| `handle_dup_posix` | a slot | first free >= min (an OUTPUT) |
| `handle_replace` | a caller-held object | a fixed index, must be LIVE |
| `handle_table_copy_into` | a slot in another table | the same index, must be FREE |
| **`handle_dup_to`** | a slot | **a fixed index, free OR live** |

It is deliberately not close-then-dup: the freed index is not reserved, so a
peer's fd-creating syscall can take it in between -- the argument
`handle_replace`'s header already records for `connect`. Rights carry VERBATIM
(POSIX; I-6 by non-increasing). Close-on-exec is SET FROM THE FLAG, never
inherited from the source (POSIX clears it, and inheriting is the classic dup2
mistake) nor from the slot's previous occupant (the reused-identity failure).
The SOURCE passes `handle_slot_may_alias`, the same predicate `handle_dup` and
the fork copy use; the DESTINATION is ungated by kind, because it is being
CLOSED and `handle_close` places no kind restriction on closing.

**The socket case declines, with the alternatives written down.** See VIVARIUM
section 9's DEGRADED tier for the full statement; in short, an unrefcounted
fd-keyed socktab cannot give two descriptors one socket's state, and both
alternatives are actively wrong rather than merely imperfect.

**The fd-freeing drop is paid in a different arm from `close`'s.** `close` pays
in the entry hook, sound there only because a close whose fd carries an entry
always proceeds. `dup3` pays inside its shell, after every refusal -- a dup3 can
be refused while `new` is a live socket, and an entry-time drop would destroy
socket state on a failed call. It also drops AFTER the install, the opposite of
`viv_sock_accept`'s unwind, whose reason does not transfer: there the number is
freed and reallocatable, here it never is.

**Coverage.** `vivarium.dup3_domain` (the decision) + `handles.dup_to` (the
primitive) + the re-stated `fd_freeing_rows_stay_unserved`, which now asserts the
TIERS rather than merely "not served", because the tier is what decides where a
row pays its drop + in-guest legs L193-L199. Revert-probed in opposite
directions: removing the socktab drop leaves the unit suite at a full
**1321/1321** and fails only in-guest at `marker=L199`; a deny-list fails
`vivarium.dup3_domain` at **1320/1321** on the bare `1u`, invisible to the guest.

**The arc gate passes** -- `L6C-A` through `L6C-I` -- and `L6C_GATE_FATAL` flips
to 1 in the same commit, because a gate that cannot redden is a disabled test.

---

## DISTRO D-5 -- the stock rootfs bundle and THE ARC GATE

### What the L-6c gate could not prove

The L-6c bundle at `/vivarium/alpine` substitutes a busybox-**static** binary we
supply over `/bin/sh` and `/bin/busybox`. That substitution was correct when it
was written -- every ELF in the stock minirootfs is an `ET_DYN` PIE linked
against `/lib/ld-musl-aarch64.so.1`, and the loader accepted neither `ET_DYN`
(until D-2) nor `PT_INTERP` (until D-4) -- but it means the one file every
`L6C-*` leg runs through is ours. Whatever those nine legs prove about
fork/exec/pipe/substitute/reap, they cannot prove that a stock distro runs.

D-5 closes that with a SECOND bundle, `/vivarium/alpine-stock`, staged from the
same tarball with nothing replaced.

### Why a second bundle rather than flipping the first

Flipping `/vivarium/alpine` to the stock dynamic shell would put all nine `L6C-*`
legs plus `D2-*`/`D3-*`/`D4-*` behind D-4, so any regression in the interpreter
dispatch would present as "the shell did not run" with no first-missing signal.
The split is not caution, it is the discrimination: a red stock gate beside a
green `L6C-A..I` isolates the fault to the stock-dynamic path specifically. It
costs a second 8.1 MiB copy in the pool, and `tools/build.sh` carries the
reasoning at both staging sites.

### "Unmodified", stated precisely

No stock file is replaced, removed, or edited. The only additions are the mount
anchors the recipe structurally requires -- a bind needs an existing mount point
-- which are the `/net` and `/env` directories (`/proc`, `/sys` and `/dev` are
already in the image) and the six `/dev` leaf files. Nothing is written into the
rootfs to carry the gate itself: the script rides in the manifest's
`process.args`, so the staged tree is the tarball plus anchors and nothing else.

Measured composition of `alpine-minirootfs-3.21.0-aarch64.tar.gz` (sha256
`f31202c4…efac1`): 520 entries = 335 symlinks, 88 regular files, 97 directories,
and **zero device nodes**. The arc plan had carried an item to skip device nodes
and document the skip; there are none to skip, so it is recorded as a
measurement instead of performed as a step. `tar -xzf` exits 0 with no
privileged operation.

### The fixture is sha256-pinned, and a mismatch is fatal

Both external inputs are pinned. Discovery deliberately stays a glob: pinning the
filename would turn a wrong-version drop into "no tarball found", a silent
hermetic skip of the arc gate. Glob-plus-hash gives the three outcomes that are
actually wanted -- absent is a skip (the default build stays hermetic),
matching proceeds, and present-but-different is a loud build failure. Fatal is
right because every expected value downstream was derived from those exact
bytes, including the `D2`/`D3`/`D4` output strings and D-5's in-guest
`VERSION_ID` assertion; a different image would quietly move what PASS means.

### The gate: five legs, one new mechanism each

`do_stock_distro_gate` (`usr/joey/joey.c`) spawns `viv run
/vivarium/alpine-stock` and matches markers against the drained container
output. Each leg adds exactly one mechanism to the one before it, so a
first-missing marker names a cause rather than a symptom.

| Leg | Marker | The one new mechanism |
|---|---|---|
| A | `DISTRO-A-stock-sh` | The stock shell starts at all: `/bin/sh` (an absolute POOL symlink, re-anchored at the container root by I-28) -> stock `ET_DYN` PIE busybox -> `PT_INTERP` -> stock ldso -> applet dispatch on `basename(argv[0]) == "sh"`. Printed by a shell BUILTIN, so no second exec stands between the chain and the signal. |
| B | `DISTRO-B-stock-exec` | fork + exec of a stock dynamic binary FROM a stock dynamic parent, in busybox's multiplexer form (`/bin/busybox echo`): a real file, argv[0]-independent, so B is clear of both symlinks and argv[0]. |
| C | `DISTRO-C-applet-by-symlink` | A second absolute pool symlink resolved for EXEC, plus argv[0] applet dispatch: `/bin/cat` (a symlink) reading `/usr/lib/os-release` (a REAL file). |
| D | `DISTRO-D-relative-symlink` | A RELATIVE pool symlink crossing `..` (`/etc/os-release -> ../usr/lib/os-release`), read through B's already-proven multiplexer form so the link is the only new variable. |
| E | `DISTRO-E-pinned-image` | The pool holds the PINNED image, asserted from inside the guest. |
| — | `DISTRO-DONE` | The script reached its end, distinguishing a failed leg from a shell that died mid-script. |

C and D are deliberately independent: C is a symlinked applet on a real target,
D a multiplexer on a symlinked target, so neither can mask the other.

Leg C is the one worth stating carefully, because it is easy to overclaim. It
does **not** discriminate `--argv0` from passing the path alone -- nothing on
this rootfs can produce a vector where those differ, for the reason the `D4-B`
comment gives, and that claim stays at the unit level in
`exec.interp_argv_shape`. What C discriminates is a separate property that
nothing tested before: **symlink resolution must not become visible in
`argv[0]`.** If the kernel ever passed the resolved path, busybox would see
`basename == "busybox"`, take the filename for an applet name, and emit nothing.
Every busybox-based distro depends on this.

### The first run went red at leg C, and the leg was the thing that was wrong

Worth recording in full, because the failure mode is a general one.

The gate's first guest run reported
`first-missing=DISTRO-C-applet-by-symlink status=0` -- A, B, D, E and DONE all
fired, and the shell exited cleanly. The obvious reading was the one the leg was
built to catch: the kernel had leaked the resolved path into `argv[0]`.

The log said otherwise. Because the container's stderr shares the drained pipe,
the actual error was already sitting next to the markers:

```
ls: can't open '/': Function not implemented
vivarium: unserved linux syscall nr=56 (T2 row declined these arguments)
```

busybox printed **`ls:`** -- it had dispatched to the `ls` applet and was naming
itself by applet, which is only possible if `argv[0]` was `/bin/ls`. The
property leg C exists to prove was **satisfied**; the leg simply could not
observe it, because the version of C that shipped ran `/bin/ls /`, and listing a
directory needs enumeration on top of dispatch. `openat` declines `O_DIRECTORY`
by design (`kernel/vivarium.c:470`) and `getdents64` has no row at all, so
nothing here can list a directory (task #209).

So the leg violated the very rule the ladder is built on -- one new mechanism
per leg -- and reddened for a mechanism it was not testing. That is strictly
worse than a leg that fails honestly: it accuses the wrong subsystem. The fix is
to read a REAL file through a SYMLINKED applet (`/bin/cat /usr/lib/os-release`),
which leaves argv[0] dispatch as the only untested variable and, as a bonus,
makes C independent of D.

Two lessons, both already project canon and both re-earned here: a leg's
dependency set must be no larger than its claim; and when a gate goes red, read
what the artifact actually printed before believing the hypothesis the gate was
designed around.

Leg D exists because the loader path cannot cover the relative class at all:
`libc.musl-aarch64.so.1` matches the `"c."` entry of musl's reserved list
(`third_party/musl/ldso/dynlink.c:1074-1082`), so `load_library` short-circuits
it to `&ldso` and never opens the file. The
`/lib/libc.musl-aarch64.so.1 -> ld-musl-aarch64.so.1` link is present in the
image and never followed.

Leg E is the **#126 stale-bake detector**. The bundle is pool-resident, so a
`PRESERVE=1` build silently serves the old rootfs; E is the one assertion here
that a stale bake cannot satisfy. It reads D's capture, so a broken D darkens E
too -- that nesting is by design, and D is the leg to read first.

### No `>` redirection anywhere in the script

Not a style rule. #201: the vivarium's `openat` refuses `O_CREAT`
unconditionally, and a plain `>` passes `O_WRONLY|O_CREAT|O_TRUNC` even onto a
file that already exists. Every assertion is therefore a `$( )` capture (a pipe)
or a `2>&1` dup, the same constraint D-3c's gate line already works under. This
is a real fidelity gap being routed around; #201 remains open.

**The "#201's full fix must re-open #192" clause is RETIRED at D-close
(2026-08-10, the arc round's F1 [P1]).** It rested on #192 being coupled to
"the phenotype cannot create/write files", and the write half was never true:
`VIV_LINUX_WRITE -> SYS_WRITE` is a tier-1 direct row (`vivarium.c:149`) and
`openat O_RDWR` is translated (`vivarium.c:532`). A container can already write
any existing file it has DAC write on, so `write(shellcode)` + `mmap(R+X)` +
jump is a `CAP_JIT`/I-42 bypass **today**, with or without `O_CREAT` — the
create fix was never what would open it. #192 is now tied to per-mount `noexec`
(task #217, user-voted 2026-08-10) rather than to #201, and re-opens at that
chunk or at any change admitting a new `PROT_EXEC` file-mapping shape.
Full analysis: `docs/DISTRO.md` section 6.

### Marker honesty (#186)

No marker can be forged by a diagnostic line. The script's only raw output is
`DISTRO-RAW-OSREL:` followed by the os-release contents, measured to contain
zero `DISTRO-` occurrences; and joey's own failure text -- which does print the
first-missing marker name -- goes to the console, never into the accumulator,
which holds container bytes only.

### Fatality

`DISTRO_GATE_FATAL` is 1 from the start, with no KNOWN-BLOCKED arm. L-6c earned
its non-fatal period by having a NAMED blocker in front of it at every moment
(#149 -> #150 -> #151 -> #140 -> #155 -> #157, each cleared in turn); this gate
has none. A soft-skip still applies when the bundle is absent, because an
external tarball is a missing input rather than a broken kernel -- the fatality
applies to a gate that RAN.

### Coverage, and what the layers cannot see

`run_viv_bundle` was factored out of `do_alpine_shell_gate` in the same commit
so both gates share one spawn/drain/reap path; the refactor's blast radius is
loud rather than silent, since L-6c is itself boot-fatal.

The leg logic was discriminated on the host before it ever booted, against a
stub busybox reproducing applet dispatch: the control lights all six markers,
an argv[0]-is-the-resolved-path sabotage darkens **C alone**, a broken relative
symlink darkens **D and E**, and a stale image darkens **E alone**. The first
run of that harness darkened C in the CONTROL -- an absolute symlink dangles on
a host with no container root -- which is the reason the harness rewrites the
link and the reason a sabotage matrix is only readable once its control is
fully green.

The harness regenerates the script by parsing it back out of `tools/build.sh`
rather than keeping its own copy, so what it discriminates is the artifact that
actually ships. A second copy would have drifted at the leg-C fix and gone on
certifying the old legs.

What the host run cannot see: anything about the guest. No pool, no stalk, no
I-28 re-anchoring, no ldso, no `PT_INTERP`. It proves the script's patterns CAN
match and that each leg reddens for its own reason -- nothing more. Legs A and B
have no host sabotage because on the host they are the harness itself.

### `run_viv_bundle` drains to EOF BEFORE it reaps (#213)

The shared helper reads the pipe to EOF and only then calls `t_wait_pid_for`.
The order is the whole safety argument.

Reaping first deadlocks on a talkative container. The ring is 4 KiB
(`PIPE_BUF_SIZE`), all three of the container's fds are the SAME write end, and
nothing drains while joey sits in `wait_pid_for` -- so a container emitting more
than 4 KiB before exiting blocks in `write()` on a full ring while joey blocks
waiting for it to exit. **That is not a corner case for these gates, it is their
failure mode**: a broken loader emits one `Error relocating` line per unresolved
symbol, so reap-first hangs precisely when the thing the gate exists to catch has
happened, and reports cleanly only when it has not.

Draining first is safe because #68/#926 moved the handle close to EXIT
(`proc_close_handles_at_exit`, from `exits()`) rather than to reap: the child's
write end closes while it is still ALIVE, so EOF arrives without joey reaping
first. joey's own `wr` is closed before the loop, and that is load-bearing rather
than tidiness -- joey holding a writer means EOF never arrives.

**The regression leg lives on the L-6c bundle, not the D-5 one, and that
placement is deliberate twice over.** L-6c's bundle is OURS and is already the
fork/exec/pipe/reap mechanism gate, which is what a pipe-drain regression is;
putting it in the stock-distro gate would have bundled a pipe mechanism into a
claim about Alpine -- the one-mechanism-per-leg rule that gate's own leg-C fix
exists to enforce. It is also the only one that FITS: D-5 drives its script
through `sh -c`, and viv bounds every process arg at `PATH_MAX` = 512
(`usr/viv/src/main.rs:226`). Measured, that script uses 431 bytes and the emitter
needs more, so the first attempt died at `viv: manifest: arg bounds` with the
container producing nothing. L-6c drives a script FILE (`/gate/run.sh`), which
has no such cap.

The leg emits 5120 bytes after `L6C-DONE` and is asserted on the BYTE COUNT, not
a marker: joey's `acc` is 2048 bytes, so nothing past it is reachable to a marker
check and the only honest assertion is the counter (`run_viv_bundle` reports a
`total_out` that `acc_len` cannot, since `acc_len` saturates). The threshold is
the RING rather than the payload, so an edit to the emitting line cannot make the
leg brittle. It reports under its own name and says explicitly that it is *not* a
busybox/vivarium leg failure -- a leg that reddens under someone else's name
accuses the wrong subsystem.

**A/B revert-probed, and the measurement corrected the prediction.** Control:
both gates PASS, 5120 bytes through the pipe, suite 1389/1389. Sabotage (restore
reap-before-drain, verified present in source AND in a changed `ramfs.cpio` hash
before booting): **the boot HANGS** -- `FAIL: timeout, no boot marker`. The
prediction had been a *partial* ~4096-byte bulk in the log; the measurement shows
**zero container stdout at all**, because busybox's stdout to a pipe is FULLY
buffered, so the whole script's output sits in libc's buffer and the ring fills
on the first flush, before a byte is relayed. Kernel-side `vivarium:` messages
keep flowing throughout (46 of them), which is what proves the guest is BLOCKED
rather than dead -- the deadlock's signature, not a crash. So the pre-fix failure
was worse than described: not a truncated diagnostic but a silent gate and a
stopped boot.

`pouch_smoke_one` still reaps first, and that is correct FOR ITS CALLERS rather
than an inconsistency: its children are our own binaries with bounded output. Its
comment used to justify the order as forced ("a zombie holds its handle table
until reap"); #68 made that false, so the order there is now a CHOICE and the
comment states the 4 KiB precondition as a live constraint to check before wiring
a new caller (#147).

---

## #218 -- MNOEXEC, witnessed from INSIDE a live container (L200-L204)

### The gap this closes, and why it was not pedantic

#217 proved every kernel link of the executable-mapping gate in isolation: five
sabotages with five distinct attributions, plus the `may_back_exec` floor that
round-1 forced underneath the flag. What none of it proved is the LAST link --
that `viv`'s **actual** mounts carry `MNOEXEC` in a running system.

That gap is invisible to every other gate. The arc gates passing shows only that
noexec does not **break** a container; it does not show noexec is **applied** to
one. Those are two readings of the same green, and only a deny-path witness
separates them. Had some path dropped the flag between `viv`'s `t_mount` and
`PgrpMount.flags`, the whole tree would still have been green -- the standing
"boot OK does not prove a gate is wired" rule.

### The pair, chosen so only the mount flag differs

| target | Dev | `may_back_exec` | arrived by | `PROT_READ\|PROT_EXEC` |
|---|---|---|---|---|
| `/bin/viv-pheno-probe` | dev9p | true | **chroot** (not a mount) | **admitted** (L201) |
| `/proc/meminfo` | dev9p | true | **MNOEXEC mount** | **refused** (L204) |

Same Dev, same mapping machinery, same phenotype arm. The only difference is the
mount flag, which is what makes L204 a statement about `MNOEXEC` rather than
about containers in general.

**`/env` and the `/dev` leaves cannot witness this at all.** The `may_back_exec`
floor refuses them *before* the flag is ever consulted (#217 round-1 F1), so a
denial there would prove nothing about `MNOEXEC` -- it would prove the floor.
Choosing a target the floor does **not** cover is the entire point of the pair.

L203 is the control that makes L204 mean something: the *same fd* maps cleanly
without `PROT_EXEC`, so the refusal cannot be explained by a bad descriptor, an
unmappable file, or a broken mount. noexec bounds what may become **code**, not
what may be **read**.

### The errno is a flat `-1`, and that is correct here

`errno.h` carries a standing warning: **do not return `-T_E_PERM` from a syscall
handler**, because value 1 collides with the pouch boundary line's flat-`-1`
sentinel and `__syscall_ret` maps it to `EIO`. That warning does not apply to
this path and L204 must not be "fixed" on the strength of it:
`sys_mmap_file_for_proc` has **exactly one caller**, the vivarium dispatch, so
the value only ever reaches a PHENO_LINUX guest -- for whom `-1` IS `-EPERM` by
Linux's own numbering. (The exec-side `MNOEXEC` site returns no errno at all; it
fails resolution and yields NULL.)

`-1` is nevertheless a WEAK value on its own, since it is also
`syscall_dispatch`'s generic sentinel. L203 is what upgrades it from "something
went wrong" to "the exec gate refused".

### Discrimination, proven by A/B

Dropping `T_MNOEXEC` from `viv`:

```
joey: V-7  viv-probe (containered) PASS          <- native leg unaffected
joey: V-1b linux-phenotype leg FAILED marker=L204 status=1
tests: 1394/1394                                 <- kernel suite unaffected
(no "Thylacine boot OK")
```

Restoring it returns `V-1b phenotype (native + containered linux) PASS` and the
boot marker. The failure isolates to the new leg alone.

**BOTH `viv` call sites must be sabotaged to see the flip.**
`mount_noexec_covers` matches the mount **source's** `(dc, devno)`, and `/dio`
and `/proc` are the SAME diorama 9P instance -- so dropping only the `/proc`
bind leaves the `/dio` entry still covering the device, the verdict does not
change, and the experiment reports "no discrimination" while having sabotaged
nothing. A sabotage that quietly passes is the finding, not the control.

### Trip hazard: these legs are POOL-RESIDENT

The probe ships inside `/vivarium/pheno`, which lives in the Stratum pool. A run
under `THYLACINE_MKFS_PRESERVE=1` executes the **stale** binary, so new legs
silently do not exist and the boot is green for the wrong reason (#126). Re-bake
the pool when changing them, and confirm from the build log's
`populate pool: viv bundles baked at /vivarium` -- **not** from a green boot, and
not by grepping `pool.img`, whose contents are encrypted and will never show a
plaintext marker.

## The diorama channel is a private pipe pair (aux, 2026-08-17)

### The defect: an interactive `viv run` never ran the container

`viv run <bundle>` from a session shell -- `ut`, hosted by `ptyhost` or not --
printed one line on the console, `viv: spawn /bin/diorama`, and returned. That
line is not progress: it is `Err(String::from("spawn /bin/diorama"))` reaching
`say`. The per-container diorama posted the fixed name `/srv/viv-dio`, so `viv`
requested `SPAWN_PERM_MAY_POST_SERVICE` for it, and `spawn_perm_grant_check`
grants that bit only to a console-attached granter or an existing holder. Nothing
past login is one: joey confers it on `/sbin/login`, login confers
`CONSOLE_OWNER` on `ut` (`usr/login/src/main.rs`, the shell spawn) and nothing
else, `ut` confers nothing on its externals. Every boot-gate `viv` was
joey-spawned WITH the bit, so no gate had ever run the interactive path. The V-7
commit body listed the seam ("interactive `viv` from a session shell needs ut to
hold+confer MAY_POST_SERVICE") and nothing enqueued it.

Two readings of the fix were on the table:

* **Widen the privilege** -- login confers `MAY_POST_SERVICE` on `ut`, `ut` on
  every command. Rejected: at v1.0 there is ONE shared boot `SrvRegistry`, so
  every user program could then squat `/srv/home-<user>` before that user logs
  in, or re-post a tombstoned trusted name (`/srv/net` after netd dies -- a
  tombstone is re-postable by any marked Proc). And the fixed-name collision
  (V-8 F3) stays.
* **Need no name.** The runner and its diorama are parent and child; the
  channel between them can be handed at spawn. This is Plan 9's own idiom --
  `mount(fd, ...)` over a pipe; `srv(3)` only PUBLISHES fds for strangers to
  find -- and the capability-microkernel one (a component's private service
  channel arrives in its startup handles). The kernel already has the primitive:
  `SYS_ATTACH_9P(tx, rx)` over two Plan 9 pipes, the P5 `stub-driver` shape,
  exercised by `test_attach_probe` since Phase 5 and until now by nothing in
  production.

The second is what landed. It removes the privilege, the name, and the
collision at once, and turns the F3 attach gate from a check into a structural
property.

### The shape

`viv` (`usr/viv/src/main.rs::run`):

```
c2s = t_pipe(); s2c = t_pipe()
spawn /bin/diorama --vivarium <my pid>   with fds [c2s_rd, s2c_wr]  (its 0 and 1)
close c2s_rd, s2c_wr                     -- ours; while held, no EOF could ever surface
root = t_attach_9p(c2s_wr, s2c_rd, "/")  -- Tversion + Tattach over the pair
close c2s_wr, s2c_rd                     -- the attach holds its own transport refs
mount root at /dio  ...                  -- unchanged from here
```

The diorama's server ends are its ONLY fds: it wants no stdio (diagnostics ride
`SYS_PUTS`), and the fewer things a per-container server holds the better. The
runner passes NO perm bits, and joey's boot `viv run`s were changed to pass none
either -- so every gate now runs the same path a session shell runs.

`diorama --vivarium` (`usr/diorama/src/main.rs`): after the selftest, verify the
argv runner is our PARENT (the ppid line of our own native
`/proc/<self>/status`; `viv_runner_is_parent` is pure and in the selftest), then
serve ONE `Conn::over(0, 1)` -- `Conn` grew a distinct `tx` (a /srv endpoint is
both; the pair is two fds) -- until `service()` reports EOF, then exit 0. No
listener, no post. The boot mode (`/srv/diorama`, joey-spawned with the bit) is
untouched.

**Who is `self` now.** There is no `SYS_SRV_PEER` on a pipe. `Conn::peer()` in
vivarium mode builds the peer from kernel state this server holds about itself
and its parent: pid = the runner (checked against our ppid at startup), alive = a
native `/proc/<runner>` resolve (unfiltered -- the runner is not a member of its
own container's view, so it cannot go through `native_pid_exists`), ids = our
own `t_getuid`/`t_getgid` (a plain spawn inherits the runner's; and I-43 holds
whatever we report -- the diorama's authority is its own principal's). Same
content SYS_SRV_PEER gave when the runner opened the name: the mounter. Task #90
(`/proc/self` names the mounter, not the reader) is unchanged by the channel.

The Tattach `n_uname` -- which `sys_attach_9p_handler` overwrites with the
caller's kernel-stamped principal -- was considered as a consistency check
against `t_getuid()` and dropped: it would guard only the Uid line's cosmetics
(authority never depended on it), and a check nothing can drive is a gate the
"boot OK proves nothing" rule forbids carrying.

### The V-8 F3 section above is superseded

Its mechanism -- `viv_attach_allowed(runner, peer_alive, peer_pid)` in
`h_attach`, the joey `#101` deny leg, the "concurrent containers are still
unsupported" note -- is gone. The property it protected (container A never
mounts container B's `/proc`) now holds because no second runner can reach the
attach at all: nobody but the runner holds an end. What remains checkable is the
startup premise (the runner is the parent), and it is checked.

### Gates

The joey `#101` leg became the **viv-channel** leg, two spawns of
`diorama --vivarium` one variable apart so neither branch passes for the wrong
reason:

| runner argv | expected | proves |
|---|---|---|
| joey's own pid | the attach over the pair SUCCEEDS; with the server provably up, `/srv/viv-dio` does NOT resolve; closing the attach root (the last client-end drop) makes the diorama exit on EOF within a bounded wait | the channel serves; nothing is posted; a dead runner's diorama does not linger |
| `4294967295` | the diorama exits at its parent check before Tversion; the reply read sees EOF; the attach FAILS | the parent check is wired |

And the V-7 leg now spawns **two** `viv run /vivarium/probe` **concurrently** and
waits for both: each probe's leg 3 asserts its pid view is EXACTLY {self}, so two
live containers prove from the inside what `#101` proved from outside (A never
shows B), and the concurrent-containers claim is gated rather than asserted.
The interactive path itself -- the one no boot leg can run -- is
`tools/interactive/viv-run.exp` (LS-CI, 36 scenarios now): from the CONSOLE
`ut`, `viv run /vivarium/probe` must print `viv-probe: FAIL: principal is not
the invoker's` -- the probe bundle's manifest expects the joey gate's SYSTEM
principal, so from a user the FIRST failing leg is #6, and that exact line
proves legs 1-5 (rootfs, `/proc`+`/sys` from the diorama, pid view == {self},
host `/srv` unreachable, `/net` absent) held under a user principal from a
plain shell (a viv that cannot spawn its diorama prints no probe line at all);
then from a `ptyhost`ed `ut`, `viv run /vivarium/alpine-ash` (the new
INTERACTIVE twin `tools/build.sh` stages beside the L-6c bundle: same rootfs,
args `/bin/sh -i`) must show the ash prompt on the pts, answer a `| tr`
pipeline through stdin/stdout, and `exit` back to `ut` -- the pts half SKIPs
when the Alpine bundle is not staged.

Measured (this chunk, first boot after the change, no retries): kernel suite
1413/1413 (the kernel binary is not rebuilt -- no `kernel/`/`arch/`/`mm/`
file changed); `joey: V-7 viv-probe (containered, x2 concurrent) PASS`
(two `viv-probe: PASS` lines, two `diorama: selftest PASS` lines);
`joey: viv-channel: private pair serves, no /srv name, EOF-exit`;
`diorama: --vivarium pid is not my parent` then `joey: viv-channel:
non-parent runner REFUSED`; V-1b + L-6c + D-5 PASS; `Thylacine boot OK`.
LS-CI `viv-run`: PASS on attempt 1 -- the console `viv run /vivarium/probe`
printed the leg-6 line, the `ptyhost`ed `viv run /vivarium/alpine-ash` showed
`/ $ ` on the pts and answered `ASH-ALIVE` through it (so the stdio question
the earlier hypothesis raised is answered: a pts trio passes `stdio_born` and
flows both ways), `exit` returned to `ut` twice over. Full LS-CI over `5336c894`
(this + the ^C masks): 34 PASS + 2 SKIP (GL) over 36, 0 retries.

### A wart worth naming: the pipe note on a dead diorama

`devpipe_write` posts a `pipe` note to the WRITING Proc when the ring's reader
is gone (`kernel/pipe.c`, the EPIPE arm), and the kernel 9P client's spoor
transport writes from the syscalling Proc's context. So a container Proc that
issues a 9P op through `/dio` after the diorama has DIED gets a `pipe` note --
under the phenotype, a `SIGPIPE`-shaped death for a `/proc` read -- where the
`/srv` transport (a dead SrvConn) returned only an error. The diorama dies only
when its channel is already gone (EOF) or when `viv` kills it after the
entrypoint has exited, so the reachable case is a container Proc that outlives
its runner (an orphaned daemon) touching `/proc` afterwards, or a diorama crash.
Linux's kernel 9P client does not signal the process on a dead transport
(`net/9p/trans_fd.c` writes from a workqueue). The clean fix is a
`MSG_NOSIGNAL`-shaped kernel-internal pipe write for transports -- a
`kernel/pipe.c` change on the Pipe wait/wake audit surface, deliberately NOT
folded into this userspace-only chunk. Recorded in `docs/AUX-ROADMAP.md`.

## ^C reaches the container, not the runner (aux, 2026-08-18)

The first thing the channel fix let anyone type at an interactive phenotype
ash was ^C, and the first ^C killed `viv`. The R5-F9 experiment scenario
(`scratchpad/r5f9/r5f9-ash.exp`, 3/3 attempts) put the console line right
where the prompt should have been: `proc: orphan pid=N name="sh" (parent
pid=M name="viv" exiting) -> adopted by pid=1974`, then the same for the
diorama. Mechanism, all as-built: the pts's ISIG cooks 0x03 into an
`interrupt` posted to the terminal's FOREGROUND PGRP; `viv` runs as `ut`'s
foreground job, so its pgrp is `viv` + its diorama + every container Proc; the
container's shell sees SIGINT and handles it, but a NATIVE Proc with no
handler and no notes fd dies of an uncaught `interrupt` (LS-5's default) --
so both native members died, the shell and its `/proc` server were orphaned to
init, and the orphaned shell went on reading the same pts as the outer `ut`,
splitting every later keystroke between two readers so that neither answered.
(That input-stealing shape is the v1.0 pts's missing TTIN arbitration, PTY-4's
own footnote, seen from the other side.)

**The fix is a mask, and only a mask.** `viv` masks `interrupt` at startup
(`SYS_NOTE_MASK`, bit 0). The container needs nothing forwarded -- it is in the
pgrp and receives the note directly -- and nothing leaks into it: a spawned
child starts with a ZERO mask (`rfork_internal` copies `note_mask` only when
the PARENT is `PHENO_LINUX`, and the native exec-image reset zeroes it). The
tty family stays UNMASKED in `viv` on purpose: ^Z (`tty:susp`) must STOP `viv`
together with the container, or `ut`'s `wait_pid(WUNTRACED)` on the job never
sees it stop and the terminal is never handed back; a hangup ends `viv` with
the container; ^\ (`tty:quit`) still kills the runner and detaches a running
container, which is what `docker run` does under SIGQUIT too. The diorama has
no such constraint -- nothing waits on it as a job -- so it masks BOTH
families: a server never dies of a keystroke, and its lifetime is its
channel's (EOF, or the runner's kill).

Kernel facts this rests on, read rather than assumed: the terminate LATCH is
armed at post regardless of the mask (`notes_arm_intr_terminate_locked` gates
on the name, kproc, a live handler and self-management), but both consumers
honour the per-thread mask -- the EL0 tail's `notes_terminate_pending_name_
locked` skips a masked family, and the #811 sleep predicate reads the latch
through the mask -- so a masked `interrupt` is neither delivered nor unwinds a
blocked `wait_pid`; the ldisc's post is `synthetic`, so repeated ^C coalesce at
the queue threshold rather than filling it.

Coverage: `tools/interactive/viv-run.exp` gained the leg -- at the ash prompt
on the pts, `uname -s | tr a-z A-Z` answers `LINUX` (the phenotype's `uname`
row; the outer `ut`'s coreutil says `THYLACINE`, so the token names WHICH shell
answered), then ^C, then the same command must answer `LINUX` again through
the same `viv`. Measured: PASS on attempt 1 (`saw: ash still answers after ^C
(the runner survived it; ut would say THYLACINE)`), on the build whose only
change from `437213c4` is the two masks.

**Found alongside, OPEN, aux's own line (`memory/bug_hosted_ut_double_ctrlc_
idle.md`):** two ^C at a `ptyhost`ed `ut`'s IDLE prompt, then the next typed
command is echoed but not executed until an extra Enter -- one ^C is fine, and
the console `ut` is fine either way (`scratchpad/r5f9/ctrlc-idle.exp`,
RESULT `outer-cc=1 inner-c=1 inner-cc=0 recover=1`). The R5-F9 question itself
(does ash's `raise_interrupt` longjmp wedge `in_handler`) is **answered YES** by
the arm-2 hunt: an escaping handler that never reaches `rt_sigreturn` leaves
`in_handler` stuck, which is exactly `bug-2` (VIVARIUM 6.23). `bug-1`
(`0149d1e3`) stopped the livelock *symptom*; `bug-2` (`438cac78`) clears the
stuck latch at the next EL0 transition. The deterministic proof is
`viv-pheno-probe` legs L245-L248: a `PHENO_LINUX` handler `siglongjmp`s out of
itself, a second `SIGPIPE` is delivered across the escape, and the leg asserts
the handler fired **twice** -- red (`joey: ... marker=L248`) on a kernel with the
two clears disabled, green with them.

## The path-mutation family (#50; aux, 2026-08-25)

Design: `VIVARIUM.md` section 6.24 (scripture `b417b307`). Four Tier-2 rows —
`openat(O_CREAT)` + `mkdirat`(34) + `unlinkat`(35) + `renameat`(38)/
`renameat2`(276, flags==0) — on ONE new kernel primitive, plus the native mint
`SYS_OPEN_CREATE = 108` (the ARCH section 11.2 `create(name, mode, perm)` row,
fulfilled). git's writes stand on all four (`git init` alone needs create +
mkdir + unlink + rename-into-place). The openat routing reads register bits
only: `O_CREAT` WITHOUT `O_PATH` goes to the create decide; WITH `O_PATH` it
stays on the plain decide, which STRIPS it — Linux's `O_PATH` ignores
`O_CREAT`, and the strip serves that contour exactly (the #50 close's SA-1;
before it the composition declined while the comment claimed the contour).

### The mechanism (extraction, not duplication — the I-43 rule)

- `sys_join_cwd_if_relative` — the LS-4 cwd join, extracted from SYS_OPEN's
  core and shared VERBATIM with the create core, so the FROM_ROOT-sentinel
  parity (the 6.20 blocker-3 hazard) is structural.
- `kpath_split_leaf` — the lexical last-component split (classify, never
  resolve; the libthyla #87 rows kernel-side) + `sys_stalk_parent` (walk-only
  stalk of the prefix; I-28 containment, symlink expansion, mount-cross all
  inherited).
- `spoor_create_install` — the create mechanics extracted from
  `sys_walk_create_handler` (dev slots, QTDIR, the A-2d W|X parent gate,
  clone-walk, `dev->create`, rights, install). The /srv service-post branch is
  fd-based-only (`srv_post_ok=false` for every path caller answers
  `-T_E_OPNOTSUPP` on a /srv-registry parent).
- `spoor_unlink_in_dir` / `spoor_rename_in_dirs` — the unlink/rename
  mechanics, same extraction; the phenotype shells run them on the split
  parent via `viv_mutation_parent`.
- `sys_open_create_kpath_for_proc` — the composition: OEXCL (0x1000, the
  pre-reserved bit) and DMDIR are ONE server-atomic create (EEXIST honest —
  the lockfile primitive); plain create is open-first (OTRUNC rides the open
  leg only), create-on-NOENT, retry-open on a lost exists-race, bounded at 2
  rounds — the Plan 9 `namec(Acreate)` / Linux v9fs idiom.

### Degradations (documented, none silent-wrong)

- A DANGLING final symlink + O_CREAT answers EEXIST after the bounded loop
  where Linux creates the TARGET (loud; git never does this).
- No umask: guest `umask` is ENOSYS and the kernel applies no mask, so modes
  arrive unmasked (0666 where Linux yields 0644). Cosmetic under A-2d.
- `mode`/`perm` admit the low-9 bits only, and the two fields above them are
  handled OPPOSITELY because they differ in kind:
  - **S_IFMT (0170000) is MASKED** (`VIV_S_IFMT`). POSIX and Linux define the
    file type on `openat`/`mkdirat` as determined by the CALL, so these bits
    carry no meaning on this argument and Linux discards them. Discarding a
    field with no meaning is exact, not a strip. Callers pass it routinely:
    busybox `tar` selects its directory branch on `S_IFMT` and then hands
    `file_header->mode` through unnarrowed, so `mkdirat` sees `S_IFDIR|0755`
    and `openat` sees `S_IFREG|0644`. Before the mask both DECLINED, and
    `tar -x` failed on every entry with "Function not implemented" -- the
    gate below was catching S_IFMT as collateral.
  - **setuid/sgid/sticky (07000) still DECLINES** census-visibly. The strength
    of that argument DIFFERS between the two arms, and the reference should not
    flatten them:
    - On **openat-create** it is a caller-protection argument: Linux keeps
      07000 via `S_IALLUGO`, so `O_CREAT|04755` really does record setuid, and
      a silent strip would record less authority than asked with nothing to
      catch it. `git`'s shared-repository shapes are the real caller.
    - On **mkdirat** it is only a stance-symmetry argument. Linux's own
      `vfs_mkdir` masks `mode & (S_IRWXUGO|S_ISVTX)`, so a setgid bit passed to
      `mkdirat` is stripped BY LINUX; setgid directories come from parent
      inheritance or a later `chmod` (git's `adjust_shared_perm` chmods). No
      caller is wronged by a strip here -- we decline to keep one rule ("refuse
      what we will not record") rather than two.
  - The regression tests pin both halves, including the cases proving the mask
    does not reach 07000 (`S_IFDIR|02755` and `S_IFREG|04644` still decline)
    and the sticky boundary (`S_IFDIR|01777`, the `/tmp` entry every real
    rootfs tarball carries).
- **Residue worth knowing before extracting a distro image**: because 07000
  still declines, `tar -x` of a real rootfs still fails on its sticky `/tmp`
  and any setuid binary. The S_IFMT mask narrows the failure set from *every*
  entry to *07000-bearing* entries; it does not empty it. Widening that is a
  deliberate decline-vs-strip-with-census design fork, not a quiet change.
- Real (non-AT_FDCWD) dirfds stay out (the 6.20 Correction-2 handle-state
  blocker, untouched). renameat2 flags != 0 (NOREPLACE/EXCHANGE/WHITEOUT)
  decline. `O_DIRECTORY` keeps its V-2b rejection on the create arm (`O_APPEND`
  translates since §6.27 -- the FS-pass-through arm).
- A trailing slash on a rename/unlink-file path answers ENOTDIR *lexically* —
  including `rename("d1/", "d2")` on a REAL directory, which Linux resolves
  and permits (the #50 holotype's F2). Strictly refuse-more: no mutation ever
  proceeds that Linux would refuse; the inverse admission would need a
  resolve-on-trailing probe, deferred until a consumer materializes (git
  renames carry no trailing slashes).
- Sticky-dir deletion restriction is not enforced (A-2d checks W|X only) —
  pre-existing SYS_UNLINK behavior, now reachable by path.

### The errno registry gained `T_E_ISDIR` (21)

User-signed-off (2026-08-25): the VALUE already crossed to EL0 through the
`[-4095,-2]` Rlerror passthrough (Stratum answers EISDIR for write-opening a
directory); the name lets the lexical leaf rows answer exactly what Linux
answers. `docs/ERRORS.md` carries the row.

### The bake gained the /tmp re-stamp

`stratum-fs put` preserves only the exec bit (dirs bake 0755 SYSTEM-owned),
so the containers' /tmp lost its 1777 and a user-principal `viv run` could
not write ANYWHERE in the rootfs — measured as `can't create /tmp/f50:
Permission denied` the first time the E2E ran the create leg as a user.
`populate_stratum_pool` now re-stamps each bundle's `rootfs/tmp` 1777 after
the put (`stratum-fs chmod` -> Tsetattr; the parser admits 4-digit octal).
The kernel enforces no sticky bit at v1.0 so 1777 behaves as 0777 today;
baking the real Linux mode means the fixture needs no revisit when sticky
enforcement lands (the #50 holotype's F4). The general gap — `put` flattens
every OTHER non-exec mode bit a rootfs carries (e.g. /var/tmp's 1777, setgid
dirs) — is Stratum-side and remains open.

### The loopback learned the async-clunk drain

The create leg legitimately double-parks the parent dir fid (dirfid_put
parks the first RPC-free, `p9_client_clunk_async`'s fire-and-forget clunks
the second), leaving an ownerless Rclunk a later reader drains on a real
transport (the demux #210 orphan-clunk arm). The single-slot test loopback
REFUSED the next send over the unread reply — `client_mark_dead_locked`, the
whole client dead, every later op EIO — killing a legitimate pattern no real
backend fails. `loopback_send` now discards exactly a WHOLE staged Rclunk
(counted in `dropped_rclunks`); every other unread-reply send still refuses.
The three dev9p legs assert the count EXACTLY (0 / 1 / 1 — MEASURED anchors,
uart-instrumented boot; a first-principles park-slot model predicted 1 / 2 / 0
and was wrong, because the choreography lives in the dirfid park/reuse pool),
so an unexpected drop — a real synchronous-clunk reply leak, the class the
guard exists for — moves a count and fails loudly instead of passing as the
modeled pattern (the holotype's F1).
Cost of finding it: six instrumented boots (the step-tracker bisection).

### Witnesses

Kernel: `stalk.open_create_*` (5 — the cwd-parity blocker-3 regression,
open-if-present + create-call economy, mkdir + nest-into-created-dir, the
lexical leaf rows, containment + ACCES/NOENT/LOOP/dangling denials) +
`dev9p.open_create_*` (3 — NOENT-then-create, the lost-race retry-open with
exactly one Tlcreate, OEXCL EEXIST-exact) + `vivarium.*_domain` (4 decides).
E2E: `tools/interactive/viv-run.exp` runs `>file`/`mkdir`/`mv`/`rm`+`rmdir`
inside the phenotype ash as a PLAIN USER on a pts. Native: libthyla-rs
`open_create_at_path` + `create_dir` rewired onto `t_open_create` (every
`File::create` caller adopted through one function; the stale create-first
rationale retired).

## getdents64 + fsync/fdatasync + O_DIRECTORY (the 6.24 follow-on; aux, 2026-08-26)

Design: `VIVARIUM.md` section 6.25. Three Tier-2 rows — `getdents64`(61) +
`fsync`(82)/`fdatasync`(83) — plus the `O_DIRECTORY` admission that unblocks
them (musl's `opendir` opens `O_RDONLY|O_DIRECTORY|O_CLOEXEC`; while the flag
sat on the V-2b reject list, `getdents64` was unreachable).

### The mechanism

- `spoor_readdir_run` (`kernel/syscall.c`) — the readdir core extracted from
  `sys_readdir_handler` with a NO-offset-advance contract: the helper reads
  raw 9P dirents + reports the last cookie; each caller commits `c->offset`
  only after its own copy-out (a faulting user buffer never advances the
  cursor — the F3 fault property). Errno rollout on the shared handler:
  `-T_E_BADF` (CWALKONLY) / `-T_E_OPNOTSUPP` (no slot) / `-T_E_IO` (malformed
  dirent) / dev errors verbatim. **`dev9p_readdir` itself now returns
  `dev9p_wire_errno(rc)`** (it was the one dev9p op still flattening every RPC
  failure to a bare `-1`, unlike its `readlink`/`fsync`/rename siblings) — so a
  caught-note EINTR (the ^C-during-`ls` path) surfaces as retryable EINTR
  rather than a fabricated EPERM at the viv boundary (the audit's F2).
- `viv_dirent64_encode_run` (`kernel/syscall.c`, non-static: unit-tested) —
  the pure 9P-dirent -> `linux_dirent64` transform: `d_ino <- qid.path`,
  `d_off <- resume cookie`, `d_type` verbatim (shared DT numbering),
  `d_reclen` = align8(19 + namelen + 1). Whole records only; stops at the
  first no-fit; reports the last EMITTED cookie (the committed cursor never
  passes what the guest received); returns 0 when the first record does not
  fit (the caller's EINVAL row).
- The `VIV_LINUX_GETDENTS64` arm — 2048-byte raw / 2560-byte encode stack
  buffers (worst growth align8(20+n)/(24+n) at n==5 = 32/29; 2048*32/29 =
  2260 < 2560, no overrun); `count==0 -> EINVAL`; **`sys_validate_user_buf`
  on the user `dirp` up front (before the fd lookup, mirroring the native
  `sys_readdir_handler`)** — the copy-out writes via `uaccess_store_u8`, whose
  fault fixup engages only for the user half, so an unvalidated kernel-half
  `dirp` would extinct (or corrupt) rather than fault-gracefully; this was the
  audit's F1 P0; no-RIGHT_READ -> EBADF; non-QTDIR -> ENOTDIR; emitted==0 with
  raw bytes -> EINVAL.
- `vivarium_openat_decide` gained `bool *dir_required_out` (NULL permitted;
  written only on TRANSLATED). The openat shell enforces it as a
  postcondition on the MINTED Spoor's own qid (`sys_lookup_spoor` -> QTDIR ->
  on miss `handle_close` + `-T_E_NOTDIR`) — no extra RPC, no TOCTOU. The
  create decide still declines `O_DIRECTORY`.
- `VIV_LINUX_FSYNC`/`FDATASYNC` — T2 shells onto `sys_fsync_handler(fd,
  datasync)` with the datasync bit passed EXPLICITLY (a T1 renumber would
  read garbage x1). `sys_fsync_handler` errno rollout: `-T_E_BADF` /
  `-T_E_OPNOTSUPP` (both formerly bare `-1` = fabricated EPERM across the
  boundary).

### Degradations (documented, none silent-wrong)

- `fsync` on an `O_RDONLY` fd answers EBADF (the native RIGHT_WRITE gate)
  where Linux syncs — git milestone A runs `core.fsync=none`; revisit on a
  real rdonly-sync consumer.
- The number rows carry damage-envelope collision paragraphs in `vivarium.h`
  (61 vs SYS_CAP_GRANT_CLEARANCE, 82 vs SYS_WEFT_MAP, 83 vs
  SYS_BURROW_ATTACH_LAZY — all fd-based, caller's-own-things envelope).
- `O_DIRECTORY|O_TRUNC` drops the TRUNC (`vivarium_openat_decide`): a directory
  is never truncated on Linux, and this combination on a regular file is
  ENOTDIR *before* truncation — carrying TRUNC would truncate the file and only
  then answer ENOTDIR (the audit's F3, silent data loss). Dropping TRUNC makes
  the open non-destructive so the ENOTDIR fires cleanly.

### Witnesses

Kernel: `vivarium.dirent64_encode` (two-record layout, partial-fit cookie,
first-no-fit 0, truncated-tail drop) + the O_DIRECTORY domain assertions
(plain, the musl-opendir flag set, `O_PATH|O_DIRECTORY`) + the 5-way NULL
guard on the decide outputs + **`vivarium.getdents64_guards_uaccess`** (the F1
regression: a kernel-range `dirp` -> EFAULT before the fd lookup; a user-ok
`dirp` with a bad fd reaches the lookup -> EBADF; count==0 -> EINVAL first —
MEASURED fails-without-fix, guard off gives BADF where EFAULT is asserted).
E2E: viv-run's 4th leg (`ls /tmp/d50` -> `G50`: busybox ls -> musl readdir ->
the 61 row, as a plain user).

### The E2E ^C leg — the settle is load-bearing (the 2026-08-26 hunt)

Adding the 4th leg deterministically re-timed the following ^C leg into a
failure whose counter-instrumented hunt exonerated the kernel at every link:
a ^C sent the instant the prior leg's output matches lands inside
busybox-ash's reap window (the shell still holds SIGINT=SIG_IGN), and the
V-6b ignore-drop discards the note at post time — Linux's own semantics for
a generated-while-ignored signal. The scenario now settles before the ^C.
The hunt measured the full caught-note chain live (fan -> arm -> wake of the
parked elected 9P reader -> SLEEP_NOTEINTR -> CLIENT_WAIT_NOTEINTR -> EINTR
-> handler -> prompt), exercising the wake-of-a-parked-reader leg for the
first time, and byte-captured ash's post-^C read returning the typed line
intact (rc=22, hex-exact). Residue, busybox-internal and kernel-blameless:
ash's own pending-interrupt latch can consume the first line completed after
a delivery (its INT_OFF/INT_ON bracketing), alignment-dependent.

## The git chunk: faccessat/chdir/fchmodat/readlinkat + geteuid/getegid + getrandom, and the three walls (milestone A: init + add; aux, 2026-08-26)

Forcing consumer: a real static aarch64 musl `git 2.51.2` under the phenotype.
Milestone A is `git init` + `git add`; `commit`/`clone` are §6.27 (the reflog
`O_APPEND`). Three walls; only the first is syscall translation.

### The seven rows (VIVARIUM.md §6.26)

| Linux # | Row | Kind | Mechanism |
|---|---|---|---|
| 48 | `faccessat(dirfd,path,mode)` | T2 | AT_FDCWD gate -> `sys_stat_for_proc` (follow) + `perm_check(mode&7)`; `mode & ~0x7` EINVAL; F_OK = existence |
| 49 | `chdir(path)` | T2 | measure len -> `sys_chdir_handler`; bare -1 -> ENOENT (documented collapse; SUCCESS exact) |
| 53 | `fchmodat(dirfd,path,mode)` | T2 | AT_FDCWD gate; RAW 3-arg (args[3] undefined residue, NOT read -- the F1 fix; flags is fchmodat2/452); open O_PATH -> `sys_wstat_for_proc(T_WSTAT_MODE, mode & T_WSTAT_MODE_MASK)`; fd closed on every path |
| 78 | `readlinkat(dirfd,path,buf,bufsiz)` | T2 | AT_FDCWD gate; NOFOLLOW `stalk_err` (F3: preserves EACCES/ELOOP) -> QTSYMLINK + `.readlink`; negative dev errno (INTR/IO) PRESERVED (F2), non-symlink/malformed-len EINVAL; **`sys_validate_user_buf(buf_va, n)` before `uaccess_copy_out`** |
| 175 | `geteuid()` | T2 | `vivarium_map_uid(principal_id)` -- effective == real (I-22, one principal) |
| 177 | `getegid()` | T2 | `vivarium_map_gid(primary_gid)` |
| 278 | `getrandom(buf,len,flags)` | T2 | `sys_getrandom_handler`; bare -1 -> EAGAIN; **CAP_CSPRNG_READ kept (I-43)** |

`readlinkat`'s copy-out is the getdents64 P0 class made safe: `n = min(tlen,
bufsiz)`, `tlen` bounded to `1..SYS_OPEN_PATH_MAX` by the `stalk.c` vtable
contract, the target staged in a `SYS_OPEN_PATH_MAX+1` kernel buffer, and the
EXACT `n` span validated before the write. `sys_readlink_for_proc` clunks its
Spoors on every early return.

### The AT_FDCWD gate is both the cwd-form contract and the collision defense

`vivarium_faccessat_decide(dirfd)` returns `VIV_TRANSLATED` iff
`(s32)(u32)dirfd == VIV_AT_FDCWD` (-100), else `VIV_FORWARD`. 48/53/78 all
route through it. Below the ceiling (108), they collide with native
`SYS_NOTE_MASK`(48) / `SYS_PIVOT_ROOT`(53) / `SYS_PCI_INFO`(78) -- each native
arg (a note bitfield, a Spoor fd, a PCI handle) is a small non-negative value,
never -100, so a mis-declared native caller FORWARDs to ENOSYS on shape.
FD-less `chdir`(49) vs native `SYS_SPAWN_FULL_ARGV` carries the damage-envelope
argument instead (caller's-own-cwd, never authority). 175/177/278 are above the
ceiling -- collision-free, `_Static_assert`ed in `vivarium.c`.

### The three walls

1. **The seven numbers** (above) -- each `FORWARD`ed to ENOSYS and killed the
   corresponding git step.
2. **The pool is SYSTEM-owned.** dev9p reports `PRINCIPAL_SYSTEM` for the boot
   FS, so a container's created files are SYSTEM-stamped regardless of the
   creating principal; git's config write chmods its own lockfile (needs
   OWNERSHIP), so a real-user container is denied and `init` dies. Per-principal
   9P ownership is **A-3, unbuilt at v1.0**. Milestone A runs git as a
   **SYSTEM-principal boot probe**, which owns the files.
3. **Phenotype fork must inherit caps.** `rfork_forked` passes `CAP_NONE`, so a
   forked child's caps = `parent & 0 = 0` -- fork zeros caps. git is FORKED from
   the entrypoint shell, so it lost `CAP_CSPRNG_READ` and `getrandom` failed.
   Fix: `rfork_forked_with_caps`, taken by `sys_rfork_core`'s `PHENO_LINUX` arm
   with `CAP_ALL`; `rfork_internal` computes
   `child->caps = (parent_caps & CAP_ALL) & ~CAP_ELEVATION_ONLY`, so the child
   gets `parent minus elevation` -- **I-2** (`<= parent`, never grown), **I-43**
   (Linux's inherit shape; no authority the parent lacked; elevation never
   propagates). Native fork keeps `CAP_NONE` (unchanged).

### The cap-conferral chain (I-2, each hop intersects)

joey grants `CAP_CSPRNG_READ` to `viv` (`run_viv_bundle(..., T_CAP_CSPRNG_READ)`
-- the new `extra_caps` param, 0 for every other gate); `viv` confers it on the
entrypoint when the bundle sets `org.thylacine.csprng: granted` (symmetric with
`org.thylacine.net`; `cap_mask` masks against viv's own caps, so viv passes on
only what it was granted); the forked git inherits it (wall 3). No annotation ->
container cap floor stays 0.

### Degradations (documented, none silent-wrong)

- `chdir`'s `ENOTDIR`/`EACCES` collapse to `ENOENT` (the native handler's bare
  -1; SUCCESS path exact; revisit on a richer native chdir errno).
- `fchmodat` drops setuid/setgid/sticky (T_WSTAT rejects them at v1.0; git's
  config modes never carry them).
- `getrandom`'s no-cap / not-seeded / fault all map to `EAGAIN` (a v1.0 backend
  cannot distinguish; Linux's "entropy not available" is the closest analog).
- git-as-a-real-user awaits A-3 (per-principal 9P ownership); milestone A is
  SYSTEM-owned.

### Witnesses

Kernel unit: `vivarium.git_chunk_rows` (all seven rows are `VIV_TIER2`, never
forward; the four sub-ceiling collision identities 48==NOTE_MASK / 49==
SPAWN_FULL_ARGV / 53==PIVOT_ROOT / 78==PCI_INFO made executable) +
`vivarium.faccessat_gate` (AT_FDCWD sign-extended AND bare-u32 -> TRANSLATED;
0, 5, -1 -> FORWARD). Boot gate: `do_git_probe_gate` (joey, SYSTEM,
boot-probe-gated) spawns `viv run /vivarium/git-probe` -> `GITPROBE-INIT` /
`-ADD` / `-DONE`; asserts the terminal marker, reports the first missing step,
SOFT-SKIPs without the static-git tarball, BOOT-FATAL when present.

### Deferred to §6.27 (commit + clone)

`commit`/`clone file://` open `.git/logs/HEAD` `O_APPEND`, which the phenotype
`openat` does not admit (no kernel append mode). A phenotype `O_APPEND`
(open-at-EOF, sound single-threaded; for git's absent reflog the open need only
RESOLVE->ENOENT instead of FORWARD->ENOSYS) is the next chunk.

## O_APPEND (FS pass-through) + pread64/pwrite64: git commit + clone (§6.27; aux, 2026-08-26)

Makes the FULL git chain run under the phenotype: init/add/commit/log/clone
file:///verify, reflogs ON. Two small walls, neither the kernel-append-mode the
§6.26 deferral feared.

### O_APPEND is delegated to Stratum, not implemented in the kernel

Stratum already enforces O_APPEND: its 9P server stores the fid's open flags at
Tlopen and, on every Twrite to an O_APPEND fid, ignores the client offset and
writes at the current size (`server.c` h_write; `_Static_assert(STM_9P_O_APPEND
== O_APPEND)`). So the kernel PASSES the flag through:

| Site | Change |
|---|---|
| `syscall.h` | NEW `SYS_WALK_OPEN_OAPPEND` 0x40 + `_Static_assert` it is inside the mask; `SYS_WALK_OPEN_OMODE_VALID` 0xB3->0xF3; the SYS_PWRITE append-stance note (kernel delegates; no append mode) |
| `dev9p.c` | `omode & SYS_WALK_OPEN_OAPPEND -> flags |= 02000` in BOTH `dev9p_open` AND `dev9p_create` (the reflog is O_CREAT|O_APPEND, so the create path matters) |
| `vivarium.c` | `VIV_OMODE_APPEND` 0x40; `VIV_OPENAT_ADMITTED += VIV_O_APPEND`; the O_APPEND arm in BOTH openat decides (plain drops it under dirreq -- append on a read-only dir is vacuous; create sets it unconditionally) |

The kernel write path is UNCHANGED (SYS_PWRITE still writes exactly at `off`);
the FS does the positioning. For an append fd `c->offset` is advisory (Stratum
ignores it): correct for a write-only append (git's reflog). The divergence is
NOT vague -- it is off by exactly the file's PRE-OPEN size S (R1-F4): open an
existing size-S file O_APPEND (`c->offset`=0), write n bytes -> data lands at
[S, S+n) but `lseek(SEEK_CUR)` returns n, where Linux returns S+n. Only a
tell-after-append consumer (log rotation, offset indexing) or an O_RDWR|O_APPEND
read sees it; git does not. Conversely `pwrite` on an O_APPEND fd APPENDS
(ignoring its explicit offset) -- Stratum's per-fid override reproduces exactly
Linux's documented `pwrite(2)` O_APPEND behavior.

**The write-behind anchor EXCLUDES append fds (R1-F1).** `dev9p_create`/
`dev9p_open` otherwise set the anchor (`wb_eligible`/`wb_base`) on create/OTRUNC,
but an append fd is now excluded -- pure write-through -- so the kernel's wb
"write at end" and Stratum's EOF-override do not co-exist on one fd. Without the
exclusion, a CONCURRENT-append flush would install own-pages at `wb_base` offsets
the server relocated to a different EOF, fabricating cached content (an I-38
violation the prosecutor caught; single-writer git was correct either way,
cursor==EOF on a fresh file). Write-through is larder-coherent for append: the
per-write attr-invalidate kills a stale size, append only extends, and the
extension range was never cached.

### pread64/pwrite64 (67/68): the clone pack read

git's `index-pack` reads the fetched pack via `pread`; untranslated, clone died
with `error reading from ...pack: Function not implemented`. `pread64(67)`/
`pwrite64(68)` have the exact `(fd, buf, count, offset)` shape of `SYS_PREAD(85)`/
`SYS_PWRITE(86)`, so they are pure T1 renumbers (no shell). Sub-ceiling,
colliding with the native LOOM pair (67=SYS_LOOM_REGISTER, 68=SYS_LOOM_ENTER);
the collision argument is the read/write renumbers' damage-envelope -- a renumber
runs the native handler with the caller's OWN args + rights, and a mis-declared
LOOM caller's loom handle is not a RIGHT_WRITE Spoor so SYS_PWRITE fails clean.

### Witnesses

Kernel unit: `vivarium.openat_domain` (O_WRONLY|O_APPEND -> OWRITE|OAPPEND 0x41) +
`vivarium.openat_create_domain` (O_CREAT|O_WRONLY|O_APPEND admitted -> the omode
bit) + `vivarium.t1_renumbers` (pread64->SYS_PREAD, pwrite64->SYS_PWRITE). Boot
gate: `do_git_probe_gate` now asserts the FULL chain (GITPROBE-INIT/ADD/COMMIT/
LOG/CLONE/VERIFY/DONE), reflogs ON so the reflog append exercises the O_APPEND
path end to end.

### What this is NOT

No kernel append mode, no new write-path mechanism, no ABI break (the omode bit
is additive; a native open that does not set it is unaffected). The arm carries
two flags/numbers to machinery that already exists (Stratum's server-side append
+ the native pread/pwrite handlers).

## Terminal control: ioctl termios/winsize + session/pgrp (C2-k1b/k2; aux, 2026-09-01)

Milestone C2's kernel half -- what makes a phenotype process INTERACTIVE on the
console. `isatty()` (which musl implements as `ioctl(fd, TIOCGWINSZ, &ws)`) was
false on every fd because there was no `ioctl` row at all, so git dropped to
non-interactive defaults and no Linux TUI could run. Thylacine has **no ioctl
surface and no kernel termios struct** -- terminal control is file-shaped (the
cons/pts 5-flag `consctl` grammar) plus the discrete session/pgrp syscalls
(89-92). The phenotype runs UNMODIFIED binaries, so the translation the pouch
PTY-3 patch does in musl must happen kernel-side.

### The ioctl shell (C2-k1b, `05e91a06`)

`viv_ioctl` (`kernel/syscall.c`) is the `viv_tier2` arm for `ioctl(29)`. It
resolves the fd rights-agnostic (`sys_lookup_spoor(p, fd, 0)` -- Linux terminal
ioctls do not check r/w mode, and `isatty()` probes fd 1/2, often write-only),
so `EBADF` beats `ENOTTY` (the fd is validated before the request is classified
by the pre-existing pure `vivarium_ioctl_decide`). The cons-fd predicate is
exactly what `isatty()` relies on -- `spoor_stat_native` + the char-device
posture + `CONS_STAT_QID_FLAG` (bit 41) -- so it covers BOTH cons doors (devcons
and the devdev `/dev/cons` leaf) and cannot drift.

On a cons fd, `viv_ioctl_cons` serves the family off the kernel-owned `g_cons`:
- `TCGETS` -- `viv_cons_to_linux_termios` maps the cons 5-flag word (`CONS_ICANON/
  ECHO/ISIG/ICRNL/ONLCR`) to a Linux `struct termios` (36-byte asm-generic
  layout, `_Static_assert`ed), fills the `INIT_C_CC` control-char baseline, and
  copies out (whole-span `sys_validate_user_buf` first -- the N-5 uaccess class).
- `TCSETS{,W,F}` -- copies in the termios, and `viv_linux_termios_to_grammar`
  builds a DETERMINISTIC 5-flag `+/-` grammar (every flag explicit, so the result
  is independent of the current mode) fed to the ONE production setter
  `cons_set_mode_cmd(g, n, /*allow_flags=*/true)`, reusing its atomic apply +
  ICANON-clear-delivers-line + poller wake. `onlcr` requires `OPOST` (Linux
  translates NL only under it).
- `TIOCGWINSZ` -- `cons_winsize_get` -> `struct winsize` (8 bytes, asserted);
  this is isatty's probe.
- `TIOCSWINSZ` -- `EPERM` (console geometry is physical, owned by the renderer;
  pouch-0029 parity).

The two PURE helpers carry the error-prone flag/grammar logic and are unit-tested
with plain structs + a behavioral round-trip through the real setter (the
getdents64-transform precedent). `T_E_NOTTY` (25, POSIX `ENOTTY`) was added to the
errno registry for the non-tty / unserved-request answer.

### Session + process-group control (C2-k2, `348e21b7`)

The Linux session/pgrp syscalls map to the native cores (89-92); arities line up.
`getpgid(155)`/`getsid(156)` are PURE T1 renumbers (the cores return the pgid/sid
or `-T_E_SRCH` = Linux `ESRCH`; pid 0 = self in both). `setsid(157)`/`setpgid(154)`
are TIER2 shells for ONE reason -- the errno: the native cores return `T_E_ACCES`
for what Linux reports as `EPERM` (setsid on a group leader; setpgid's
cross-session / session-leader / no-such-group contour), so the shells remap
`ACCES -> PERM` (INVAL/SRCH and the success value pass through). All four Linux
numbers sit above `VIV_NATIVE_CEILING`, so the renumbers cannot mis-dispatch.

### Degradations (documented, none silent-wrong)

- **pts fds answer `ENOTTY` (C2-k1c deferred).** A pts's line discipline lives in
  the ptyfs userspace server; reaching it from the kernel (walk `/dev/pts/<N>ctl`
  + 9P I/O) is the deferred half. No regression -- ioctl was `ENOSYS` before.
- **The console termios is GLOBAL** (single physical console; per-fd termios is a
  fiction Thylacine does not maintain). A phenotype `TCSETS` mutates it for every
  console user -- faithful to Linux's shared-tty termios, and ldisc flags are not
  a security boundary (I-27 SAK/attach is independent). The background-`tcsetattr`
  `SIGTTOU` strictness is a fidelity gap, not a soundness one.
- **The termios subset is 5 ldisc flags** (the pouch PTY-3 honesty); other bits
  are 0 on get and ignored on set, which round-trips a guest's
  tcgetattr/modify/tcsetattr for the flags we implement.

### I-43 / I-20

A phenotype gets ABI SHAPE, not AUTHORITY: the ioctl shell touches only the
caller's own cons fd + the global ldisc (no privilege bit), and setsid/setpgid/
get* call the SAME native cores a native Proc uses -- no widening.

### Witnesses

Kernel units: `vivarium.ioctl_termios_map` (the pure cons<->Linux termios map,
each flag + the c_cc/c_cflag baseline), `vivarium.ioctl_grammar_roundtrip` (the
grammar via a behavioral round-trip through the real setter, incl. the
ONLCR-requires-OPOST gate), `vivarium.ioctl_dispatch_ebadf` (the fd-first
ordering: EBADF beats ENOTTY), `vivarium.session_errno_remap` (setsid/setpgid
ACCES->EPERM + the INVAL passthrough discriminator). The full cons serve (a real
cons fd + mapped user memory) is covered by the in-guest viv-run E2E.
