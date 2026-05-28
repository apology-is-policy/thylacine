# Convergence Detour — Status

**TL;DR.** One comprehensive arc off the Utopia shell pause (U-6d-a) that brings
Thylacine to a clean boundary line where **the system really is what it says it
is** (scripture == reality). Binding design + governing bar + full BUILD/SEAM/DOC
dispositions: **`docs/IDENTITY-DESIGN.md §8`**. This doc tracks the implementation
sub-chunks + cross-stop sequencing. Opened 2026-05-28.

**Governing bar** (IDENTITY-DESIGN §8.1): *build to the extent of the foreseeable
future + design every seam so extension is additive.* Not "build everything
maximally." Per item: concrete caller / verified threat -> BUILD (with exit
criterion); foreseeable-but-not-yet -> SEAM (scripture records the trigger).

**The one arc, three stops** (not split, not deferred to v1.x):
- **A** — identity / access / privilege (sub-chunks below).
- **B** — DMA isolation (ARM SMMUv3, in v1.0).
- **C** — kernel completeness-sweep dispositions (IDENTITY-DESIGN §8.4). The DOC
  reconciliations (MTE/CFI, demand-paging/COW, RNG stir, `ipi.c` drift) **landed
  2026-05-28** in the convergence-detour scripture commit.

---

## Stop A — sub-chunks (ordered by dependency)

Each is audit-bearing (identity + capabilities are invariant-bearing). `design-first`
marks an ABI / capability-model decision that gets a scripture mini-commit before
code (per "design conversation -> scripture commit -> code").

### A-1 · Kernel identity model + corvus identity authority *(design-first)*
- **Builds:** durable `principal-id` on the Proc; `stripes` refined to carry
  `{principal-id, current caps, clearance annotations}`; first-class **groups**;
  corvus owns the `id<->name<->groups<->keys<->clearance-eligibility` DB + resolver;
  minimal identity establishment (corvus authenticates -> kernel stamps a
  non-system principal-id).
- **Depends:** corvus (exists). **Seam:** verifiable-credential projection point
  (distributed identity, v1.x).
- **Design-first: PINNED 2026-05-28 -> IDENTITY-DESIGN §9.1** (principal-id u32 ABI
  + reserved non-privileged SYSTEM/NONE + Proc record [168->240 bytes] +
  `srv_peer_info` 24->40 + **identity-at-spawn** via extended `sys_spawn_args`
  [56->80] gated by `CAP_SET_IDENTITY` [refined from stamp-a-child -> race-free] +
  corvus DB schema + `RESOLVE_*` verbs + CRVS v2). **Splits A-1a (kernel) /
  A-1b (corvus).**
  - **A-1a scripture-reconcile 2026-05-28 (two corrections to the §9.1 pin, both
    faithful to intent, both verified against the tree):** (1) `srv_peer_info` is
    24->**40** not 24->32 -- the pin placed principal_id@20 "was pad" but @20 is the
    LIVE `alive` field (SYS_SRV_PEER); the new fields append after `alive`
    (principal_id@24 / primary_gid@28 / flags@32 / _reserved@36). (2) the
    `CAP_SET_IDENTITY` gate is **fail-closed** -- a SPAWN_IDENTITY_SET request
    WITHOUT the cap returns -1 (rejected loudly), not silent-inherit; reserved-value
    set (INVALID/SYSTEM) also -> -1. Identity applied in the spawn thunk before
    userland_enter (console-attach precedent).
  - **A-1a (kernel) LANDED 2026-05-28** -- scripture `8d1d05d` + code `3610edb` +
    audit close `9b0c638`. Proc identity (168->240) + inheritance + kproc=SYSTEM +
    CAP_SET_IDENTITY + identity-at-spawn (fail-closed) + srv_peer_info 24->40 + all 4
    userspace mirrors. **Audit R1 CLEAN** (0 P0/0 P1/1 P2/4 P3, all fixed). 618/618
    PASS x (default + ASan + UBSan). Reference doc 95. A real stack-clobber (stale
    24-byte pouch srv_peer_info mirror vs the 40-byte kernel write) found + fixed
    mid-chunk. **NEXT: the FS-mutation foundation (A-1.5 below), THEN A-1b**
    (reorder, user-chosen 2026-05-28 -- see A-1.5).
- **Tests:** identity establishment at spawn; inheritance; capped vs uncapped
  identity-set; `srv_peer_info` exposes principal_id/primary_gid; group checks; I-22
  holds (no id bypasses).

### A-1.5 · FS-mutation foundation *(pulled forward; precedes A-1b; design-first)*

**Reorder, user-chosen 2026-05-28.** A-1b's corvus identity DB persists to disk
(user chose the full DB + UPG + `GROUP_CREATE` + **real persistence**, not an
in-memory seam). Real persistence needs file **create + fsync + readdir**, none
of which existed at A-1a: the only `*_CREATE` syscalls are the HW ones, there is
no `SYS_WALK_CREATE` (that was A-2b), and `fsync` / `readdir` are the unbuilt
"G1 FS-mutation" sweep items. So A-2b is **pulled forward** and bundled with the
G1 durability/enumeration items as one FS foundation that lands BEFORE A-1b.

- **Builds (3 syscalls; ABI pinned in IDENTITY-DESIGN.md §9.2):**
  - `SYS_WALK_CREATE = 54` -- create-then-open sibling of `SYS_WALK_OPEN`;
    `perm`'s `DMDIR` bit folds mkdir into create (Tmkdir vs Tlcreate); carries
    caller `primary_gid` into the 9P gid field. Real `dev9p_create` (today a
    stub).
  - `SYS_FSYNC = 55` -- durability barrier; new `Dev.fsync` slot -> Tsync.
  - `SYS_READDIR = 56` -- enumeration; new `Dev.readdir` slot -> Treaddir.
- **Why cheap-ish:** the kernel 9P client already implements the wire half
  (`p9_client_lcreate`/`mkdir`/`fsync`/`readdir`, verified 2026-05-28); `Dev`
  already has a `.create` slot. This is syscall wrappers + the real
  `dev9p_create` + two new vtable slots + rights gates + tests + audit.
- **Split:** **FS-alpha** (`SYS_WALK_CREATE`) -> **FS-beta** (`SYS_FSYNC` +
  `SYS_READDIR`) -> **one focused audit** over the whole create/write/fsync
  surface (the AEGIS/mallocng-adjacent path -- prosecute hard).
- **Depends:** A-1a (identity on the Proc, for the primary_gid stamp). **Seam:**
  ownership-on-create attribution + per-file rwx ENFORCEMENT are A-2 (this is the
  create MECHANISM; I-22 holds -- no enforcement exists yet to bypass).
- **Audit:** the create + write + fsync surface -> full round.
- **Tests:** create a file + write + fsync + re-read; mkdir-via-DMDIR + readdir
  round-trip; rights-gate reject (no `RIGHT_WRITE` parent); name bounds; reserved
  `DM*` bit reject; readdir buffer bounds + EOD.
- **FS-alpha LANDED** (`SYS_WALK_CREATE`): real `dev9p_create` (Tlcreate /
  Tmkdir+walk+lopen), the `Dev` `.create` vtable widened to `struct Spoor
  *(*)(c,name,omode,perm,gid)` across all 13 Devs (only dev9p creates), the
  handler with cross-Dev clone-walk safety, libt + libthyla-rs `t_walk_create`,
  2 dev9p loopback tests (create_file / create_dir), reference doc 96. 620/620
  PASS (default; +2 from 618).
- **FS-beta LANDED** (`SYS_FSYNC=55` + `SYS_READDIR=56`): new `Dev.fsync` +
  `.readdir` vtable slots (NULL-permitted like `.poll`, so NO 13-Dev churn --
  only dev9p sets real impls + devramfs a no-op fsync); `dev9p_fsync` -> Tsync,
  `dev9p_readdir` -> Treaddir; the `SYS_READDIR` handler does the 9P2000.L
  Treaddir cookie-advance + EOD parse; libt + libthyla-rs `t_fsync` / `t_readdir`;
  2 dev9p loopback tests (fsync / readdir) + a **joey E2E probe** that does
  create+write+fsync+read-back + mkdir+readdir+EOD against the REAL disk-backed
  Stratum FS (covers the handlers end-to-end -- passed: `d1=51` dirent bytes,
  EOD on the 2nd readdir). 622/622 PASS (default; +4 from 618). **Next: the
  FS-audit** (one focused adversarial round over create/write/fsync).

### A-2 · FS permission + ownership surface *(splits a/b/c)*

> **Note (2026-05-28):** A-2b's `SYS_WALK_CREATE` is **pulled forward into A-1.5**
> (the FS-mutation foundation). What remains of A-2b here is the ownership +
> permission SEMANTICS on top of the create mechanism (owner-stamp =
> principal_id, group = parent-dir, mode = default & umask) -- which interlocks
> with A-2a (`t_stat` owner/group/mode) and A-2d (the kernel rwx layer).
- **A-2a:** extend `t_stat` with owner/group/mode (versioned, ACL-extensible
  **seam**) + `SYS_WSTAT` / chmod / chown (drives `p9_client_setattr`).
- **A-2b:** `SYS_WALK_CREATE` — make `dev9p_create` / `p9_client_lcreate` live;
  stamp owner = caller principal-id, group = parent-dir (Plan 9/BSD), mode =
  default & umask.
- **A-2c:** uniform **mount-cape** for self-declared-non-POSIX backings — folds
  into A-2d as the metadata source for permissionless backings.
- **A-2d:** the **kernel rwx-permission layer** (Linux-VFS model; IDENTITY-DESIGN
  §3.7). At walk/open/create, check the file's mode/uid/gid (Dev `stat` /
  mount-cape) against the Proc's `principal_id` + groups; no uid bypass (I-22);
  chmod/chown ownership-change policy enforced here. **Replaces the dropped
  "server enforces" assumption** — Stratum enforces dataset-scope ONLY, not file
  rwx (agent-verified 2026-05-28; see §3.7).
- **Depends:** A-1. Interlocks with Stop C FS-mutation (this *is* create+setattr).
- **Audit:** the create + setattr 9P-write surface (AEGIS-adjacent) -> full round.
- **Tests:** create stamps owner=caller + group=parent; chmod/chown round-trip;
  mount-cape uniform perms on a FAT-like backing; ownership survives reboot.

### A-3 · 9P identity presentation + per-user stratumd
- **Builds:** trust gate (corvus stamps the `/srv` posting; kernel forwards
  principal-id as `n_uname` ONLY to stamped servers; untrusted -> `none`); per-user
  stratumd spawn (`--role client`, dataset-scoped, Stratum A2); EACCES-at-`Tattach`.
- **Depends:** A-1. **Prereq to verify first:** Stratum A2 is merged into the
  `thylacine-pouch-arm` branch we cross-build.
- **Audit:** privilege boundary (I-2 / I-4 / I-6) -> full round.
- **Tests:** trusted-local attach presents principal-id; untrusted -> `none`;
  per-user stratumd serves only its scoped dataset; out-of-scope -> EACCES.

### A-4 · Clearance + legate elevation + CAP_KILL + trusted path *(splits a/b/c; design-first)*
- **A-4a:** clearance-level policy objects `{caps, auth_required, time_bound,
  scope}` (corvus-held; grant/revoke via per-user wrap chains); the **legate**
  (kernel mints an ephemeral principal forked from the durable user; caps =
  clearance ∩ self-restriction; bounded to process-subtree + time; evaporates on
  exit; kernel cap-stamp, NO local crypto proof).
- **A-4b:** `CAP_KILL` + cross-process signaling (resolves the open ARCH question).
- **A-4c:** the **trusted path** (console + secure-attention via corvus).
- **Depends:** A-1 + corvus. **Seams:** resource-scoped HW-cap allowlist;
  distributed clearance crypto-proof (v1.x).
- **Design-first:** clearance wire format + legate scope semantics + CAP_KILL
  permission model.
- **Audit:** highest-stakes privilege surface -> full round.
- **Tests:** grant->activate->legate-has-caps->scope-exit-drops; revoke blocks
  future activation; `CAP_KILL` targets a non-child with the cap; trusted-path
  rejects a spoofed prompt; clearance secret != data key.

### A-5 · Login + session lifecycle + hostowner-c + corvus Q11 seam *(integration)*
- **Builds:** `/sbin/login` (console -> corvus auth -> principal-id -> spawn
  per-user stratumd -> bind home -> spawn shell); logout lifecycle (DEK eviction
  via A4-notify + process reap + mount teardown); P5-hostowner-c (RECOVER paper
  phrase); the corvus Q11 4-byte request-header seam (live A3 interop).
- **Depends:** A-1..A-4.
- **Audit:** boot/login path + DEK lifecycle -> full round.
- **Tests:** login as a user -> correct principal-id + home + shell; logout -> DEK
  evicted + procs reaped; RECOVER round-trip; full single-user -> multi-user boot.

---

## Cross-stop sequencing

- **A is first** — unblocks the shell; dependency root for most of C.
- **B (SMMU)** is largely independent (hardware/memory mechanism) — parallel with A
  or right after; the DMA-API change (device-IOVAs, not raw PAs) is self-contained.
- **C non-A items** (clock, exit-status, demand-zero, PAN, PAC-entropy, resource
  floor, orphan reaper, FS-mutation-beyond-create, cheap spec detection) are mostly
  independent kernel work; the syscall-surface ones interleave (A-2 already builds
  create/setattr; the rest of FS-mutation rides alongside).

---

## Stop-A exit criteria

- [ ] A user logs in via corvus, gets a principal-id + their home territory + a shell.
- [ ] A created file is owned by the caller's principal-id; group inherits the dir.
- [ ] chmod/chown round-trip; the v1.0 coreutils ownership story (`ls -l`, `chown`).
- [ ] A second user cannot read/write the first's home (namespace + per-user stratumd
      + server-enforced perms; no superuser identity, I-22).
- [ ] Legate elevation: grant -> activate (auth) -> scoped caps -> evaporate on exit;
      `CAP_KILL` works; trusted-path resists spoofing.
- [ ] Logout evicts DEKs + reaps procs + tears down mounts.
- [ ] Every A sub-chunk: full adversarial audit round closed; sanitizer matrix green.

---

## Build + verify

```bash
tools/build.sh all && tools/test.sh
tools/build.sh kernel --sanitize=address && tools/test.sh
tools/build.sh kernel --sanitize=undefined && tools/test.sh
```

---

## Trip hazards

- **Stratum A2 branch-merge prereq** (A-3): confirm `--role coordinator|client` is
  in the `thylacine-pouch-arm` branch before leaning on per-user stratumd.
- **A-1.5 / A-2 9P-write audit**: the create + write + fsync path (now landing in
  the A-1.5 FS-mutation foundation) is the AEGIS/mallocng-adjacent surface from
  Phase 6 -> prosecute hard.
- **Design-first sub-commits**: A-1a done; the FS-mutation foundation (A-1.5) is
  pinned in IDENTITY-DESIGN.md §9.2 (this scripture commit); A-1b's corvus wire
  ABIs + CRVS v2 byte format still owed (pin in CORVUS-DESIGN.md before A-1b
  code); A-4 cap-model still owed.
- **Seams are load-bearing**: A-2 leaves the ACL seam; A-4 leaves the
  resource-scoped-HW-cap allowlist + the distributed crypto-proof seam. Don't bake
  fixed bitfields where a policy object belongs.
- **Open verify** (from the scripture pass): `ARCHITECTURE.md:660/662` describe
  `rfork(RFPROC)` as "copy-on-write address space" -- confirm the actual copy
  behavior; add a seam-note like §16 if it doesn't truly COW.
- **A-4 prerequisite (P1-class, surfaced by the A-1a audit, NOT A-1a):**
  `kernel/proc.c::rfork_internal` sets `child->caps = parent_caps & caps_mask` WITHOUT
  `& ~CAP_ELEVATION_ONLY`, but `caps.h:99-105` (P5-hostowner-b) asserts it strips
  elevation-only caps. Real I-2 hole: a `CAP_HOSTOWNER`-elevated parent can rfork /
  `SYS_SPAWN_*`(cap_mask incl. HOSTOWNER) a child that inherits `CAP_HOSTOWNER`,
  bypassing the console-attached `ADMIN_ELEVATE` gate. Does NOT touch identity
  (`CAP_SET_IDENTITY` is correctly fork-grantable, in CAP_ALL, not elevation-only).
  Fix is a 1-line AND matching the existing scripture + a regression test, but rfork
  is an audit-trigger surface and sibling elevation paths want a sweep -> fold into the
  A-4 (clearance/legate/hostowner) chunk with its own focused audit. Recorded in
  `memory/audit_a1a_closed_list.md` (carried-out-of-scope).

---

## References

- `docs/IDENTITY-DESIGN.md §8` — binding arc design + dispositions (the source of truth).
- `docs/ARCHITECTURE.md §28` (I-22), §25.4 (audit surfaces), §15.5/§24 (hardening reconciled).
- `docs/ROADMAP.md §8.0` — the detour arc within Phase 7.
- `docs/STRATUM-API-V1.md` + `-RESPONSE.md` — A2 (per-user stratumd), A3 (corvus wire), Q11.
- `docs/CORVUS-DESIGN.md` — key agent, hostowner model, identity authority.
