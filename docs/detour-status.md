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
  EOD on the 2nd readdir). 622/622 PASS (default; +4 from 618).
- **FS-audit LANDED** (Opus prosecutor R1 CLEAN: 0 P0 / 0 P1 / 1 P2 / 3 P3, all
  dispositioned; kernel create/write/fsync encode path SOUND).

### #713 GATE — ROOT-CAUSED + FIXED (2026-05-29) → A-1b UNBLOCKED

The user-mandated "full root-cause before A-1b" gate. The long-hunted
"AEGIS-256 / mallocng content-sensitive write-path corruption" (the Phase-6
saga, tasks 710-725) was **not a write-path or heap bug at all** -- it was an
`eret`-window **IRQ race** in BOTH EL0-entry trampolines (`userland_enter` in
`arch/arm64/userland.S` + `thread_user_trampoline` in `arch/arm64/context.S`):
each sets `ELR_EL1 = entry_pc` then runs ~30 instructions to the `eret` **with
IRQs enabled** (`thread_user_trampoline` even did an explicit `msr daifclr, #2`
right before the ELR set). A timer/IRQ in that window overwrites `ELR_EL1` with
the interrupted kernel PC (`<trampoline>+0x10`); the `eret` then returns EL0 to
a kernel VA -> rare (~3-13%, IRQ-timing/SMP-load-dependent, `-smp 1`-clean)
instruction-permission fault in ANY freshly-spawned native Proc (test children,
corvus, **stratumd worker threads** -- the "AEGIS corruption" was a worker-thread
spawn landing EL0 at a kernel PC, sometimes a kernel stack overflow). **Fix**:
`msr daifset, #0xf` across the window in both trampolines; the `eret`'s
`SPSR_EL1=0` re-enables IRQs at EL0 atomically (kernel `thread_trampoline`, EL1,
no `eret`, correctly keeps its unmask). Verified **0 faults / 75 boots**; an
adversarial sweep of all `arch/arm64/*.S` found **no sibling** of the class;
622/622 default + UBSan. Two adjacent fixes landed with it: (a) the dominant
`fs-mut create FAILED` symptom was a **non-idempotent test** on the persistent
pool (EEXIST), now joey's probe is open-or-create (reboot-clean + a cross-reboot
persistence test); (b) the documented-but-undone `vma_lock` extension to the
demand-page reader + `SYS_MMIO_MAP`/`SYS_DMA_MAP` (independent SMP hardening; its
own audit: 1 P1 [the map paths] + 1 P3, both fixed). Detail:
`docs/reference/08-exception.md` "eret-window IRQ race (P6 #713)".

### A-1.6 · FS-gamma: rename + unlink *(pulled forward; precedes A-1b; design-first)*

**Reorder, user-chosen 2026-05-29.** When pinning A-1b's persistence model, the
user chose the classic **write-tmp + fsync + atomic rename-swap** substrate for
corvus's identity DB over an append-only log. With no `SYS_RENAME` at v1.0, a
whole-file rewrite cannot be atomic -- so `rename` (atomic replace) + `unlink`
(stale-tmp cleanup) are pulled forward ahead of A-1b. Both are owed for the A-2
coreutils (`mv` / `rm` / `rmdir`) regardless, so building them here serves two
callers (same pull-forward pattern as A-1.5's `SYS_WALK_CREATE`).

- **Builds (2 syscalls; ABI pinned in IDENTITY-DESIGN.md §9.3):**
  - `SYS_RENAME = 57` -- atomic rename/move; `Trenameat` shape (olddir+oldname
    -> newdir+newname); destination atomically replaced (POSIX `rename(2)`). New
    `Dev.rename` slot -> `dev9p_rename` -> `p9_client_renameat`.
  - `SYS_UNLINK = 58` -- remove a non-dir, or rmdir an empty dir via
    `SYS_UNLINK_REMOVEDIR` (`0x200` = `P9_UNLINK_AT_REMOVEDIR`). New `Dev.unlink`
    slot -> `dev9p_unlink` -> `p9_client_unlinkat`.
- **Why cheap-ish:** the wire half (`p9_client_renameat` / `unlinkat`) already
  exists -- but is IMPLEMENTED-YET-UNEXERCISED (no syscall drives it;
  `dev9p_remove` is a `void` stub). Syscall wrappers + 2 new NULL-permitted Dev
  slots + dev9p impls + rights gates + tests + the **first end-to-end audit** of
  those wire functions. The `void (*remove)` Plan 9 slot is left as-is (wrong
  shape); `SYS_UNLINK` uses the new `.unlink`.
- **Depends:** A-1.5 (the FS-mutation foundation + the clone-walk-to-cursor
  discipline). **Used by:** A-1b's atomic-swap persistence; A-2 coreutils.
- **Audit:** the rename/unlink write surface (AEGIS-adjacent; first exercise of
  the renameat/unlinkat wire half) -> full round. Focus: the two-cursor +
  cross-Dev reject on rename, fid lifecycle on every error path, the rename-swap
  durability detail (post-rename `Tsync`-on-dir as the metadata barrier).
- **Tests:** create->rename->read-back-under-new-name; create->unlink->gone;
  mkdir->rmdir-via-REMOVEDIR; rights-gate reject (no `RIGHT_WRITE`); cross-Dev
  reject; name bounds; reserved-flag reject; joey E2E against real Stratum.
- **LANDED 2026-05-29.** Scripture `163b16b` (§9.3 ABI pin) + impl `92522f0`
  (`SYS_RENAME=57` + `SYS_UNLINK=58`; new NULL-permitted `.rename`/`.unlink` Dev
  slots, dev9p only; handlers run DIRECTLY on the looked-up dir Spoor(s) -- NO
  clone-walk, since renameat/unlinkat don't transition the dirfid [the §9.3 pin
  was corrected in the impl commit]; RIGHT_WRITE on every dir; cross-Dev reject +
  same-`p9_client` reject; flags `{0, REMOVEDIR}`; `_Static_assert
  SYS_UNLINK_REMOVEDIR == P9_UNLINK_AT_REMOVEDIR`; libt + libthyla-rs `t_rename`
  / `t_unlink`; 2 dev9p loopback tests [622->624]; idempotent joey E2E proving
  atomic-replace + unlink + rmdir vs REAL Stratum). **624/624 PASS default +
  UBSan; joey status=0.** Opus prosecutor R1 **CLEAN** (0 P0 / 0 P1 / 0 P2 / 3 P3
  all informational/pre-existing -- F1 synchronous-client no-op guard, F2
  pre-existing surface-wide `handle_get` TOCTOU [not worsened; rename/unlink
  touch no per-Spoor mutable state], F3 server-delegated QTDIR check; NO code
  change). First end-to-end exercise of `p9_client_renameat`/`unlinkat` (wire
  half existed since Phase 5, never driven). `audit_fs_gamma_closed_list.md`;
  `docs/reference/96-fs-mutation.md` FS-gamma sections. **A-1b now builds corvus
  persistence on this atomic-swap substrate.**

### FS-delta · O_PATH walkable directory handles *(3rd-order detour; precedes A-1.7; design-first)*

**LANDED 2026-05-29** -- scripture `cd22572` + impl `bb59c6b` (`T_OPATH=0x80`;
`SYS_WALK_OPEN_OMODE_VALID` 0x13->0x93; F5 audit-fix: T_OPATH handles born `R|W`).
624/624, no regression. Audit folded into the A-1.7 R1 close (CLEAN).

**Surfaced 2026-05-29 building A-1.7; user-chosen Option 1 (corrects a mistake).**
A-1.7's confinement must create files in a confined subtree -- which hit a real
wall: `SYS_WALK_OPEN`/`SYS_WALK_CREATE` `Tlopen` the fid they return, and 9P
forbids `Twalk` from an opened fid, so children cannot be created under any
returned handle; only the non-opened territory root works. Net: no nested dirs,
no confined writable subtree (and `File::open` multi-component is latently
broken). A Plan 9 divergence (Plan 9 walk = clone-UNOPENED; open is separate);
FS-delta restores it. **Full design + ABI: `IDENTITY-DESIGN.md §9.4`.**

- **Builds:** `T_OPATH = 0x80` omode flag (`SYS_WALK_OPEN_OMODE_VALID` 0x13 ->
  0x93); `sys_walk_open_handler` skips `dev->open` when `T_OPATH` -> returns a
  non-opened, walkable `KObj_Spoor` (rights `R|W|TRANSFER` unchanged). `T_OPATH`
  in syscall.h + libt + libthyla-rs. No new syscall number; dev9p unchanged.
  Lineage: Linux `O_PATH`; Plan 9 walk; Fuchsia/Genode dir-handles.
- **Depends:** A-1.5 (the walk/create surface). **Used by:** A-1.7 (mkdir -p +
  the chroot-target capability); `File::open` multi-component (fixes the latent
  bug); A-2 coreutils (`mkdir -p`, `cd`).
- **Audit:** folded into the A-1.7 round (its prerequisite) -- the no-open walk
  path must bound the name, reject `..`/`/`/`\0`, stamp rights identically, leave
  no half-walked Spoor on error.
- **Tests:** the A-1.7 joey E2E against REAL Stratum is the behavioral test
  (mkdir -p /var/lib/corvus + corvus chroot + create-under-cap). The in-kernel
  dev9p loopback mock does NOT enforce 9P's no-Twalk-from-an-opened-fid rule
  (that is a real-server behavior), so it cannot reproduce the wall; the suite
  confirms no regression (624/624). The handler skip-open path itself is `static`
  + not unit-reachable.

### A-1.7 · Capability-scoped service storage *(2nd-order detour; pulled before A-1b; design-first)*

**LANDED 2026-05-29** -- `41e417d` scripture / `aa11b17` corvus-side / `67f3ec6`
joey-side + audit-close fixes. corvus is confined to its handed `/var/lib/corvus`
capability on the real disk-backed Stratum FS ("storage capability OK (confined;
/thylacine-version unreachable)"); 624/624. **Audit R1 CLEAN** (0 P0 / 1 P1 / 1 P2
/ 3 P3): F1 (the "withholding TRANSFER blocks re-handing" claim was false ->
scripture-corrected to the monotonic bound; security core holds), F2 (confinement
was cooperative -> corvus chroots FIRST), F5 (`T_OPATH` born `R|W`); F3 (smoke
proof), F4 (no T_OPATH unit test) documented P3. The feared shared-9P-session
lifetime (corvus outlives joey) + fid/tag interleaving traced SOUND. Reference doc
`98-capability-storage.md`; closed list `audit_a1_7_fs_delta_closed_list.md`.
**A-1b RESUMES next on this substrate** (corvus identity DB inside the chrooted cap).

**Reorder, user-chosen 2026-05-29.** While pinning A-1b's persistence, the
question "where does a service's storage live + with what authority?" surfaced a
foundational substrate. Rather than build corvus on the ambient-inherited-root
model and migrate later, the substrate is built once, first, with **corvus as its
first + most-important consumer** (a secret-holding daemon that should be
confined). A genuine depth-first push (see
`memory/feedback_depth_first_dependencies.md`): A-1b is preempted at a clean
boundary (its mechanism-independent core -- identity model + CRVS v2
serialize/parse -- drafted then reverted at `6df247f`) and RESUMES immediately
after this closes.

**The angle (scripture, this commit):** NOVEL.md §3.10 (lead angle #10) + ARCH
§3.6 + invariant **I-23** (a service's FS authority is bounded by the storage
capability it is handed). A service is *handed* a storage-root `KObj_Spoor` at
spawn (endowed like fd 0/1/2, but for state), reduced to `R|W` (no `TRANSFER` --
least authority), chroots to it as its FIRST action (filesystem world IS the cap),
and does all persistence at `FROM_ROOT` (now the cap). POLA for service state.
[A-1.7 audit R1 CLEAN -- F1: authority is bounded `<=` grant + subtree + monotonic
(the "withholding TRANSFER blocks re-handing" claim was false; scripture-corrected);
F2: confinement is cooperative (corvus chroots first), spawner-set-root is v1.x.]

- **Builds (userspace, on the FS-delta O_PATH primitive landed first):**
  - joey (post-pivot): `mkdir -p /var/lib/corvus` (create-then-reopen-`T_OPATH`
    per component) -> a NON-OPENED walkable handle -> `handle_dup` to `R|W` (drop
    `TRANSFER`) -> endow as corvus's fd 0 via the existing `t_spawn_with_perms`
    fds array.
  - corvus: `SYS_CHROOT` to fd 0 (the non-opened capability) so its filesystem
    world IS the storage dir; create/read state at `FROM_ROOT`; a confinement
    smoke (write+read a file; prove a path above the cap is unreachable). [the
    corvus-side landed guarded at `aa11b17`.]
- **Why FS-delta is needed (the "zero kernel surface" claim was WRONG):**
  `walk_open`/`walk_create` `Tlopen` the returned fid, and 9P forbids `Twalk`
  from an opened fid -- so a returned handle is NOT a valid create/walk base for
  children (confirmed: `mkdir var`@root=fd0, `mkdir lib` UNDER it=-1). FS-delta
  (`T_OPATH`; IDENTITY §9.4) adds a walk-without-open mode -> a non-opened
  walkable handle, which IS a valid base + a valid chroot target. `handle_dup`
  reduces rights subset-checked (`handle.c`, the I-6 point); spawn installs
  arbitrary `KObj_Spoor` preserving I-6; `SYS_CHROOT` gates `RIGHT_READ`.
- **Depends:** **FS-delta** (the O_PATH primitive, landed first); the post-pivot
  Stratum root (so joey has a persistent place to scope from); A-1.5/A-1.6 (the
  FS-mutation surface). **Used by:** A-1b (corvus persistence); every future
  system daemon's storage.
- **Audit:** the capability/privilege surface -- spawn-time rights reduction
  (`R|W`, no `TRANSFER` -> no re-hand), no-escape confinement (the service cannot
  name a path outside its storage subtree), I-6 monotonicity through the spawn-fd
  endow. Full round.
- **Tests:** corvus confinement smoke (read/write inside the handed capability; a
  path outside it is unreachable); joey grants `R|W`-no-`TRANSFER` and corvus
  cannot re-transfer; survives reboot (the storage dir persists on Stratum).
- **RESUME POINTER (the exact return point):** when A-1.7 closes, **resume A-1b**
  (task #776). Re-apply the reverted mechanism-independent core (identity model +
  UPG allocator + CRVS v2 serialize/parse -- reconstruct from CORVUS-DESIGN.md §16
  / the session record) and build corvus's `identity.db` + per-user wraps **inside
  the handed `storage_root`** (NOT via `FROM_ROOT` absolute paths). The §16
  persist/load flow is unchanged except `storage_root` is the handed capability,
  not a walked territory path.

### A-1b · corvus identity DB + cross-reboot persistence *(CLOSED 2026-05-29; cross-reboot blocker fully resolved 2026-05-30, `b7066e4`)*

**LANDED + audit-CLEAN 2026-05-29 (2nd session).** corvus is the authoritative
`id <-> name <-> groups` resolver with REAL on-disk persistence -- the first
end-to-end SECRET-on-disk write/read-back path in the OS. boot1 creates michael +
susan + groups + the AEGIS-256-wrapped keypair wraps; boot2 on the SAME pool
loads `identity.db` ("already provisioned") + reloads each secret wrap from disk +
AUTHs (UNWRAP DEK round-trip verified). Wired into the
suite as a standing guard (`tools/test-cross-reboot.sh` / `make test-cross-reboot`;
re-bake + 2 boots; hard-fails a missing persistence marker AND hard-fails the
EBADTAG corruption below -- it does NOT retry-mask either).

**BLOCKER RESOLVED 2026-05-30 (4th session): the recurring stratumd-mount
`STM_EBADTAG` was a BUILD-HARNESS stale-key bug -- NOT corruption, NOT a read-path
bug.** `tools/build.sh`'s `pool` target re-baked `build/fixtures/system.key`
(libsodium-random per run) WITHOUT rebuilding the ramfs that bakes it in at
`/system.key`. So `tools/test-cross-reboot.sh`'s `build.sh pool` re-bake (line
119) left the ramfs holding a STALE key; the VM mounted the FRESH pool with the
WRONG key; stratumd derived the wrong metadata key; AEAD *correctly* rejected the
first btree node -> `rc=-201` at mount -> joey FATAL -> extinction. The "content
intermittency" was build-command HISTORY: it failed after `build.sh pool` and
passed after `build.sh kernel`/`all` (which rebuild the ramfs together with the
pool -- so every kernel-rebuild boot passed). Ground truth: 23-boot
decrypt-chokepoint instrumentation showed every node BYTE-IDENTICAL to a host
reference (no corruption in the read->decrypt window); `hash(fixtures key) !=
hash(ramfs key)` after `build.sh pool`; mismatch boots -> `rc=-201`, matched
boots -> OK. **Fixed `b7066e4`** (the `pool` target now runs `build_ramfs`);
**`tools/test-cross-reboot.sh` passes 3/3, first try, both boots.** The preserved
`build/fixtures/REPRO-ebadtag-201/` was a MISMATCHED pair (pre-fix failing log +
post-fix passing pool) -- not a live reproducer. Full coda:
`docs/DEBUGGING-PLAYBOOK.md` section 6.10 + `bug_large_9p_write_srvconn_runtime.md`.

- **On the A-1.7 substrate:** identity.db + `users/<name>/hybrid.corvus` live
  inside the handed `storage_root` cap (corvus chrooted to `/var/lib/corvus`) on
  the disk-backed Stratum FS, via the A-1.6 atomic rename-swap. Verbs RESOLVE_ID=11
  / RESOLVE_NAME=12 (ungated) + GROUP_CREATE=13 (CAP_HOSTOWNER live re-query) +
  USER_CREATE supp_gids extension. CRVS v2 identity.db (20-B header + bounds-checked
  records) + CRVS v1 wrap (3752 B = an extent file). UPG one shared monotonic counter.
- **The cross-reboot UNBLOCK (the year-long "AEGIS-256 corruption"):** a 3-bug
  masking stack in the disk-backed extent path -- (1) bdev partial-tail zero-pad
  CLOBBER -> RMW (Stratum `91ae5d8`); (2) corvus missing dir-fsync (Thylacine
  `573b984`; audit-R1-refined to forward-portable, NOT load-bearing on current
  whole-pool-Tfsync Stratum); (3) fs raw read-offset -> EINVAL on the 2nd Tread
  -> `fs_read_extent_aligned_locked` (Stratum `91ae5d8`). **Load-bearing pair =
  1 + 3.** Method + journal: `docs/DEBUGGING-PLAYBOOK.md`.
- **Audit R1 CLEAN** (0 P0 / 0 P1 / 1 P2 / 4 P3; `audit_a1b_closed_list.md`,
  SUPERSEDES the blocker-fix list): F1 (P2) GROUP_CREATE had no count cap vs the
  parse `>512` reject -> privileged boot-brick -> CAPPED; F2 (P3) supp_gid/
  primary_gid value validation -> added (USER_CREATE + parse); F4 (P3) read-helper
  scratch unbounded -> capped (Stratum); F5 (P3) whole-pool-Tfsync refinement ->
  doc; F3 (P3) dropped-wrap record not pruned -> DEFERRED to USER_DELETE.
- Reference doc `97-corvus-identity-db.md`. **A-1b CLOSES the corvus identity arc.
  RESUME the detour at A-2 (FS permission / A-2d kernel rwx).**

### A-2 · FS permission + ownership surface *(splits a/b/c)*

> **Note (2026-05-28):** A-2b's `SYS_WALK_CREATE` is **pulled forward into A-1.5**
> (the FS-mutation foundation). What remains of A-2b here is the ownership +
> permission SEMANTICS on top of the create mechanism (owner-stamp =
> principal_id, group = parent-dir, mode = default & umask) -- which interlocks
> with A-2a (`t_stat` owner/group/mode) and A-2d (the kernel rwx layer).
- **A-2a (LANDED 2026-05-30):** `t_stat` += owner/group (72 -> 80 bytes;
  versioned, ACL-extensible seam) + `SYS_WSTAT = 59` / `t_chmod` / `t_chown`
  (drives `p9_client_setattr`). dev9p gains a real `Tgetattr`-backed
  `stat_native` (was a stub) + a NULL-permitted `.wstat_native` slot; devramfs
  reports `PRINCIPAL_SYSTEM`/`GID_SYSTEM`. The MECHANISM only -- per-file rwx
  enforcement is A-2d. libt + libthyla-rs + pouch `0010` consumers updated in
  lockstep. Tests: dev9p getattr/setattr loopback + devramfs sentinels + joey
  `/system.key` reject-path probe; 0 FAIL. **Opus audit R1 CLEAN (0 P0/1 P1/0 P2/
  3 P3):** F1 (P1) stale-sysroot overflow on the committed build path -> FIXED by
  the `build.sh sysroot_is_stale()` cache-invalidation (`b4dda9c`); F2 (P3)
  `dev9p_stat_native` ignores the Rgetattr valid mask -> deferred to A-2d; F3 (P3)
  "72-byte" doc-rot -> swept. Headline overflow question SOUND (no live 72-byte
  consumer; 629/629 default + UBSan). Detail: `docs/reference/99-fs-permission.md`
  + IDENTITY-DESIGN.md §9.5 + `audit_a2a_closed_list.md`.
- **A-2b:** `SYS_WALK_CREATE` — make `dev9p_create` / `p9_client_lcreate` live;
  stamp owner = caller principal-id, group = parent-dir (Plan 9/BSD), mode =
  default & umask.
- **A-2c:** uniform **mount-cape** for self-declared-non-POSIX backings — folds
  into A-2d as the metadata source for permissionless backings.
- **A-2d (LANDED 2026-05-30; devramfs-live, dev9p deferred to A-3; audit R1
  CLEAN 0/1/1/1):** the **kernel rwx-permission layer** (`kernel/perm.c` +
  walk/open/create/wstat chokepoints; 8 `perm.*` tests; 637/637 default+UBSan +
  boot OK + cross-reboot PASS). **Audit R1 CLEAN at v1.0** -- the active surface
  is sound (I-22 no-identity-bypass, owner-first, fail-closed, wstat policy,
  O_PATH, A-2a F2 closed, boot survival); the F1(P1)+F2(P2) are **dormant at
  v1.0** (devramfs RO+world-readable; dev9p unenforced) and **deferred to A-3 as
  named activation prerequisites** -- A-3 MUST, in the pass that flips
  `dev9p.perm_enforced=true`: (F1) derive the SYS_WALK_OPEN handle rights from the
  omode (today hardcoded R|W[|TRANSFER]); (F2) add `perm_check(parent, W|X)` to
  rename+unlink. Loud in-code prereq notes at both sites; full record in
  `audit_a2d_closed_list.md`. (Linux-VFS model; IDENTITY-DESIGN §3.7 + the §3.7.1
  privilege-model refinement + the §9.6 implementation contract). At walk/open/
  create, `perm_check` the file's mode/uid/gid (`spoor_stat_native`) against the
  Proc's `principal_id` + groups (owner-first POSIX); enforcement points
  walk[X]/open[R/W, `O_PATH` exempt]/create[W+X]; read/write not re-checked
  (open-time snapshot). **`CAP_HOSTOWNER` is the unified v1.0 fs-admin authority**
  (user-voted 2026-05-30: DAC-override + chmod/chown/chgrp-any; a capability, never
  an identity -> **I-22 preserved**, no `principal_id` bypasses); owner keeps
  owner|self-group chmod + chgrp-to-own-group; **no-give-away chown**. **Replaces
  the dropped "server enforces" assumption** — Stratum enforces dataset-scope ONLY,
  not file rwx (agent-verified 2026-05-28; see §3.7). Folds A-2b's create-check +
  **closes A-2a audit F2** (gate `dev9p_stat_native` on the `Rgetattr` valid mask);
  A-2c mount-cape stays a seam; A-4 splits a finer `fs-admin` clearance
  (`CAP_DAC_OVERRIDE`+`CAP_CHOWN`) additively. **Testable now** via
  `CAP_SET_IDENTITY` (devramfs deny/allow). **dev9p enforcement DEFERRED to A-3**
  (user-signed-off 2026-05-30): ground truth shows uniform dev9p enforcement bricks
  the boot (host-bake stamps pool entries host-uid-owned 0644/0755; the
  `PRINCIPAL_SYSTEM` boot chain has no `CAP_HOSTOWNER`, so as *other* it cannot
  write the pool -> post-pivot creates `/var/lib/corvus` + `susan` denied). Gated by
  a new `Dev.perm_enforced` flag (devramfs=true, dev9p=false; A-3 = one-line flip);
  dev9p stays handle-RIGHT-gated only at v1.0. The wstat ownership-change policy is
  also `perm_enforced`-gated (dormant + unit-tested at v1.0).
- **Depends:** A-1. Interlocks with Stop C FS-mutation (this *is* create+setattr).
- **Audit:** the create + setattr 9P-write surface (AEGIS-adjacent) -> full round.
- **Tests:** create stamps owner=caller + group=parent; chmod/chown round-trip;
  mount-cape uniform perms on a FAT-like backing; ownership survives reboot.

### A-3 · 9P identity presentation + dev9p enforcement activation + per-user stratumd *(design-first; scripture + A-3a + A-3b + A-3c LANDED + audited CLEAN; A-3 arc DONE)*

**A-3a + A-3b LANDED 2026-05-31.** A-3a = reconciliation mechanism (pouch SO_PEERCRED
→ principal + Stratum `--bake-owner-uid` + kernel `n_uname` = principal + the
`stratum_host_tools_stale` build-staleness fix); behaviorally inert (enforcement off);
637/637 + boot OK + cross-reboot PASS. A-3b = enforcement activation: flipped
`dev9p.perm_enforced = true`, closed A-2d **F1** (`rights_for_omode`) + **F2**
(`perm_check` on rename/unlink), added the `stratumd-stub` Tgetattr + the
`stratum-mkfs --root-uid`/`--root-gid` no-brick fix (the mkfs root inode was uid=0/0755,
so the PRINCIPAL_SYSTEM boot chain hit it as *other* and joey's create in the root was
denied — M2 had to stamp the ROOT SYSTEM-owned, not just the baked files). 639/639
(smp1) + boot OK + cross-reboot PASS under live enforcement (full corvus identity-DB
create / rename-swap / cross-reboot reload as PRINCIPAL_SYSTEM owner). **A-3 audit R1
CLEAN** (Opus prosecutor: 0 P0 / 0 P1 / 1 P2 / 3 P3, all fixed -- the P2 was the
create-leg of F1, `sys_walk_create_handler` now also derives rights from omode;
`audit_a3_closed_list.md`).

**A-3c LANDED.** M6 -- proved the per-user-stratumd dataset-scope EACCES channel is
reachable from Thylacine: the kernel collapsed every Tattach failure to a bare -1 in
`p9_attached_create` (the `Rlerror` ecode the 9P client already maps to `-errno` was
lost), so an out-of-scope attach was indistinguishable from any other failure. A-3c
threads the ecode out via a new `int *out_err`, and both `sys_attach_9p*_handler`s map
it through `attach_err_to_ret` so an out-of-scope attach returns `-T_E_ACCES` (pouch ->
EACCES). The Stratum `--role client`/`--user-policy` mechanism itself is built + tested
upstream (`tests/test_proxy_9p.c`); the **per-login spawn is A-5**. M5 -- the corvus
trust-stamp gate recorded as a v1.x SEAM (no v1.0 caller; every v1.0 attach is local
`SrvConn` SO_PEERCRED, unforgeable; no `trusted_for_identity_fwd` field exists -- clean
add). User-chosen scope 2026-06-01: "plumb the ecode" (focused) over the heavier live
boot probe. The live multi-principal dev9p enforcement probe (A-3 audit confidence note)
deferred to A-5. Kernel test extends `handshake_failure_returns_null` to assert
`-T_E_ACCES`. **A-3c audit R1 CLEAN** (Opus prosecutor a923edeff825a7866 + self-audit:
0 P0 / 0 P1 / 1 P2 / 2 P3, all fixed -- verdict: the M6 commit introduces NO new defect;
the 3 findings were PRE-EXISTING `map_error` robustness issues A-3c raised in salience).
The close fix: `map_error` now bounds the wire ecode before negating
(`ecode == 0 || ecode > 4095 -> -EIO`), killing a server-controlled signed-overflow UB
(`-(int)0x80000000` traps under UBSan -- a kernel halt reachable by any `Rlerror` on any
op) and folding the `ecode=0`-as-success corner; +regression test
`handshake_rlerror_ecode_overflow_clamped`. MATRIX GREEN: default(smp4) + UBSan + smp8
ALL 651/651, 0 EXTINCTION, boot OK. `audit_a3c_closed_list.md`.

**Reshaped by ground truth (corrects F-4).** The 2026-05-28 sketch said "forward
principal-id as `n_uname`." Ground truth: **Stratum ignores `n_uname`** and reconciles
identity via **`SO_PEERCRED`** (which pouch already marshals from the kernel's
unforgeable `SYS_srv_peer`, but the shim hardcodes `uid=0`, a pre-A-1a stub). Two user
votes 2026-05-31: **`SO_PEERCRED` is the load-bearing local channel** (n_uname demoted
to the v1.x foreign/authenticated path); **flip dev9p enforcement now**. Full design:
IDENTITY-DESIGN.md §9.7 + the §3.5 F-4 correction + the §3.7.1 activation note.
**Stratum A2 (`--role client`) verified MERGED** on `thylacine-pouch-arm` (`run.c:301`).

- **Builds (split a/b/c):**
  - **A-3a** -- reconciliation: pouch SO_PEERCRED shim `ucred.uid = principal_id` /
    `gid = primary_gid` (M1); Stratum `--bake-owner-uid`/`--bake-owner-gid` flag +
    `build.sh` stamps the pool `PRINCIPAL_SYSTEM`-owned (M2; NOT an on-disk-format
    change); kernel substitutes `principal_id` for `n_uname` at the two Tattach sites
    (M4, belt-and-suspenders).
  - **A-3b** -- activation: flip `dev9p.perm_enforced = true` (M3) + close A-2d **F1**
    (derive SYS_WALK_OPEN handle rights from omode; `T_OPATH` keeps the A-1.7 born-R|W
    base) + **F2** (`perm_check(parent, W|X)` on rename [both dirs] + unlink). Depends
    on A-3a (the flip bricks without the reconciliation).
  - **A-3c (LANDED)** -- M6: surface the Tattach `Rlerror` ecode (`p9_attached_create`
    `int *out_err` + `attach_err_to_ret` in both handlers) so an out-of-scope attach to a
    per-user stratumd returns `-EACCES`, not bare `-1` (the dataset-scope channel is now
    observably reachable from Thylacine; Stratum mechanism + per-login spawn are upstream-
    built / A-5). M5: trust-stamp gate recorded as a v1.x SEAM. Live multi-principal dev9p
    enforcement boot probe deferred to A-5.
- **Depends:** A-1, A-2d (closes its F1+F2). **Prereq verified:** Stratum A2 merged.
- **Audit:** privilege boundary (I-22 / I-2 / I-4 / I-6; AEGIS-adjacent write path) ->
  full round. Prosecute the kernel-stamped-vs-forgeable identity, F1 rights-narrowing
  (no latent wrong-omode caller; T_OPATH base preserved), F2 both rename parents, the
  host-bake no-brick, the `--bake-owner-uid` override placement.
- **Tests:** a `CAP_SET_IDENTITY` non-system child is DENIED write to SYSTEM-owned dev9p
  + ALLOWED read/exec (same shape as the A-2d devramfs test, now on dev9p); F1 OREAD ->
  RIGHT_READ-only handle, O_PATH -> R|W; F2 non-owner cannot mutate a no-other-w dir;
  out-of-scope dataset -> EACCES; boot OK + cross-reboot PASS (no brick).

### A-4 · Clearance + legate elevation + CAP_KILL + trusted path *(DESIGN RESOLVED 2026-06-01; scripture landed; splits pre/a/b/c)*

**Design-first pass DONE** (this scripture commit; no code). Prior-art grounding (Plan 9
`/proc`-ctl + seL4/Zircon/Genode derived authority + NT/AIX SAK + Genode Nitpicker) +
verified tree facts. Full design pinned in **IDENTITY-DESIGN.md §9.8** (+ the §3.1/§3.3
reconcile), **ARCH** §28 (new I-25/I-26/I-27) + §7.6 (OPEN Q 7.6.B CLOSED) + §25.4 audit
rows, **CORVUS-DESIGN** §5.5.1 / §5.7 / §6.4. **Two user votes 2026-06-01:** (1) cross-process
kill = BOTH the namespace `/proc/<pid>/ctl` surface AND a narrow elevation-only `CAP_KILL`;
(2) trusted path = build the kernel SAK now (pulling the kernel console RX path forward).

- **A-4-pre** *(LANDED `c617429`)* -- closed the **P5-hostowner I-2 hole** (the named
  prerequisite below): `rfork_internal` now ANDs `~CAP_ELEVATION_ONLY`. At v1.0
  `CAP_ELEVATION_ONLY = {CAP_HOSTOWNER}`; the strip auto-covers DAC_OVERRIDE + CHOWN + KILL
  as A-4a/b add those bits to the macro. + the `caps.rfork_strips_elevation_only` regression
  test (652/653 FAIL pre-fix, 653/653 post). Formal audit folds into A-4a's round.
- **A-4a** -- clearance-level policy objects (corvus-held; **structured-TLV** caps so v1.x
  resource-scoping is additive) + the **legate** = the existing `cap`-device two-phase grant
  generalized (corvus registers via the new `CAP_GRANT_CLEARANCE`; the Proc redeems; kernel
  stamps `caps |= clearance ∩ self_restriction`). Durable `principal_id` UNCHANGED + an
  ephemeral `legate_session_id`; scope = `legate_scope_id` subtree, **fully torn down**
  (group-terminate, reusing #809/#811) on the legate root's exit OR `valid_until` expiry; no
  local crypto. New corvus verbs CLEARANCE_LIST / ACTIVATE / GRANT / REVOKE (14-17). *Split:*
    - **A-4a-1** *(LANDED `6ea2d2c`)* -- caps.h foundation: `CAP_GRANT_CLEARANCE`=1<<6
      (fork-grantable) + `CAP_DAC_OVERRIDE`=1<<7 / `CAP_CHOWN`=1<<8 / `CAP_KILL`=1<<9
      (elevation-only); `CAP_ELEVATION_ONLY` -> the 4-bit set; CAP_ALL/ELEVATION disjoint assert.
    - **A-4a-2a** *(LANDED `1ee8cab`)* -- legate scope fields on `struct Proc` (248->264:
      `legate_session_id`@248 / `legate_scope_id`@252 / `legate_valid_until`@256 +
      `PROC_FLAG_LEGATE_ROOT`) + rfork-inherit (a child JOINS the scope; the ROOT flag is NOT
      inherited). DORMANT until 2b.
    - **A-4a-2b** *(LANDED `85efd7c` -- the kernel mechanism)* -- the `cap` device clearance grant/redeem
      (`/grant` length-discriminated 16=hostowner / 32=clearance; `/cap/use` ONE locked
      kind-branched redeem; the redeem rides the EXISTING `/cap/use` file -- NO new syscall) +
      `proc_become_legate` + the two teardown triggers (exits() root-exit locked walk; EL0-tail
      `valid_until` expiry lockless walk) + `perm.c` honoring `CAP_DAC_OVERRIDE`/`CAP_CHOWN`. 13
      kernel tests (devcap clearance lifecycle + teardown walk via synthetic Procs + perm
      cap-mapping); 667/667 default. **v1.0 model:** the clearance set is all elevation-only ->
      rfork-stripped -> only the ROOT is elevated; member teardown is the scripture tidiness
      sweep (a spawn-race straggler is benign + UNELEVATED, not an I-25 violation). Ref:
      `docs/reference/102-legate.md`.
    - **A-4a-3** *(LANDED: alpha `b7edcc7` / beta `6224028` / gamma `e48a14c`)* -- the
      userspace half + the boot E2E. **alpha**: `SYS_CAP_GRANT_CLEARANCE = 61` (the grant-side
      bridge -- corvus is chrooted, reaches the cap device by syscall, like the hostowner grant;
      "NO new syscall" was always about the REDEEM, which rides `SYS_CAP_USE`) + the libthyla-rs
      cap mirror. **beta**: the corvus clearance subsystem -- a built-in level table (`fs-admin`
      = DAC_OVERRIDE|CHOWN + `supervisor` = KILL; both RE_AUTH) + per-user eligibility persisted
      in `/var/lib/corvus/clearance.db` (rename-swap) + the four verbs 14-17 (LIST/ACTIVATE
      [reads peer stripes via SYS_SRV_PEER, registers the grant] / GRANT/REVOKE [CAP_HOSTOWNER]).
      **gamma**: the boot E2E legate prover (`usr/legate-prover/`) -- AUTH michael -> LIST ->
      ACTIVATE fs-admin -> redeem -> legate root; joey grants corvus CAP_GRANT_CLEARANCE +
      CLEARANCE_GRANTs michael fs-admin. GREEN end-to-end (`legate E2E OK` -> `Thylacine boot
      OK`; 667/667). **The prover is a NO-MEMBER legate (the v1.0 model: a legate HOLDS the
      clearance + does the work itself).** A scope-MEMBER teardown E2E is v1.x -- it needs BOTH
      fork-grantable clearance caps AND a general kproc orphan-reaper (a torn-down member orphans
      to kproc's strict wait_pid -> extinction; the documented `wait_pid_for(pid)` lift). The
      member walk is unit-covered (test_proc) + the death is #809/#811. Ref:
      `docs/reference/102-legate.md`.
    - **A-4a audit** *(LANDED `a19b586` -- Opus prosecutor a597f2841af126e72 + an in-session
      self-audit; R1 CLEAN **0 P0 / 0 P1 / 1 P2 / 2 P3**; **I-2 / I-22 / I-25 HELD**)* -- covers
      A-4-pre + 1 + 2a + 2b + 3. **F1 (P2) FIXED**: the legate-root scope teardown was `exits()`-only
      and skipped on the kill / group-terminate death path (the path **A-4b's `CAP_KILL` drives**);
      extracted `proc_legate_teardown_if_root` and relocated the sweep to `proc_become_zombie_locked`
      (the shared ZOMBIE chokepoint) so it fires on EVERY root death path + a regression test
      (`proc.legate_teardown_from_zombie_chokepoint`: the chokepoint sweep + the ROOT-flag guard).
      **F3 (P3) FIXED**: saturating `valid_until` add (removes the wrap-to-0 "no time bound" sentinel
      alias). **F2 (P3) DEFERRED** to the member-bearing v1.x work (double-redeem scope orphan;
      UNELEVATED stragglers, unreachable at v1.0). The **kproc-orphan-reaper** debt is confirmed
      carried v1.x (`wait_pid_for(pid)`), not an A-4a defect (the no-member model keeps it dormant).
      Matrix GREEN: default(smp4) + UBSan + smp8 all **668/668** + legate E2E OK + boot OK.
      `audit_a4a_closed_list.md`. **A-4a arc DONE.**
- **A-4b** *(DONE -- impl `4edd65c`; scripture reconcile `17dcb77` + fix `585d519`; audit close `c6dfced`)* --
  cross-process kill: `/proc/<pid>/ctl` write parses `kill`/`killgrp` -> `proc_group_terminate`
  (uniform, single + multi thread; the #811 wake-total primitive), under `g_proc_table_lock`
  via the `proc_for_each` resolve+authorize idiom (the audited `sys_postnote` pattern; no
  reap-UAF). devproc gains `stat_native` (per-pid owner + mode: ctl 0600, info 0444).
  **Two-axis** authority (I-26), enforced at the **WRITE** site (`perm_enforced` stays FALSE):
  owner (same `principal_id` on 0600 ctl) OR `CAP_HOSTOWNER` OR `CAP_KILL` -- computed
  DIRECTLY (NOT via `perm_check`), so `CAP_DAC_OVERRIDE` is NOT a kill axis (fs-admin stays
  orthogonal to process-kill). **kproc (pid 0) is unkillable.** Two scripture reconciles
  (user-voted 2026-06-01): (1) enforcement moved open->write because the shared open
  chokepoint hard-rejects pre-`devproc.open`, so the gate-at-open sketch could not host the
  `CAP_KILL` axis (`17dcb77`); (2) the kill authority excludes `CAP_DAC_OVERRIDE` -- the
  `perm_check`-routing would have folded it in (`585d519`). **USER-REACHABILITY of `/proc` is
  a Utopia namespace seam** (the `namec` multi-component mount-crossing resolver + a boot
  `SYS_MOUNT`), user-confirmed NOT in A-4b -- the mechanism + authority are kernel-unit-tested
  (4 devproc tests: predicate / stat_native / dispatch / rejects). Resolves ARCH OPEN Q 7.6.B.
  **Audit R1 CLEAN** (Opus prosecutor a20761e4222353045 + an in-session self-audit;
  **0 P0 / 0 P1 / 0 P2 / 3 P3**; I-26 / I-22 / I-1 all HELD): F3 (denied-test principal landed
  on the PRINCIPAL_NONE sentinel) FIXED; F1 (SYSTEM-owns-SYSTEM owner breadth + joey/init not
  guarded -- correct identity-model consequence, dormant: /proc unmounted + joey exits) +
  F2 (stat_native lockless find -- same class as devproc_read, NOT on a security path:
  perm_enforced=false) DEFERRED-with-justification to the /proc-mount chunk. Matrix:
  default + UBSan + smp8 ALL **671/671** + boot OK + 0 EXTINCTION. Closed list:
  `audit_a4b_closed_list.md`. **A-4b arc DONE.**
- **A-4c** -- trusted path (vote: build the SAK now). **As-built design refined + landed
  scripture-first 2026-06-01** (IDENTITY-DESIGN §9.8 "As-built resolution" + ARCH §28 I-27 +
  §25.4 + CLAUDE.md rows; the implementer's calls within the approved shape after a
  GIC/UART/notes/poll ground-truth + Plan-9 / Linux-serial-SysRq / NT-AIX-SAK prior-art pass):
  - **SAK = serial BREAK** (a line condition, not a data byte -> EL0 data cannot forge it,
    stateless recognizer; the "cannot be starved/spoofed" obligation becomes structural).
    Rejected the "reserved multi-byte escape" alternative (more surface + byte-forgeable).
  - **Deferred-action via a `console_mgr` kproc kthread**: the RX IRQ handler is wakeup-only
    (`notes_post`/`poll_waiter_list_wake` use plain `spin_lock`, NOT IRQ-safe; only `wakeup()`
    on a `Rendez` is) -- it drains the PL011 RX FIFO into a ring, classifies (BREAK->SAK /
    Ctrl-C->interrupt, cooked-consumed / other->data) + wakes; the kthread does the privileged
    work in process context.
  - **`g_console_owner`** under `g_proc_table_lock`, init to joey, cleared by `exits()` on
    owner-exit; `proc_revoke_console_attached` clears the flag atomically; SAK re-grants to
    corvus via `g_console_trusted_proc`, FAIL-SAFE revoke-only (`=NULL`) if absent.
  - **Data wait = single `Rendez` + single-reader busy-guard** (poll_waiter_list isn't IRQ-safe
    to wake; 2nd concurrent blocking read returns -1, not the single-waiter extinction).
  - **PL011 IRQ = SPI 33** hardcoded as the QEMU-virt fallback (DTB `interrupts`-parsing = a
    Lazarus seam); reserved in `irqfwd_init` like the timer.
  - **Test (harness-honest)**: the harness can't inject UART RX non-interactively without
    touching the boot-banner ABI -> in-kernel unit tests (synthetic drive) + boot survival +
    the interactive `Ctrl-A b` BREAK path.
  - **Split**: **c-1** = console RX (RX IRQ + ring + blocking `devcons_read` + Ctrl-C + the
    kthread + `g_console_owner` + the `exits()` clear-hook) lands + audits first; **c-2** = the
    BREAK->SAK revoke/re-grant + `g_console_trusted_proc` + the I-27 handoff lands + audits
    second. On the kernel UART Dev `dc='c'`, NOT the userspace virtio-input path (they coexist).
  - **c-1 LANDED + audit CLEAN** *(impl `426c10e`; audit close below)*: `uart.c` RX IRQ +
    `uart_rx_init` + `UART_INTID_PL011=33`; `cons.c` ring + blocking `devcons_read`
    (single-reader busy-guard) + `cons_rx_input` (IRQ wakeup-only) + the `console_mgr` kthread
    (deferred Ctrl-C `interrupt` post); `proc.c` `g_console_owner` + `proc_set_console_owner` +
    `proc_console_post_interrupt` + the zombie-chokepoint owner-clear; `joey.c` sets the boot
    owner; `main.c` boot wiring; `irqfwd.c` SPI-33 reservation. 9 `cons.*` tests; removed the
    stale `cons.read_returns_eof` (its EOF contract is superseded by the blocking read).
    **Audit R1 CLEAN (Opus + self-audit converged; 0 P0 / 0 P1 / 0 P2 / 3 P3, all FIXED):**
    F1 cross-lock `count`/`intr_pending` race -> RELAXED atomics + corrected comment;
    F2 `_Static_assert` CONS_RING_SIZE power-of-two; F3 the two-thread `cons.blocking_read_wakeup`
    I-9 regression test. Matrix: default(smp4) + UBSan + smp8 all **678/678** + boot OK.
    Closed list: `audit_a4c1_closed_list.md`. **c-2 (SAK) NEXT.**
    - **INTERLEAVED (2026-06-02): #810 SMP secondary-timer fix landed** (a "long bug
      hunt" preempting c-2, user-directed). The intermittent boot HANG at
      `pouch-hello-exitgroup` was root-caused (QEMU gdbstub, not the handoff's
      "cascade-reap" guess) to **secondary CPUs having no per-CPU timer** -> no
      preemption on secondaries -> a CPU-bound EL0 thread (the exitgroup test's
      `main` spinning `while(g_started<2)`) monopolized a secondary while its worker
      starved -> `wait_pid` hung. Fix: arm a per-CPU timer on every secondary,
      DEFERRED to the production transition (`smp_enable_secondary_preemption()` in
      `boot_main`) so the UP-like in-kernel tests stay quiescent. Invariants I-8 / I-17
      now hold on secondaries. Matrix: smp4 0/20 hangs + UBSan 5/5 + smp8 0/8, all
      678/678; adversarial prosecutor + self-audit CLEAN (0 P0/0 P1/0 P2/3 P3 fixed).
      The two twice-reverted P2-Dd blockers (thread_free/on_cpu; SP_EL1/SPSel) are
      closed by #788 + EL1h. See `memory/bug_810_smp_no_secondary_timer.md` +
      `docs/DEBUGGING-PLAYBOOK.md` §6.14 + `docs/reference/11-timer.md`. **c-2 still NEXT.**
  - **c-2 LANDED** *(impl + audit close `a0f6163`)*: the BREAK->SAK revoke/re-grant +
    the I-27 trusted-path handoff. `cons.c` a BREAK sets `sak_pending` (was discarded) + wakes
    `console_mgr`, which (process context) runs the new `proc.c::proc_console_sak`. `proc.c`
    adds `g_console_trusted_proc` (under `g_proc_table_lock`, zombie-chokepoint-cleared like
    `g_console_owner`) + `proc_set_console_trusted` + `proc_revoke_console_attached` (atomic
    AND); `proc_mark_console_attached`/`proc_is_console_attached` became `__atomic_*` RELAXED
    (the console bit is now multi-writer -- mark+revoke from the kthread). `proc_console_sak`
    under `g_proc_table_lock`: revoke from owner (+ post `interrupt`) -> re-grant to
    `g_console_trusted_proc` -> owner=trusted; FAIL-SAFE revoke-only (owner=NULL) if no trusted
    Proc alive; idempotent no-op if trusted already owns. Wiring: `SPAWN_PERM_CONSOLE_TRUSTED`
    (syscall.h bit 1 + both spawn thunks + libt + libthyla-rs); joey grants it to corvus
    (console-attached-caller-gated). I-27 anchor unchanged: the devcap HOSTOWNER redeem gate
    keys on `PROC_FLAG_CONSOLE_ATTACHED`, so post-SAK only corvus can redeem. The note reuses
    `interrupt` (closed notes table; a dedicated console-revoked name is a v1.x notes SEAM).
    Inert at boot (no BREAK injectable). 4 new tests (`proc.revoke_console_attached` +
    `cons.sak_revoke_regrant` / `cons.sak_failsafe_revoke_only` / `cons.sak_idempotent_flood`)
    + `cons.break_discarded` -> `cons.break_sets_sak` + the F3 end-to-end
    `cons.sak_via_console_mgr`. Matrix: default(smp4) + UBSan + smp8 all **683/683** + boot OK
    + 0 EXTINCTION. **Audit R1 CLEAN** (Opus `aa343d43` + an in-session self-audit CONVERGED on
    F1; 0 P0 / 1 P1 / 0 P2 / 2 P3, all fixed): F1 the multi-writer-`proc_flags` torn-RMW race
    (MLOCKALL/SET_DUMPABLE/SET_TRACEABLE left non-atomic vs the now-multi-writer console bit ->
    a Ctrl-A b BREAK in corvus's startup window could clobber the console-bit clear -> I-27;
    fixed by making every `proc_flags` RMW atomic) -- self-audit found + fixed it before the
    prosecutor returned; F2 `break_sets_sak` console_mgr drain; F3 the `sak_via_console_mgr`
    dispatch test. Closed list: `audit_a4c2_closed_list.md`. **A-4 (the whole arc) DONE.**
- **Depends:** A-1 + corvus. **Seams:** resource-scoped HW-cap allowlist (the structured caps
  TLV); distributed clearance crypto-proof (v1.x); the graphical Nitpicker-style trusted
  screen (Halcyon); a finer per-target kill handle (vs the blanket `CAP_KILL`).
- **New invariants:** I-25 (legate scope bounded + fully revoked), I-26 (cross-process kill
  two-axis), I-27 (trusted path: unspoofable elevation prompt). All ARCH §28.
- **Audit:** highest-stakes privilege surface -> a focused adversarial round per sub-chunk.
- **Tests:** I-2 strip (elevated parent rforks -> child lacks the elevation-only bits);
  activate -> legate-has-caps -> scope-exit / `valid_until`-expiry tears down the subtree;
  revoke blocks future activation; kill via `/proc/ctl` owner-rwx + `CAP_KILL` + the parent
  case; a non-owner without `CAP_KILL` is denied; console RX read + Ctrl-C; SAK revokes +
  re-grants; only the console holder redeems elevation; clearance secret != data key.

### A-5 · Login + session lifecycle + per-user encrypted home + hostowner-c *(integration)*
- **Design RESOLVED 2026-06-02** (IDENTITY-DESIGN §9.9; 3 votes + a refining 4th, after a
  Plan 9 / capability-microkernel prior-art pass + a background Stratum
  per-user-encryption-readiness verification): (1) Full encrypted home; (2) stratumd-asks-
  corvus for the DEK (login never holds the raw key); (3) userspace session-leader (logout
  reaps the group via A-4b CAP_KILL + unmount + corvus SESSION_CLOSE; no new kernel construct);
  (4) at-rest + session-scoped encryption (the scripture property + the Linux/macOS norm), NOT
  the coordinator-blind property (recorded as a v1.x NOVEL). NATIVE /sbin/login (libthyla-rs);
  shell = ut; single-console-serial at v1.0.
- **Builds:** `/sbin/login` (SAK-gated console prompt -> corvus AUTH -> CAP_SET_IDENTITY stamp
  via SYS_SPAWN_FULL_ARGV -> per-user `--role client` stratumd -> bind /home -> spawn ut as the
  session leader); logout lifecycle (DEK evict + group-terminate + mount teardown +
  SESSION_CLOSE); P5-hostowner-c (RECOVER paper phrase). The corvus Q11 4-byte header is
  already live (A-3).
- **A-5a LANDED** (login core; "live session now" boot shape, user-voted 2026-06-02):
  A-5a-alpha (`97a3af5`) = kernel substrate (3 syscalls SYS_BOOT_COMPLETE=62 /
  SYS_CONSOLE_RELINQUISH=63 / SYS_CONSOLE_OPEN=64 + `boot_mark_complete` banner move +
  `proc_console_relinquish` + 3 kernel tests, 686/686; the `Thylacine boot OK` string is
  unchanged, it now fires on init's SYS_BOOT_COMPLETE not joey's exit -- TOOLING.md 10 +
  CLAUDE.md updated). A-5a-beta (`161efa1`) = native `/sbin/login` (`usr/login`; fd-0 creds -> corvus
  AUTH -> RESOLVE_NAME/ID -> spawn `ut` stamped via `Command::identity()`) + joey
  `do_login_e2e` seeded CI proof (michael -> uid=1000 gid=1000 -> ut spawned+reaped, gated on
  login exit) + `session_getty_loop` (the boot log reaches a live `Thylacine login:` prompt).
  As-built: `docs/reference/103-login.md`. **A-5a audit R1 CLEAN** (0 P0/0 P1/2 P2/3 P3, NOT dirty;
  prosecutor + self-audit converged): F1 (test.sh masked a post-banner extinction -> EXTINCTION-
  precedence + a post-banner grace window) + F2 (`SYS_CONSOLE_OPEN` ungated -> gated on
  console-attach; joey opens /dev/cons pre-relinquish + reuses it) + F4 (comment) FIXED; F3 (assert
  ut's stamped id -- needs a self-id syscall) + F5 (getty backoff -- needs a sleep primitive)
  deferred-justified. Matrix default+UBSan+smp8 GREEN (smp8 wants BOOT_TIMEOUT=1200 on M2).
  `SPAWN_PERM_CONSOLE_OWNER` deferred (I-27: non-attached login can't pass the console-gated perm).
  NEXT: A-5b. See `memory/audit_a5a_closed_list.md`.
- **Stratum-side** (in-scope; verified NO format/wire-ABI break): deferred-unwrap soft-skip +
  runtime DEK install/evict (reusing `stm_corvus_unwrap`) + token-forward, on `thylacine-pouch-arm`.
- **Split:** A-5a (login core; Stratum-independent; lands first) -> A-5b (encrypted home + the
  Stratum sub-chunk) -> A-5c (RECOVER + hostowner-c). Each audit-bearing.
- **Depends:** A-1..A-4.
- **Audit:** trusted-path login + identity-stamp + the DEK handoff (AEGIS/mallocng-adjacent --
  prosecute hard) -> a focused round per sub-chunk.
- **Tests:** login as a user -> correct principal-id + home + shell; a 2nd user cannot
  read/attach the first's home; logout -> DEK evicted + procs reaped + mounts torn down;
  cross-reboot at-rest (home unreadable without the passphrase); RECOVER round-trip.
- **I-27 carry (A-4c-2 forward-note):** during a session corvus is the SOLE console-attached
  Proc; login + shell are NEVER attached; joey relinquishes its boot attach at the
  bringup->session boundary. Optional `SPAWN_PERM_CONSOLE_OWNER` for Ctrl-C-to-shell.
- **A-5b open impl-design -- RESOLVED 2026-06-02** (ground-truth + a user vote): **the
  coordinator PULLS** the DEK with the login-forwarded token over its own corvus connection
  (the §6.3 bearer-token-forward; push rejected -- inverts corvus's role + corvus lacks the
  storage layout). The first same-day "no corvus lift / all Stratum-side" note was INCOMPLETE
  (corrected `ecbd8f4` -> this commit): no pouch stratumd has ever reached corvus, so the pull
  needs TWO enabling changes -- (1) **pouch->corvus transport**: the pouch `connect()` must
  walk to corvus's `ctl` fid (`sun_path_to_name` rejects `/` today; the kernel 2-arg
  `SYS_srv_connect` already drives the walk -- login proves it; no corvus/Stratum-client
  change); (2) **corvus session-ownership lift (user-voted Option B)**: clear the global AUTH
  SESSION only on the OWNING connection's close or explicit SESSION_CLOSE, not on any close
  (`close_conn`@`usr/corvus/src/main.rs:3352`) -- else the coordinator's transient connection
  wipes login's session mid-session + breaks A-4 elevation. PLUS the Stratum-side: deferred-
  unwrap soft-skip + runtime DEK install/evict (two new writable `/ctl` `install-dek`/`evict-dek`
  kinds over `--ctl-listen`) + host-bake. NO on-disk-format/wire-ABI break. Full reasoning:
  IDENTITY-DESIGN §9.9 "The call (CORRECTED 2026-06-02)" + CORVUS-DESIGN §6.2 "AUTH-session
  ownership".
- **A-5b-0 (stalk) -- the isolation FOUNDATION, sequenced FIRST (user-voted 2026-06-02; #831).**
  The A-5b design prosecution (#830) downgraded F1 (susan-reads-michael's-home) from P0 to
  P1-latent (the A-3 kernel rwx layer gates it), but its CORE stood: the per-Proc-territory model
  needs a real namespace so a 2nd user cannot even NAME another's coordinator -- and the global
  flat `/srv` registry (8 slots, ONE poster per name) literally cannot host two per-user
  `stratum-fs` coordinators. User voted (twice): build territory-scoped `/srv` as the FULL
  namespace-resident Dev, which requires building the multi-component pathname resolver
  (**stalk**; Plan 9 `namec`, user-named) that does NOT exist today (`SYS_WALK_OPEN` is
  single-component + never crosses a mount; the mount table is abstract-`path_id_t`-keyed with no
  string layer). DESIGN SIGNED OFF: `docs/STALK-DESIGN.md` (D1 defer bind-in-walk / D2
  post-by-create / D3 retire `SYS_SRV_CONNECT`+`POST_SERVICE` / D4 Plan-9 M1 mount-points-must-
  exist). Invariant **I-28** (path-resolution containment + per-component X-search; ARCH §9.6.7 +
  the §25.4 audit row + the §28 row). Sub-chunks: **stalk-1** (resolver core + `SYS_OPEN` ->
  absolute FS paths) -> **stalk-2** (mount re-key to Spoor identity + crossing) -> **stalk-3**
  (devsrv per-territory + `/srv` + retire old syscalls), each its OWN audit (path resolution = a
  privilege boundary). The A-5b body (corvus lift completion #829 + Stratum coordinator #826 +
  login wiring #827) resumes ON TOP of namespace-resident `/srv`. Scripture-first: this entry +
  STALK-DESIGN + ARCH land as the scripture commit (no code); impl follows citing its SHA.

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

- **DEPTH-FIRST PUSH IN PROGRESS -> A-1.7 before A-1b (read first).** A-1.7
  (capability-scoped service storage; 2nd-order detour; scripture landed this
  commit -- NOVEL §3.10 / ARCH §3.6 / I-23) is being built BEFORE A-1b because
  corvus is its first consumer. **A-1b (#776) is preempted at a clean boundary**
  (its mechanism-independent core was reverted at `6df247f`; reconstruct from
  CORVUS-DESIGN.md §16). **RESUME A-1b immediately after A-1.7 closes** -- build
  corvus's `identity.db` + wraps inside the handed `storage_root`, not via
  `FROM_ROOT`. Exact resume pointer: the A-1.7 section above. See
  `memory/feedback_depth_first_dependencies.md`.
- **Stratum A2 branch-merge prereq** (A-3): confirm `--role coordinator|client` is
  in the `thylacine-pouch-arm` branch before leaning on per-user stratumd.
- **A-1.5 / A-2 9P-write audit**: the create + write + fsync path (now landing in
  the A-1.5 FS-mutation foundation) is the AEGIS/mallocng-adjacent surface from
  Phase 6 -> prosecute hard.
- **Design-first sub-commits**: A-1a done; A-1.5 FS-mutation foundation done
  (§9.2, landed + audited); A-1.6 / FS-gamma (`SYS_RENAME` + `SYS_UNLINK`) done
  (§9.3, landed + audit-CLEAN); A-1b's corvus wire ABIs (RESOLVE_ID=11 /
  RESOLVE_NAME=12 / GROUP_CREATE=13 + USER_CREATE supp_gids extension) + the CRVS
  v2 `identity.db` byte format + the rename-swap persist/load flow are PINNED
  in **CORVUS-DESIGN.md §16** (`a433956`; UPG one-counter id==gid, atomic
  rename-swap on A-1.6, full identity-DB + keypair-wrap persistence) -- but
  **A-1.7 (capability-scoped service storage; 2nd-order detour, scripture this
  commit) is pulled BEFORE A-1b**: A-1.7 impl + audit, THEN resume A-1b (#776)
  building corvus persistence inside the handed `storage_root`; A-4 cap-model
  still owed.
- **Seams are load-bearing**: A-2 leaves the ACL seam; A-4 leaves the
  resource-scoped-HW-cap allowlist + the distributed crypto-proof seam. Don't bake
  fixed bitfields where a policy object belongs.
- **Open verify** (from the scripture pass): `ARCHITECTURE.md:660/662` describe
  `rfork(RFPROC)` as "copy-on-write address space" -- confirm the actual copy
  behavior; add a seam-note like §16 if it doesn't truly COW.
- **A-4 prerequisite (P1-class) -- CLOSED by A-4-pre `c617429`:** the P5-hostowner I-2
  hole (surfaced by the A-1a audit, NOT A-1a). `kernel/proc.c::rfork_internal` set
  `child->caps = parent_caps & caps_mask` WITHOUT `& ~CAP_ELEVATION_ONLY`, despite
  `caps.h:99-105` (P5-hostowner-b) asserting it strips elevation-only caps -- a
  `CAP_HOSTOWNER`-elevated parent could rfork / `SYS_SPAWN_*`(cap_mask incl. HOSTOWNER) a
  child inheriting `CAP_HOSTOWNER`, bypassing the console-attached `ADMIN_ELEVATE` gate
  (does NOT touch identity -- `CAP_SET_IDENTITY` is correctly fork-grantable, in CAP_ALL).
  A-4-pre added the 1-line strip + the `caps.rfork_strips_elevation_only` regression test;
  the sibling sweep confirmed `rfork_internal` is the single fork-time chokepoint (all three
  `SYS_SPAWN_*_CAPS` route through it; the devcap redeem is the sanctioned grant path, not a
  fork leak). Formal audit folds into A-4a. Was recorded in `memory/audit_a1a_closed_list.md`
  (carried-out-of-scope).

---

## References

- `docs/IDENTITY-DESIGN.md §8` — binding arc design + dispositions (the source of truth).
- `docs/ARCHITECTURE.md §28` (I-22), §25.4 (audit surfaces), §15.5/§24 (hardening reconciled).
- `docs/ROADMAP.md §8.0` — the detour arc within Phase 7.
- `docs/STRATUM-API-V1.md` + `-RESPONSE.md` — A2 (per-user stratumd), A3 (corvus wire), Q11.
- `docs/CORVUS-DESIGN.md` — key agent, hostowner model, identity authority.
