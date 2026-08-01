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
unassigned (the highest assigned native number is 256, sparsely), so natively
they reach the dispatcher's `default:` and answer `-1`. The argument still holds,
but by a different clause than the collision table's: a mis-declared Proc issuing
222 now receives an anonymous mapping where it would have received `-1`, and that
is not new authority because **both targets are ungated syscalls that operate
only on the caller's own address space** — `SYS_BURROW_ATTACH_LAZY` (83) and
`SYS_BURROW_DETACH` (38) require no capability and the Proc could call either
directly. `mprotect` dispatches nothing at all. Arity: `mmap` supplies six
arguments and the shell reads exactly `args[0..5]`; `munmap` supplies two and the
shell reads `args[0..1]`.

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

Three impure shells, one per translator, all in `kernel/syscall.c` (the pure
halves stay in `kernel/vivarium.c` so they remain unit-testable with no kernel
plumbing):

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
- **`munmap` (215) → `SYS_BURROW_DETACH`**, exact-match subset. This row has no
  pure `_decide` because its domain is a question about *state*; the resolution
  is that it needs none, since `sys_burrow_detach_for_proc` already enforces the
  match. Attempt, and read the answer.
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
lazily-allocated per-`Proc` `struct viv_sigtab` (`Proc.sigtab`, freed at
`proc_free`, not `rfork`-inherited — the `handler_va` precedent), and an ignored
signal's note is then **discarded at generation** inside `notes_post`, exactly as
Linux discards it, returning success because Linux's `kill()` to a process
ignoring the signal succeeds.

Post-time and not delivery-time is the load-bearing choice. An ignored note that
reached the queue would occupy one of 16 slots, would arm the LS-5c terminate
latch (an ignoring Proc has no handler and is not self-managing, so it passes
every arm gate), and would leave blocked threads unwinding `*_INTR` until the
EL0-return tail got round to dropping it. Never posting touches none of that.

**A real handler still declines**, and that is deliberate rather than
unfinished: the Tier-1 frame that would call it is V-6c, and accepting an install
we would never honour is the silent mistranslation §4 forbids — worse than
`ENOSYS`, because the guest would believe it is protected. One line in
`vivarium_sigaction_decide` moves when delivery lands.

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
boundary, and an EMPTY record chain is the truthful report -- note delivery does
not save Q0-Q31 (task #96), so an FPSIMD record would claim state that is not
there.

**Two deliberate delivery behaviours beyond "call the handler".** A `SIG_IGN`
disposition drops a note that was already queued when the disposition changed
(Linux discards pending signals on `SIG_IGN`; the V-6b post-time hook cannot
catch one that arrived first), and a `SIG_DFL` whose Linux default is *ignore*
(SIGCHLD/SIGWINCH/SIGCONT) is dropped rather than left in the ring -- a Linux
guest has no notes fd, so nothing would ever consume it and the queue would
fill. `SA_RESETHAND` is honoured: the disposition returns to `SIG_DFL` before
the handler is entered.

**Proving it in-guest needed the one signal a v1.0 guest can raise.** `kill`,
`tkill` and `clone` are not table rows, so a Linux guest can signal neither
another Proc nor itself through the obvious route -- and cannot spawn a thread
to race its own disposition table either, which is what makes the lock-free
`viv_sigtab` sound today (the only cross-thread reader is `notes_post`'s
`SIG_IGN` hook, and it touches one naturally-aligned `u64`). What remains is
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

Landed as honest declines with their reasons in the table: `setsockopt`/
`getsockopt` (`/net` exposes no option surface, and answering "success" to a
`TCP_NODELAY` nothing honours is the silent lie this tier exists to prevent),
`socketpair`, `sendmsg`/`recvmsg` (scatter-gather plus `SCM_RIGHTS`, which is
I-4's domain). `bind`/`listen`/`accept` are V-5b; `shutdown`/`getsockname`/
`getpeername`/`sendto`/`recvfrom` are V-5a's remainder.

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

Every earlier V-table row is above 100, the highest assigned native syscall
number, so ARCH section 25.4's mandated collision re-check was discharged by
construction. These two are not: **72 is `SYS_GETPID` and 73 is `SYS_GETUID`.**

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

Unreachable rather than untested: no clone/fork/execve number is a table row, so
a PHENO_LINUX Proc cannot create another Proc at all. Task #93 is what makes it
reachable and where the copy belongs. Pinned in a comment beside the line whose
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
