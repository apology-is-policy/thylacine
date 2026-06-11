# HOLOTYPE RW-10 — Cross-cut: Consistency (rubric + invariant ledger + spec/doc drift)

**Status**: CLOSED. Reviewers: 4× Fable-max `holotype-reviewer` (A = ledger
I-1..I-12; B = ledger I-13..I-21 + I-31; C = ledger I-22..I-30 + spec-vs-code
drift; D = the consistency rubric sweep; all `claude-fable-5`, no fallback) +
the Fable main-loop self-audit. Full reviewer reports:
`/tmp/holotype-rw10-{A,B,C,D}.md`.

**Headline**: **zero soundness findings** — the entire §28 enforcement layer
is present, located, and tested; the drift is concentrated in the scripture
DESCRIBING it (15 X-lens findings: 12 fixed in-arc as trivially-mechanical
doc/test/tooling items, 3 registered → #56/#57/#58 + RW-13). The invariant
ledger (§1) is the table deliverable.

Tier: STANDARD (HOLOTYPE.md row RW-10). Tip at review start: `56d6412`.

**Named exemptions honored** (user-set 2026-06-10): the Loom ring transport +
the pouch byte-mode socket transport are deliberate Consistency departures;
`docs/manual/` is deferred (user 2026-05-31); the REFERENCE.md snapshot
staleness is known (#24); Roman vocabulary for the security subsystem is
deliberate (IMPERIUM-DESIGN.md).

---

## 1. The invariant ledger (I-1..I-31)

Verdict legend — **Enforced**: the mechanism the §28 row claims, located and
checked (VERIFIED / DRIFTED = mechanism present but the row describes it
stale / ABSENT = claimed enforcement does not exist / UNBUILT = whole
mechanism not in the tree). **Tested**: the suites/probes that exercise it
(NONE is recorded honestly). **Doc**: §28 row + cited spec artifact +
CLAUDE.md mirror currency. **Residuals**: carried items (task #N / register).
Full per-cell citations in `/tmp/holotype-rw10-{A,B,C}.md`.

| Inv | Enforced (where) | Tested (where) | Doc current? | Residuals |
|---|---|---|---|---|
| I-1 | VERIFIED — per-Proc Territory deep-copy clone `kernel/territory.c:100-152` + the RW-4 `ns_lock`; RFNAMEG unsupported (`proc.c:798`, matches spec note) | `test_namespace_fork_isolated` + mount-clone/destroy + in-VM /srv isolation | row OK; `territory.tla` DRIFTED (F-A4: stalk-2 Spoor-identity re-key + mount-graph cycle check unmodeled) | — |
| I-2 | mechanism VERIFIED — strip `proc.c:743`; devcap atomic-OR redeem | caps + devcap suites + legate E2E | row DRIFTED (F-A3: elevation-only is 4 bits, not just CAP_HOSTOWNER; clearance redeem deliberately NOT console-gated, argued at `devcap.c:269-276`) | #26 (caps accessor sweep) |
| I-3 | VERIFIED, stronger than spec'd — bind-graph DFS `territory.c:437` + unmodeled mount-graph cycle check `territory.c:587` | `cycle_rejected` + `mount_rejects_cycle` + buggy cfg | spec under-models the second check (part of F-A4) | — |
| I-4 | VERIFIED (vacuously) — NO transfer codepath exists at all; the positive 9P-transfer path is still future; spawn-endow is the only conveyance | spawn-fds rejection/subset tests + `handles_buggy_direct.cfg` | row OK; SPEC-TO-CODE claims a never-built `-ENOTSUP` stub (F-A5) | 9P-transfer = Phase-5+ seam |
| I-5 | VERIFIED — no transfer path + `handle_dup` non-transferable reject `handle.c:520` + partition static_asserts `handle.h:109-123` (4 partitions incl. KOBJ_LOOM) | hw dup-reject tests + 4 buggy cfgs | SPEC-TO-CODE says "three" partitions (F-A5) | — |
| I-6 | VERIFIED — dup subset check + spawn-endow parent-rights capture + A-3 omode narrowing | `rights_monotonic` + `child_rights_subset_of_parent` | row OK | — |
| I-7 | VERIFIED — #847 dual-count free decision under `v->lock` (`burrow.c:274-308`); #926 inversion safe under it | 30+ burrow tests + in-VM #926 regression + 3 buggy cfgs | row OK (`burrow.tla`); CLAUDE.md mirror still says `vmo.tla` (F-A7) | — |
| I-8 | cell DRIFTED — `sched.c:19-21` self-admits full EEVDF math never landed; dispatch is monotonic-vd_t FIFO-like | sched suite + ci-smp-gate + `scheduler_liveness` cfgs | row overstates "EEVDF deadline computation" | 2A-F6 → RW-13 (registered) |
| I-9 | mechanism VERIFIED verbatim — register-then-observe under `wait_lock` `sched.c:1339-1409` + LS-5c widening; `torpor_lock`; poll | rendez/pipe/poll/torpor/tsleep suites + `death_wake` cfgs | spec cell DRIFTED (F-A1): cites phantom `futex.tla`; omits `tsleep.tla` + `death_wake.tla` | #10 (deterministic multi-CPU cascade test) |
| I-10 | mechanism VERIFIED — `alloc_tag` index-reservation `9p_session.c:99-106` + #845 `awaiting_flush` no-reuse-until-Rflush | wrong-tag/flush/late-reply tests + `tag_collision` cfg | row DRIFTED (F-A2): "monotonic generation" never existed | — |
| I-11 | VERIFIED — `fid_bind`/`unbind` table `9p_session.c:67-92` + send preconditions | 5 unbound-fid/clunk tests + buggy cfg | row OK | RW-4 R3-F2 fid burn (#23) |
| I-12 | VERIFIED on 3 legs — PTE asserts `mmu.c:277-296` + `pte_violates_wxe` + ELF reject `elf.c:140-144,197` + W1.5 transient alias `mmu.c:861-880` | RW-1 sweep + W1.5 tests | "mprotect rejection" leg DRIFTED (F-A9: no such syscall exists — enforcement is the stronger structural absence + RW-only mints); audit tables cite phantom `mm/vm.c`+`mm/wxe.c` (F-A8) | — |
| I-13 | VERIFIED — split built `mmu.c:112-152`; high-half reject `mmu.c:1542,1670` | `test_proc_pgtable` ttbr0_swap + directmap + demand_page | row OK | — |
| I-14 | VERIFIED — Stratum-side by design; OS observation point = bounded Rlerror errno passthrough `9p_client.c:88-101` | synthetic `test_9p_session.c:373-389` | row OK | hostile-ecode bound untested (F-B7, H4) |
| I-15 | VERIFIED — GIC/virtio/PSCI/RAM/MMIO-reservations DTB-driven; 2 documented fallbacks (PL011 base `uart.c:56`, INTID 33 `uart.h:57`, both scripture-argued) | none (honest per row: code review + audit) | row's `arch/arm64/<platform>/` locus names a nonexistent dir (F-B5, H4) | #7 (dense-MPIDR map) |
| I-16 | mechanism VERIFIED — never-zero slide `kaslr.c:263-265`; seed chain `:225-241`; relocs `start.S:295-302` | `test_kaslr` mix64 + `test_devctl.c:158-164` | row path DRIFTED (F-B3): surface is `/ctl/kernel-base` (`devctl.c:256`), not `/ctl/kernel/base` | — |
| I-17 | enforcement ABSENT as claimed (F-B2) — "EEVDF deadline math" not in tree; `scheduler.tla` `LatencyBound` is eventual-progress (I-8's property), not the quantitative bound | — | row + spec/cfg labels mislabel eventual-progress as I-17 | 2A-F6 → RW-13 |
| I-18 | VERIFIED — both legs `gic.c:644-687` (v2 GICD_SGIR, v3 ICC_SGI1R+isb); single SGI INTID as-built makes reorder structurally trivial | `scheduler.tla` IPIOrdering + `buggy_ipi` cfg (version-agnostic) + smp tests | row OK | #7 |
| I-19 | VERIFIED — `q->lock` discipline + dual EL0-return-tail dispatch (`exception.c:357,411` → `notes.c:689`); §7.6.7 N-1..N-5 exists verbatim | 38 `test_notes_*` + `ls-5.exp` | row OK incl. honest "notes.tla planned then dropped" | — |
| I-20 | UNBUILT (F-B1) — no pty code (sole hit: `cons.c:9` deferral comment), no `specs/pty.tla`; LS-8/#952 records the deferral | — | row + ARCH §23.5:3248 (present-tense "covers") + ROADMAP:1214 read as enforced/spec'd | LS-8 (#952) |
| I-21 | VERIFIED — SPSel=1 on primary `start.S:181` AND secondary `start.S:570-576`, never lowered | `test_smp.c:106-109` asserts it naming I-21 | row OK; `sched_ctxsw.tla` models as-built + buggy cfg | — |
| I-22 | VERIFIED — `perm.c:27-34` ("a capability, NEVER an identity") + `perm.c:95-114` + `devcap.c:310` + `devproc.c:483-491` | `perm.*`, `proc_identity.*`, `devcap.use_gate_no_console` | CURRENT (IDENTITY-DESIGN §3.3/§8.2 verbatim-match) | #26, #20 |
| I-23 | VERIFIED as composition — `handle_dup` subset; corvus self-chroot `main.rs:404`; cooperative-confinement caveat still the as-built truth (no spawner-set-root, `syscall.h:1377-1378`) | `handles.*` + `territory.chroot_*` + A-1.7 joey E2E | CURRENT (ARCH §3.6 + detour-status A-1.7) | spawner-set-root = v1.x |
| I-24 | VERIFIED — CAS `proc.c:1882`; die-check `exception.c:356,410` + `vectors.S:257` + `userland.S:63` | `proc.group_terminate_smoke` + `wait_pid_for_*` + #811 set | spec column DRIFTED (F-C8): says prose-only while `death_wake.tla` is committed and ARCH §8.8.1 already cites it | #10 |
| I-25 | VERIFIED — `proc.h:374-376` fields; `devcap.c:231,269,296-299` grant→redeem→`proc_become_legate`; teardown walk `proc.c:1303-1309`; RW-5 subset assert | 9 `devcap.clearance_*` + `caps.rfork_inherits_legate_scope` | CURRENT | — |
| I-26 | VERIFIED — `devproc.c:485-491` two-axis at the write site; DAC_OVERRIDE deliberately not a kill axis | `devproc.kill_authorized_predicate` + 3 | CURRENT | R4-H4 /proc-mount horizon (#26) |
| I-27 | invariant text VERIFIED — attach-only redeem `devcap.c:310`; SAK = attach-only re-grant, owner→NULL unconditional (`proc.c:1015-1018`) | 10 `cons.*`/devcap tests | row + IDENTITY-DESIGN §9.8:1832-1836 DRIFTED (F-C12): both carry pre-`@2608c88` semantics (notify note + owner=corvus; owner/attach split + `SPAWN_PERM_CONSOLE_OWNER` unnamed) — the RW-7 doc sweep missed these two scripture sites | #35 |
| I-28 | VERIFIED — `stalk.c:125-131` amode whitelist; X-search `:193-200`; trail containment; PgrpMount devno key; #957's walk-crossing uses the same audited `cross_mounts` (does NOT falsify the row); LS-4 cwd-join handler-level | 16 `stalk.*` tests | row OK; STALK-DESIGN:76 "never consults mounts" superseded by #957, unannotated (F-C10b) | — |
| I-29 | VERIFIED-CURRENT — all cited cfg names exist verbatim; generalization invariants located (`loom_multishot.tla:34-58`, `loom_order.tla:33-47`); enforcement `loom.c:489` | loom suites + cfgs | rows CURRENT; but the suite's "pre-commit gate" has NO operational runner (F-C1/F-C2) | Loom-6 owed harness (delivered 6d); — |
| I-30 | VERIFIED-CURRENT — `live_sqe_reread`/`recheck_at_completion` cfgs exist; pins `loom.c:361,1045` | `loom.register_*`/`enter_*`/`dup_rejected` + `9p_client.loom_*_rejects` | CURRENT | — |
| I-31 | VERIFIED — row matches `asid.c` symbol-for-symbol (`:14-16,33,59-61,136-138`); ARCH §6.2.1 current | 4 `test_asid` tests + SMP gate | row's spec cell cites 1 buggy cfg; the set is FIVE (F-B4, H4) | #7 |

---

## 2. The consistency rubric (reviewer D; full detail `/tmp/holotype-rw10-D.md`)

1. **9P/file-first — PASS w/ residue.** The stalk-3c migration is exemplary
   (SYS 26/30/43 retired, numbers not reused). Every named deviation
   verified ARGUED in place: the `cap`-device syscall bridges (chrooted
   writers — the syscall.h comment now carries the current argument), Loom
   (exempt), fstat/lseek/wstat (POSIX arms), torpor, fd-first notes (with
   the `/proc` re-entry note), `SYS_PIVOT_ROOT` (heritage mount-family).
   Residue → F8 (the unbound §9.4 layout) + F9 (spawn).
2. **Per-Proc namespace — PASS except spawn** (F9): all five spawn variants
   resolve the binary via the flat global `devramfs_lookup`
   (`kernel/syscall.c:3611,3797,3867,3968,4400`), bypassing
   territory/stalk/X-search — documented as-built but never argued, no
   exec-from-namespace seam recorded. Everything else stalk-resolved;
   mount/unmount PATH-keyed; the SYS_BIND absence is recorded
   (STALK-DESIGN:530).
3. **Two-axis authority — PASS.** dev9p + devramfs enforce; devproc
   (`devproc.c:15,364`) + devsrv (`devsrv.c:789,819`) argued in-code; the
   remaining `perm_enforced=false` Devs are path-unreachable today (their
   forward obligation folds into F8).
4. **fd-first notes — PASS.** The `snare:` forge-reject holds
   (`notes.c:266`, `!synthetic` gate); no new name family bypasses the
   substrate. The reject was UNTESTED → F11 (fixed:
   `notes.snare_forge_rejected`).
5. **No ambient authority — PASS clean.** `PRINCIPAL_SYSTEM` is
   data/forge-reject only (`perm.c:27-28`, `proc.c:1231`,
   `syscall.c:4241`); no post-A-4 surface snuck in an identity-keyed
   privilege.
6. **Error-convention coherence — PASS w/ residuals.** Zero `-T_E_PERM`
   tree-wide (the known trap holds); the `-errno` family is exactly torpor
   + thread_spawn (the latter's raw literals → F12, fixed). #20 (the
   retrospective `-T_E_*` rollout) remains the pending USER re-vote — not
   reopened.
7. **ABI `_Static_assert` coverage — PASS except the ELF loader** (F13,
   fixed: 6 pins in `elf.h`). 9p-wire / Handle / PTE / FDT / pollfd /
   notes / loom / `t_stat` / spawn-args / `srv_peer_info` all present; the
   RW-3 offset cluster (#21) verified applied.
8. **Naming/style — PASS** → F15 (registered weighs: the Territory-vs-Pgrp
   internal split; LS-K's planned `id`/`whoami`/`date` file-shape homes).
9. **Doc-per-chunk currency — FAIL on the RW-fix-commit class** (F10, the
   9-item cluster — all fixed this close). The RW-3..RW-9 *fix commits*
   changed documented behavior without updating their reference docs; the
   verification pass surfaced a 10th sibling (the 36-irqfwd single-waiter
   caveat) — the same class, same fix.

---

## 3. Spec-vs-code drift

Reviewer C's sweep (full detail `/tmp/holotype-rw10-C.md`), converging with +
extending the main-loop self-audit (§3.1 below):

1. **The spec gate never landed, and the one runner cannot gate** (F-C1/F-C2,
   H2): `Makefile:59-64` (`make specs`) loops every `*.tla` through TLC piped
   to `tail -3` — errors are swallowed, the loop's exit status is `tail`'s,
   so it CANNOT fail; it errors on `sched_oncpu.tla` (per-option cfgs only,
   no bare `sched_oncpu.cfg`); and it runs ZERO of the **71 buggy cfgs** —
   every "buggy cfgs remain pre-commit gates" claim (CLAUDE.md, LOOM.md §7,
   the death_wake row) is manual-only discipline today.
   `specs/SPEC-TO-CODE.md:3` ("Phase 2 close adds the CI gate") + the ARCH
   §25.3 verification-cadence claim are both drifted-to-aspirational.
2. **Stale sections describing retired mechanisms** (F-C3, H2): the
   `scheduler.tla` section (P4-Ic6 text) affirms `g_bootcpu_idle` as the
   current deadlock path — retired by the SMP redesign (#863; survives only
   in explanatory comments); "try_steal line 273" is now `:788`. No
   supersession note pointing at `sched_alpha.tla`.
3. **Missing sections for landed specs** (F-C4, H2): `sched_alpha.tla`,
   `sched_oncpu.tla`, `asid.tla` — the three spec-first-RE-ENABLED modules —
   plus `pipe.tla` have NO SPEC-TO-CODE sections (death_wake.tla DID get
   its section: the discipline is alive but uneven).
4. **Pre-rework section drift** (F-C5/F-C6/F-C7/F-C10, H3): territory.tla
   section pre-stalk-2 (path-keyed rows vs the Spoor-identity re-key);
   handles.tla section pre-LOOM/#844 ("three partitions" vs four; a phantom
   `handle_transfer_via_9p` `-ENOTSUP` stub that was never built);
   corvus.tla section maps to the stalk-3c-retired syscalls
   (SYS_POST_SERVICE / SYS_SRV_CONNECT / SRV_CONN_PER_PROC_MAX) and its
   one-way console-bit claim is falsified by `proc_revoke_console_attached`
   (no live violation — RW-6 F2's axis); 9p_client.tla section silent on
   #841/#845/Loom-2b (`awaiting_flush` sits directly on the modeled I-10
   surface).
5. **"Nine specs" fossils** (F-C9, H3): ARCH §25.2 + `specs/README.md` +
   CLAUDE.md still enumerate the Phase-0 nine (incl. never-written
   futex/notes/pty) while 17 modules are committed; §28 I-9/I-20 cite the
   phantoms (= SA-2/F-A1/F-B1).
6. **Hygiene pre-check REFUTED** (= SA-4): zero TTrace/states files
   committed; `.gitignore:8/27/29` covers them. (Local debris does break
   `make specs` iteration when present — folded into F-C1's runner rework.)

### 3.1 Self-audit pre-findings (main loop; ground-truthed before reviewer returns)

- **SA-1 [X/H3, trivially-mechanical]** `CLAUDE.md` "Invariants that must
  hold" claims *"Verbatim from ARCHITECTURE.md §28 ... Keep in sync with
  ARCH"* and is NOT in sync: 21 rows vs ARCH's 31 (I-22..I-31 missing
  entirely), and stale cells in the rows it has — I-1 says "Namespace
  operations" (ARCH: "Territory operations"), spec column cites
  `namespace.tla` (I-1/I-3; the file is `territory.tla`), `vmo.tla` (I-7; the
  file is `burrow.tla`), `futex.tla`/`notes.tla`/`pty.tla` (none exist in
  `specs/`). Fix: re-sync the table verbatim from §28 in the report commit.
- **SA-2 [X/H3, mechanical]** ARCH §28 spec-column drift: **I-9** cites
  `futex.tla` — never written (the torpor wait-on-address was prose-validated
  per the 2026-05-23 suspension), and the row does not cite
  `specs/death_wake.tla`, which since RW-2 SA-1 (2026-06-10) machine-checks
  exactly the I-9 death-wake generalization the row describes in prose.
  **I-24** likewise omits `death_wake.tla` (which pins its
  exactly-once-ZOMBIE / no-EL0-after-ZOMBIE core). **I-20** cites `pty.tla`
  (never written) for a mechanism that is itself UNBUILT — the row should
  carry the honest "planned then dropped/deferred" form that I-19's row
  already uses.
- **SA-3 [X/H3]** `specs/SPEC-TO-CODE.md` mapping-discipline lapses:
  (a) NO section exists for `pipe.tla`, `asid.tla`, `sched_alpha.tla`,
  `sched_oncpu.tla` — four landed spec modules with no canonical
  action↔source mapping (the file is scripture: "the canonical mapping lives
  in specs/SPEC-TO-CODE.md");
  (b) the `scheduler.tla` section ("P4-Ic6-impl landed") still describes
  `g_bootcpu_idle` as the current deadlock-path mechanism — retired by the
  SMP redesign (#863; survives only in explanatory comments,
  `kernel/sched.c:396,817,973`) — with no superseding note pointing at
  `sched_alpha.tla`;
  (c) the file header's "CI will eventually verify the mapping is current
  ... (Phase 2 close adds the CI gate)" never landed — the only spec runner
  is the manual `Makefile:60` all-specs target; no pre-commit/CI automation,
  nothing verifies mapping currency.
- **SA-4 [checked clean]** `specs/` TLC droppings (`*_TTrace_*.bin/.tla`,
  `states/`) are NOT committed (already gitignored) — hygiene candidate
  disproven by `git ls-files`.

---

## 4. X/P/G-lens aggregation from RW-1..RW-9 (the cross-cut's aggregation duty)

Per HOLOTYPE.md §3, per-area reviews record cross-cut observations in place
and the cross-cut aggregates them. Sweep of `docs/holotype/00-register.md`
lens column over HT00..HT09:

- **X-lens rows: exactly one** — HT07.R3-F8 [T/X, H2] (per-device
  KObj_VIRTIO_DEV device-session model; the structural close for R3-F1's
  proc-exit quiesce walk). Already REGISTERED → RW-13 triage; nothing further
  owed here.
- **P-lens rows: two** — HT01.B-F6 (per-page `dsb ish` TLBI loop) +
  HT01.C-F5 (`vma_lock` across 256 MiB zeroing). Both REGISTERED → RW-11
  (performance cross-cut), which is their home.
- The large C-lens population (36 rows) is doc-currency/completeness, all
  dispositioned per-RW (FIXED) or tracked (#21, #23, #24, #26, #28, #46,
  #54). RW-10 does not re-disposition them; the ledger's Residuals column
  cites them where they touch an invariant.

---

## 5. Findings (canonical HT10 numbering; reviewer + self-audit merged)

**0 P0 / 0 P1 / 0 P2 / 0 P3 — zero soundness findings.** Every §28
enforcement mechanism exists, was located, and (where claimed) is tested;
nothing is violable today. All findings are X-lens (H-scale). NOT a dirty
close (no P0; P1+P2 = 0; the fixes are docs + tests + comments + one
Makefile target — nothing structurally invasive).

| HT10 | Sev | Finding (sources) | Disposition |
|---|---|---|---|
| F1 | X/H2 | ARCH §28 row-cell drift cluster: I-2 (elevation-only set + console-gate overclaim), I-8/I-17 (EEVDF math absent as claimed), I-9 (phantom `futex.tla`; `tsleep`/`death_wake` uncited), I-10 (phantom "monotonic generation"), I-12 ("mprotect rejection"), I-15 (`<platform>/` dir), I-16 (ctl path), I-20 (UNBUILT read as enforced), I-24 (death_wake.tla uncited), I-27 (pre-`@2608c88`), I-31 (1-of-5 cfgs) — [A-F1/F2/F3/F9, B-F1/F2/F3/F4/F5, C-F8, SA-2] | FIXED (the §28 cell pass, this close) |
| F2 | X/H2 | CLAUDE.md operational-scripture drift: the invariant mirror at 21/31 rows with phantom spec names + pre-#811 text; the W^X audit row citing phantom `mm/vm.c`+`mm/wxe.c` (ARCH §25.4 + ARCH:488/:3438 mirrors); the nine-spec gate table — [A-F7/F8, B-F6, SA-1] | FIXED (mirror re-synced condensed-31; "verbatim" header retired; paths corrected in BOTH tables) |
| F3 | X/H2 | The spec gate is inoperative: `make specs` could not fail (pipe-swallowed exits), errored on `sched_oncpu` (no default cfg), runs zero of the 71 buggy cfgs; SPEC-TO-CODE:3 + ARCH §25.3 claim the CI gate in present tense — [C-F1/F2, SA-3c] | Makefile honesty fix (skip-no-cfg/TTrace + propagate failure; 4-leg smoke-verified) + doc claims rewritten honest — FIXED; **the tiered clean+buggy runner REGISTERED → #56** |
| F4 | X/H2 | SPEC-TO-CODE.md currency: NO sections for `sched_alpha`/`sched_oncpu`/`asid`/`pipe`; the scheduler section affirmed the retired `g_bootcpu_idle`; territory/handles/9p_client/corvus sections pre-date stalk-2/#844+LOOM/#841+#845/stalk-3c; Loom + HostownerGrant status lag — [C-F3/F4/F5/F6/F7/F10/F13-part, A-F4/F5/F6, SA-3a/b] | FIXED (4 new mapping sections + 5 currency notes + header + 2 status lines) |
| F5 | X/H3 | "Nine specs" fossils: ARCH §25.2 + `specs/README.md` (incl. "no specs written yet"!) + CLAUDE.md cite never-written futex/notes/pty + omit the 11 added modules; ARCH §23.4/§23.5 + ARCH:105 + ROADMAP:985/:1214 phantom-spec prose — [C-F9, A-F1-part, B-F1-part] | FIXED (all sites → the 17-module reality / honest deferral wording) |
| F6 | X/H2 | The RW-7 console role-split (`@2608c88`) missed two scripture sites: the §28 I-27 row + IDENTITY-DESIGN §9.8's SAK transition (notify-note + owner=corvus, both retired) — [C-F12] | FIXED (both rewritten to as-built; verified against `proc.c:995-1020`) |
| F7 | X/H3 | STALK-DESIGN:76 present-tense survey row falsified by #957, unannotated — [C-F10b] | FIXED (supersession annotation) |
| F8 | X/H2 | ARCH §9.4's "v1.0 target" namespace layout (/dev + /proc + /ctl) is unbound — only /srv is mounted (`joey.c:213-248`); the /proc seam's recorded blocker ("no resolver") EXPIRED when stalk landed; the /dev + /ctl seams were never recorded — [D-F1] | REGISTERED → **#57** (record the seams + reconcile §9.4; USER scope call at RW-13; overlaps RW-12) |
| F9 | X/H3 | Spawn bypasses the per-Proc namespace: all five spawn variants resolve via the flat global `devramfs_lookup`, no X-search/perm_check on exec; documented as-built but never ARGUED, no exec-from-namespace seam recorded — [D-F2] | REGISTERED → **#58** (argue-or-fix at RW-13; no live violation — initrd binaries are system-owned world-x) |
| F10 | X/H2 | Doc-per-chunk violations from the RW-3..RW-9 FIX commits (cluster of 9 + 1 sibling): 99-fs-permission OEXEC; 83-pouch-signals retired NDFLT + kill→EIO contracts; 108-utopia-repl pre-`cd7eae2` Accept + "lazily"; 93/94-utopia missing the RW-9 recursion/interrupt machinery; syscall.h pre-A-3b rights envelope + expired "we lack t_open" premise; irqfwd.c phantom doc-section cite; 76-admin-elevate cached-peer + byte-compare; (+verification-found) 36-irqfwd single-waiter "extincts" caveat — [D-F3] | FIXED (all 10; each verified against as-built code first) |
| F11 | X/H3 | The `snare:` forge-reject — an ERRORS.md ABI commitment — had no regression test — [D-F4] | FIXED (`notes.snare_forge_rejected`, behavior-pinning + non-vacuous control) |
| F12 | X/H4 | `sys_thread_spawn_handler` raw `-22`/`-12` literals vs the named `-T_E_*` convention — [D-F5] | FIXED (13 literals → `-T_E_INVAL`/`-T_E_NOMEM`; `errno.h` included) |
| F13 | X/H4 | The ELF loader had zero ABI `_Static_assert`s (scripture demands the e_machine/ABI pins) — [D-F7] | FIXED (6 pins in `elf.h`) |
| F14 | X/H4 | The I-14 hostile-Rlerror ecode bound (`9p_client.c:99` — a UBSan-trappable kernel-halt class) was untested — [B-F7] | FIXED (`9p_client.rlerror_hostile_ecode_bounded`: ecode 0 / 2^31 / 4096 → -EIO; 4095 control) |
| F15 | X/H4 | Naming weighs: the Territory-vs-Pgrp internal split (`territory.h:40-74`); LS-K's planned `id`/`whoami`/`date` syscall-shaped homes — [D-F6] | REGISTERED → RW-13 (folded into #57's scope-call context) |

**Withdrawn after verification:** the TTrace/states repo-hygiene candidate
(SA-4 — already gitignored, confirmed independently by C); C-F13's
"LOOM.md §7 vs §10" leg (the spec-first statement IS in §7, LOOM.md:248).

---

## 6. Verified sound / checked clean (do not re-prosecute without new code)

- **The whole §28 enforcement layer is PRESENT**: 31/31 invariants located
  to live mechanisms (file:line in the ledger), 0 ABSENT — the two
  non-VERIFIED rows are honesty defects in the rows themselves (I-17
  quantitative bound = design target; I-20 = unbuilt mechanism), both now
  marked in §28.
- The I-2/I-22/I-25/I-26/I-27 authority spine, the I-9/I-24 death-wake
  machinery, I-28 stalk containment (incl. #957's walk-crossing — uses the
  same audited `cross_mounts`), and I-29/I-30 Loom rows verified CURRENT
  with their cfg sets existing verbatim.
- No ambient authority anywhere post-A-4 (rubric 5, PASS clean); the
  `-T_E_PERM` trap holds tree-wide; the snare: forge-reject holds (now
  tested); the SO_PEERCRED + syscall-number tables were RW-8-clean and not
  re-opened.
- `specs/` hygiene clean (TTrace/states ignored, not committed).
- All four reviewers `claude-fable-5` with no mid-run fallback (A/B/C/D
  end-lines all Fable; a fallback cannot recover, so end=Fable ⇒ whole-run
  Fable).

## 7. Verification

- Kernel build clean; **825/825 kernel tests PASS** (823 baseline + the 2
  new: `notes.snare_forge_rejected`, `9p_client.rlerror_hostile_ecode_bounded`),
  all u-* probes OK, boot OK, **0 EXTINCTION**; the lone tools/test.sh FAIL
  is the pre-existing #34 virtio-input QMP flake (tracked).
- The new `make specs` loop smoke-verified on all four legs (pass /
  skip-no-cfg / skip-TTrace / fail→exit-1).
- ARCH §28 = 31 rows; CLAUDE.md mirror = 31 rows (counted post-edit).
- Doc fixes each verified against as-built code BEFORE editing
  (`perm_want_for_omode`, `rights_for_omode`, the SAK transition
  `proc.c:995-1020`, the irqfwd busy/dying guards, devctl's `kernel-base`
  leaf, the §25.2 inventory vs `ls specs/`).
