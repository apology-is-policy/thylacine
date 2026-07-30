# 145 — Vivarium: the phenotype and the syscall-entry branch

**Status**: as-built at V-2d. The declaration (`SPAWN_PHENO_LINUX`), the
syscall-entry branch, and the Tier-1/Tier-2 dispatch shells — including `mmap`
and `munmap` — are live and boot-gated. Sockets (V-5) and signals (V-6) are not
built; a call that needs them answers `-ENOSYS`. **V-3's supervisor is deferred,
and not merely unbuilt**: its sketched destination cannot serve the forwarded set
at all (§7 below, and `VIVARIUM.md` §4.1). Its shape is decided by V-5.

Design: `docs/VIVARIUM.md` (§4 the hybrid split, §5 the mechanism, §12.1 the
declaration rules, §8 invariant **I-43**). Invariant: `ARCHITECTURE.md §28
I-43`. Audit surface: `ARCHITECTURE.md §25.4` (the V-1b row) — the focused
round is **V-8**.

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
