# HOLOTYPE RW-3 — Exception entry + syscall surface (FULL)

The privilege chokepoint: every syscall, IRQ, fault, and EL0 entry/exit crosses
this surface, and userspace-controlled arguments enter the kernel through the
68 syscall handlers. FULL tier — read every line; assume prior reviews missed
things.

**Scope.** Exception entry (`arch/arm64/vectors.S`, `start.S`, `userland.S`,
`context.S` trampolines, the #713 eret windows), `arch/arm64/exception.c` +
`fault.c` + `uaccess.{S,c}`, the 5559-line `kernel/syscall.c` dispatch + every
handler's arg validation + rights gates, `kernel/perm.c`, and the ABI sweep
(`syscall.h` numbering, `errno.h`, `notes.h` snare family, `ERRORS.md`, the libt
+ libthyla-rs + pouch mirrors).

**Method.** Four Fable-max `holotype-reviewer` agents split by sub-surface
(never by lens): R1 arch-entry, R2 syscall-process/mem/handle/hw/loom-half +
the dispatch core, R3 syscall-fs/namespace/identity/cap/notes-half, R4 ABI
coherence. All reported MODEL `claude-fable-5` (R1/R2 start==end; R3/R4 echoed
only `MODEL(end)`, content Fable-grade + ABI/mechanical findings independently
verified). Plus an Opus main self-audit (run concurrently — the four highest-
stakes leads, all SOUND) and a focused round-2 prosecutor on the two chokepoint
fixes (CLEAN). Reports: `/tmp/rw3-r{1,2,3,4}-*.md`, `/tmp/rw3-round2.md`,
`/tmp/rw3-self-audit.md`. Closed list: `audit_holotype_rw3_closed_list.md`.

**Status: CLOSED `@01e1b40`.** 0 P0 / 0 P1 / 4 P2 (all FIXED) + 1 P3 code + the
ERRORS.md coherence cluster (FIXED) + H registerables. **The chokepoint is
SOUND** — the #713 trampolines, `el0_return_die_check` coverage, the uaccess
fixup, the I-2 cap strip, SYS_NOTED's restore, I-28 containment, and the two-axis
rights+perm gate all survived prosecution. The four P2s were latent or masked,
not actively broken; the most interesting (R3-F1) was a masking-bug stack hiding
behind a downstream Dev quirk.

---

## Findings

### Fixed in-arc (soundness)

**HT03.R1-F1 [P2] — `userland_enter` skips `el0_return_die_check` (I-24 gap).**
`arch/arm64/userland.S`. The one EL0-transition trampoline with no group-
terminate die-check, where its sibling `thread_user_trampoline` (context.S:300)
added one for exactly this case. Reachable **without /proc**: a child inherits
`legate_scope_id` at rfork; scope teardown / deadline-expiry calls
`proc_group_terminate` on a child still inside `exec_setup` on another CPU, which
then erets to EL0 and runs its own ELF entry — and even dispatches one full
syscall, since the SVC-tail die-check (exception.c:409) runs *after*
`syscall_dispatch` — before self-healing at the next trap. Bounded + self-healing
+ runs under the Proc's own unescalated authority → P2, not P1.
**Fix:** `bl el0_return_die_check` before the daifset window, entry_pc/user_sp
parked in callee-saved x19/x20 (survive the C call; zeroed in the existing GPR
sweep). The #713 masked ELR-set..eret window is byte-identical except the mov
sources; round-2 confirmed the sweep zeroes x30 (the new `bl`'s KASLR-revealing
return address) — no I-13 leak. *(Opus self-audit L1 flagged the gap; R1 proved
the reachability — the two-prosecutor value.)*

**HT03.R2-F1 [P2] — `sys_explicit_bzero` silent truncation (secret retention).**
`kernel/syscall.c`. The handler validated the full `len`, then `len = SYS_RW_MAX`
capped it, zeroed only the cap, and returned **0 (success)** — so a caller
scrubbing an oversize secret buffer kept the tail, and the kernel violated the
libthyla-rs `-1`-on-oversize contract. Dormant (all call sites <4096).
**Fix:** reject `len > SYS_RW_MAX` (matching SYS_PUTS/SYS_READDIR + the doc).
getrandom/READ/WRITE legitimately cap-and-return-a-count — left as-is.

**HT03.R3-F1 [P2] — OEXEC execute→read leak on the I-22 chokepoint (masked).**
`kernel/perm.c`. `perm_want_for_omode(OEXEC)` validated `PERM_X` while
`rights_for_omode(OEXEC)` minted a `RIGHT_READ` handle — the grant exceeded the
check, violating perm.h:47's own invariant and falsifying an A-3 closed-list
SOUND claim (R3 re-prosecuted it per the mandate and found it false). A
**masking-bug stack**: masked today only because `dev9p_open` forwards
`omode & 3 = 3` to `lopen` (server-rejected), so the readable handle is never
minted; the obvious "make exec-opens work" change would *unmask* a
read-any-`--x`-file bypass. **Fix:** `perm_want_for_omode(OEXEC) = PERM_R |
PERM_X` (require read to match the read-capable handle). Regression
`perm.oexec_no_read_leak` (denies an execute-only file under OEXEC; non-vacuous —
fails pre-fix).

**HT03.R4-F1 [P2] — err.rs `-1` → `NotPermitted` (the native EPERM alias).**
`usr/lib/libthyla-rs/src/err.rs`. `from_syscall_return(-1)` → `Error::from(1)` →
`NotPermitted`. Since nearly every kernel handler returns flat `-1`, **every**
native failure (missing file, denied write, bad fd) decoded as "operation not
permitted" — the native mirror of the kernel's `T_E_PERM=1` trap (pouch chose
neutral EIO; err.rs accidentally chose the loaded EPERM). **Fix:** special-case
`rc == -1` → `Error::Io` (matching the pouch `-1` → EIO boundary). `errno.h`
forbids handlers returning `-T_E_PERM`, so a kernel `-1` never means EPERM. The
systemic fix (the per-cause `-T_E_*` rollout) is registered (R4-F7 re-vote);
`rand.rs`'s getrandom-local `-1` → `NotPermitted` is a documented, alloc-smoke-
relied-on special case — left as-is.

**HT03.R2-F2 [P3] — `sys_thread_exit` `__builtin_unreachable`.** `kernel/
syscall.c`. The noreturn handler used `__builtin_unreachable()` where the
dispatch case has no trailing `return`; a (UB) return would fall through to
`SYS_NOTE_OPEN`. **Fix:** `extinction(...)` backstop matching the EXITS/
EXIT_GROUP siblings.

### Fixed in-arc (doc coherence)

**HT03.R1-F3 + R4-F2 [P3] — `docs/ERRORS.md` drift** (both reviewers,
independently). The multi-thread-fault carve-out described **dead behavior** —
it claimed a multi-thread Proc fault extincts the kernel, but RW-1 C-F1
(`2891bf2`) retired that branch (`proc_fault_terminate` now group-terminates via
#809/#811). **Fixed:** rewrote it to the as-built CLOSED behavior; appended the
missing `T_E_CANCELED=125` registry row; marked `snare:bus` RESERVED (no v1.0
emitter, like `snare:fpe`); corrected "asserts" → "rejects" on `notes_post`.
*(Residual minor: the boundary-table `r<=-4096` row nuance + the `sys_exits_
handler` file-location reference — tracked, low value.)*

### Registered (non-soundness; RW-13 / future)

| ID | Lens | Sev | Finding | Disposition |
|---|---|---|---|---|
| HT03.R1-F2 | C | H3 | `snare:fpe` never posted — EL0 FP traps fall to `default` → `snare:ill` (benign; wrong tag) | REGISTERED (v1.x wires EC_FP_* → snare:fpe) |
| HT03.R4-F7 | T | H3 | The `-T_E_*` rollout now has TWO degrading consumers (pouch EIO + the R4-F1 native alias); "no retrospective sweep" deserves a re-vote | REGISTERED — **user decision** (the systemic fix behind R4-F1) |
| HT03.R4-F3 | C | H4 | Identity-bearing adjacent-u32 pairs (TSpawnArgs principal_id@56/primary_gid@60; TSrvPeerInfo @24/@28) have size-only asserts — a silent transposition is privilege-relevant (latent; correct today) | REGISTERED (add full offset asserts; RW-8/RW-10) |
| HT03.R4-F4/F5 | C | H4 | Mirror hygiene (libt enum gap 66-70; `t_poll` x8=29 w/ nonexistent `T_SYS_POLL`; lib.rs stale 0x13 mask comment); patch-0006 stale prose | REGISTERED (doc follow-up) |
| HT03.R2-F3 | C | H4 | Stale `burrow_attach` comment (claims single-thread v1.0; pouch-threads landed) | REGISTERED (doc follow-up) |
| HT03.R2-H4 / R1-H4 | T | H4 | Spawn-entry convergence on FULL_ARGV; per-Proc rlimits; word-granular `copy_*_user`; sorted+bsearch fixup table | REGISTERED RW-11/RW-12 |
| HT03.OBS-1 | C | H4 | `exec.h:245` decl of `userland_enter` lacks `__attribute__((noreturn))` (machine-code-enforced anyway) | REGISTERED (round-2; doc follow-up) |
| HT03.OBS-2 | C | H4 | `rand.rs` documents `Error::BadAddress` reachable but maps every rc<0 → NotPermitted (R4-F1 class, local wrapper) | REGISTERED (round-2; doc follow-up) |

---

## Verified SOUND (do not re-prosecute without new code)

**Privilege boundary** (R1 + Opus self-audit):
- **#713 eret-window**: only 3 eret-to-EL0 sites (`userland_enter`,
  `thread_user_trampoline`, `KERNEL_EXIT`); both trampolines mask DAIF before
  `msr elr_el1`; `KERNEL_EXIT` is always reached IRQ-masked. Eret census
  complete — no other EL0-entry path lacks the discipline. (Post-RW-3:
  `userland_enter`'s die-check runs *before* the mask, IRQs-on, noreturn if it
  fires — window unchanged.)
- **`el0_return_die_check` coverage (I-24)**: on every return-to-EL0 sync path
  (FAULT_HANDLED tail exception.c:355; syscall-return tail :409), the IRQ-from-
  EL0 tail (vectors.S:257), and now the `userland_enter` first-entry. Die-check-
  before-notes ordering correct.
- **uaccess fixup + the alignment-DoS (disproven)**: recovery is PC-keyed +
  conservative (a stray non-uaccess kernel deref of a user VA still extincts).
  `SCTLR_EL1.A=0` system-wide (init `0x30D00800`; `mmu_enable` ORs only M|C|I,
  mmu.c:729), so an unaligned `uaccess_*_u32` to user Normal memory does not
  fault; `torpor_wait`/`set_tid_address`/`thread_spawn entry_va`/`NOTED
  handler_va` validate 4-byte alignment anyway.
- **I-13**: both trampolines zero x0–x30 (incl. x30 — the bl return addr) + SP_EL0
  + TPIDR + SPSR before eret. **#806** re-entrancy guard set-before-deref, per-CPU,
  wild-MPIDR-clamped. **demand_page_locked** holds `vma_lock` across
  lookup→resolve→install; access-type vs VMA prot (W^X at fault time);
  burrow-offset bound. **cpu_switch_context** saves/restores the full register
  set (ISB after FPCR + TTBR0); ASID pre-hook ordered before it. SPSel never
  cleared → the 0x000/0x080 group is unreachable-by-construction.

**Capability + identity** (R2 + R3 + self-audit):
- **I-2**: `rfork_internal` (proc.c:663) `(parent_caps & caps_mask) &
  ~CAP_ELEVATION_ONLY` — unconditional strip, child ⊆ parent, `rfork` →
  CAP_NONE. No spawn path grows caps or confers an elevation-only/CONSOLE_TRUSTED
  bit a caller lacks. The named A-1a hostowner hole stays closed.
- **I-5/I-6**: spawn fd-inheritance is KOBJ_SPOOR-only; DUP rights-subset.
- **#844** handle_get-snapshot-with-ref + handle_put-on-every-exit balanced
  across every traced handler (incl. od==nd / tx==rx aliasing). No UAF/leak.
- **SYS_NOTED** restores kernel-captured ELR/SPSR/regs (notes.c:852-857), not
  user-supplied; SPSR preserved EL0t; in_handler + N-3 nesting guards. Cannot land
  EL0 at an attacker state.
- **Loom** syscall boundary: setup copy-out rollback uses the kernel kp; I-30
  pin; non-dup/non-transferable; no lock nesting. **TORPOR_WAIT** register-then-
  observe (I-9) + the align/+4-straddle guard.

**Namespace + FS** (R3):
- **I-28**: `..` clamps at root across OPEN/WALK_OPEN/CHDIR/CREATE/RENAME/UNLINK/
  MOUNT; per-component X-search; the cwd-join is bounded + not a containment
  authority.
- **Two-axis (I-6/I-22)**: O_PATH born-R|W safe; WALK_CREATE W+X; RENAME/UNLINK
  W+X both dirs; WSTAT ownership policy (setuid/setgid/sticky rejected) — gated,
  no bypass except R3-F1 (fixed). **I-2 identity**: ATTACH_9P[_SRV] substitute the
  kernel principal_id; SRV_PEER value-captures before clunk. **I-19**: notes_post
  rejects snare:-prefixed names from userspace; kill non-catchable.

**ABI** (R4): the four-surface numbering map agrees on every defined number (68
live, zero collisions); NO kernel code returns `-T_E_PERM`; all 17 errno pins
POSIX-correct; pouch sentinel guards hold; `t_stat` 80 bytes on all 4 surfaces;
Loom 6c-F4 + 6d-F1 assert sets intact.

---

## Posture

`811/811` default boot (+1 `perm.oexec_no_read_leak`). Userspace compiles. **SMP
gate PASS — 0 corruption** (default+UBSan × smp4/smp8 N=10; the death-path
witness for the `userland_enter` I-24 change). Round-2 on the two chokepoint
fixes CLEAN (0/0/0/0). **CLEAN close** — round-1 was 0 P0 / 0 P1 / 4 P2 (all
fixed), not dirty by count, fixes non-invasive, round-2 confirmed.
