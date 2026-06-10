# HOLOTYPE RW-5 — Authority (handles / caps / legate / perm / identity)

**Tier**: STANDARD. **Status**: CLOSED. **Verdict**: the authority surface is
SOUND. 0 P0 / 0 P1 / 1 P2 / 8 P3 — all fixed in-arc. NOT a dirty close (P1+P2
= 1 < 6; every fix is a local atomic upgrade / hardening / comment / assert,
no lock-order or wait/wake-protocol change).

The headline is the recurrent multi-thread-lift theme this arc keeps surfacing
(RW-2 SA-F1-class, RW-4 SA-F1, A-4c2 F1): **A-4a made `p->caps` a
cross-thread-mutable word — `proc_become_legate` ORs it atomically — but the
*other* writer (the hostowner redeem) and the *readers* (perm.c / devproc.c /
peer_snapshot) were never atomized to match.** Strictly fail-safe (a cap can
only be lost, never spuriously granted), so P2, not the P1 the analogous
console-bit corruption (a4c2 F1) earned. The I-2 / I-22 / I-25 / I-26 / I-27
spine is intact under prosecution.

---

## Scope + method

Four Fable-max `holotype-reviewer` prosecutors, split by sub-surface (never by
lens), each carrying S(oundness) + C(ompleteness) + local-T, plus an Opus
self-audit on the whole surface (the audit-in-flight discipline). All four
reviewers self-reported `claude-fable-5` at BOTH ends (no mid-run fallback).

| Reviewer | Sub-surface | Files |
|---|---|---|
| R1 | Handle table (#844 DELTA) | `kernel/handle.c` + `.h`; the #926 at-exit close interaction |
| R2 | Caps + legate + identity plumbing | `caps.h`; `proc.c` (rfork I-2 strip, `proc_become_legate`, scope teardown, `proc_apply_identity`, peer-snapshot); the spawn-identity gate |
| R3 | The `cap` device (grant/redeem) | `devcap.c` + `.h`; the `SYS_CAP_*` handlers |
| R4 | rwx `perm.c` + the `/proc` kill gate | `perm.c` + `.h`; `devproc.c` |

Closed-list preambles supplied: `audit_844`, `audit_a4a`, `audit_a4b`,
`audit_a4c2`, `audit_828`. Per the RW-4 / RW-5 sequencing, RW-3 had already
cleared the syscall *boundary* (the two-axis gate from syscall entry, the I-2
strip, the OEXEC leak); RW-5 prosecuted the handle-table internals + the
cap/clearance/legate lifecycle + perm.c internals + the devcap/devproc gate
logic.

**Convergence**: R2, R3, and the self-audit *independently* found the same
headline defect (`devcap.c:327`), and R2+R3 *independently* rated it P2 (not P1)
with the same fail-safe reasoning. R1 corroborated the self-audit's KOBJ_SRV
finding. Four prosecutors → four overlapping-but-distinct finding sets.

---

## Findings + dispositions

| ID | Sev | Area | Finding | Disposition |
|---|---|---|---|---|
| F1 | **P2** | devcap caps OR | `devcap.c:327` hostowner redeem `writer->caps \|= to_or` is a non-atomic RMW racing `proc_become_legate`'s `__atomic_fetch_or`. The hostowner arm runs AFTER the one-shot consume freed the grant slot + AFTER the lock release, so corvus can re-register a CLEARANCE grant for the same stripes and a SIBLING thread redeem it (atomic OR), which the resumed plain STORE clobbers → lost cap. Fail-safe (cap lost, never spuriously set; contrast a4c2 F1's I-27 break = P1). Triple-converged (R2-F1 = R3-F1 = self-SA-1). | **FIXED** — `__atomic_fetch_or(ACQ_REL)`, matching the clearance path |
| F2 | P3 | caps readers | Plain reads of `p->caps` on the cap gates race the legate atomic OR — `perm.c:27/84/88` (R3/R4), `devproc.c:488` (devproc_kill_authorized), `proc.c:1086` (peer_snapshot_cb). Benign on aligned ARM64 (single-copy-atomic load → old-or-new, fail-safe stale=deny), C11-UB / TSan-positive. R3 rated the readers P2; **adjudicated DOWN to P3** per R2's analysis (the readers cannot lose anything; only the writer F1 can). | **FIXED** — `__atomic_load_n(ACQUIRE)` at all five sites |
| F3 (R4-F1) | P3 | perm.c | `perm_check(p, st, 0)` returns 0 (ALLOW): `(bits & 0) == 0` is vacuously true, AND it short-circuits before the cap-bypass. Unreachable today (all 8 sites pass non-zero `want`; `perm_want_for_omode` never returns 0) but fail-OPEN default polarity on a security gate. | **FIXED** — deny `want==0` before the DAC-override; regression `perm.check_want_zero_denied` |
| F4 (R4-F2) | P3 | perm.c | `perm_wstat_check` gates only {MODE,UID,GID}; a future `T_WSTAT_*` bit passes ungated (guarded only upstream at `syscall.c`). | **FIXED** — reject `valid & ~T_WSTAT_VALID`; regression `perm.wstat_rejects_unknown_valid_bit` |
| F5 (SA-2) | P3 | caps invariant | No `_Static_assert` pinned `CAP_GRANTABLE_CLEARANCE ⊆ CAP_ELEVATION_ONLY` (nor `CAP_GRANTABLE`). The load-bearing I-25 "scope members stay UNELEVATED" guarantee rests on a clearance conferring only rfork-stripped caps; R2 verified the value by hand. A future fork-grantable cap added to either set would silently break I-25. | **FIXED** — both asserts added in `devcap.h` |
| F6 (R1-F1/SA-3) | P3 | handle.c | The KOBJ_SRV `handle_acquire_obj` comment claimed "never reached" — false since #844 (`handle_get` reaches it). The no-op is balanced only because post-stalk-3c a KObj_Srv handle is always a SERVICE (no-op release); a future SrvConn-backed one would be an acquire-noop/release-unref UAF. | **FIXED** — comment corrected to name the latent asymmetry |
| F7 (R1-F2) | P3 | handle.c | `handle_alloc` guards the consumed-ref convention for KOBJ_BURROW (handle_count>0) but not KOBJ_LOOM — defense-in-depth gap parallel to burrow; dormant (sole loom caller correct). | **FIXED** — added a KOBJ_LOOM refcount>0 guard arm |
| F8 (R3-F3) | P3 | dev.c | `dev.c:124` registration comment says devcap `dc='C'`; it is `'k'`. | **FIXED** — comment corrected |
| F9 (R3-F4) | P3 | syscall.c | The `syscall.c:209/251` comment "there's no cross-thread writer of p->caps" is now false (`proc_become_legate`); it is the exact rationale that left F2's plain readers looking safe. | **FIXED** — comment corrected |
| R2-F3 | P3 | legate | A second clearance redeem rewrites `valid_until`/`scope_id` as an uncoordinated scalar pair (deeply benign). | **REGISTERED** — folds under the deferred a4a-F2 double-redeem tidiness |

### Registered (non-soundness / owed; not fixed in-arc)
- **The `proc_caps_load()/proc_caps_or()` accessor sweep** (T/H): close the
  mixed-atomicity class permanently — every `p->caps` access through one pair of
  accessors, mirroring RW-4's mounts[]/root_spoor lock sweep. The F1/F2 fixes
  close the *known* racy sites; the accessor sweep makes it structural. → task #23-class.
- **The deterministic multi-thread TSan caps harness** (a two-thread-same-Proc
  redeem-vs-`perm_check` interleaving) — the witness F1/F2 structurally lack; the
  same shared multi-thread-test debt as the #844/#841 multi-in-flight harness. → SA-class tracked.
- **R4 horizon H4**: the a4b deferrals re-checked + still correctly scoped to the
  future `/proc`-mount chunk — the SYSTEM-owns-SYSTEM owner-axis breadth, PID-1
  unkillability, and the `devproc_stat_native` lockless-`proc_find_by_pid` window.
  Live only once `/proc` is bound into a territory (it is `dev_register`'d but
  mounted nowhere at v1.0).

---

## Verified SOUND (do not re-prosecute without new code)

**Handle table (R1 + self).** The #844 verified-sound set still holds; the three
deltas are sound: KOBJ_LOOM get/put balance (`loom_ref`/`loom_unref`, non-dup),
NoSrvSpoorDup (dc=='s' reject, dev9p dc=='9' stays dup-able), and the **#926
at-exit lockless-teardown premise** (`thread_count==1` is a stable read implying
no live peer; no production path touches a foreign ALIVE Proc's table; no
`/proc/<pid>/fd` surface yet). Lock serializes alloc/close/get/dup; every
under-lock acquire is non-blocking (`loom_ref` is a bare atomic fetch_add;
`burrow_ref` is the one acyclic lock-under-lock); releases run outside the lock.
KOBJ_SPOOR get-borrow + concurrent close clunks exactly once. The future
`/proc/<pid>/fd` footgun is in-code (handle.c:296-300) — that sub-chunk MUST add
the table lock to `handle_table_free`.

**Caps + legate (R2 + self).** The I-2 elevation-only strip
(`(parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY`) is unconditional on every
fork/spawn path; the only non-fork caps write is the cap-device self-elevation.
Cap-mask bounding is airtight (register gates ⊆ CAP_GRANTABLE / CLEARANCE +
corvus-only grant caps; redeem self-restricts subset/exact; no redeem can OR a
fork-grantable or self-amplifying cap). `CAP_GRANTABLE_CLEARANCE ⊆
CAP_ELEVATION_ONLY` (now compile-pinned) → scope members are always unelevated →
the F2 orphan cannot strand an *elevated* member. Legate teardown fires on every
death path (the a4a-F1 zombie chokepoint) + on `valid_until` expiry; both
triggers acyclic; `proc_group_terminate` never re-takes the proc-table lock;
except/kproc/scope-0 guards present. `legate_scope_alloc` nonzero + 0-skip.

**Identity (R2 + R4 + self).** No EL0-reachable ALIVE Proc carries
`principal_id == PRINCIPAL_INVALID(0)`: rfork copies the inherited principal at
`proc.c:676` *before* the publication `proc_link_child` at `:743` (both under
`g_proc_table_lock`), so the transient-INVALID window is never table-visible;
`proc_apply_identity` extinctions on INVALID/SYSTEM and runs only in the child's
own thunk pre-userland; kproc is stamped SYSTEM. So neither `perm_check`'s owner
branch nor `devproc_kill_authorized`'s owner axis can falsely fire on a
principal-0 pair. `spawn_identity_value_ok` rejects INVALID *and* SYSTEM on the
principal, primary_gid, AND every supp_gid (A-1a R1 F1) → no GID_SYSTEM smuggled
into a user's supplementary set. peer_snapshot leaks only scalar values under the
lock (no Proc-pointer UAF), fail-closed on stripes==0.

**devcap grant/redeem (R3 + self).** Single locked lookup reads `kind` atomically
(no peek/redeem TOCTOU); one-shot consume; cross-stripes reject (stripes==0
sentinel); self_restriction subset-only without consume-on-failure; console gate
on hostowner only (clearance is corvus-side auth, by design); total field reset
on re-register (no kind confusion); `valid_until` saturating add (no wrap-to-0
alias); `cap_proc_exit_notify` complete on both ZOMBIE paths with
monotonic-never-reused stripes making any leak inert; leaf-ref aux lifecycle
(no double-free, fail-loud corruption guard); exact-length write discrimination
(every `le64` in-bounds); write-only open; thin syscall forwarders deferring all
gates. The "clearance security == corvus integrity" model + the a4a-F2
double-redeem deferral remain signed-off scripture.

**perm.c + /proc kill (R4 + self).** Owner-first POSIX (an owner judged on owner
bits only, even when group/other grant more); the DAC-override is exactly
`(CAP_HOSTOWNER|CAP_DAC_OVERRIDE)`. `perm_wstat_check`: chmod=owner|HOSTOWNER,
chown=HOSTOWNER|CHOWN (no give-away), chgrp=(owner & member)|chown-any.
`proc_in_group` OOB-safe (count clamped). `perm_want_for_omode`/`rights_for_omode`
coherent (grant ≤ check for every omode incl. OTRUNC; the OEXEC RW-3 R3-F1 fix
present). `devproc_kill_authorized` = owner OR `(HOSTOWNER|KILL)`, checked
DIRECTLY — never via `perm_check` — so `CAP_DAC_OVERRIDE` is genuinely not a kill
axis (I-26 orthogonality; the bits are disjoint). Kill walk: kproc
double-guarded unkillable; non-ALIVE refused; resolve+authorize+terminate fully
under `g_proc_table_lock`; no pid-enumeration oracle. `parse_ctl_verb` bounded by
`n` (no OOB; kill != killgrp). devproc is `dev_register`'d but mounted into no
territory → the gate is kernel-internal exactly as a4b documented.

**proc_flags discipline (self).** Every `proc_flags` RMW is atomic (A-4c2 F1
held): the INTR latch (notes.c), console/may_post/self_managing/legate_root
(proc.c), MLOCKALL/NODUMP/NOTRACE (syscall.c). No torn-RMW regression.

---

## Posture

- default build **816/816** PASS (+2 from RW-4's 814: the two new perm
  regressions, both non-vacuous — fail pre-fix).
- SMP soundness gate (default+UBSan × smp4/smp8): **0 corruption** — the witness
  for the F1/F2 caps-atomicity fixes (the deterministic multi-thread harness is
  owed). Boot OK + login + legate E2E intact, 0 EXTINCTION.
- Reviewers: 4× `claude-fable-5`, MODEL start==end on all four (no fallback).
- NOT a dirty close → no round-2 required.
